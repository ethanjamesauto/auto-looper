#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include <stdio.h>

repeating_timer_t timer;
uint slice_num;
uint channel;

#define OUTPUT_PIN 0 // The PWM DAC output pin

#define SKIP 4 // number of ADC bits to not use. This should be set to 4 at the moment due to data being stored as bytes.
#define PWM_PERIOD (4096 >> SKIP)
#define SAMPLE_RATE_US 23 // about 44.1 kHz

#define BUFFER_SIZE (200 * 1024) // Max of 200k samples

int8_t loop_buffer[BUFFER_SIZE];
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

/**
 * @brief Callback function for the repeating timer. Runs at the sample rate.
 */
bool timer_callback(repeating_timer_t* rt)
{
    // 12-bit conversion, assume max value == ADC_VREF == 3.3 V
    // const float conversion_factor = 3.3f / (1 << 12);

    uint16_t result = adc_read();
    int8_t byte = (result >> SKIP) - 128; // convert to signed 8-bit value

    loop_buffer[buffer_index] = byte; // buffer the value for later
    buffer_index = next(buffer_index); // increment the buffer index, which will now point to the oldest value stored, giving a digital delay.

    // add the current and delayed values together, convert back to unsigned 8-bit value
    // TODO: add clipping to prevent overflow
    uint8_t delayed = (loop_buffer[buffer_index] >> 1) + (byte >> 1) + 128;

    pwm_set_chan_level(slice_num, channel, delayed);

    // printf("Raw value: 0x%03x, voltage: %f V\n", result, result * conversion_factor);
    // printf("Voltage: %f V\n", result * conversion_factor);

    return true; // keep repeating
}

int main()
{
    stdio_init_all();
    printf("ADC Example, measuring GPIO26\n");

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

    // Start the sampling timer at about 44.1 kHz
    add_repeating_timer_us(SAMPLE_RATE_US, timer_callback, NULL, &timer);

    // It seems as if something needs to be running in the main loop for the program to work. TODO: confirm this
    while (1) {
        float bpm;
        scanf("%f", &bpm);
        new_loop_length = bpm_to_samples(bpm);
        if (new_loop_length > BUFFER_SIZE) {
            printf("Loop length too long, using %u samples\n", BUFFER_SIZE);
            new_loop_length = BUFFER_SIZE;
        } else {
            printf("New loop length: %f (%f seconds, %u samples)\n", bpm, 60. / bpm, new_loop_length);
        }
    }
}
