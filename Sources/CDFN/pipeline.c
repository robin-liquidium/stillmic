#include "pipeline.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "dfn.h"

// Mono samples at 48 kHz between the mic thread (producer) and the engine thread (consumer).
enum { RING = 32768, RING_MASK = RING - 1, CHUNK = 1024, MAX_INPUT_CH = 2 };

struct nn_pipeline {
    dfn_t *dfn;
    AudioObjectID mic, engine;
    AudioDeviceIOProcID mic_proc, engine_proc;
    AudioConverterRef converter;  // only when the mic is not 48 kHz

    _Atomic bool enabled;
    bool was_enabled;  // mic thread only

    // Mic thread
    float hop[DFN_HOP];
    int hop_fill;
    const float *conv_in;
    UInt32 conv_in_frames;
    UInt32 conv_in_bytes_per_frame;

    // SPSC ring
    float ring[RING];
    _Atomic uint64_t write_pos, read_pos;

    // Engine thread
    int target_fill, drift_band;  // set before IO starts
    bool primed;
    float fill_avg;
    float last;
};

static void ring_write(nn_pipeline_t *p, const float *x, int n) {
    const uint64_t w = atomic_load_explicit(&p->write_pos, memory_order_relaxed);
    const uint64_t r = atomic_load_explicit(&p->read_pos, memory_order_acquire);
    if (w - r + (uint64_t)n > RING) return;  // consumer stopped: drop rather than overwrite
    for (int i = 0; i < n; i++) p->ring[(w + (uint64_t)i) & RING_MASK] = x[i];
    atomic_store_explicit(&p->write_pos, w + (uint64_t)n, memory_order_release);
}

static void push_48k(nn_pipeline_t *p, const float *x, int n) {
    while (n > 0) {
        int take = DFN_HOP - p->hop_fill;
        if (take > n) take = n;
        memcpy(p->hop + p->hop_fill, x, sizeof(float) * (size_t)take);
        p->hop_fill += take;
        x += take;
        n -= take;
        if (p->hop_fill == DFN_HOP) {
            p->hop_fill = 0;
            const bool on = atomic_load_explicit(&p->enabled, memory_order_relaxed);
            if (on && !p->was_enabled) dfn_reset(p->dfn);
            p->was_enabled = on;
            if (on) dfn_process(p->dfn, p->hop, p->hop);
            ring_write(p, p->hop, DFN_HOP);
        }
    }
}

static OSStatus conv_input(AudioConverterRef c, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **d, void *ctx) {
    nn_pipeline_t *p = ctx;
    if (p->conv_in_frames == 0) {
        *packets = 0;
        return 1;  // no more input for this callback; the converter keeps its state
    }
    if (*packets > p->conv_in_frames) *packets = p->conv_in_frames;
    data->mBuffers[0].mData = (void *)p->conv_in;
    data->mBuffers[0].mDataByteSize = *packets * (UInt32)sizeof(float);
    data->mBuffers[0].mNumberChannels = 1;
    p->conv_in += *packets;
    p->conv_in_frames -= *packets;
    return noErr;
}

static OSStatus mic_io(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *in_t,
                       AudioBufferList *out, const AudioTimeStamp *out_t, void *ctx) {
    nn_pipeline_t *p = ctx;

    // Mix the first two channels, whether interleaved in one buffer or split across buffers.
    // Larger interfaces carry unrelated inputs on the other channels.
    const float *chan[MAX_INPUT_CH];
    UInt32 stride[MAX_INPUT_CH], frames = 0;
    int nch = 0;
    for (UInt32 i = 0; i < in->mNumberBuffers && nch < MAX_INPUT_CH; i++) {
        const AudioBuffer *b = &in->mBuffers[i];
        if (!b->mData || b->mNumberChannels == 0) continue;
        const UInt32 f = b->mDataByteSize / (b->mNumberChannels * (UInt32)sizeof(float));
        if (nch == 0) frames = f;
        if (f < frames) frames = f;
        for (UInt32 c = 0; c < b->mNumberChannels && nch < MAX_INPUT_CH; c++, nch++) {
            chan[nch] = (const float *)b->mData + c;
            stride[nch] = b->mNumberChannels;
        }
    }
    if (nch == 0) return noErr;

    UInt32 pos = 0;
    float mono[CHUNK], conv[CHUNK * 4];
    while (pos < frames) {
        const UInt32 n = frames - pos < CHUNK ? frames - pos : CHUNK;
        const float *x;
        if (nch == 1 && stride[0] == 1) {
            x = chan[0] + pos;
        } else {
            for (UInt32 i = 0; i < n; i++) {
                float s = chan[0][(pos + i) * stride[0]];
                if (nch == 2) s = 0.5f * (s + chan[1][(pos + i) * stride[1]]);
                mono[i] = s;
            }
            x = mono;
        }
        if (p->converter) {
            p->conv_in = x;
            p->conv_in_frames = n;
            for (;;) {
                UInt32 got = CHUNK * 4;
                AudioBufferList o = {1, {{1, (UInt32)sizeof(conv), conv}}};
                const OSStatus st = AudioConverterFillComplexBuffer(p->converter, conv_input, p, &got, &o, NULL);
                if (got) push_48k(p, conv, (int)got);
                if (st != noErr || got == 0) break;
            }
        } else {
            push_48k(p, x, (int)n);
        }
        pos += n;
    }
    return noErr;
}

static OSStatus engine_io(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *in_t,
                          AudioBufferList *out, const AudioTimeStamp *out_t, void *ctx) {
    nn_pipeline_t *p = ctx;
    if (out->mNumberBuffers == 0 || !out->mBuffers[0].mData) return noErr;
    float *y = out->mBuffers[0].mData;  // driver format: interleaved stereo float
    const int frames = (int)(out->mBuffers[0].mDataByteSize / (2 * sizeof(float)));

    const uint64_t w = atomic_load_explicit(&p->write_pos, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&p->read_pos, memory_order_relaxed);
    int avail = (int)(w - r);

    if (!p->primed) {
        if (avail < p->target_fill) {
            memset(y, 0, (size_t)frames * 2 * sizeof(float));
            return noErr;
        }
        p->primed = true;
        p->fill_avg = (float)avail;
    }

    // The mic and the driver run on different clocks. Slip one sample at a time so the fill level
    // stays near the target, which is inaudible compared with dropping whole buffers.
    p->fill_avg += 0.02f * ((float)avail - p->fill_avg);
    bool repeat = false;
    if (p->fill_avg > p->target_fill + p->drift_band && avail > frames) {
        r++;
        avail--;
    } else if (p->fill_avg < p->target_fill - p->drift_band) {
        repeat = true;
    }

    int i = 0;
    if (repeat && frames > 0) {
        y[0] = y[1] = p->last;
        i = 1;
    }
    for (; i < frames && avail > 0; i++, avail--, r++) {
        const float s = p->ring[r & RING_MASK];
        y[2 * i] = y[2 * i + 1] = s;
        p->last = s;
    }
    if (i < frames) {  // underrun: fill with silence and re-prime
        memset(y + 2 * i, 0, (size_t)(frames - i) * 2 * sizeof(float));
        p->primed = false;
        p->last = 0.f;
    }
    atomic_store_explicit(&p->read_pos, r, memory_order_release);
    return noErr;
}

static Float64 device_rate(AudioObjectID dev) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float64 r = 0;
    UInt32 n = sizeof(r);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &r);
    return r;
}

// Requests a buffer size and returns the one actually in effect, since the HAL may refuse or clamp it.
static UInt32 set_buffer_frames(AudioObjectID dev, AudioObjectPropertyScope scope, UInt32 frames) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, scope, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(frames), &frames);
    UInt32 actual = frames, n = sizeof(actual);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &actual);
    return actual;
}

// IOProcs normally receive native 32-bit float. Refuse anything else instead of playing garbage.
static bool input_is_float32(AudioObjectID dev) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    AudioObjectID stream = 0;
    UInt32 n = sizeof(stream);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &stream) != noErr && n == 0) return false;
    if (stream == 0) return false;
    AudioObjectPropertyAddress f = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioStreamBasicDescription d;
    n = sizeof(d);
    if (AudioObjectGetPropertyData(stream, &f, 0, NULL, &n, &d) != noErr) return false;
    return d.mFormatID == kAudioFormatLinearPCM && (d.mFormatFlags & kAudioFormatFlagIsFloat) && d.mBitsPerChannel == 32;
}

nn_pipeline_t *nn_pipeline_create(const char *weights_path) {
    nn_pipeline_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->dfn = dfn_create(weights_path);
    if (!p->dfn) {
        free(p);
        return NULL;
    }
    atomic_init(&p->enabled, true);
    return p;
}

void nn_pipeline_destroy(nn_pipeline_t *p) {
    if (!p) return;
    nn_pipeline_stop(p);
    dfn_destroy(p->dfn);
    free(p);
}

void nn_pipeline_set_enabled(nn_pipeline_t *p, bool enabled) { atomic_store(&p->enabled, enabled); }

OSStatus nn_pipeline_start(nn_pipeline_t *p, AudioObjectID mic, AudioObjectID engine) {
    nn_pipeline_stop(p);
    dfn_reset(p->dfn);
    p->hop_fill = 0;
    p->was_enabled = atomic_load(&p->enabled);
    p->primed = false;
    p->fill_avg = 0.f;
    p->last = 0.f;
    atomic_store(&p->write_pos, 0);
    atomic_store(&p->read_pos, 0);

    const Float64 rate = device_rate(mic);
    if (rate <= 0) return kAudioHardwareBadDeviceError;
    if (!input_is_float32(mic)) return kAudioDeviceUnsupportedFormatError;
    if (rate != 48000.0) {
        AudioStreamBasicDescription in = {rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4, 1, 4, 1, 32, 0};
        AudioStreamBasicDescription o = in;
        o.mSampleRate = 48000.0;
        const OSStatus st = AudioConverterNew(&in, &o, &p->converter);
        if (st != noErr) return st;
    }

    // One hop per callback keeps wake-ups and latency low. The HAL clamps to the device's range.
    const UInt32 mic_frames = set_buffer_frames(mic, kAudioObjectPropertyScopeInput, (UInt32)(DFN_HOP * rate / 48000.0 + 0.5));
    const int burst = (int)((double)mic_frames * 48000.0 / rate + 0.5);
    int engine_frames = DFN_HOP;

    OSStatus st = AudioDeviceCreateIOProcID(mic, mic_io, p, &p->mic_proc);
    if (st != noErr) goto fail;
    p->mic = mic;
    if (engine) {
        engine_frames = (int)set_buffer_frames(engine, kAudioObjectPropertyScopeOutput, DFN_HOP);
        // Enough to survive the gap between mic bursts. Default one-hop buffers settle at 15 ms.
        const bool small = burst <= DFN_HOP && engine_frames <= DFN_HOP;
        p->target_fill = small ? 720 : burst + DFN_HOP + engine_frames;
        p->drift_band = small ? 240 : (burst + DFN_HOP) / 2;
        if (p->target_fill + DFN_HOP > RING) {
            st = kAudioDeviceUnsupportedFormatError;
            goto fail;
        }
        st = AudioDeviceCreateIOProcID(engine, engine_io, p, &p->engine_proc);
        if (st != noErr) goto fail;
        p->engine = engine;
        st = AudioDeviceStart(engine, p->engine_proc);
        if (st != noErr) goto fail;
    }
    st = AudioDeviceStart(mic, p->mic_proc);
    if (st != noErr) goto fail;
    return noErr;

fail:
    nn_pipeline_stop(p);
    return st;
}

void nn_pipeline_stop(nn_pipeline_t *p) {
    if (p->mic_proc) {
        AudioDeviceStop(p->mic, p->mic_proc);
        AudioDeviceDestroyIOProcID(p->mic, p->mic_proc);
        p->mic_proc = NULL;
    }
    if (p->engine_proc) {
        AudioDeviceStop(p->engine, p->engine_proc);
        AudioDeviceDestroyIOProcID(p->engine, p->engine_proc);
        p->engine_proc = NULL;
    }
    if (p->converter) {
        AudioConverterDispose(p->converter);
        p->converter = NULL;
    }
    p->mic = p->engine = 0;
}
