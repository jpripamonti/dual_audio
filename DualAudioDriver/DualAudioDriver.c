// DualAudioDriver.c
// HAL AudioServerPlugin – exposes "Dual Audio" as a virtual output device.
// Phase 1: device visible in Sound preferences; audio callbacks are no-ops.
// Phase 2+ will add real output routing via shared ring buffer.

#include "DualAudioDriver.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

#define LOG_SUBSYSTEM "com.dualaudio.DualAudioDriver"
#define LOG_CATEGORY  "driver"

#define DA_LOG(state, fmt, ...) \
    os_log((state)->log, "[DualAudio] " fmt, ##__VA_ARGS__)

#define DA_LOCK(state)   pthread_mutex_lock(&(state)->mutex)
#define DA_UNLOCK(state) pthread_mutex_unlock(&(state)->mutex)

// inDriver == &state->iface_ptr because iface_ptr is the first field
static inline DualAudioState* GetState(AudioServerPlugInDriverRef ref)
{
    return (DualAudioState*)ref;
}

static const char* DualAudio_ObjectLabel(AudioObjectID objectID)
{
    switch (objectID) {
        case kPlugin_ObjectID: return "plugin";
        case kDevice_ObjectID: return "device";
        case kStream_ObjectID: return "stream";
        default: return "unknown";
    }
}

static void DualAudio_FourCCString(UInt32 value, char out[5])
{
    out[0] = (char)((value >> 24) & 0xFF);
    out[1] = (char)((value >> 16) & 0xFF);
    out[2] = (char)((value >> 8) & 0xFF);
    out[3] = (char)(value & 0xFF);

    for (int i = 0; i < 4; ++i) {
        if (out[i] < 32 || out[i] > 126) {
            out[i] = '?';
        }
    }
    out[4] = '\0';
}

static void DualAudio_LogUnknownProperty(DualAudioState* state,
                                          const char* operation,
                                          AudioObjectID objectID,
                                          const AudioObjectPropertyAddress* address)
{
    char selector[5];
    char scope[5];
    DualAudio_FourCCString(address->mSelector, selector);
    DualAudio_FourCCString(address->mScope, scope);
    DA_LOG(state, "%s unsupported property object=%s(%u) selector='%s' scope='%s' element=%u",
           operation,
           DualAudio_ObjectLabel(objectID),
           objectID,
           selector,
           scope,
           (unsigned)address->mElement);
}

static bool DualAudio_QualifierMatchesClass(UInt32 qualifierDataSize,
                                             const void* qualifierData,
                                             AudioClassID objectClass,
                                             AudioClassID baseClass)
{
    if (qualifierDataSize == 0 || qualifierData == NULL) {
        return true;
    }

    const AudioClassID* classIDs = (const AudioClassID*)qualifierData;
    UInt32 count = qualifierDataSize / (UInt32)sizeof(AudioClassID);
    for (UInt32 i = 0; i < count; ++i) {
        AudioClassID candidate = classIDs[i];
        if (candidate == objectClass ||
            candidate == baseClass ||
            candidate == kAudioObjectClassID) {
            return true;
        }
    }
    return false;
}

// ── Forward declarations ──────────────────────────────────────────────────────

static HRESULT  DualAudio_QueryInterface(void*, REFIID, LPVOID*);
static ULONG    DualAudio_AddRef(void*);
static ULONG    DualAudio_Release(void*);

static OSStatus DualAudio_Initialize(
    AudioServerPlugInDriverRef, AudioServerPlugInHostRef);

static OSStatus DualAudio_CreateDevice(
    AudioServerPlugInDriverRef, CFDictionaryRef,
    const AudioServerPlugInClientInfo*, AudioObjectID*);

static OSStatus DualAudio_DestroyDevice(
    AudioServerPlugInDriverRef, AudioObjectID);

static OSStatus DualAudio_AddDeviceClient(
    AudioServerPlugInDriverRef, AudioObjectID,
    const AudioServerPlugInClientInfo*);

static OSStatus DualAudio_RemoveDeviceClient(
    AudioServerPlugInDriverRef, AudioObjectID,
    const AudioServerPlugInClientInfo*);

static OSStatus DualAudio_PerformDeviceConfigurationChange(
    AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);

static OSStatus DualAudio_AbortDeviceConfigurationChange(
    AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*);

static Boolean  DualAudio_HasProperty(
    AudioServerPlugInDriverRef, AudioObjectID,
    pid_t, const AudioObjectPropertyAddress*);

static OSStatus DualAudio_IsPropertySettable(
    AudioServerPlugInDriverRef, AudioObjectID,
    pid_t, const AudioObjectPropertyAddress*, Boolean*);

static OSStatus DualAudio_GetPropertyDataSize(
    AudioServerPlugInDriverRef, AudioObjectID,
    pid_t, const AudioObjectPropertyAddress*,
    UInt32, const void*, UInt32*);

static OSStatus DualAudio_GetPropertyData(
    AudioServerPlugInDriverRef, AudioObjectID,
    pid_t, const AudioObjectPropertyAddress*,
    UInt32, const void*, UInt32, UInt32*, void*);

static OSStatus DualAudio_SetPropertyData(
    AudioServerPlugInDriverRef, AudioObjectID,
    pid_t, const AudioObjectPropertyAddress*,
    UInt32, const void*, UInt32, const void*);

static OSStatus DualAudio_StartIO(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32);

static OSStatus DualAudio_StopIO(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32);

static OSStatus DualAudio_GetZeroTimeStamp(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32,
    Float64*, UInt64*, UInt64*);

static OSStatus DualAudio_WillDoIOOperation(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32,
    UInt32, Boolean*, Boolean*);

static OSStatus DualAudio_BeginIOOperation(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32,
    UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

static OSStatus DualAudio_DoIOOperation(
    AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID,
    UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*,
    void*, void*);

static OSStatus DualAudio_EndIOOperation(
    AudioServerPlugInDriverRef, AudioObjectID, UInt32,
    UInt32, UInt32, const AudioServerPlugInIOCycleInfo*);

// ── Global singleton ──────────────────────────────────────────────────────────

static DualAudioState gState;
static Boolean        gStateInitialized = false;

// ── ASBD helper ───────────────────────────────────────────────────────────────

static AudioStreamBasicDescription DualAudio_ASBD(void)
{
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = kDevice_SampleRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
    asbd.mBitsPerChannel   = kDevice_BitsPerChannel;
    asbd.mChannelsPerFrame = kDevice_ChannelsPerFrame;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = kDevice_BytesPerFrame;
    asbd.mBytesPerPacket   = kDevice_BytesPerPacket;
    return asbd;
}

// ── Entry point ───────────────────────────────────────────────────────────────

AudioServerPlugInDriverRef DualAudio_Create(CFAllocatorRef inAllocator,
                                             CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;

    // kAudioServerPlugInTypeUUID expands to a CFUUIDRef constant
    if (!CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return NULL;

    if (gStateInitialized) {
        DualAudio_AddRef((void*)&gState);
        return &gState.iface_ptr;
    }

    memset(&gState, 0, sizeof(gState));

    // Build vtable
    gState.iface.QueryInterface                   = DualAudio_QueryInterface;
    gState.iface.AddRef                           = DualAudio_AddRef;
    gState.iface.Release                          = DualAudio_Release;
    gState.iface.Initialize                       = DualAudio_Initialize;
    gState.iface.CreateDevice                     = DualAudio_CreateDevice;
    gState.iface.DestroyDevice                    = DualAudio_DestroyDevice;
    gState.iface.AddDeviceClient                  = DualAudio_AddDeviceClient;
    gState.iface.RemoveDeviceClient               = DualAudio_RemoveDeviceClient;
    gState.iface.PerformDeviceConfigurationChange = DualAudio_PerformDeviceConfigurationChange;
    gState.iface.AbortDeviceConfigurationChange   = DualAudio_AbortDeviceConfigurationChange;
    gState.iface.HasProperty                      = DualAudio_HasProperty;
    gState.iface.IsPropertySettable               = DualAudio_IsPropertySettable;
    gState.iface.GetPropertyDataSize              = DualAudio_GetPropertyDataSize;
    gState.iface.GetPropertyData                  = DualAudio_GetPropertyData;
    gState.iface.SetPropertyData                  = DualAudio_SetPropertyData;
    gState.iface.StartIO                          = DualAudio_StartIO;
    gState.iface.StopIO                           = DualAudio_StopIO;
    gState.iface.GetZeroTimeStamp                 = DualAudio_GetZeroTimeStamp;
    gState.iface.WillDoIOOperation                = DualAudio_WillDoIOOperation;
    gState.iface.BeginIOOperation                 = DualAudio_BeginIOOperation;
    gState.iface.DoIOOperation                    = DualAudio_DoIOOperation;
    gState.iface.EndIOOperation                   = DualAudio_EndIOOperation;

    // iface_ptr is the first field → &gState == &gState.iface_ptr
    gState.iface_ptr         = &gState.iface;
    gState.ref_count         = 1;
    gState.buffer_frame_size = kDevice_DefaultBufferSize;
    gState.io_count          = 0;
    gState.log               = os_log_create(LOG_SUBSYSTEM, LOG_CATEGORY);

    pthread_mutex_init(&gState.mutex, NULL);
    gStateInitialized = true;

    os_log(gState.log, "[DualAudio] driver created");
    return &gState.iface_ptr;
}

// ── IUnknown ──────────────────────────────────────────────────────────────────

static HRESULT DualAudio_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (!inDriver || !outInterface) return E_POINTER;

    // REFIID is CFUUIDBytes (a value type) – create a temporary CFUUIDRef for comparison
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, inUUID);

    // kAudioServerPlugInDriverInterfaceUUID and IUnknownUUID are already CFUUIDRef constants
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID) ||
        CFEqual(uuid, IUnknownUUID)) {
        DualAudio_AddRef(inDriver);
        *outInterface = inDriver;
        result = S_OK;
    }

    CFRelease(uuid);
    return result;
}

static ULONG DualAudio_AddRef(void* inDriver)
{
    DualAudioState* state = GetState(inDriver);
    DA_LOCK(state);
    ++state->ref_count;
    ULONG rc = state->ref_count;
    DA_UNLOCK(state);
    return rc;
}

static ULONG DualAudio_Release(void* inDriver)
{
    DualAudioState* state = GetState(inDriver);
    DA_LOCK(state);
    UInt32 rc = --state->ref_count;
    DA_UNLOCK(state);
    if (rc == 0) {
        os_log(state->log, "[DualAudio] driver released (ref=0)");
    }
    return rc;
}

// ── Initialize ────────────────────────────────────────────────────────────────

static OSStatus DualAudio_Initialize(AudioServerPlugInDriverRef inDriver,
                                      AudioServerPlugInHostRef inHost)
{
    (void)inHost;
    DualAudioState* state = GetState(inDriver);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double nanos_per_frame = 1e9 / kDevice_SampleRate;
    state->host_ticks_per_frame = nanos_per_frame * ((double)tb.denom / (double)tb.numer);
    state->start_host_time = mach_absolute_time();

    // Open (or create) the shared memory ring buffer
    int fd = shm_open(DUAL_AUDIO_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        DA_LOG(state, "shm_open failed – audio will not be forwarded to app");
    } else {
        ftruncate(fd, (off_t)sizeof(DualAudioRingBuffer));
        state->ring = (DualAudioRingBuffer*)mmap(
            NULL, sizeof(DualAudioRingBuffer),
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (state->ring == MAP_FAILED) {
            DA_LOG(state, "mmap failed errno=%d", errno);
            state->ring = NULL;
        }
        state->shm_fd = fd;
        DA_LOG(state, "ring buffer mapped (%zu bytes)", sizeof(DualAudioRingBuffer));
    }

    DA_LOG(state, "initialized – %.0f Hz %u-ch %u-bit buf=%u",
           kDevice_SampleRate, kDevice_ChannelsPerFrame,
           kDevice_BitsPerChannel, state->buffer_frame_size);
    return noErr;
}

// ── Device management ─────────────────────────────────────────────────────────

static OSStatus DualAudio_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                        CFDictionaryRef inDescription,
                                        const AudioServerPlugInClientInfo* inClientInfo,
                                        AudioObjectID* outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DualAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DualAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                           AudioObjectID inDeviceObjectID,
                                           const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDeviceObjectID;
    DualAudioState* state = GetState(inDriver);
    DA_LOG(state, "client added id=%u", inClientInfo->mClientID);
    return noErr;
}

static OSStatus DualAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                              AudioObjectID inDeviceObjectID,
                                              const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inDeviceObjectID;
    DualAudioState* state = GetState(inDriver);
    DA_LOG(state, "client removed id=%u", inClientInfo->mClientID);
    return noErr;
}

static OSStatus DualAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                            AudioObjectID inDeviceObjectID,
                                                            UInt64 inChangeAction,
                                                            void* inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static OSStatus DualAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                          AudioObjectID inDeviceObjectID,
                                                          UInt64 inChangeAction,
                                                          void* inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

// ── Property helpers ──────────────────────────────────────────────────────────

static Boolean IsPluginProperty(const AudioObjectPropertyAddress* addr)
{
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyTranslateUIDToBox:
            return true;
    }
    return false;
}

static Boolean IsDeviceProperty(const AudioObjectPropertyAddress* addr)
{
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyConfigurationApplication:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyBufferFrameSizeRange:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyStreamConfiguration:
        case kAudioDevicePropertyIcon:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioDevicePropertyUsesVariableBufferFrameSizes:
            return true;
    }
    return false;
}

static Boolean IsStreamProperty(const AudioObjectPropertyAddress* addr)
{
    switch (addr->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyModelName:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyControlList:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

// ── HasProperty ───────────────────────────────────────────────────────────────

static Boolean DualAudio_HasProperty(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inObjectID,
                                      pid_t inClientProcessID,
                                      const AudioObjectPropertyAddress* inAddress)
{
    DualAudioState* state = GetState(inDriver);
    (void)inClientProcessID;

    Boolean hasProperty = false;
    switch (inObjectID) {
        case kPlugin_ObjectID:
            hasProperty = IsPluginProperty(inAddress);
            break;
        case kDevice_ObjectID:
            hasProperty = IsDeviceProperty(inAddress);
            break;
        case kStream_ObjectID:
            hasProperty = IsStreamProperty(inAddress);
            break;
        default:
            hasProperty = false;
            break;
    }

    if (!hasProperty) {
        DualAudio_LogUnknownProperty(state, "has", inObjectID, inAddress);
    }
    return hasProperty;
}

// ── IsPropertySettable ────────────────────────────────────────────────────────

static OSStatus DualAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                              AudioObjectID inObjectID,
                                              pid_t inClientProcessID,
                                              const AudioObjectPropertyAddress* inAddress,
                                              Boolean* outIsSettable)
{
    (void)inClientProcessID;
    if (!DualAudio_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress))
        return kAudioHardwareUnknownPropertyError;

    *outIsSettable = false;
    if (inObjectID == kDevice_ObjectID &&
        (inAddress->mSelector == kAudioDevicePropertyBufferFrameSize ||
         inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)) {
        *outIsSettable = true;
    }
    if (inObjectID == kStream_ObjectID &&
        (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
         inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        *outIsSettable = true;
    }
    return noErr;
}

// ── GetPropertyDataSize ───────────────────────────────────────────────────────

static OSStatus DualAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inObjectID,
                                               pid_t inClientProcessID,
                                               const AudioObjectPropertyAddress* inAddress,
                                               UInt32 inQualifierDataSize,
                                               const void* inQualifierData,
                                               UInt32* outDataSize)
{
    DualAudioState* state = GetState(inDriver);
    (void)inClientProcessID;

    switch (inObjectID) {

    case kPlugin_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyResourceBundle:
                *outDataSize = sizeof(CFStringRef); return noErr;
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyTranslateUIDToBox:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioPlugInPropertyDeviceList:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = DualAudio_QualifierMatchesClass(inQualifierDataSize, inQualifierData,
                                                               kAudioDeviceClassID, kAudioObjectClassID)
                               ? sizeof(AudioObjectID)
                               : 0;
                return noErr;
            case kAudioPlugInPropertyBoxList:
                *outDataSize = 0; return noErr;
            default:
                DualAudio_LogUnknownProperty(state, "size", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    case kDevice_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyConfigurationApplication:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef); return noErr;
            case kAudioDevicePropertyIcon:
                *outDataSize = sizeof(CFURLRef); return noErr;
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyBufferFrameSize:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyUsesVariableBufferFrameSizes:
                *outDataSize = sizeof(UInt32); return noErr;
            case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64); return noErr;
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyBufferFrameSizeRange:
                *outDataSize = sizeof(AudioValueRange); return noErr;
            case kAudioDevicePropertyRelatedDevices:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = DualAudio_QualifierMatchesClass(inQualifierDataSize, inQualifierData,
                                                               kAudioStreamClassID, kAudioObjectClassID)
                               ? sizeof(AudioObjectID)
                               : 0;
                return noErr;
            case kAudioDevicePropertyStreams:
                *outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeInput)
                               ? 0 : sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return noErr;
            case kAudioDevicePropertyStreamConfiguration:
                *outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeInput)
                    ? (UInt32)offsetof(AudioBufferList, mBuffers)
                    : (UInt32)(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer));
                return noErr;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                *outDataSize = 2 * sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyPreferredChannelLayout:
                *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                                + kDevice_ChannelsPerFrame * sizeof(AudioChannelDescription));
                return noErr;
            default:
                DualAudio_LogUnknownProperty(state, "size", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    case kStream_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioObjectID); return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
                *outDataSize = sizeof(CFStringRef); return noErr;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0; return noErr;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0; return noErr;
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
                *outDataSize = sizeof(UInt32); return noErr;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                *outDataSize = sizeof(AudioStreamRangedDescription); return noErr;
            default:
                DualAudio_LogUnknownProperty(state, "size", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    default:
        DualAudio_LogUnknownProperty(state, "size", inObjectID, inAddress);
        return kAudioHardwareBadObjectError;
    }
}

// ── GetPropertyData ───────────────────────────────────────────────────────────

static OSStatus DualAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                           AudioObjectID inObjectID,
                                           pid_t inClientProcessID,
                                           const AudioObjectPropertyAddress* inAddress,
                                           UInt32 inQualifierDataSize,
                                           const void* inQualifierData,
                                           UInt32 inDataSize,
                                           UInt32* outDataSize,
                                           void* outData)
{
    DualAudioState* state = GetState(inDriver);
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    (void)inDataSize; // used for bounds checking; we guard via outDataSize

    switch (inObjectID) {

    // ── Plugin ────────────────────────────────────────────────────────────────
    case kPlugin_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioPlugInClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kAudioObjectSystemObject;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = CFSTR("DualAudio Plugin");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyModelName:
                *(CFStringRef*)outData = CFSTR("DualAudio Plugin");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFSTR(kDevice_Manufacturer);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioPlugInPropertyResourceBundle:
                *(CFStringRef*)outData = CFSTR("");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioPlugInPropertyDeviceList:
                *(AudioObjectID*)outData = kDevice_ObjectID;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyOwnedObjects:
                if (DualAudio_QualifierMatchesClass(inQualifierDataSize, inQualifierData,
                                                    kAudioDeviceClassID, kAudioObjectClassID)) {
                    *(AudioObjectID*)outData = kDevice_ObjectID;
                    *outDataSize = sizeof(AudioObjectID);
                } else {
                    *outDataSize = 0;
                }
                return noErr;
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                AudioObjectID result = kAudioObjectUnknown;
                if (inQualifierDataSize >= sizeof(CFStringRef)) {
                    CFStringRef uid = *(CFStringRef*)inQualifierData;
                    if (uid && CFStringCompare(uid, CFSTR(kDevice_UID), 0)
                            == kCFCompareEqualTo) {
                        result = kDevice_ObjectID;
                    }
                }
                *(AudioObjectID*)outData = result;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            }
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyTranslateUIDToBox:
                *outDataSize = 0;
                return noErr;
            default:
                DualAudio_LogUnknownProperty(state, "get", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    // ── Device ────────────────────────────────────────────────────────────────
    case kDevice_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioDeviceClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kPlugin_ObjectID;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = CFSTR(kDevice_Name);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyModelName:
                *(CFStringRef*)outData = CFSTR(kDevice_Name);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                *(CFStringRef*)outData = CFSTR(kDevice_Manufacturer);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioDevicePropertyConfigurationApplication:
                *(CFStringRef*)outData = CFSTR("com.dualaudio.DualAudio");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioDevicePropertyIcon:
                *(CFURLRef*)outData = NULL;
                *outDataSize = sizeof(CFURLRef);
                return noErr;
            case kAudioDevicePropertyDeviceUID:
                *(CFStringRef*)outData = CFSTR(kDevice_UID);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioDevicePropertyModelUID:
                *(CFStringRef*)outData = CFSTR(kDevice_ModelUID);
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioDevicePropertyTransportType:
                *(UInt32*)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyRelatedDevices:
                *(AudioObjectID*)outData = kDevice_ObjectID;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            case kAudioDevicePropertyClockDomain:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyDeviceIsAlive:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyDeviceIsRunning:
                DA_LOCK(state);
                *(UInt32*)outData = (state->io_count > 0) ? 1 : 0;
                DA_UNLOCK(state);
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                *(UInt32*)outData =
                    (inAddress->mScope == kAudioObjectPropertyScopeOutput ||
                     inAddress->mScope == kAudioObjectPropertyScopeGlobal) ? 1 : 0;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyUsesVariableBufferFrameSizes:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyBufferFrameSize:
                DA_LOCK(state);
                *(UInt32*)outData = state->buffer_frame_size;
                DA_UNLOCK(state);
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioDevicePropertyBufferFrameSizeRange: {
                AudioValueRange* r = (AudioValueRange*)outData;
                r->mMinimum = 1;
                r->mMaximum = 131072;
                *outDataSize = sizeof(AudioValueRange);
                return noErr;
            }
            case kAudioDevicePropertyNominalSampleRate:
                *(Float64*)outData = kDevice_SampleRate;
                *outDataSize = sizeof(Float64);
                return noErr;
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange* r = (AudioValueRange*)outData;
                r->mMinimum = r->mMaximum = kDevice_SampleRate;
                *outDataSize = sizeof(AudioValueRange);
                return noErr;
            }
            case kAudioDevicePropertyZeroTimeStampPeriod:
                *(UInt32*)outData = kDevice_ZeroTimePeriod;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioObjectPropertyOwnedObjects:
                if (DualAudio_QualifierMatchesClass(inQualifierDataSize, inQualifierData,
                                                    kAudioStreamClassID, kAudioObjectClassID)) {
                    *(AudioObjectID*)outData = kStream_ObjectID;
                    *outDataSize = sizeof(AudioObjectID);
                } else {
                    *outDataSize = 0;
                }
                return noErr;
            case kAudioDevicePropertyStreams:
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    *(AudioObjectID*)outData = kStream_ObjectID;
                    *outDataSize = sizeof(AudioObjectID);
                }
                return noErr;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return noErr;
            case kAudioDevicePropertyStreamConfiguration: {
                AudioBufferList* abl = (AudioBufferList*)outData;
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    abl->mNumberBuffers = 0;
                    *outDataSize = (UInt32)offsetof(AudioBufferList, mBuffers);
                } else {
                    abl->mNumberBuffers = 1;
                    DA_LOCK(state);
                    abl->mBuffers[0].mNumberChannels = kDevice_ChannelsPerFrame;
                    abl->mBuffers[0].mDataByteSize =
                        state->buffer_frame_size * kDevice_BytesPerFrame;
                    abl->mBuffers[0].mData = NULL;
                    DA_UNLOCK(state);
                    *outDataSize = (UInt32)(offsetof(AudioBufferList, mBuffers)
                                            + sizeof(AudioBuffer));
                }
                return noErr;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                UInt32* channels = (UInt32*)outData;
                channels[0] = 1;
                channels[1] = 2;
                *outDataSize = 2 * sizeof(UInt32);
                return noErr;
            }
            case kAudioDevicePropertyPreferredChannelLayout: {
                AudioChannelLayout* acl = (AudioChannelLayout*)outData;
                acl->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                acl->mChannelBitmap    = 0;
                acl->mNumberChannelDescriptions = kDevice_ChannelsPerFrame;
                acl->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                acl->mChannelDescriptions[0].mChannelFlags = 0;
                memset(acl->mChannelDescriptions[0].mCoordinates, 0,
                       sizeof(acl->mChannelDescriptions[0].mCoordinates));
                acl->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                acl->mChannelDescriptions[1].mChannelFlags = 0;
                memset(acl->mChannelDescriptions[1].mCoordinates, 0,
                       sizeof(acl->mChannelDescriptions[1].mCoordinates));
                *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                               + kDevice_ChannelsPerFrame * sizeof(AudioChannelDescription));
                return noErr;
            }
            default:
                DualAudio_LogUnknownProperty(state, "get", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    // ── Stream ────────────────────────────────────────────────────────────────
    case kStream_ObjectID:
        switch (inAddress->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = kAudioStreamClassID;
                *outDataSize = sizeof(AudioClassID); return noErr;
            case kAudioObjectPropertyOwner:
                *(AudioObjectID*)outData = kDevice_ObjectID;
                *outDataSize = sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = CFSTR("Dual Audio Output");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyModelName:
                *(CFStringRef*)outData = CFSTR("Dual Audio Output");
                CFRetain(*(CFStringRef*)outData);
                *outDataSize = sizeof(CFStringRef);
                return noErr;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return noErr;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return noErr;
            case kAudioStreamPropertyIsActive:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioStreamPropertyDirection:
                *(UInt32*)outData = 0; // output
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioStreamPropertyTerminalType:
                *(UInt32*)outData = kAudioStreamTerminalTypeLine;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioStreamPropertyStartingChannel:
                *(UInt32*)outData = 1;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioStreamPropertyLatency:
                *(UInt32*)outData = 0;
                *outDataSize = sizeof(UInt32);
                return noErr;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *(AudioStreamBasicDescription*)outData = DualAudio_ASBD();
                *outDataSize = sizeof(AudioStreamBasicDescription);
                return noErr;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* d = (AudioStreamRangedDescription*)outData;
                d->mFormat = DualAudio_ASBD();
                d->mSampleRateRange.mMinimum = kDevice_SampleRate;
                d->mSampleRateRange.mMaximum = kDevice_SampleRate;
                *outDataSize = sizeof(AudioStreamRangedDescription);
                return noErr;
            }
            default:
                DualAudio_LogUnknownProperty(state, "get", inObjectID, inAddress);
                return kAudioHardwareUnknownPropertyError;
        }

    default:
        DualAudio_LogUnknownProperty(state, "get", inObjectID, inAddress);
        return kAudioHardwareBadObjectError;
    }
}

// ── SetPropertyData ───────────────────────────────────────────────────────────

static OSStatus DualAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                           AudioObjectID inObjectID,
                                           pid_t inClientProcessID,
                                           const AudioObjectPropertyAddress* inAddress,
                                           UInt32 inQualifierDataSize,
                                           const void* inQualifierData,
                                           UInt32 inDataSize,
                                           const void* inData)
{
    DualAudioState* state = GetState(inDriver);
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    (void)inDataSize;

    if (inObjectID == kDevice_ObjectID &&
        inAddress->mSelector == kAudioDevicePropertyBufferFrameSize) {
        UInt32 newSize = *(const UInt32*)inData;
        if (newSize == 0) return kAudioHardwareIllegalOperationError;
        DA_LOCK(state);
        state->buffer_frame_size = newSize;
        DA_UNLOCK(state);
        DA_LOG(state, "buffer frame size → %u", newSize);
        return noErr;
    }
    // Accept (but ignore) format and sample rate changes – we support only one format
    if (inObjectID == kDevice_ObjectID &&
        inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        return noErr;
    }
    if (inObjectID == kStream_ObjectID &&
        (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
         inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        return noErr;
    }
    DualAudio_LogUnknownProperty(state, "set", inObjectID, inAddress);
    return kAudioHardwareUnsupportedOperationError;
}

// ── I/O ───────────────────────────────────────────────────────────────────────

static OSStatus DualAudio_StartIO(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID,
                                   UInt32 inClientID)
{
    (void)inDeviceObjectID; (void)inClientID;
    DualAudioState* state = GetState(inDriver);
    DA_LOCK(state);
    if (state->io_count == 0) {
        state->start_host_time = mach_absolute_time();
        DA_LOG(state, "IO started");
    }
    ++state->io_count;
    DA_UNLOCK(state);
    return noErr;
}

static OSStatus DualAudio_StopIO(AudioServerPlugInDriverRef inDriver,
                                  AudioObjectID inDeviceObjectID,
                                  UInt32 inClientID)
{
    (void)inDeviceObjectID; (void)inClientID;
    DualAudioState* state = GetState(inDriver);
    DA_LOCK(state);
    if (state->io_count > 0 && --state->io_count == 0) {
        DA_LOG(state, "IO stopped");
    }
    DA_UNLOCK(state);
    return noErr;
}

static OSStatus DualAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inDeviceObjectID,
                                            UInt32 inClientID,
                                            Float64* outSampleTime,
                                            UInt64*  outHostTime,
                                            UInt64*  outSeed)
{
    (void)inDeviceObjectID; (void)inClientID;
    DualAudioState* state = GetState(inDriver);

    DA_LOCK(state);
    UInt64  startTime      = state->start_host_time;
    Float64 ticksPerFrame  = state->host_ticks_per_frame;
    DA_UNLOCK(state);

    UInt64  now     = mach_absolute_time();
    UInt64  elapsed = (now >= startTime) ? (now - startTime) : 0;
    Float64 frames  = (Float64)elapsed / ticksPerFrame;
    UInt64  period  = (UInt64)(frames / kDevice_ZeroTimePeriod);

    *outSampleTime = (Float64)(period * kDevice_ZeroTimePeriod);
    *outHostTime   = startTime + (UInt64)(*outSampleTime * ticksPerFrame);
    *outSeed       = 1;
    return noErr;
}

static OSStatus DualAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                             AudioObjectID inDeviceObjectID,
                                             UInt32 inClientID,
                                             UInt32 inOperationID,
                                             Boolean* outWillDo,
                                             Boolean* outWillDoInPlace)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    *outWillDo        = (inOperationID == kAudioServerPlugInIOOperationWriteMix);
    *outWillDoInPlace = true;
    return noErr;
}

static OSStatus DualAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inDeviceObjectID,
                                            UInt32 inClientID,
                                            UInt32 inOperationID,
                                            UInt32 inIOBufferFrameSize,
                                            const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus DualAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inDeviceObjectID,
                                         AudioObjectID inStreamObjectID,
                                         UInt32 inClientID,
                                         UInt32 inOperationID,
                                         UInt32 inIOBufferFrameSize,
                                         const AudioServerPlugInIOCycleInfo* inIOCycleInfo,
                                         void* ioMainBuffer,
                                         void* ioSecondaryBuffer)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inStreamObjectID; (void)inClientID;
    (void)inIOCycleInfo; (void)ioSecondaryBuffer;

    // Forward mixed audio to the app via shared memory ring buffer.
    // ioMainBuffer contains the interleaved float32 stereo mix from all clients.
    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && ioMainBuffer) {
        DualAudioState* state = GetState(inDriver);
        if (state->ring) {
            DualAudioRingBuffer_write(state->ring,
                                      (const float*)ioMainBuffer,
                                      inIOBufferFrameSize);
        }
    }
    return noErr;
}

static OSStatus DualAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                          AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID,
                                          UInt32 inOperationID,
                                          UInt32 inIOBufferFrameSize,
                                          const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}
