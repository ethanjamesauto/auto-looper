#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "boards/pico_ice.h"
#include "ice_sram.h"
#include "pico/stdlib.h"

// tinyusb
#include "tusb.h"

#include "button.h"
#include "i2s.h"

#include "auto_looper.h"

#define FOOTSWITCH_PIN 6 // The footswitch pin

looper_t looper;

#define PSRAM_ACCESS_BUFFER (!looper.which)   // the buffer that is read from and then written to by PSRAM
#define LOOP_BUFFER (looper.which)            // the buffer that is used for looping by the CPU

// used to tell the main loop to write to PSRAM and then read from it
volatile bool signal_write = false; 
volatile uint read_location = 0;

static __attribute__((aligned(8))) pio_i2s i2s; // i2s instance

enum state_t { 
    IDLE, 
    FIRST_RECORD, FIRST_TMP_RECORD, TEMP_RECORD, RECORD, 
    FIRST_PLAYBACK, PLAY, PLAYBACK1,
    FIRST_STOP, STOPPED 
};

// used for debugging
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

// TODO: stop using PSRAM for short loop lengths. Minimum loop length right now is BUFFER_SIZE

static void process_audio(const int32_t* input, int32_t* output, size_t num_frames) {
    // Just copy the input to the output
    for (size_t i = 0; i < num_frames * 2; i++) {
        output[i] = get_next_sample(input[i] >> 16) << 16;
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
    uint write_size = looper.buffer_offset[PSRAM_ACCESS_BUFFER];
    uint write_location = looper.buffer_start[PSRAM_ACCESS_BUFFER];
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
        ice_sram_write_blocking(psram_address, (uint8_t*) looper.buffer[PSRAM_ACCESS_BUFFER], psram_write_size);// write_callback, NULL);
    }

    // TODO: read scratch buffer if needed, using old active buffer as well!
    if (looper.scratch_buffer_size == SCRATCH_BUFFER_SIZE) {
        // read from psram, mix with scratch buffer, write back to psram.
        // also mix active buffer into main buffer if old active buffer is not empty
        uint start_time = looper.scratch_buffer_start + looper.scratch_buffer_ptr;
        uint32_t psram_address = start_time * 4;
        uint32_t psram_rw_size = BUFFER_SIZE * 4;
        //printf("Reading from address %d, var which is %d\n", psram_address/2, which);
        ice_sram_read_blocking(psram_address, (uint8_t*) looper.buffer[PSRAM_ACCESS_BUFFER], psram_rw_size);// read_callback, NULL);

        for (int i = 0; i < BUFFER_SIZE; i++) {
            if (looper.in_old_active_region(start_time + i)) { // TODO: check old active region logic
                // mix active buffer into main buffer
                looper.buffer[PSRAM_ACCESS_BUFFER][i][MAIN_SAMPLE] = add(looper.buffer[PSRAM_ACCESS_BUFFER][i][MAIN_SAMPLE], looper.buffer[PSRAM_ACCESS_BUFFER][i][ACTIVE_SAMPLE]);
            }
            // mix scratch buffer into active buffer
            looper.buffer[PSRAM_ACCESS_BUFFER][i][ACTIVE_SAMPLE] = looper.scratch_buffer[looper.scratch_buffer_ptr + i];
        }

        // write back to psram
        ice_sram_write_blocking(psram_address, (uint8_t*) looper.buffer[PSRAM_ACCESS_BUFFER], psram_rw_size);// write_callback, NULL);

        looper.scratch_buffer_ptr += BUFFER_SIZE;
        if (looper.scratch_buffer_ptr >= looper.scratch_buffer_size) {
            looper.scratch_buffer_size = 0;
            looper.scratch_buffer_ptr = 0;
            printf("Finished writing scratch buffer\n");
        }
    }

    looper.buffer_offset[PSRAM_ACCESS_BUFFER] = 0;
    looper.buffer_start[PSRAM_ACCESS_BUFFER] = read_location;
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
        ice_sram_read_blocking(psram_address, (uint8_t*) looper.buffer[PSRAM_ACCESS_BUFFER], psram_write_size);// read_callback, NULL);
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
    return time_us_64() - last_time > 660000;
}

// for debugging
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
        looper = looper_t(); // reset the looper
    }

    if (state == RECORD) {
        if (looper.undo_mode) {
            // we don't need to save the old active region, so we can just overwrite it
            looper.set_undo_mode(false);
        } else {
            // mark the old active region for writing
            looper.add_old_active_region(looper.active_start, looper.active_size);
        }

        if (looper.scratch_buffer_size != SCRATCH_BUFFER_SIZE) {
            printf("Error: scratch buffer not full\n");
        }
        looper.active_start = (looper.loop_time - looper.scratch_buffer_size + looper.loop_length) % looper.loop_length;
        looper.active_size = looper.scratch_buffer_size;

        printf("New active region: %d, %d\n", looper.active_start, looper.active_size);
    }

    if (state == FIRST_TMP_RECORD || state == TEMP_RECORD) {
        // reset the scratch buffer
        looper.scratch_buffer_start = looper.loop_time;
        looper.scratch_buffer_size = 0;
        looper.scratch_buffer_ptr = 0;
    }

    reset_button();
    printf("State changed to %s\n", state_names[state]);
    printf("Current status: %s\n\n", get_state_type(state));
}

int16_t get_next_sample(int16_t current) {

    if (state == IDLE) {
        if (button_pressed && button_released) {
            update_state(FIRST_RECORD);
        }
    }

    if (state == FIRST_RECORD) {
        if (button_pressed && button_released) {
            looper.loop_length = (looper.loop_length / BUFFER_SIZE) * BUFFER_SIZE; // TODO: tmp
            printf("Loop length: %d\n\n", looper.loop_length);
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
        bool done = looper.scratch_buffer_size >= SCRATCH_BUFFER_SIZE;
        if (!button_pressed && !button_released && done) {
            // invalidate tmp buffer
            update_state(IDLE);
        } else if (button_pressed && button_released && !done) {
            // invalidate tmp buffer
            update_state(STOPPED);
        } else if (done) {
            update_state(RECORD);
        }
    }

    if (state == RECORD) {
        // scratch buffer must be flushed before finishing recording
        if (button_pressed && looper.scratch_buffer_size == 0/* && button_released*/) {
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
        } else if (!button_released && time_up()) {
            // undo
            looper.set_undo_mode(true);
            reset_button();
        }
    }

    if (state == TEMP_RECORD) {
        bool done = looper.scratch_buffer_size >= SCRATCH_BUFFER_SIZE;
        if (!button_pressed && !button_released && done) {
            // invalidate tmp buffer
            looper.set_undo_mode(!looper.undo_mode);
            update_state(PLAY);
        } else if (button_pressed && button_released && !done) {
            // invalidate tmp buffer
            update_state(STOPPED);
        } else if (done) {
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
        index = looper.buffer_offset[LOOP_BUFFER]++;
    }

    if (state == IDLE || state == STOPPED || state == FIRST_STOP) { 
        return current; // we're done
    }

    int16_t mixed = current;

    bool in_old_active_region = looper.in_old_active_region();
    uint16_t main = looper.buffer[LOOP_BUFFER][index][MAIN_SAMPLE];
    uint16_t active = looper.buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE];

    if ((!looper.undo_mode && looper.in_active_region()) || in_old_active_region) { // TODO: check this line more carefully
        // we're in the active region, so we need to mix the current sample with the active sample
        mixed = add(mixed, active);
    }
    if (state != FIRST_RECORD) {
        // we're not in the first record state, so we need to mix the current sample with the main sample
        mixed = add(mixed, main);
    }

    // TODO: hasn't been verified yet
    if (looper.active_size == looper.loop_length) {
        looper.buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = add(active, current);
    }

    if (in_old_active_region) {
        // we need to write the old active region
        looper.buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = add(active, main);
    }

    if (state == RECORD) {
        looper.buffer[LOOP_BUFFER][index][ACTIVE_SAMPLE] = current;
    } else if (state == FIRST_RECORD) {
        looper.buffer[LOOP_BUFFER][index][MAIN_SAMPLE] = current;
    }

    if (state == TEMP_RECORD || state == FIRST_TMP_RECORD) {
        looper.scratch_buffer[looper.scratch_buffer_size] = current;
        looper.scratch_buffer_size++;
    }

    // now increment time and length
    looper.loop_time++;
    if (state == FIRST_RECORD) looper.loop_length++;

    if (state == RECORD) {
        if (looper.active_size < looper.loop_length) looper.active_size++;
    }

    if (looper.buffer_offset[LOOP_BUFFER] >= BUFFER_SIZE) {
        // we're out of bounds, so we need to swap buffers
        looper.which = !looper.which; // swap buffers. The other buffer must contain the next audio to be played

        // special hack for first reads
        if (state == FIRST_RECORD) {
            looper.buffer_start[LOOP_BUFFER] = looper.buffer_start[PSRAM_ACCESS_BUFFER] + BUFFER_SIZE;
            read_location = 0; // always read from 0 for first_record
        } else {
            read_location = (looper.buffer_start[LOOP_BUFFER] + BUFFER_SIZE) % looper.loop_length;
        }

        // see if signal_write is true here, and signal an error if it is.
        if (signal_write) {
            printf("Error: previous write not complete\n");
        }
        signal_write = true;
    }

    if (looper.loop_time >= looper.loop_length) {
        looper.loop_time = 0;
    }

    return mixed;
}

int main()
{
    set_sys_clock_khz(132000, true);
    tusb_init();
    stdio_init_all();

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
    ice_sram_init(); // TODO: NOTE: you MUST modify ice_spi.c to stop it from setting i2s pins to SIO.
    // comment out lines 120-122 inclusive in ice_spi.c

    // initialize the footswitch button
    button_t* footswitch = create_button(FOOTSWITCH_PIN, footswitch_onchange);

    while (1) {
        tud_task(); // tinyusb device task
        if (signal_write) {
            write_routine();
        }
    }
}
