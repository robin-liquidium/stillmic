// Streaming DeepFilterNet3 inference. One hop is 480 mono samples at 48 kHz (10 ms).
#ifndef DFN_H
#define DFN_H

#define DFN_HOP 480

typedef struct dfn dfn_t;

/// Maps the weights file and allocates all state. Returns NULL on failure.
dfn_t *dfn_create(const char *weights_path);
void dfn_destroy(dfn_t *d);

/// Clears audio history and model memory. Call before processing a new stream.
void dfn_reset(dfn_t *d);

/// Real-time safe: no allocation, locks or syscalls. `in` and `out` may be the same buffer.
void dfn_process(dfn_t *d, const float *in, float *out);

#endif
