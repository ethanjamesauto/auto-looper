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

#include "button.h"

repeating_timer_t timer;
uint slice_num;
uint channel;

#define OUTPUT_PIN 5 // The PWM DAC output pin
#define FOOTSWITCH_PIN 21 // The footswitch pin

#define SKIP 4 // number of ADC bits to not use. This should be set to 4 at the moment due to data being stored as bytes.
#define PWM_PERIOD (4096 >> SKIP)
#define SAMPLE_RATE_US 23 // about 44.1 kHz

#define BUFFER_SIZE (4*1024) // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)

uint8_t ram_buffer[2][BUFFER_SIZE][2];
uint ram_buffer_start[2];
uint ram_buffer_offset[2];
uint loop_length = 0; // loop length in samples (2ish seconds maybe)
uint loop_time = 0; // current time in samples

// buffer swapping variables
bool which = false;
#define PSRAM_ACCESS_BUFFER (!which)   // the buffer that is read from and then written to by PSRAM
#define LOOP_BUFFER which        // the buffer that is used for looping by the CPU

// button single press
bool single_press = false;

// used for ram_buffer indexing
#define MAIN_SAMPLE 0
#define ACTIVE_SAMPLE 1

enum state_type {
    IDLE,
    FIRST_RECORD,
    RECORD_OVER,
    PLAYBACK,
};

state_type state = IDLE;
volatile bool signal_write = false;

// TODO: stop using PSRAM for short loop lengths. Minimum loop length right now is BUFFER_SIZE
// TODO: break up into inline functions

/**
 * @brief Callback function for the repeating timer. Runs at the sample rate.
 */
bool process_sample(repeating_timer_t* rt)
{
    bool do_write;
    bool do_playback;

    uint16_t result = adc_read();
    uint8_t current = (result >> SKIP) /*- 128*/; // convert to signed 8-bit value


    if (single_press) {
        if (state != FIRST_RECORD) single_press = false; // TODO: quick hack

        if (state == IDLE) {
            state = FIRST_RECORD;
        } else if (state == RECORD_OVER) {
            state = PLAYBACK;
        } else if (state == PLAYBACK) {
            state = RECORD_OVER;
        }
    }

    // add the current and looped values together, then write back
    uint index = 0;
    if (state != IDLE) {
        index = ram_buffer_offset[LOOP_BUFFER]++;
    }

    uint8_t mixed;
    if (state == FIRST_RECORD || state == IDLE) {
        mixed = current;
    } else {
        mixed = ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] + ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] + current;
    }
    pwm_set_chan_level(slice_num, channel, mixed);

    if (state == IDLE) { 
        return true; // we're done
    }
    
    loop_time++;

    if (state == RECORD_OVER) {
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] += ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE];
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
    } else if (state == FIRST_RECORD) {
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = 0;
    }
    

    if (state == FIRST_RECORD) {
        loop_length++;
        if (single_press) {
            single_press = false;
            state = RECORD_OVER;
            printf("Loop length is %d\n", loop_length);
            which = !which;
            // printf("Loop buffer start is %d, and PSRAM buffer start is %d\n", ram_buffer_start[LOOP_BUFFER], ram_buffer_start[PSRAM_ACCESS_BUFFER]);
            // printf("Loop buffer offset is %d, and PSRAM buffer offset is %d\n", ram_buffer_offset[LOOP_BUFFER], ram_buffer_offset[PSRAM_ACCESS_BUFFER]);
            signal_write = true;
            goto done;
        }
    }

    if (ram_buffer_offset[LOOP_BUFFER] >= BUFFER_SIZE) {
        // we're out of bounds, so we need to swap buffers
        which = !which; // swap buffers. The other buffer must contain the next audio to be played

        // special hack for first reads
        if (state == FIRST_RECORD) ram_buffer_start[LOOP_BUFFER] = ram_buffer_start[PSRAM_ACCESS_BUFFER] + BUFFER_SIZE;

        signal_write = true;
    }

done:
    if (loop_time >= loop_length) {
        loop_time = 0;
    }

    return true; // keep repeating
}

void write_routine() {
    uint num_write = ram_buffer_offset[PSRAM_ACCESS_BUFFER];
    uint sample_num = ram_buffer_start[PSRAM_ACCESS_BUFFER];
    if (sample_num + num_write > loop_length) {
        uint write_size_one = loop_length - sample_num;
        uint write_size_two = num_write - write_size_one;
        uint32_t psram_size_one = write_size_one * 2;
        uint32_t psram_size_two = write_size_two * 2;
        uint32_t psram_address_one = sample_num * 2;
        uint32_t psram_address_two = 0;
        // printf("Writing to address %d, var which is %d, writing %d samples\n", psram_address_one/2, which, write_size_one);
        ice_sram_write_blocking(psram_address_one, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], psram_size_one);
        // printf("Also writing to address %d, writing %d samples\n", psram_address_two/2, write_size_two);
        ice_sram_write_blocking(psram_address_two, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER][write_size_one], psram_size_two);
    } else {
        uint32_t psram_address = sample_num * 2;
        // printf("Writing to address %d, var which is %d, writing %d samples\n", psram_address/2, which, num_write);
        ice_sram_write_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], num_write * 2);// write_callback, NULL);
    }

    ram_buffer_offset[PSRAM_ACCESS_BUFFER] = 0;
        
    // calculate the next block to load. 
    if (state == FIRST_RECORD) {
        sample_num = 0; // we need to keep the first block ready for when the user hits the button
    } else {
        sample_num = (ram_buffer_start[LOOP_BUFFER] + BUFFER_SIZE) % loop_length;
    }

    ram_buffer_start[PSRAM_ACCESS_BUFFER] = sample_num;
    if (sample_num + BUFFER_SIZE > loop_length) {
        uint read_size_one = loop_length - sample_num;
        uint read_size_two = BUFFER_SIZE - read_size_one;
        uint32_t psram_size_one = read_size_one * 2;
        uint32_t psram_size_two = read_size_two * 2;
        uint32_t psram_address_one = sample_num * 2;
        uint32_t psram_address_two = 0;
        // printf("Reading from address %d, var which is %d\n", psram_address_one/2, which);
        ice_sram_read_blocking(psram_address_one, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], psram_size_one);
        // printf("Also reading from address %d, reading %d samples\n", psram_address_two/2, read_size_two);
        ice_sram_read_blocking(psram_address_two, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER][read_size_one], psram_size_two);
    } else {
        uint32_t psram_address = sample_num * 2;
        // printf("Reading from address %d, var which is %d\n", psram_address/2, which);
        ice_sram_read_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], BUFFER_SIZE * 2);// read_callback, NULL);
    }
    signal_write = false;
}

/**
 * @brief Called when the footswitch is pressed or released.
*/
void footswitch_onchange(button_t *button_p) {
    button_t *button = (button_t*)button_p;
    // printf("Button on pin %d changed its state to %d\n", button->pin, button->state);

    if(button->state) return; // Ignore button release.

    single_press = true;
}

int main()
{
    tusb_init();
    stdio_init_all();

    //TODO: delete
    for (int i = 0; i < BUFFER_SIZE; i++) {
        ram_buffer[0][i][MAIN_SAMPLE] = 0;
        ram_buffer[1][i][MAIN_SAMPLE] = 0;
        ram_buffer[0][i][ACTIVE_SAMPLE] = 0;
        ram_buffer[1][i][ACTIVE_SAMPLE] = 0;
    }

    ram_buffer_start[0] = 0;
    ram_buffer_start[1] = 0;
    ram_buffer_offset[0] = 0;
    ram_buffer_offset[1] = 0;

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

    // initialize the footswitch button
    button_t* footswitch = create_button(FOOTSWITCH_PIN, footswitch_onchange);

    // Start the sampling timer at about 44.1 kHz
    add_repeating_timer_us(SAMPLE_RATE_US, process_sample, NULL, &timer);

    while (1) {
        tud_task(); // tinyusb device task
        if (signal_write) {
            write_routine();
        }
    }
}
