#define BUFFER_SIZE (128) // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)


// used for ram_buffer indexing
#define MAIN_SAMPLE 0
#define ACTIVE_SAMPLE 1

struct looper_t {
    int16_t buffer[2][BUFFER_SIZE][2];
    uint buffer_start[2];
    uint buffer_offset[2];
    uint loop_length; // loop length in samples (2ish seconds maybe)
    uint loop_time; // current time in samples
    bool which = false;

    looper_t() {
        buffer_start[0] = 0;
        buffer_start[1] = 0;
        buffer_offset[0] = 0;
        buffer_offset[1] = 0;
        loop_length = 0;
        loop_time = 0;
    }
};

// run the main state machine and get the next sample
int16_t get_next_sample(int16_t current);