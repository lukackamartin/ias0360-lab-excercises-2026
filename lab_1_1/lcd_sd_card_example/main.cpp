#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"

#include "ff.h"
#include "sd_card.h"
#include "f_util.h"
#include "hw_config.h"
#include "LCD_Driver.h"
#include "LCD_Touch.h"
#include "LCD_GUI.h"
#include "DEV_Config.h"

extern LCD_DIS sLCD_DIS;

#define PATH_MAX_LEN 256

#ifndef WRITE_SUCCESS_FLAG
#define WRITE_SUCCESS_FLAG 0xABCDEF02
#endif

mutex_t mutex;
TP_DATA tp_data;

// Static pre-allocated snapshot buffer to prevent malloc() in TP_Save()
// Double-buffered with sDrawShadow for non-blocking Core 1 SD export
static uint8_t s_tp_save_buf[BOX_W * BOX_H];

// Sector-aligned streaming chunk buffer (4096 B = 8 physical 512-byte sectors)
// Invokes CMD25_WRITE_MULTIPLE_BLOCK in FatFs via DMA
static char s_chunk_buf[4096];

// Static file object and path buffers to prevent stack overflow on Core 1's 4 KB stack
static FIL s_fil;
static char s_path[PATH_MAX_LEN];
static char s_name[64];

// --------- Globals (FatFs requires the FS to outlive the mount) ----------
static FATFS fs;                    // must be static/global (lives as long as the mount)
static sd_card_t *g_sd = NULL;      // active SD card
static const char *g_drive = NULL;  // typically "0:"

static void join_path(char *out, size_t out_sz, const char *drive, const char *rel) {
    // Drive = "0:" or "0:/", ensure exactly one slash when joining
    if (rel && rel[0] == '/') rel++;  // avoid double slashes
    if (drive && drive[strlen(drive) - 1] == '/')
        snprintf(out, out_sz, "%s%s", drive, rel ? rel : "");
    else
        snprintf(out, out_sz, "%s/%s", drive, rel ? rel : "");
}

// ------------------------- Initialization ---------------------------------
static bool sd_init_and_mount(void) {
    if (!sd_init_driver()) {
        printf("sd_init_driver() failed\n");
        return false;
    }

    g_sd = sd_get_by_num(0);
    if (!g_sd) {
        printf("No SD config found (sd_get_by_num(0) == NULL)\n");
        return false;
    }

    g_drive = sd_get_drive_prefix(g_sd);  // usually "0:"
    if (!g_drive) {
        printf("sd_get_drive_prefix() returned NULL\n");
        return false;
    }

    // Force driver state to uninitialized so f_mount performs a fresh hardware bus probe
    if (g_sd->deinit) {
        g_sd->deinit(g_sd);
    }

    FRESULT fr = f_mount(&fs, g_drive, 1);
    printf("f_mount -> %s (%d)\n", FRESULT_str(fr), fr);

    if (fr == FR_NO_FILESYSTEM) {
        static BYTE work[4096];  // >= FF_MAX_SS (static to avoid stack overflow)
        MKFS_PARM opt = { FM_FAT | FM_SFD, 0, 0, 0, 0 };
        fr = f_mkfs(g_drive, &opt, work, sizeof work);
        printf("f_mkfs -> %s (%d)\n", FRESULT_str(fr), fr);
        if (fr == FR_OK) {
            fr = f_mount(&fs, g_drive, 1);
            printf("f_mount(after mkfs) -> %s (%d)\n", FRESULT_str(fr), fr);
        }
    }

    if (fr != FR_OK) {
        printf("Mount failed: %s (%d)\n", FRESULT_str(fr), fr);
        f_unmount(g_drive);
        if (g_sd->deinit) {
            g_sd->deinit(g_sd);
        }
        return false;
    }

    return true;
}

// ------------------------- File creation ----------------------------------
static FRESULT create_file(const char *abs_path, FIL *out_file) {
    // Creates/truncates a file and opens it for writing
    return f_open(out_file, abs_path, FA_WRITE | FA_CREATE_ALWAYS);
}

// ------------------------- File checking/listing --------------------------
static bool is_dot_or_dotdot(const char *name) {
    return (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
}

// Public checker: non-recursive root listing using static structures (0 stack overhead)
static FRESULT check_and_list_files(const char *root_drive) {
    char root[PATH_MAX_LEN];
    join_path(root, sizeof root, root_drive, "");

    static DIR dir;
    static FILINFO fno;
    FRESULT fr = f_opendir(&dir, root);
    if (fr != FR_OK) {
        printf("f_opendir('%s') -> %s (%d)\n", root, FRESULT_str(fr), fr);
        return fr;
    }

    uint32_t files = 0, dirs = 0;
    uint64_t total_bytes = 0;

    printf("\n--- SD Card File Listing for '%s' ---\n", root_drive);
    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') break;
        if (is_dot_or_dotdot(fno.fname)) continue;

        if (fno.fattrib & AM_DIR) {
            dirs++;
            printf("[DIR]  %s\n", fno.fname);
        } else {
            files++;
            total_bytes += (uint64_t)fno.fsize;
            printf("[FILE] %s  (%lu bytes)\n", fno.fname, (unsigned long)fno.fsize);
        }
    }
    f_closedir(&dir);

    printf("Summary: %lu file(s), %lu dir(s), %llu bytes.\n",
           (unsigned long)files, (unsigned long)dirs, (unsigned long long)total_bytes);
    return FR_OK;
}

// ------------------------------ Main -------------------------------------

void core1_entry() {

    printf("Core 1 entry: write to SD card\n");

    // Init and mount filesystem (Core 0 never touches SDIO)
    bool mounted = sd_init_and_mount();
    if (mounted) {
        check_and_list_files(g_drive);
    } else {
        printf("Core 1: SD card not detected at startup. Will initialize on demand.\n");
    }

    int count = 0;

    while (true) {

        uint32_t msg = multicore_fifo_pop_blocking();
        if (msg != DATA_READY_FLAG) {
            printf("Core 1 received unexpected message: 0x%08lX\n", msg);
            continue;
        }

        // If card was removed or not detected at boot, attempt to mount now
        if (!mounted) {
            printf("Core 1: Attempting to mount SD card on demand...\n");
            mounted = sd_init_and_mount();
            if (!mounted) {
                printf("Core 1: SD card unavailable / not mounted\n");
                multicore_fifo_push_blocking(SD_UNAVAILABLE_FLAG);
                continue;
            }
            check_and_list_files(g_drive);
        }

        printf("Writing data to a file\n");

        // Validate snapshot under mutex
        mutex_enter_blocking(&mutex);
        size_t len = tp_data.data_len;
        bool valid = (tp_data.data != NULL && len == (size_t)(BOX_W * BOX_H));
        mutex_exit(&mutex);

        if (!valid) {
            printf("ERROR: tp_data.data is NULL or data_len is 0!\n");
            multicore_fifo_push_blocking(WRITE_FAILED_FLAG);
            continue;
        }

        // Build absolute file path: <drive>/lcd_sd_card_example_<iteration>.txt
        snprintf(s_name, sizeof(s_name), "lcd_sd_card_example_%d.txt", count);
        join_path(s_path, sizeof(s_path), g_drive, s_name);

        printf("Core 1: Creating and writing to file: %s\n", s_path);

        // Create the file
        FRESULT fr = create_file(s_path, &s_fil);
        if (fr != FR_OK) {
            printf("Core 1: create_file failed: %s (%d)\n", FRESULT_str(fr), fr);
            f_unmount(g_drive);
            if (g_sd && g_sd->deinit) {
                g_sd->deinit(g_sd);
            }
            mounted = false;
            multicore_fifo_push_blocking(SD_UNAVAILABLE_FLAG);
            continue;
        }

        // Stream bitmap formatted into sector-aligned 4096-byte chunks (CMD25 via DMA)
        size_t buf_pos = 0;
        int hlen = snprintf(s_chunk_buf, sizeof(s_chunk_buf),
                            "# LCD Drawing %d (%dx%d)\n", count, BOX_W, BOX_H);
        if (hlen > 0) {
            buf_pos = (size_t)hlen;
        }

        bool write_ok = true;
        UINT total_bw = 0;

        for (uint16_t y = 0; y < BOX_H && write_ok; y++) {
            const uint8_t *row_src = &s_tp_save_buf[y * BOX_W];
            for (uint16_t x = 0; x < BOX_W; x++) {
                s_chunk_buf[buf_pos++] = row_src[x] ? '1' : '0';
                if (buf_pos == sizeof(s_chunk_buf)) {
                    UINT bw = 0;
                    fr = f_write(&s_fil, s_chunk_buf, sizeof(s_chunk_buf), &bw);
                    if (fr != FR_OK || bw != sizeof(s_chunk_buf)) {
                        write_ok = false;
                        break;
                    }
                    total_bw += bw;
                    buf_pos = 0;
                }
            }
            if (!write_ok) break;

            s_chunk_buf[buf_pos++] = '\n';
            if (buf_pos == sizeof(s_chunk_buf)) {
                UINT bw = 0;
                fr = f_write(&s_fil, s_chunk_buf, sizeof(s_chunk_buf), &bw);
                if (fr != FR_OK || bw != sizeof(s_chunk_buf)) {
                    write_ok = false;
                    break;
                }
                total_bw += bw;
                buf_pos = 0;
            }
        }

        // Flush remaining bytes in chunk buffer
        if (write_ok && buf_pos > 0) {
            UINT bw = 0;
            fr = f_write(&s_fil, s_chunk_buf, (UINT)buf_pos, &bw);
            if (fr != FR_OK || bw != (UINT)buf_pos) {
                write_ok = false;
            } else {
                total_bw += bw;
            }
        }

        if (write_ok) {
            fr = f_sync(&s_fil);
            if (fr != FR_OK) {
                write_ok = false;
            }
        }

        // Close the file
        f_close(&s_fil);

        // Explicitly terminate SDIO multiblock write (CMD12) so card is not left in receive mode
        if (g_sd && g_sd->sync) {
            g_sd->sync(g_sd);
        }

        if (write_ok) {
            count++;
            printf("Core 1: File write succeeded, wrote %u bytes (%u rows)\n", total_bw, BOX_H);
            multicore_fifo_push_blocking(WRITE_SUCCESS_FLAG);
            printf("----- File write iteration %d -----\n", count);
        } else {
            printf("ERROR: Write failed! FR=%d, wrote %u bytes\n", fr, total_bw);
            f_unmount(g_drive);
            if (g_sd && g_sd->deinit) {
                g_sd->deinit(g_sd);
            }
            mounted = false;
            if (fr == FR_NOT_READY || fr == FR_DISK_ERR) {
                multicore_fifo_push_blocking(SD_UNAVAILABLE_FLAG);
            } else {
                multicore_fifo_push_blocking(WRITE_FAILED_FLAG);
            }
        }
    }
}

int main(void) {

    System_Init();

    mutex_init(&mutex);

    // Pre-initialize tp_data buffer to static storage so TP_Save() never calls malloc()
    tp_data.data = s_tp_save_buf;
    tp_data.data_len = sizeof(s_tp_save_buf);

	LCD_SCAN_DIR  lcd_scan_dir = SCAN_DIR_DFT;
	LCD_Init(lcd_scan_dir,1000);
	TP_Init(lcd_scan_dir, &tp_data, &mutex);
	TP_GetAdFac();
	TP_Dialog();

    multicore_launch_core1(core1_entry);

	while(1){
        TP_GetSaveBusy();
        if (multicore_fifo_rvalid()) {
            uint32_t msg = multicore_fifo_pop_blocking();
            if (msg == WRITE_SUCCESS_FLAG) {
                printf("Core 0: Core 1 reported write success.\n");
                GUI_DisString_EN(sLCD_DIS.LCD_Dis_Column - 120, 24,
                                 "SAVED!", &Font16, BLACK, GREEN);
                TP_SetSaveBusy(false);
            } else if (msg == WRITE_FAILED_FLAG) {
                printf("Core 0: Core 1 reported write failure.\n");
                GUI_DisString_EN(sLCD_DIS.LCD_Dis_Column - 120, 24,
                                 "FAILED", &Font16, BLACK, RED);
                TP_SetSaveBusy(false);
            } else if (msg == SD_UNAVAILABLE_FLAG) {
                printf("Core 0: Core 1 reported SD card unavailable.\n");
                GUI_DisString_EN(sLCD_DIS.LCD_Dis_Column - 120, 24,
                                 "NO SD!", &Font16, BLACK, YELLOW);
                TP_SetSaveBusy(false);
            }
        }
        TP_DrawBoard();
	}

	return 0;
}
