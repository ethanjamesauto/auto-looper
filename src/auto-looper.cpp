#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include <stdio.h>

#include "boards/pico_ice.h"
#include "ice_sram.h"

// tinyusb
#include "tusb.h"

#include "stdio_mem.h"

repeating_timer_t timer;
uint slice_num;
uint channel;

#define OUTPUT_PIN 5 // The PWM DAC output pin

#define SKIP 4 // number of ADC bits to not use. This should be set to 4 at the moment due to data being stored as bytes.
#define PWM_PERIOD (4096 >> SKIP)
#define SAMPLE_RATE_US 23 // about 44.1 kHz

#define BUFFER_SIZE (30*1024) // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)

uint8_t ram_buffer[2][BUFFER_SIZE][2];
uint ram_buffer_start[2];
uint ram_buffer_offset[2];
uint loop_length = 0; // loop length in samples (2ish seconds maybe)
uint loop_time = 0; // current time in samples

// buffer swapping variables
bool which = false;
#define PSRAM_ACCESS_BUFFER (!which)   // the buffer that is read from and then written to by PSRAM
#define LOOP_BUFFER which        // the buffer that is used for looping by the CPU

// used for ram_buffer indexing
#define MAIN_SAMPLE 0
#define ACTIVE_SAMPLE 1

enum state_type {
    IDLE,
    FIRST_RECORD,
    RECORD_OVER,
    PLAYBACK,
};

state_type state = FIRST_RECORD; // TODO: change this
volatile bool signal_write = false;
volatile uint32_t psram_address;

/**
 * @brief Callback function for the repeating timer. Runs at the sample rate.
 */
bool timer_callback(repeating_timer_t* rt)
{
    bool do_write;
    bool do_playback;

    uint16_t result = adc_read();
    uint8_t current = (result >> SKIP) /*- 128*/; // convert to signed 8-bit value

    uint index = loop_time % BUFFER_SIZE;

    // add the current and looped values together, then write back
    uint8_t mixed;
    if (state == FIRST_RECORD) {
        mixed = current;
    } else {
        mixed = ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] + ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] + current;
    }
    pwm_set_chan_level(slice_num, channel, mixed);

    if (state == IDLE) { 
        return true; // we're done
    }

    if (state == RECORD_OVER) {
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] += ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE];
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
    } else if (state == FIRST_RECORD) {
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = 0;
    }
    

    loop_time++;    

    //printf("Loop time: %d\n", loop_time);
    // now, see if we're out of bounds

    if (loop_time >= ram_buffer_start[LOOP_BUFFER] + BUFFER_SIZE) {
        // we're out of bounds, so we need to swap buffers
        //printf("loop buffer location is: %d, and psram buffer location is: %d\n", ram_buffer_start[LOOP_BUFFER], ram_buffer_start[PSRAM_ACCESS_BUFFER]);
        which = !which; // swap buffers. The other buffer must contain the next audio to be played

        // special hack for first reads
        if (state == FIRST_RECORD) ram_buffer_start[LOOP_BUFFER] = ram_buffer_start[PSRAM_ACCESS_BUFFER] + BUFFER_SIZE;
        
        psram_address = ram_buffer_start[PSRAM_ACCESS_BUFFER] * 2; // TODO: this is the current calculation. might change
        size_t data_size = BUFFER_SIZE * 2;
        signal_write = true;
        //printf("Now playing sample: %d\n", ram_buffer_start[LOOP_BUFFER]);
    }

    if (state == FIRST_RECORD) {
        loop_length++;
        if (loop_length >= 3 * BUFFER_SIZE) {
            state = RECORD_OVER;
            printf("Loop length is %d\n", loop_length);
            //which = !which;
            //signal_write = true;
            //ram_buffer_start[LOOP_BUFFER] = 0;
            //ram_buffer_start[PSRAM_ACCESS_BUFFER] = BUFFER_SIZE;
        }
    }

    if (loop_time >= loop_length) {
        loop_time = 0;
    }

    return true; // keep repeating
}


int main()
{
    tusb_init();
    stdio_init_all();
    printf("ADC Example, measuring GPIO26\n");

    //TODO: delete
    for (int i = 0; i < BUFFER_SIZE; i++) {
        ram_buffer[0][i][MAIN_SAMPLE] = 0;
        ram_buffer[1][i][MAIN_SAMPLE] = 0;
        ram_buffer[0][i][ACTIVE_SAMPLE] =0;
        ram_buffer[1][i][ACTIVE_SAMPLE] = 0;
    }
    // write blocking
    ram_buffer_start[0] = 0;
    ram_buffer_start[1] = 0;

    // Initialize the PSRAM
    ice_sram_init();
    ice_sram_reset();

    // Initialize the ADC
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

    //new_loop_length = bpm_to_samples(120);

    // It seems as if something needs to be running in the main loop for the program to work. TODO: confirm this
    //*
    while (1) {
        tud_task(); // tinyusb device task
        if (signal_write) {
            printf("Writing to address %d, var which is %d\n", psram_address / 2, which);
            ice_sram_write_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], BUFFER_SIZE * 2);// write_callback, NULL);
                
            // calculate the next block to load. 
            uint next_start;
            if (state == FIRST_RECORD) {
                next_start = 0; // we need to keep the first block ready for when the user hits the button
            } else {
                next_start = ram_buffer_start[LOOP_BUFFER] + BUFFER_SIZE;
            }

            if (next_start >= loop_length) {
                // we're done recording
                next_start = 0;
                //printf("Wrapping around because next_start is %d\n", next_start);
            }
            int num_samples_read = BUFFER_SIZE * 2;
            uint next_address = next_start * 2; // TODO: this is the current calculation. might change
            uint next_size = num_samples_read * 2;
            ram_buffer_start[PSRAM_ACCESS_BUFFER] = next_start;
            psram_address = next_address;

            printf("Reading from address %d, var which is %d\n", psram_address / 2, which);
            ice_sram_read_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], BUFFER_SIZE * 2);
            signal_write = false;
        }
    }//*/
}
