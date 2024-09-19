#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <stdio.h>

#include "stdio_mem.h"

repeating_timer_t timer;
uint slice_num;
uint channel;

#define OUTPUT_PIN 0 // The PWM DAC output pin

#define SKIP 4 // number of ADC bits to not use. This should be set to 4 at the moment due to data being stored as bytes.
#define PWM_PERIOD (4096 >> SKIP)
#define SAMPLE_RATE_US 23 // about 44.1 kHz

#define BUFFER_SIZE (40 * 1024)

int8_t loop_buffer[2][BUFFER_SIZE];
bool which_buffer = 0;
#define ACTIVE_BUFFER which_buffer
#define TRANSFER_BUFFER (!which_buffer)

uint buffer_index = 0;
uint loop_length = 10 * 1024;
uint new_loop_length = 10 * 1024;

/**
 * @brief Convert a BPM value to the number of samples required to loop at that BPM
 */
uint bpm_to_samples(float bpm)
{
    return (60 * 1000000) / (bpm * SAMPLE_RATE_US);
}

/**
 * @brief Return the previous buffer index
 */
inline uint prev(uint i)
{
    return (i + loop_length - 1) % loop_length;
}

/**
 * @brief Return the next buffer index
 * TODO: increasing buffer size plays old samples at the end of the buffer. Fix this.
 */
inline uint next(uint i)
{
    uint n = (i + 1) % loop_length;
    if (n == 0) {
        loop_length = new_loop_length;
    }
    return n;
}

bool do_run = false;
void ram_routine()
{
    while (1) {
        if (!do_run)
            continue;
        write_blocking((char*)loop_buffer[TRANSFER_BUFFER], 0, loop_length);
        read_blocking((char*)loop_buffer[TRANSFER_BUFFER], 0, loop_length);
        do_run = false;
    }
}

/**
 * @brief Callback function for the repeating timer. Runs at the sample rate.
 */
bool timer_callback(repeating_timer_t* rt)
{
    // 12-bit conversion, assume max value == ADC_VREF == 3.3 V
    // const float conversion_factor = 3.3f / (1 << 12);

    uint16_t result = adc_read();
    int8_t byte = (result >> SKIP) - 128; // convert to signed 8-bit value

    // add the current and looped values together, convert back to unsigned 8-bit value
    // TODO: add clipping to prevent overflows
    int8_t combined = (loop_buffer[ACTIVE_BUFFER][buffer_index] >> 1) + (byte >> 1);

    loop_buffer[ACTIVE_BUFFER][buffer_index] = combined; // buffer the value for later
    buffer_index = next(buffer_index);
    if (buffer_index == 0 && !do_run) {
        which_buffer = !which_buffer;
        do_run = true;
    }

    pwm_set_chan_level(slice_num, channel, (uint8_t)(combined + 128)); // write to the DAC

    // printf("Raw value: 0x%03x, voltage: %f V\n", result, result * conversion_factor);
    // printf("Voltage: %f V\n", result * conversion_factor);

    return true; // keep repeating
}

int main()
{
    stdio_init_all();
    // printf("ADC Example, measuring GPIO26\n");

    adc_init();

    // Make sure GPIO is high-impedance, no pullups etc
    adc_gpio_init(26);
    // Select ADC input 0 (GPIO26)
    adc_select_input(0);

    // initialize PWM - a simple PWM dac is used for testing
    gpio_set_function(OUTPUT_PIN, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(OUTPUT_PIN);
    channel = pwm_gpio_to_channel(OUTPUT_PIN);
    pwm_set_wrap(slice_num, PWM_PERIOD);
    pwm_set_chan_level(slice_num, channel, 100);
    pwm_set_enabled(slice_num, true);

    // initialize buffer
    for (uint i = 0; i < BUFFER_SIZE; i++) {
        loop_buffer[0][i] = 0;
        loop_buffer[1][i] = 0;
    }

    getc(stdin);

    multicore_launch_core1(ram_routine);
    // Start the sampling timer at about 44.1 kHz
    add_repeating_timer_us(SAMPLE_RATE_US, timer_callback, NULL, &timer);

    // new_loop_length = bpm_to_samples(120);

    while (1) {
        tight_loop_contents();
    }
}
