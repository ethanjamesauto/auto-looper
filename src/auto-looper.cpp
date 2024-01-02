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
#include "i2s.h"

repeating_timer_t timer;
uint slice_num;
uint channel;

#define OUTPUT_PIN 5 // The PWM DAC output pin
#define FOOTSWITCH_PIN 6 // The footswitch pin

#define SKIP 4 // number of ADC bits to not use. This should be set to 4 at the moment due to data being stored as bytes.
#define PWM_PERIOD (4096 >> SKIP)
#define SAMPLE_RATE_US 23 // about 44.1 kHz

#define BUFFER_SIZE (64) // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)

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
    STATE_IDLE,
    STATE_FIRST_RECORD,
    RECORD_OVER,
    PLAYBACK,
};

state_type state = STATE_IDLE;
volatile bool signal_write = false;

static __attribute__((aligned(8))) pio_i2s i2s;

// TODO: stop using PSRAM for short loop lengths. Minimum loop length right now is BUFFER_SIZE
// TODO: break up into inline functions

/**
 * @brief Callback function for the repeating timer. Runs at the sample rate.
 */
uint8_t process_sample(uint8_t sample)
{
    uint8_t current = sample;

    if (single_press) {
        if (state != STATE_FIRST_RECORD) single_press = false; // TODO: quick hack

        if (state == STATE_IDLE) {
            state = STATE_FIRST_RECORD;
        } else if (state == RECORD_OVER) {
            state = PLAYBACK;
            printf("Done recording\n");
        } else if (state == PLAYBACK) {
            state = RECORD_OVER;
            printf("Recording again\n");
        }
    }

    // add the current and looped values together, then write back
    uint index = 0;
    if (state != STATE_IDLE) {
        index = ram_buffer_offset[LOOP_BUFFER]++;
    }

    uint8_t mixed;
    if (state == STATE_FIRST_RECORD || state == STATE_IDLE) {
        mixed = current;
    } else {
        mixed = ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] + ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] + current;
    }
    //pwm_set_chan_level(slice_num, channel, mixed);

    if (state == STATE_IDLE) { 
        return mixed; // we're done
    }
    
    loop_time++;

    if (state == RECORD_OVER) {
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] += ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE];
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
    } else if (state == STATE_FIRST_RECORD) {
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = 0;
    }
    

    if (state == STATE_FIRST_RECORD) {
        loop_length++;
        if (single_press) {
            single_press = false;
            state = RECORD_OVER;
            loop_length = (loop_length / BUFFER_SIZE) * BUFFER_SIZE; // TODO: tmp
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
        if (state == STATE_FIRST_RECORD) ram_buffer_start[LOOP_BUFFER] = ram_buffer_start[PSRAM_ACCESS_BUFFER] + BUFFER_SIZE;

        signal_write = true;
    }

done:
    if (loop_time >= loop_length) {
        loop_time = 0;
    }

    return mixed;
}

static void process_audio(const int32_t* input, int32_t* output, size_t num_frames) {
    // Just copy the input to the output
    for (size_t i = 0; i < num_frames * 2; i++) {
        output[i] = process_sample(input[i] >> 24) << 24;
        i++;
        if (i < num_frames * 2) {
            output[i] = output[i-1];
        }
    }
}

static void dma_i2s_in_handler(void) {
        dma_hw->ints0 = 1u << i2s.dma_ch_in_data;  // clear the IRQ
    /* We're double buffering using chained TCBs. By checking which buffer the
     * DMA is currently reading from, we can identify which buffer it has just
     * finished reading (the completion of which has triggered this interrupt).
     */
    if (*(int32_t**)dma_hw->ch[i2s.dma_ch_in_ctrl].read_addr == i2s.input_buffer) {
        // It is inputting to the second buffer so we can overwrite the first
        process_audio(i2s.input_buffer, i2s.output_buffer, AUDIO_BUFFER_FRAMES);
    } else {
        // It is currently inputting the first buffer, so we write to the second
        process_audio(&i2s.input_buffer[STEREO_BUFFER_SIZE], &i2s.output_buffer[STEREO_BUFFER_SIZE], AUDIO_BUFFER_FRAMES);
    }
}

void write_routine() {
    uint num_write = ram_buffer_offset[PSRAM_ACCESS_BUFFER];
    uint sample_num = ram_buffer_start[PSRAM_ACCESS_BUFFER];
    /*if (sample_num + num_write > loop_length) {
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
    } else*/ {
        uint32_t psram_address = sample_num * 2;
        // printf("Writing to address %d, var which is %d, writing %d samples\n", psram_address/2, which, num_write);
        ice_sram_write_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], num_write * 2);// write_callback, NULL);
    }

    ram_buffer_offset[PSRAM_ACCESS_BUFFER] = 0;
        
    // calculate the next block to load. 
    if (state == STATE_FIRST_RECORD) {
        sample_num = 0; // we need to keep the first block ready for when the user hits the button
    } else {
        sample_num = (ram_buffer_start[LOOP_BUFFER] + BUFFER_SIZE) % loop_length;
    }

    ram_buffer_start[PSRAM_ACCESS_BUFFER] = sample_num;
    /*if (sample_num + BUFFER_SIZE > loop_length) {
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
    } else*/ {
        uint32_t psram_address = sample_num * 2;
        // printf("Reading from address %d, var which is %d\n", psram_address/2, which);
        ice_sram_read_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], BUFFER_SIZE * 2);// read_callback, NULL);
    }
    signal_write = false;
}

enum state_t { 
    IDLE, 
    FIRST_RECORD, FIRST_TMP_RECORD, TEMP_RECORD, RECORD, 
    FIRST_PLAYBACK, PLAY, PLAYBACK1,
    FIRST_STOP, STOPPED 
};

const char* state_names[] = {
    "IDLE", 
    "FIRST_RECORD", "FIRST_TMP_RECORD", "TEMP_RECORD", "RECORD", 
    "FIRST_PLAYBACK", "PLAY", "PLAYBACK1",
    "FIRST_STOP", "STOPPED"
};

// state machine variables
bool button_released = true;
bool button_pressed = false;
uint64_t last_time = 0;
state_t state_var = IDLE;

/**
 * @brief Called when the footswitch is pressed or released.
*/
void footswitch_onchange(button_t *button_p) {
    button_t *button = (button_t*)button_p;
    //printf("Button on pin %d changed its state to %d\n", button->pin, button->state);

    if (button->state) {
        button_released = true;
        button_pressed = false;
    } else {
        button_pressed = true;
    }
}

void reset_button() {
    button_released = false;
    button_pressed = false;
    last_time = time_us_64();
}

inline bool second_passed() {
    return time_us_64() - last_time > 750000;
}

inline const char* get_state_type(state_t state) {
    if (state == 0) return "Waiting";
    if (state >= 1 && state <= 4) return "Recording";
    if (state >= 5 && state <= 7) return "Playing";
    if (state >= 8 && state <= 9) return "Stopped";
    return "Unknown";
}

inline void update_state(state_t new_state) {
    state_var = new_state;
    reset_button();
    printf("State changed to %s\n", state_names[state_var]);
    printf("Current status: %s\n\n", get_state_type(state_var));
}

void sm() {
    
    state_t old_state = state_var;

    if (state_var == IDLE) {
        if (button_pressed && button_released) {
            update_state(FIRST_RECORD);
        }
    }

    if (state_var == FIRST_RECORD) {
        if (button_pressed && button_released) {
            update_state(FIRST_PLAYBACK);
        }
    }

    if (state_var == FIRST_PLAYBACK) {
        if (button_pressed && button_released) {
            if (second_passed()) {
                update_state(FIRST_TMP_RECORD);
            } else {
                update_state(FIRST_STOP);
            }
        }
    }

    if (state_var == FIRST_STOP) {
        if (button_pressed && button_released) {
            update_state(FIRST_PLAYBACK);
        } else if (button_released) {

        } else if (second_passed()) {
            update_state(IDLE);
        }
    }

    if (state_var == FIRST_TMP_RECORD) {
        if (!button_pressed && !button_released && second_passed()) {
            update_state(IDLE);
        } else if (button_pressed && button_released && !second_passed()) {
            update_state(STOPPED);
        } else if (second_passed()) {
            update_state(RECORD);
        }
    }

    if (state_var == RECORD) {
        if (button_pressed /* && button_released*/) {
            update_state(PLAY);
        }
    }

    if (state_var == PLAY) {
        if (button_pressed) {
            if (second_passed()) {
                update_state(TEMP_RECORD);
            } else {
                update_state(STOPPED);
            }
        }
    }

    if (state_var == TEMP_RECORD) {
        if (!button_pressed && !button_released && second_passed()) {
            update_state(PLAY);
        } else if (button_pressed && button_released && !second_passed()) {
            update_state(STOPPED);
        } else if (button_released && second_passed()) {
            update_state(RECORD);
        }
    }

    if (state_var == STOPPED) {
        if (button_pressed && button_released) {
            update_state(PLAYBACK1);
        } else if (!button_released && second_passed()) {
            update_state(IDLE);
        }
    }

    if (state_var == PLAYBACK1) {
        if (button_released && second_passed()) {
            update_state(PLAY);
        } else if (!button_released && second_passed()) {
            update_state(IDLE);
        }
    }
}

int main()
{
    //set_sys_clock_khz(132000, true);
    tusb_init();
    stdio_init_all();

    ram_buffer_start[0] = 0;
    ram_buffer_start[1] = 0;
    ram_buffer_offset[0] = 0;
    ram_buffer_offset[1] = 0;


    i2s_config my_config;
    my_config.fs = 48000;
    my_config.sck_mult = 256;
    my_config.bit_depth = 16;
    my_config.sck_pin = 21;
    my_config.dout_pin = 18;
    my_config.din_pin = 22;
    my_config.clock_pin_base = 19;
    my_config.sck_enable = true;

    //i2s_program_start_synched(pio0, &my_config, dma_i2s_in_handler, &i2s);
    
    // Initialize the PSRAM
    //ice_sram_init();

    // initialize the footswitch button
    button_t* footswitch = create_button(FOOTSWITCH_PIN, footswitch_onchange);

    while (1) {
        tud_task(); // tinyusb device task
        //if (signal_write) {
        //    write_routine();
        //}
        sm();
    }
}
