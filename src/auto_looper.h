#define BUFFER_SIZE 256 // Size in 2 SAMPLES (one active, one main). Max of 200k samples (now 100k because we use 2 buffers)
#define SCRATCH_BUFFER_SIZE (48000 * 2 / 3) // Should be a second long

// used for ram_buffer indexing
#define MAIN_SAMPLE 0
#define ACTIVE_SAMPLE 1

#include <vector>

using std::vector;

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

    // TODO: must use a vector of old active regions if using a completely linear buffer system :(
    vector<uint> old_active_start;
    vector<uint> old_active_size;
    vector<uint> old_active_left;

    bool in_region(uint start, uint size) {
        // must consider that the region can wrap past the loop end
        if (start + size > loop_length) {
            return loop_time >= start || loop_time < (start + size) % loop_length;
        } else {
            return loop_time >= start && loop_time < start + size;
        }
    }

    bool in_active_region() {
        return in_region(active_start, active_size);
    }

    bool in_old_active_region() {
        bool ret = false;
        for (int i = 0; i < old_active_start.size(); i++) {
            if (in_region(old_active_start[i], old_active_size[i])) {
                ret = true;
                old_active_left[i]--;
                if (old_active_left[i] == 0) {
                    printf("Erased old active region with start %d and size %d\n", old_active_start[i], old_active_size[i]);
                    old_active_start.erase(old_active_start.begin() + i);
                    old_active_size.erase(old_active_size.begin() + i);
                    old_active_left.erase(old_active_left.begin() + i);
                    i--;
                }
            }
        }
        return ret;
    }

    void add_old_active_region(uint start, uint size) {
        old_active_start.push_back(start);
        old_active_size.push_back(size);
        old_active_left.push_back(size);
    }
    
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
        old_active_start = vector<uint>();
        old_active_size = vector<uint>();
        old_active_left = vector<uint>();
        undo_mode = false;
    }

    inline void set_undo_mode(bool mode) {
        undo_mode = mode;
        printf("Undo mode set to %d\n", mode);
    }
};

// run the main state machine and get the next sample
int16_t get_next_sample(int16_t current);