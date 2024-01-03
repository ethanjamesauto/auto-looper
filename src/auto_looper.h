#define BUFFER_SIZE 128 // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)
#define SCRATCH_BUFFER_SIZE 48000 // Should be a second long

// used for ram_buffer indexing
#define MAIN_SAMPLE 0
#define ACTIVE_SAMPLE 1

struct looper_t {
    int16_t scratch_buffer[SCRATCH_BUFFER_SIZE];
    uint scratch_buffer_start;
    uint scratch_buffer_size;

    int16_t buffer[2][BUFFER_SIZE][2];

    uint buffer_start[2];
    uint buffer_offset[2];
    uint loop_length; // loop length in samples (2ish seconds maybe)
    uint loop_time; // current time in samples
    bool which = false; // which buffer is active

    uint active_start;
    uint active_size;
    bool undo_mode; // when true, the active region is not played back

    uint old_active_start;
    uint old_active_size;

    looper_t() {
        buffer_start[0] = 0;
        buffer_start[1] = 0;
        buffer_offset[0] = 0;
        buffer_offset[1] = 0;
        loop_length = 0;
        loop_time = 0;

        // TODO: needed?
        scratch_buffer_start = 0;
        scratch_buffer_size = 0;
        active_start = 0;
        active_size = 0;
        old_active_start = 0;
        old_active_size = 0;
        undo_mode = false;
    }
};

// run the main state machine and get the next sample
int16_t get_next_sample(int16_t current);