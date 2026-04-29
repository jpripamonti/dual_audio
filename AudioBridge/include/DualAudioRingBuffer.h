#pragma once
// Lock-free SPSC ring buffer in shared memory.
// Writer: HAL driver (coreaudiod IO thread)
// Reader: app's AudioOutputUnit render callback

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define DUAL_AUDIO_SHM_NAME    "/DualAudio_shm_v1"
#define DUAL_AUDIO_CHANNELS    2
#define DUAL_AUDIO_RING_FRAMES 8192              // power-of-2 ≈ 186 ms @ 44100
#define DUAL_AUDIO_RING_MASK   (DUAL_AUDIO_RING_FRAMES - 1)

typedef struct {
    _Atomic(uint64_t) write_pos;                // total frames written (monotonic)
    char              _pad1[56];                // separate cache lines
    _Atomic(uint64_t) read_pos;                 // total frames consumed (monotonic)
    char              _pad2[56];
    float             samples[DUAL_AUDIO_RING_FRAMES * DUAL_AUDIO_CHANNELS];
} DualAudioRingBuffer;

// Called by the HAL driver on the coreaudiod IO thread.
// Drops the newest frames if the buffer is full (reader too slow).
static inline void DualAudioRingBuffer_write(DualAudioRingBuffer* rb,
                                              const float*          src,
                                              uint32_t              frameCount)
{
    uint64_t wp    = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    uint64_t rp    = atomic_load_explicit(&rb->read_pos,  memory_order_acquire);
    uint64_t avail = (uint64_t)DUAL_AUDIO_RING_FRAMES - (wp - rp);
    if (avail < frameCount) frameCount = (uint32_t)avail;

    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t idx = ((uint32_t)(wp + i) & DUAL_AUDIO_RING_MASK) * DUAL_AUDIO_CHANNELS;
        rb->samples[idx]     = src[i * DUAL_AUDIO_CHANNELS];
        rb->samples[idx + 1] = src[i * DUAL_AUDIO_CHANNELS + 1];
    }
    atomic_store_explicit(&rb->write_pos, wp + frameCount, memory_order_release);
}

// Called by the AudioUnit render callback (real-time thread, no locks allowed).
// Outputs silence on underrun.
static inline void DualAudioRingBuffer_read(DualAudioRingBuffer* rb,
                                             float*               dst,
                                             uint32_t             frameCount)
{
    uint64_t rp    = atomic_load_explicit(&rb->read_pos,  memory_order_relaxed);
    uint64_t wp    = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    uint64_t avail = wp - rp;

    if (avail < frameCount) {
        memset(dst, 0, frameCount * DUAL_AUDIO_CHANNELS * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t idx = ((uint32_t)(rp + i) & DUAL_AUDIO_RING_MASK) * DUAL_AUDIO_CHANNELS;
        dst[i * DUAL_AUDIO_CHANNELS]     = rb->samples[idx];
        dst[i * DUAL_AUDIO_CHANNELS + 1] = rb->samples[idx + 1];
    }
    atomic_store_explicit(&rb->read_pos, rp + frameCount, memory_order_release);
}
