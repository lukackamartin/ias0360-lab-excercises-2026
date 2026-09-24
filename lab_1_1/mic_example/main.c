#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/pdm_microphone.h"
#include "tusb.h"

// Configuration
const struct pdm_microphone_config config = {
    // GPIO pin for the PDM DAT signal
    .gpio_data = 2,

    // GPIO pin for the PDM CLK signal
    .gpio_clk = 3,

    // PIO instance to use
    .pio = pio0,

    // PIO State Machine instance to use
    .pio_sm = 0,

    // Sample rate in Hz
    .sample_rate = 8000,

    // Number of samples to buffer
    .sample_buffer_size = 256,
};

// Variables
int16_t sample_buffer[256];
volatile int samples_read = 0;

void on_pdm_samples_ready()
{
    // Callback from library when all the samples in the library
    // internal sample buffer are ready for reading
    samples_read = pdm_microphone_read(sample_buffer, 256);
}

int main(void)
{
    stdio_init_all();

    // Wait up to 2 seconds for USB CDC connection
    for (int i = 0; i < 200 && !tud_cdc_connected(); i++) {
        sleep_ms(10);
    }

    printf("hello PDM microphone\n");

    // Initialize the PDM microphone
    if (pdm_microphone_init(&config) < 0) {
        printf("PDM microphone initialization failed!\n");
        while (1) { tight_loop_contents(); }
    }

    // Set callback that is called when all the samples in the library
    // internal sample buffer are ready for reading
    pdm_microphone_set_samples_ready_handler(on_pdm_samples_ready);

     // Start capturing data from the PDM microphone
    if (pdm_microphone_start() < 0) {
        printf("PDM microphone start failed!\n");
        while (1) { tight_loop_contents(); }
    }

    while (1) {
        // Wait for new samples
        while (samples_read == 0) { tight_loop_contents(); }

        // Store and clear the samples read from the callback
        int sample_count = samples_read;
        samples_read = 0;

        // Loop through any new collected samples
        for (int i = 0; i < sample_count; i++) {
            printf("%d\n", sample_buffer[i]);
        }
    }

    return 0;
}
