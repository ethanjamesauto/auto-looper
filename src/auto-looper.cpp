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

#define BUFFER_SIZE (128) // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)

int16_t ram_buffer[2][BUFFER_SIZE][2];
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

int16_t sm(int16_t current);

volatile bool signal_write = false;

static __attribute__((aligned(8))) pio_i2s i2s;

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
state_t state = IDLE;
uint read_location = 0;

// TODO: stop using PSRAM for short loop lengths. Minimum loop length right now is BUFFER_SIZE
// TODO: break up into inline functions

static void process_audio(const int32_t* input, int32_t* output, size_t num_frames) {
    // Just copy the input to the output
    for (size_t i = 0; i < num_frames * 2; i++) {
        output[i] = sm(input[i] >> 16) << 16;
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
    uint write_size = ram_buffer_offset[PSRAM_ACCESS_BUFFER];
    uint write_location = ram_buffer_start[PSRAM_ACCESS_BUFFER];
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
        uint32_t psram_address = write_location * 4;
        uint32_t psram_write_size = write_size * 4;
        //printf("Writing to address %d, var which is %d, writing %d samples\n", psram_address/2, which, write_size);
        ice_sram_write_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], psram_write_size);// write_callback, NULL);
    }

    ram_buffer_offset[PSRAM_ACCESS_BUFFER] = 0;
    ram_buffer_start[PSRAM_ACCESS_BUFFER] = read_location;
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
        uint32_t psram_address = read_location * 4;
        uint32_t psram_write_size = BUFFER_SIZE * 4;
        //printf("Reading from address %d, var which is %d\n", psram_address/2, which);
        ice_sram_read_blocking(psram_address, (uint8_t*) ram_buffer[PSRAM_ACCESS_BUFFER], psram_write_size);// read_callback, NULL);
    }
    signal_write = false;
}

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

inline bool time_up() {
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
    state = new_state;

    if (state == IDLE) {
        loop_length = 0;
        loop_time = 0;
        ram_buffer_offset[LOOP_BUFFER] = 0;
        ram_buffer_start[LOOP_BUFFER] = 0;
        ram_buffer_offset[PSRAM_ACCESS_BUFFER] = 0;
        ram_buffer_start[PSRAM_ACCESS_BUFFER] = 0;
    }

    reset_button();
    printf("State changed to %s\n", state_names[state]);
    printf("Current status: %s\n\n", get_state_type(state));
}

/**
 * Add two signed ints, but if the result overflows or underflows, clip instead
*/
inline constexpr int16_t add_no_overflow(int16_t a, int16_t b) {
    int16_t result = a + b;
    if ((a > 0 && b > 0 && result < 0) || (a < 0 && b < 0 && result > 0)) {
        return a > 0 ? INT16_MAX : INT16_MIN;
    }
    return result;
}

int16_t sm(int16_t current) {

    if (state == IDLE) {
        if (button_pressed && button_released) {
            update_state(FIRST_RECORD);
        }
    }

    if (state == FIRST_RECORD) {
        if (button_pressed && button_released) {
            loop_length = (loop_length / BUFFER_SIZE) * BUFFER_SIZE; // TODO: tmp
            printf("Loop length: %d\n\n", loop_length);
            update_state(FIRST_PLAYBACK);
        }
    }

    if (state == FIRST_PLAYBACK) {
        if (button_pressed && button_released) {
            if (time_up()) {
                update_state(FIRST_TMP_RECORD);
            } else {
                update_state(FIRST_STOP);
            }
        } else if (!button_released && time_up()) {
            update_state(IDLE);
        }
    }

    if (state == FIRST_STOP) {
        if (button_pressed && button_released) {
            update_state(FIRST_PLAYBACK);
        } else if (button_released) {

        } else if (time_up()) {
            update_state(IDLE);
        }
    }

    if (state == FIRST_TMP_RECORD) {
        if (!button_pressed && !button_released && time_up()) {
            // invalidate tmp buffer
            update_state(IDLE);
        } else if (button_pressed && button_released && !time_up()) {
            // invalidate tmp buffer
            update_state(STOPPED);
        } else if (time_up()) {
            update_state(RECORD);
        }
    }

    if (state == RECORD) {
        if (button_pressed /* && button_released*/) {
            update_state(PLAY);
        }
    }

    if (state == PLAY) {
        if (button_pressed /* && button_released*/) {
            if (time_up()) {
                update_state(TEMP_RECORD);
            } else {
                update_state(STOPPED);
            }
        }
    }

    if (state == TEMP_RECORD) {
        if (!button_pressed && !button_released && time_up()) {
            // invalidate tmp buffer
            update_state(PLAY);
        } else if (button_pressed && button_released && !time_up()) {
            // invalidate tmp buffer
            update_state(STOPPED);
        } else if (button_released && time_up()) {
            update_state(RECORD);
        }
    }

    if (state == STOPPED) {
        if (button_pressed && button_released) {
            update_state(PLAYBACK1);
        } else if (!button_released && time_up()) {
            update_state(IDLE);
        }
    }

    if (state == PLAYBACK1) {
        if (button_released && button_pressed && !time_up()) {
            update_state(STOPPED);
        } else if (!button_released && time_up()) {
            update_state(IDLE);
        } else if (button_released && button_pressed /*&& time_up() implied*/) {
            update_state(TEMP_RECORD);
        }
    }

    // old code below
    // add the current and looped values together, then write back
    uint index = 0;
    if (state != IDLE && state != STOPPED && state != FIRST_STOP) {
        index = ram_buffer_offset[LOOP_BUFFER]++;
    }

    int16_t mixed;
    if (state == FIRST_RECORD || state == IDLE || state == STOPPED || state == FIRST_STOP) {
        mixed = current;
    } else {
        mixed = add_no_overflow(add_no_overflow(ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE], ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE]), current);
    }

    if (state == IDLE || state == STOPPED || state == FIRST_STOP) { 
        return mixed; // we're done
    }
    
    loop_time++;
    if (state == FIRST_RECORD) loop_length++;

    if (state == RECORD || state == TEMP_RECORD || state == FIRST_TMP_RECORD) {
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = add_no_overflow(ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE], ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE]);
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
    } else if (state == FIRST_RECORD) {
        ram_buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
        ram_buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = 0;
    }

    if (ram_buffer_offset[LOOP_BUFFER] >= BUFFER_SIZE) {
        // we're out of bounds, so we need to swap buffers
        which = !which; // swap buffers. The other buffer must contain the next audio to be played

        // special hack for first reads
        if (state == FIRST_RECORD) {
            ram_buffer_start[LOOP_BUFFER] = ram_buffer_start[PSRAM_ACCESS_BUFFER] + BUFFER_SIZE;
            read_location = 0; // always read from 0 for first_record
        } else {
            read_location = (ram_buffer_start[LOOP_BUFFER] + BUFFER_SIZE) % loop_length;
        }

        signal_write = true;
    }

    if (loop_time >= loop_length) {
        loop_time = 0;
    }

    return mixed;
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

    i2s_program_start_synched(pio0, &my_config, dma_i2s_in_handler, &i2s);
    
    // Initialize the PSRAM
    ice_sram_init();

    // initialize the footswitch button
    button_t* footswitch = create_button(FOOTSWITCH_PIN, footswitch_onchange);

    while (1) {
        tud_task(); // tinyusb device task
        if (signal_write) {
            write_routine();
        }
    }
}
