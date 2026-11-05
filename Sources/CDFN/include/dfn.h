// Streaming DeepFilterNet3 inference. One hop is 480 mono samples at 48 kHz (10 ms).
#ifndef DFN_H
#define DFN_H

#define DFN_HOP 480

typedef struct dfn dfn_t;

typedef enum {
    DFN_STANDARD = 0,    // keeps the model running on every frame for a smoother voice
    DFN_AGGRESSIVE = 1,  // libDF's default thresholds: hard cuts and less CPU in quiet rooms
} dfn_mode_t;

/// Safe to call from any thread while audio is running.
void dfn_set_mode(dfn_t *d, dfn_mode_t mode);

/// Maps the weights file and allocates all state. Returns NULL on failure.
dfn_t *dfn_create(const char *weights_path);
void dfn_destroy(dfn_t *d);

/// Clears audio history and model memory. Call before processing a new stream.
void dfn_reset(dfn_t *d);

/// Real-time safe: no allocation, locks or syscalls. `in` and `out` may be the same buffer.
void dfn_process(dfn_t *d, const float *in, float *out);

#endif
