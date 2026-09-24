#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "LCD_Driver.h"
#include "LCD_Touch.h"
#include "LCD_GUI.h"
#include "DEV_Config.h"

#include "model.h"
#include "model_settings.h"
#include "mnist_model_data.h"

using namespace std;

INFERENCE inference;
Model ml_model;

// Core 1 entry that displays UI and handles user touch input
void core1_entry() {
    while (true) {
        TP_DrawBoard();
    }
}

int main(void)
{
    System_Init();

    sleep_ms(100);

    // Initialize LCD display
    LCD_SCAN_DIR  lcd_scan_dir = SCAN_DIR_DFT;
    LCD_Init(lcd_scan_dir, 1000);
    TP_Init(lcd_scan_dir);
    LCD_SCAN_DIR bmp_scan_dir = D2U_R2L;
    TP_GetAdFac();
    reset_inference(&inference);
    init_gui();

    // Run Core 1 loop that handles user interface
    multicore_launch_core1(core1_entry);

    // Initialize ML model
    if (!ml_model.setup()) {
        printf("Failed to initialize ML model!\n");
        while (1) { tight_loop_contents(); }
    }
    printf("Model initialized\n");

    while (true) {
        // Block the process until data being filled
        uint32_t g = multicore_fifo_pop_blocking();

        inference.IsProcessing = true;
        __dmb();

        // Run inference on each of the DIGIT_INPUT_COUNT boxes the user drew.
        for (int index = 0; index < DIGIT_INPUT_COUNT; index++) {
          const uint8_t* drawn_digit = inference.UserInputs[index].InputData;

          // Print the drawn digit to the serial console for debugging.
          for (int i = 0; i < INPUT_IMAGE_SIZE; i++) {
              for (int j = 0; j < INPUT_IMAGE_SIZE; j++) {
                  printf("%3d", drawn_digit[i * INPUT_IMAGE_SIZE + j]);
              }
              printf("\n");
          }

          if (!ml_model.set_input_image(drawn_digit)) {
              printf("Failed to set input image\n");
              inference.UserInputs[index].PredictedDigit = UNKNOWN_PREDICTION;
              continue;
          }

          int result = ml_model.predict();
          if (result == -1) {
              printf("Failed to run inference\n");
              inference.UserInputs[index].PredictedDigit = UNKNOWN_PREDICTION;
          } else {
              printf("Predicted: %d\n", result);
              inference.UserInputs[index].PredictedDigit = result;
          }
        }

        printf("Inference pass finished.\n");

        __dmb();
        inference.IsProcessing = false;
    }
    return 0;
}
