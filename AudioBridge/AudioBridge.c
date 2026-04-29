// AudioBridge.c
// Reads the shared-memory ring buffer written by DualAudioDriver and plays it
// to one or two real outputs via HALOutput AudioUnits.
// If secondaryDeviceID != kAudioObjectUnknown at create time, the same frames
// are also written into an in-process SPSC fan-out ring that feeds the second unit.

#include "include/AudioBridge.h"
#include "include/DualAudioRingBuffer.h"

#include <AudioUnit/AudioUnit.h>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>

#define BRIDGE_SUBSYSTEM "com.dualaudio.AudioBridge"
#define BRIDGE_CATEGORY  "engine"

// ── In-process fan-out ring buffer (SPSC, heap-allocated) ─────────────────────
// Producer: primary render callback. Consumer: secondary render callback.

#define FANOUT_RING_FRAMES 8192
#define FANOUT_RING_MASK   (FANOUT_RING_FRAMES - 1)

typedef struct {
    _Atomic(uint64_t) write_pos;
    char              _pad1[56];
    _Atomic(uint64_t) read_pos;
    char              _pad2[56];
    float             samples[FANOUT_RING_FRAMES * DUAL_AUDIO_CHANNELS];
} FanoutRing;

static inline void FanoutRing_write(FanoutRing* fr, const float* src, uint32_t n)
{
    uint64_t wp    = atomic_load_explicit(&fr->write_pos, memory_order_relaxed);
    uint64_t rp    = atomic_load_explicit(&fr->read_pos,  memory_order_acquire);
    uint64_t avail = (uint64_t)FANOUT_RING_FRAMES - (wp - rp);
    if (avail < n) n = (uint32_t)avail;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = ((uint32_t)(wp + i) & FANOUT_RING_MASK) * DUAL_AUDIO_CHANNELS;
        fr->samples[idx]     = src[i * DUAL_AUDIO_CHANNELS];
        fr->samples[idx + 1] = src[i * DUAL_AUDIO_CHANNELS + 1];
    }
    atomic_store_explicit(&fr->write_pos, wp + n, memory_order_release);
}

static inline void FanoutRing_read(FanoutRing* fr, float* dst, uint32_t n)
{
    uint64_t rp    = atomic_load_explicit(&fr->read_pos,  memory_order_relaxed);
    uint64_t wp    = atomic_load_explicit(&fr->write_pos, memory_order_acquire);
    uint64_t avail = wp - rp;
    if (avail < n) {
        memset(dst, 0, n * DUAL_AUDIO_CHANNELS * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = ((uint32_t)(rp + i) & FANOUT_RING_MASK) * DUAL_AUDIO_CHANNELS;
        dst[i * DUAL_AUDIO_CHANNELS]     = fr->samples[idx];
        dst[i * DUAL_AUDIO_CHANNELS + 1] = fr->samples[idx + 1];
    }
    atomic_store_explicit(&fr->read_pos, rp + n, memory_order_release);
}

// ── In-process delay line (heap-allocated) ────────────────────────────────────

#define MAX_DELAY_FRAMES 88200 // 2 seconds at 44.1kHz

typedef struct {
    float*   buffer;
    uint32_t write_pos;
} DelayLine;

static inline void DelayLine_init(DelayLine* dl) {
    dl->buffer = calloc(MAX_DELAY_FRAMES * DUAL_AUDIO_CHANNELS, sizeof(float));
    dl->write_pos = 0;
}

static inline void DelayLine_free(DelayLine* dl) {
    if (dl->buffer) {
        free(dl->buffer);
        dl->buffer = NULL;
    }
}

static inline void DelayLine_process(DelayLine* dl, float* io_frames, uint32_t num_frames, uint32_t delay_frames) {
    if (!dl->buffer || delay_frames == 0) return;
    if (delay_frames > MAX_DELAY_FRAMES) delay_frames = MAX_DELAY_FRAMES;
    
    for (uint32_t i = 0; i < num_frames; i++) {
        uint32_t read_idx = (MAX_DELAY_FRAMES + dl->write_pos - delay_frames) % MAX_DELAY_FRAMES;
        
        float l_in = io_frames[i * DUAL_AUDIO_CHANNELS];
        float r_in = io_frames[i * DUAL_AUDIO_CHANNELS + 1];
        
        float l_out = dl->buffer[read_idx * DUAL_AUDIO_CHANNELS];
        float r_out = dl->buffer[read_idx * DUAL_AUDIO_CHANNELS + 1];
        
        dl->buffer[dl->write_pos * DUAL_AUDIO_CHANNELS]     = l_in;
        dl->buffer[dl->write_pos * DUAL_AUDIO_CHANNELS + 1] = r_in;
        
        io_frames[i * DUAL_AUDIO_CHANNELS]     = l_out;
        io_frames[i * DUAL_AUDIO_CHANNELS + 1] = r_out;
        
        dl->write_pos = (dl->write_pos + 1) % MAX_DELAY_FRAMES;
    }
}


// ── AudioBridge struct ─────────────────────────────────────────────────────────

struct AudioBridge {
    AudioDeviceID        primary_id;
    AudioUnit            primary_unit;
    AudioDeviceID        secondary_id;
    AudioUnit            secondary_unit;   // NULL if not in use
    int                  shm_fd;
    DualAudioRingBuffer* ring;
    FanoutRing*          fanout;           // NULL if no secondary
    _Atomic(float)       primary_vol;
    _Atomic(float)       secondary_vol;
    _Atomic(uint32_t)    primary_delay;
    _Atomic(uint32_t)    secondary_delay;
    DelayLine            primary_dl;
    DelayLine            secondary_dl;
    os_log_t             log;
    bool                 running;
};

// ── Render callbacks (real-time thread – no locks, no allocation) ──────────────

static OSStatus PrimaryRenderCallback(void*                       refCon,
                                       AudioUnitRenderActionFlags* ioFlags,
                                       const AudioTimeStamp*       inTimeStamp,
                                       UInt32                      inBusNumber,
                                       UInt32                      inFrameCount,
                                       AudioBufferList*            ioData)
{
    (void)ioFlags; (void)inTimeStamp; (void)inBusNumber;
    struct AudioBridge* b = (struct AudioBridge*)refCon;

    if (!ioData || ioData->mNumberBuffers == 0) return noErr;
    float* out = (float*)ioData->mBuffers[0].mData;
    if (!out) return noErr;

    if (b->ring) {
        DualAudioRingBuffer_read(b->ring, out, inFrameCount);
    } else {
        memset(out, 0, inFrameCount * DUAL_AUDIO_CHANNELS * sizeof(float));
    }

    if (b->fanout) {
        FanoutRing_write(b->fanout, out, inFrameCount);
    }

    uint32_t delay = atomic_load_explicit(&b->primary_delay, memory_order_relaxed);
    if (delay > 0) {
        DelayLine_process(&b->primary_dl, out, inFrameCount, delay);
    }
    
    float vol = atomic_load_explicit(&b->primary_vol, memory_order_relaxed);
    if (vol != 1.0f) {
        for (uint32_t i = 0; i < inFrameCount * DUAL_AUDIO_CHANNELS; i++) {
            out[i] *= vol;
        }
    }

    return noErr;
}

static OSStatus SecondaryRenderCallback(void*                       refCon,
                                         AudioUnitRenderActionFlags* ioFlags,
                                         const AudioTimeStamp*       inTimeStamp,
                                         UInt32                      inBusNumber,
                                         UInt32                      inFrameCount,
                                         AudioBufferList*            ioData)
{
    (void)ioFlags; (void)inTimeStamp; (void)inBusNumber;
    struct AudioBridge* b = (struct AudioBridge*)refCon;

    if (!ioData || ioData->mNumberBuffers == 0) return noErr;
    float* out = (float*)ioData->mBuffers[0].mData;
    if (!out) return noErr;

    if (b->fanout) {
        FanoutRing_read(b->fanout, out, inFrameCount);
    } else {
        memset(out, 0, inFrameCount * DUAL_AUDIO_CHANNELS * sizeof(float));
    }

    uint32_t delay = atomic_load_explicit(&b->secondary_delay, memory_order_relaxed);
    if (delay > 0) {
        DelayLine_process(&b->secondary_dl, out, inFrameCount, delay);
    }
    
    float vol = atomic_load_explicit(&b->secondary_vol, memory_order_relaxed);
    if (vol != 1.0f) {
        for (uint32_t i = 0; i < inFrameCount * DUAL_AUDIO_CHANNELS; i++) {
            out[i] *= vol;
        }
    }

    return noErr;
}

// ── AudioStreamBasicDescription matching the virtual device format ────────────

static AudioStreamBasicDescription BridgeASBD(void)
{
    AudioStreamBasicDescription d = {0};
    d.mSampleRate       = 44100.0;
    d.mFormatID         = kAudioFormatLinearPCM;
    d.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
    d.mBitsPerChannel   = 32;
    d.mChannelsPerFrame = 2;
    d.mFramesPerPacket  = 1;
    d.mBytesPerFrame    = 8;
    d.mBytesPerPacket   = 8;
    return d;
}

// ── HAL unit setup helper ─────────────────────────────────────────────────────

static OSStatus setup_hal_unit(AudioDeviceID     device_id,
                                AURenderCallback  callback,
                                void*             refcon,
                                os_log_t          log,
                                AudioUnit*        unit_out)
{
    AudioComponentDescription desc = {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        os_log_error(log, "[AudioBridge] AudioComponentFindNext failed");
        return -1;
    }

    OSStatus err = AudioComponentInstanceNew(comp, unit_out);
    if (err != noErr) {
        os_log_error(log, "[AudioBridge] ComponentInstanceNew err=%d", (int)err);
        return err;
    }

    err = AudioUnitSetProperty(*unit_out,
                               kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &device_id, sizeof(device_id));
    if (err != noErr) {
        os_log_error(log, "[AudioBridge] set device err=%d device=%u", (int)err, device_id);
        return err;
    }

    AudioStreamBasicDescription fmt = BridgeASBD();
    err = AudioUnitSetProperty(*unit_out,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0,
                               &fmt, sizeof(fmt));
    if (err != noErr) {
        os_log_error(log, "[AudioBridge] set format err=%d", (int)err);
        return err;
    }

    AURenderCallbackStruct cb = { .inputProc = callback, .inputProcRefCon = refcon };
    err = AudioUnitSetProperty(*unit_out,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0,
                               &cb, sizeof(cb));
    if (err != noErr) {
        os_log_error(log, "[AudioBridge] set callback err=%d", (int)err);
        return err;
    }

    err = AudioUnitInitialize(*unit_out);
    if (err != noErr) {
        os_log_error(log, "[AudioBridge] Initialize err=%d", (int)err);
        return err;
    }

    return noErr;
}

// ── Public API ────────────────────────────────────────────────────────────────

AudioBridgeRef AudioBridgeCreate(AudioDeviceID primaryDeviceID,
                                  AudioDeviceID secondaryDeviceID)
{
    struct AudioBridge* b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->primary_id   = primaryDeviceID;
    b->secondary_id = secondaryDeviceID;
    b->running      = false;
    b->log          = os_log_create(BRIDGE_SUBSYSTEM, BRIDGE_CATEGORY);

    atomic_store_explicit(&b->primary_vol, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&b->secondary_vol, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&b->primary_delay, 0, memory_order_relaxed);
    atomic_store_explicit(&b->secondary_delay, 0, memory_order_relaxed);
    DelayLine_init(&b->primary_dl);

    // ── Open (or create) shared memory ───────────────────────────────────────
    b->shm_fd = shm_open(DUAL_AUDIO_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (b->shm_fd < 0) {
        os_log_error(b->log, "[AudioBridge] shm_open failed errno=%d", errno);
    } else {
        ftruncate(b->shm_fd, (off_t)sizeof(DualAudioRingBuffer));
        b->ring = (DualAudioRingBuffer*)mmap(
            NULL, sizeof(DualAudioRingBuffer),
            PROT_READ | PROT_WRITE, MAP_SHARED, b->shm_fd, 0);
        if (b->ring == MAP_FAILED) {
            os_log_error(b->log, "[AudioBridge] mmap failed errno=%d", errno);
            b->ring = NULL;
        }
    }

    // ── Fan-out ring (only if secondary is requested) ─────────────────────────
    bool has_secondary = (secondaryDeviceID != kAudioObjectUnknown);
    if (has_secondary) {
        DelayLine_init(&b->secondary_dl);
        b->fanout = calloc(1, sizeof(FanoutRing));
        if (!b->fanout) {
            os_log_error(b->log, "[AudioBridge] fanout alloc failed");
            AudioBridgeDestroy(b);
            return NULL;
        }
        os_log(b->log, "[AudioBridge] fanout ring allocated");
    }

    // ── Primary HAL unit ─────────────────────────────────────────────────────
    OSStatus err = setup_hal_unit(primaryDeviceID, PrimaryRenderCallback, b,
                                   b->log, &b->primary_unit);
    if (err != noErr) {
        os_log_error(b->log, "[AudioBridge] primary unit setup failed err=%d", (int)err);
        AudioBridgeDestroy(b);
        return NULL;
    }
    os_log(b->log, "[AudioBridge] primary unit created, device=%u", primaryDeviceID);

    // ── Secondary HAL unit (optional) ────────────────────────────────────────
    if (has_secondary) {
        err = setup_hal_unit(secondaryDeviceID, SecondaryRenderCallback, b,
                              b->log, &b->secondary_unit);
        if (err != noErr) {
            // Degraded mode: log and continue with primary only
            os_log_error(b->log,
                         "[AudioBridge] secondary unit setup failed err=%d device=%u — degraded mode",
                         (int)err, secondaryDeviceID);
            if (b->secondary_unit) {
                AudioUnitUninitialize(b->secondary_unit);
                AudioComponentInstanceDispose(b->secondary_unit);
                b->secondary_unit = NULL;
            }
        } else {
            os_log(b->log, "[AudioBridge] secondary unit created, device=%u", secondaryDeviceID);
        }
    }

    return (AudioBridgeRef)b;
}

OSStatus AudioBridgeStart(AudioBridgeRef bridge)
{
    if (!bridge) return -50; // paramErr
    struct AudioBridge* b = (struct AudioBridge*)bridge;
    if (b->running) return noErr;

    OSStatus err = AudioOutputUnitStart(b->primary_unit);
    if (err != noErr) {
        os_log_error(b->log, "[AudioBridge] primary start err=%d", (int)err);
        return err;
    }
    os_log(b->log, "[AudioBridge] primary started, device=%u", b->primary_id);

    if (b->secondary_unit) {
        OSStatus err2 = AudioOutputUnitStart(b->secondary_unit);
        if (err2 != noErr) {
            os_log_error(b->log,
                         "[AudioBridge] secondary start err=%d device=%u — continuing without secondary",
                         (int)err2, b->secondary_id);
        } else {
            os_log(b->log, "[AudioBridge] secondary started, device=%u", b->secondary_id);
        }
    }

    b->running = true;
    return noErr;
}

void AudioBridgeStop(AudioBridgeRef bridge)
{
    if (!bridge) return;
    struct AudioBridge* b = (struct AudioBridge*)bridge;
    if (!b->running) return;

    if (b->secondary_unit) {
        AudioOutputUnitStop(b->secondary_unit);
        os_log(b->log, "[AudioBridge] secondary stopped, device=%u", b->secondary_id);
    }
    AudioOutputUnitStop(b->primary_unit);
    os_log(b->log, "[AudioBridge] primary stopped, device=%u", b->primary_id);

    b->running = false;
}

void AudioBridgeDestroy(AudioBridgeRef bridge)
{
    if (!bridge) return;
    struct AudioBridge* b = (struct AudioBridge*)bridge;
    AudioBridgeStop(bridge);

    if (b->secondary_unit) {
        AudioUnitUninitialize(b->secondary_unit);
        AudioComponentInstanceDispose(b->secondary_unit);
    }
    if (b->primary_unit) {
        AudioUnitUninitialize(b->primary_unit);
        AudioComponentInstanceDispose(b->primary_unit);
    }
    if (b->fanout) free(b->fanout);
    if (b->ring && b->ring != MAP_FAILED) munmap(b->ring, sizeof(DualAudioRingBuffer));
    if (b->shm_fd >= 0) close(b->shm_fd);
    
    DelayLine_free(&b->primary_dl);
    DelayLine_free(&b->secondary_dl);

    os_log(b->log, "[AudioBridge] destroyed");
    free(b);
}

void AudioBridgeSetVolumes(AudioBridgeRef bridge, float primaryVol, float secondaryVol) {
    if (!bridge) return;
    struct AudioBridge* b = (struct AudioBridge*)bridge;
    atomic_store_explicit(&b->primary_vol, primaryVol, memory_order_relaxed);
    atomic_store_explicit(&b->secondary_vol, secondaryVol, memory_order_relaxed);
}

void AudioBridgeSetDelays(AudioBridgeRef bridge, uint32_t primaryDelayFrames, uint32_t secondaryDelayFrames) {
    if (!bridge) return;
    struct AudioBridge* b = (struct AudioBridge*)bridge;
    atomic_store_explicit(&b->primary_delay, primaryDelayFrames, memory_order_relaxed);
    atomic_store_explicit(&b->secondary_delay, secondaryDelayFrames, memory_order_relaxed);
}
