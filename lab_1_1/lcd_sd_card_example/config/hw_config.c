
/* hw_config.c
Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

   http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/

/*

This file should be tailored to match the hardware design.


There should be one element of the spi[] array for each RP2040 hardware SPI used.

There should be one element of the spi_ifs[] array for each SPI interface object.
* Each element of spi_ifs[] must point to an spi_t instance with member "spi".

There should be one element of the sdio_ifs[] array for each SDIO interface object.

There should be one element of the sd_cards[] array for each SD card slot.
* Each element of sd_cards[] must point to its interface with spi_if_p or sdio_if_p.
*/

/* Hardware configuration for Waveshare Pico-Eval-Board (SKU 20159)
with Raspberry Pi Pico (RP2040) / Pico W.
*/

#include <assert.h>
//
#include "hw_config.h"

#ifndef USE_SPI
#define USE_SPI 0
#endif

#if USE_SPI
// Hardware Configuration of SPI "objects"
// Note: multiple SD cards can be driven by one SPI if they use different slave
// selects (or "chip selects").
// Using SPI1 to avoid conflicts with CYW43439 WiFi chip (which uses SPI0 internally on Pico W)
static spi_t spis[] = {
    {  // SPI1 for SD card (avoiding CYW43439 conflict)
        .hw_inst = spi1,  // Use SPI1 instead of SPI0
        .sck_gpio = 10,   // SPI1 SCK  (GP10)
        .mosi_gpio = 11,  // SPI1 MOSI (GP11) - TX
        .miso_gpio = 12,  // SPI1 MISO (GP12) - RX
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength  = GPIO_DRIVE_STRENGTH_12MA,
        .no_miso_gpio_pull_up = true,
        .baud_rate = 125 * 1000 * 1000 / 6  // 20833333 Hz
    }
};

/* SPI Interfaces */
static sd_spi_if_t spi_ifs[] = {
    {
        .spi = &spis[0],
        .ss_gpio = 22,  // CS (Chip Select) pin for SD card (GP22)
        .set_drive_strength = true,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA
    }
};
#else
/* SDIO Interfaces */
/*
Pins CLK_gpio, D1_gpio, D2_gpio, and D3_gpio are at offsets from pin D0_gpio.
The offsets are determined by rp2040_sdio.pio:
    CLK_gpio = (D0_gpio + SDIO_CLK_PIN_D0_OFFSET) % 32;
    For Waveshare Pico-Eval-Board:
        SD_CLK is GP5, SD_D0 is GP19.
        SDIO_CLK_PIN_D0_OFFSET is 18: (19 + 18) % 32 = 37 % 32 = 5 (GP5).
    D1_gpio = D0_gpio + 1 = GP20
    D2_gpio = D0_gpio + 2 = GP21 (via R11 0R)
    D3_gpio = D0_gpio + 3 = GP22 (via R14 0R)
*/
static sd_sdio_if_t sdio_ifs[] = {
    {
        .CMD_gpio = 18,
        .D0_gpio = 19,
        .set_drive_strength = true,
        .CLK_gpio_drive_strength = GPIO_DRIVE_STRENGTH_12MA,
        .CMD_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .D0_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .D1_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .D2_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .D3_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .SDIO_PIO = pio0,          // Use PIO0 (CYW43 uses PIO internally, but PIO0 is dedicated to SDIO)
        .DMA_IRQ_num = DMA_IRQ_1,  // Use DMA_IRQ_1 to avoid conflict with WiFi (which may use DMA_IRQ_0)
        .baud_rate = 125 * 1000 * 1000 / 7  // 17857143 Hz
    }
};
#endif

/* Hardware Configuration of the SD Card "objects"
    These correspond to SD card sockets
*/
static sd_card_t sd_cards[] = {
    {  // sd_cards[0]: Socket sd0 on Waveshare Pico-Eval-Board
#if USE_SPI
        .type = SD_IF_SPI,
        .spi_if_p = &spi_ifs[0],  // Pointer to the SPI interface driving this card
#else
        .type = SD_IF_SDIO,
        .sdio_if_p = &sdio_ifs[0],  // Pointer to the SDIO interface driving this card
#endif
        // SD Card detect:
        .use_card_detect = false,
        .card_detect_gpio = 0,
        .card_detected_true = 0,  // What the GPIO read returns when a card is present
        .card_detect_use_pull = false,
        .card_detect_pull_hi = false
    }
};
/* ********************************************************************** */

size_t sd_get_num() { return count_of(sd_cards); }

sd_card_t *sd_get_by_num(size_t num) {
    assert(num < sd_get_num());
    if (num < sd_get_num()) {
        return &sd_cards[num];
    } else {
        return NULL;
    }
}

/* [] END OF FILE */
