// Real-time path: physical mic -> DeepFilterNet3 -> Stillmic driver.
#ifndef NN_PIPELINE_H
#define NN_PIPELINE_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

typedef struct nn_pipeline nn_pipeline_t;

nn_pipeline_t *nn_pipeline_create(const char *weights_path);
void nn_pipeline_destroy(nn_pipeline_t *p);

/// Starts audio IO. `engine` may be 0 to run capture and denoising with no output (benchmarking).
/// Returns noErr on success. Call from one non-real-time thread only.
OSStatus nn_pipeline_start(nn_pipeline_t *p, AudioObjectID mic, AudioObjectID engine);
void nn_pipeline_stop(nn_pipeline_t *p);

/// false passes the mic through unprocessed. Safe to call while running.
void nn_pipeline_set_enabled(nn_pipeline_t *p, bool enabled);

void nn_pipeline_set_aggressive(nn_pipeline_t *p, bool aggressive);

#endif
