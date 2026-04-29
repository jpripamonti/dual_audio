#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdint.h>
#include "../AudioBridge/include/DualAudioRingBuffer.h"

// ── Factory UUID (must match Info.plist AudioServerPlugIn_FactoryUUIDs) ──────
#define kDualAudio_FactoryUUID "7E5A2B94-F6C8-4E1D-B3A5-9C0E6F2D1847"

// ── HAL object IDs ────────────────────────────────────────────────────────────
#define kPlugin_ObjectID   ((AudioObjectID)1)
#define kDevice_ObjectID   ((AudioObjectID)2)
#define kStream_ObjectID   ((AudioObjectID)3)

// ── Device / stream constants ─────────────────────────────────────────────────
#define kDevice_Name               "Dual Audio"
#define kDevice_Manufacturer       "DualAudio"
#define kDevice_UID                "DualAudio_UID_v1"
#define kDevice_ModelUID           "DualAudio_Model_v1"
#define kDevice_SampleRate         44100.0
#define kDevice_ChannelsPerFrame   2
#define kDevice_BitsPerChannel     32
#define kDevice_BytesPerFrame      (kDevice_ChannelsPerFrame * kDevice_BitsPerChannel / 8)
#define kDevice_BytesPerPacket     kDevice_BytesPerFrame
#define kDevice_DefaultBufferSize  512   // frames
#define kDevice_ZeroTimePeriod     441   // frames (~10 ms at 44100 Hz)

// ── Driver state ──────────────────────────────────────────────────────────────
// The first field MUST be the interface pointer so that
// (AudioServerPlugInDriverRef)&state == &state.iface_ptr is valid.
typedef struct DualAudioState {
    AudioServerPlugInDriverInterface*  iface_ptr;   // HAL ref = &this->iface_ptr
    AudioServerPlugInDriverInterface   iface;        // vtable

    UInt32                             ref_count;
    pthread_mutex_t                    mutex;

    // I/O state
    UInt32                             io_count;
    UInt32                             buffer_frame_size;

    // Timing (GetZeroTimeStamp)
    UInt64                             start_host_time;
    Float64                            host_ticks_per_frame;

    // Shared memory ring buffer → app AudioEngine
    int                                shm_fd;
    DualAudioRingBuffer*               ring;

    os_log_t                           log;
} DualAudioState;

// ── Entry point (registered as CF plug-in factory) ────────────────────────────
extern AudioServerPlugInDriverRef DualAudio_Create(CFAllocatorRef inAllocator,
                                                    CFUUIDRef inRequestedTypeUUID);
