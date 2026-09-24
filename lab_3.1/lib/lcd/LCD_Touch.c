#include "LCD_Touch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

TP_DEV* pTP_DEV;
TP_DRAW* pTP_Draw;

static void TP_DumpBitmapToSerial(int h, int w, const uint8_t bmp[h][w])
{
    // Build and print one line at a time (faster than putchar per pixel)
    char line[w + 2];  // + '\n' + '\0'
    line[w]   = '\n';
    line[w+1] = '\0';

    for (uint16_t y = 0; y < h; ++y) {
        for (uint16_t x = 0; x < w; ++x) {
            line[x] = bmp[y][x] ? '1' : '0';
        }
        printf("%s", line);  // prints 280 chars of 0/1, then newline
    }
}

#define TP_CMD_X              0xD0  // differential X position
#define TP_CMD_Y              0x90  // differential Y position
#define TP_CMD_Z1             0xB0  // differential Z1 touch pressure
#define TP_CMD_Z2             0xC0  // differential Z2 touch pressure

#define TP_TOUCH_SPI_BAUDRATE 1000000                  // 1.0 MHz for settling SAR ADC
#define TP_LCD_SPI_BAUDRATE   (125 * 1000 * 1000 / 6)  // 20.833 MHz for ILI9488

#define TP_ADC_VALID_MIN      80
#define TP_ADC_VALID_MAX      4015
#define TP_SAMPLES_COUNT      5
#define TP_ERR_RANGE          30    // cluster tolerance in ADC counts (~3.5 px, Linux ti,debounce-tol)
#define TP_Z1_MIN             90    // minimum Z1 ADC count for physical contact
#define TP_X_PLATE_OHMS       400   // Waveshare panel sheet resistance in Ohms (Device Tree specification)
#define TP_R_TOUCH_MAX        1500  // maximum physical contact resistance in Ohms (Linux ti,pressure-max)

static uint8_t sTP_PenDownCount = 0;

static uint16_t TP_AbsDiff(uint16_t Value1, uint16_t Value2)
{
    return (Value1 > Value2) ? (Value1 - Value2) : (Value2 - Value1);
}

static void TP_Sort_ADC(uint16_t *pData, uint8_t Count)
{
    uint8_t i, j;
    uint16_t Temp;

    if (Count < 2)
        return;

    for (i = 0; i < Count - 1; i++)
    {
        for (j = i + 1; j < Count; j++)
        {
            if (pData[i] > pData[j])
            {
                Temp = pData[i];
                pData[i] = pData[j];
                pData[j] = Temp;
            }
        }
    }
}

static bool TP_Is_ADC_Valid(uint16_t Xpoint, uint16_t Ypoint)
{
    return Xpoint > TP_ADC_VALID_MIN && Xpoint < TP_ADC_VALID_MAX &&
           Ypoint > TP_ADC_VALID_MIN && Ypoint < TP_ADC_VALID_MAX;
}

static uint16_t TP_Read_ADC(uint8_t CMD)
{
    uint8_t tx[3] = {CMD, 0x00, 0x00};
    uint8_t rx[3] = {0, 0, 0};

    DEV_Digital_Write(TP_CS_PIN, 0);
    spi_write_read_blocking(SPI_PORT, tx, rx, 3);
    DEV_Digital_Write(TP_CS_PIN, 1);

    uint16_t Data = ((uint16_t)rx[1] << 8) | rx[2];
    Data >>= 3;
    return Data;
}

uint16_t TP_GetPressure(void)
{
    return pTP_DEV ? pTP_DEV->Z1 : 0;
}

static bool TP_Read_TwiceADC(uint16_t *pXCh_Adc, uint16_t *pYCh_Adc)
{
    uint16_t x_buf[TP_SAMPLES_COUNT];
    uint16_t y_buf[TP_SAMPLES_COUNT];

    // Hardware PENIRQ line check (active low)
    if (DEV_Digital_Read(TP_IRQ_PIN))
        return false;

    spi_set_baudrate(SPI_PORT, TP_TOUCH_SPI_BAUDRATE);

    // Measure touch pressure (Z1, Z2)
    (void)TP_Read_ADC(TP_CMD_Z1);
    Driver_Delay_us(30);
    uint16_t z1 = TP_Read_ADC(TP_CMD_Z1);

    (void)TP_Read_ADC(TP_CMD_Z2);
    Driver_Delay_us(30);
    uint16_t z2 = TP_Read_ADC(TP_CMD_Z2);

    if (z1 < TP_Z1_MIN || z2 <= z1)
    {
        // Re-enable PENIRQ and restore SPI speed
        (void)TP_Read_ADC(TP_CMD_X);
        spi_set_baudrate(SPI_PORT, TP_LCD_SPI_BAUDRATE);
        return false;
    }

    // Sample X channel with settling delay
    (void)TP_Read_ADC(TP_CMD_X);
    Driver_Delay_us(40);
    for (uint8_t i = 0; i < TP_SAMPLES_COUNT; i++)
    {
        x_buf[i] = TP_Read_ADC(TP_CMD_X);
        Driver_Delay_us(20);
    }

    // Sample Y channel with settling delay
    (void)TP_Read_ADC(TP_CMD_Y);
    Driver_Delay_us(40);
    for (uint8_t i = 0; i < TP_SAMPLES_COUNT; i++)
    {
        y_buf[i] = TP_Read_ADC(TP_CMD_Y);
        Driver_Delay_us(20);
    }

    // Post-measurement pressure verification (Linux ads7846.c sandwich check)
    // Ensures stylus did not lift or lose pressure while X and Y were sampled
    (void)TP_Read_ADC(TP_CMD_Z1);
    Driver_Delay_us(20);
    uint16_t z1_post = TP_Read_ADC(TP_CMD_Z1);

    // Power down and re-enable PENIRQ with PD1=0, PD0=0
    (void)TP_Read_ADC(TP_CMD_X);
    spi_set_baudrate(SPI_PORT, TP_LCD_SPI_BAUDRATE);

    // Verify touch remained active during sampling
    if (DEV_Digital_Read(TP_IRQ_PIN) || z1_post < TP_Z1_MIN)
        return false;

    // Median filter and spread validation for X
    TP_Sort_ADC(x_buf, TP_SAMPLES_COUNT);
    if ((x_buf[3] - x_buf[1]) > TP_ERR_RANGE)
        return false;
    uint16_t x_avg = (uint16_t)(((uint32_t)x_buf[1] + x_buf[2] + x_buf[3]) / 3);

    // Median filter and spread validation for Y
    TP_Sort_ADC(y_buf, TP_SAMPLES_COUNT);
    if ((y_buf[3] - y_buf[1]) > TP_ERR_RANGE)
        return false;
    uint16_t y_avg = (uint16_t)(((uint32_t)y_buf[1] + y_buf[2] + y_buf[3]) / 3);

    // Range check
    if (!TP_Is_ADC_Valid(x_avg, y_avg))
        return false;

    // Linux ads7846.c / XPT2046 datasheet Eq. 3 contact resistance (in Ohms):
    // Rt = ((Z2 - Z1) * X * X_PLATE_OHMS) / (Z1 * 4096)
    uint32_t r_touch = (uint32_t)(((uint64_t)(z2 - z1) * (uint64_t)x_avg * TP_X_PLATE_OHMS) /
                                  ((uint64_t)z1 * 4096));
    if (pTP_DEV)
        pTP_DEV->Z1 = (r_touch > 65535) ? 65535 : (uint16_t)r_touch;

    if (r_touch > TP_R_TOUCH_MAX)
        return false;

    *pXCh_Adc = x_avg;
    *pYCh_Adc = y_avg;
    return true;
}

static void TP_Read_ADC_XY(uint16_t *pXCh_Adc, uint16_t *pYCh_Adc)
{
    TP_Read_TwiceADC(pXCh_Adc, pYCh_Adc);
}

uint8_t TP_Scan(uint8_t chCoordType)
{
    // In X, Y coordinate measurement, IRQ is disabled and output is low
    if (!DEV_Digital_Read(TP_IRQ_PIN))
    {  // press the button to press
        // Read the physical coordinates
        if (chCoordType)
        {
            TP_Read_TwiceADC(&pTP_DEV->Xpoint, &pTP_DEV->Ypoint);
            // Read the screen coordinates
        }
        else if (TP_Read_TwiceADC(&pTP_DEV->Xpoint, &pTP_DEV->Ypoint))
        {

            if (LCD_2_8 == id)
            {
                pTP_Draw->Xpoint = sLCD_DIS.LCD_Dis_Column -
                                  pTP_DEV->fXfac * pTP_DEV->Xpoint -
                                  pTP_DEV->iXoff;
                pTP_Draw->Ypoint = sLCD_DIS.LCD_Dis_Page -
                                  pTP_DEV->fYfac * pTP_DEV->Ypoint -
                                  pTP_DEV->iYoff;
            }
            else
            {
                //DEBUG("(Xad,Yad) = %d,%d\r\n",pTP_DEV->Xpoint,pTP_DEV->Ypoint);
                if (pTP_DEV->TP_Scan_Dir == R2L_D2U)
                {  // converts the result to screen coordinates
                    pTP_Draw->Xpoint = pTP_DEV->fXfac * pTP_DEV->Xpoint +
                                      pTP_DEV->iXoff;
                    pTP_Draw->Ypoint = pTP_DEV->fYfac * pTP_DEV->Ypoint +
                                      pTP_DEV->iYoff;
                }
                else if (pTP_DEV->TP_Scan_Dir == L2R_U2D)
                {
                    pTP_Draw->Xpoint = sLCD_DIS.LCD_Dis_Column -
                                      pTP_DEV->fXfac * pTP_DEV->Xpoint -
                                      pTP_DEV->iXoff;
                    pTP_Draw->Ypoint = sLCD_DIS.LCD_Dis_Page -
                                      pTP_DEV->fYfac * pTP_DEV->Ypoint -
                                      pTP_DEV->iYoff;
                }
                else if (pTP_DEV->TP_Scan_Dir == U2D_R2L)
                {
                    pTP_Draw->Xpoint = pTP_DEV->fXfac * pTP_DEV->Ypoint +
                                      pTP_DEV->iXoff;
                    pTP_Draw->Ypoint = pTP_DEV->fYfac * pTP_DEV->Xpoint +
                                      pTP_DEV->iYoff;
                }
                else
                {
                    pTP_Draw->Xpoint = sLCD_DIS.LCD_Dis_Column -
                                      pTP_DEV->fXfac * pTP_DEV->Ypoint -
                                      pTP_DEV->iXoff;
                    pTP_Draw->Ypoint = sLCD_DIS.LCD_Dis_Page -
                                      pTP_DEV->fYfac * pTP_DEV->Xpoint -
                                      pTP_DEV->iYoff;
                }
                //DEBUG("( x , y ) = %d,%d\r\n",pTP_Draw->Xpoint,pTP_Draw->Ypoint);
            }
        }
        if (0 == (pTP_DEV->chStatus & TP_PRESS_DOWN))
        {  // not being pressed
            if (++sTP_PenDownCount >= 2)
            {
                pTP_DEV->chStatus = TP_PRESS_DOWN | TP_PRESSED;
                pTP_DEV->Xpoint0 = pTP_DEV->Xpoint;
                pTP_DEV->Ypoint0 = pTP_DEV->Ypoint;
            }
        }
    }
    else
    {
        sTP_PenDownCount = 0;
        if (pTP_DEV->chStatus & TP_PRESS_DOWN)
        {                                  // 0x80
            pTP_DEV->chStatus &= ~(1 << 7);  // 0x00
        }
        else
        {
            pTP_DEV->Xpoint0 = 0;
            pTP_DEV->Ypoint0 = 0;
            pTP_DEV->Xpoint = 0xffff;
            pTP_DEV->Ypoint = 0xffff;
        }
    }

    return (pTP_DEV->chStatus & TP_PRESS_DOWN);
}

void TP_GetAdFac(void)
{
    if (LCD_2_8 == id)
    {
        pTP_DEV->fXfac = 0.066626;
        pTP_DEV->fYfac = 0.089779;
        pTP_DEV->iXoff = -20;
        pTP_DEV->iYoff = -34;
    }
    else
    {
        if (pTP_DEV->TP_Scan_Dir == D2U_L2R)
        {  // SCAN_DIR_DFT = D2U_L2R
            pTP_DEV->fXfac = -0.132443;
            pTP_DEV->fYfac = 0.089997;
            pTP_DEV->iXoff = 516;
            pTP_DEV->iYoff = -22;
        }
        else if (pTP_DEV->TP_Scan_Dir == L2R_U2D)
        {
            pTP_DEV->fXfac = 0.089697;
            pTP_DEV->fYfac = 0.134792;
            pTP_DEV->iXoff = -21;
            pTP_DEV->iYoff = -39;
        }
        else if (pTP_DEV->TP_Scan_Dir == R2L_D2U)
        {
            pTP_DEV->fXfac = 0.089915;
            pTP_DEV->fYfac = 0.133178;
            pTP_DEV->iXoff = -22;
            pTP_DEV->iYoff = -38;
        }
        else if (pTP_DEV->TP_Scan_Dir == U2D_R2L)
        {
            pTP_DEV->fXfac = -0.132906;
            pTP_DEV->fYfac = 0.087964;
            pTP_DEV->iXoff = 517;
            pTP_DEV->iYoff = -20;
        }
        else
        {
            LCD_Clear(LCD_BACKGROUND);
            GUI_DisString_EN(0, 60, "Does not support touch-screen \
							calibration in this direction",
                             &Font16, FONT_BACKGROUND, RED);
        }
    }
}

static int count_digits(int number)
{
    if (number == 0)
        return 1;  // 0 has one digit

    number = abs(number);  // make positive
    int count = 0;
    while (number > 0) {
        number /= 10;
        count++;
    }
    return count;
}

void TP_display_input(int h, int w, const uint8_t* src)
{
    for (int i=0; i<h; i++) {
        for (int j=0; j<w; j++) {
            uint8_t num = src[i * w + j];
            int space = 3 - count_digits(num);
            for (int i = 0; i < space; ++i) {
                printf("\xE2\x80\x8A");
            }
            printf("%d", num);
        }
        printf("\n");
    }
}

void TP_DrawHeader(char* app_name)
{
    GUI_DisString_EN(130, 20, "WELCOME TO", &Font24, WHITE, BLACK);
    GUI_DisString_EN(120, 45, app_name, &Font24, WHITE, BLACK);
}

void TP_Init(LCD_SCAN_DIR Lcd_ScanDir, TP_DEV* tp_dev, TP_DRAW* tp_draw)
{

    DEV_Digital_Write(TP_CS_PIN, 1);

    pTP_DEV = tp_dev;
    pTP_Draw = tp_draw;

    pTP_DEV->TP_Scan_Dir = Lcd_ScanDir;
    pTP_DEV->Z1 = 0;
    sTP_PenDownCount = 0;

    TP_Read_ADC_XY(&pTP_DEV->Xpoint, &pTP_DEV->Ypoint);
}
