#pragma once
#include <CoreAudio/CoreAudio.h>

// Opaque handle
typedef struct AudioBridge* AudioBridgeRef;

// Create a bridge that reads from the shared ring buffer and plays to primaryDeviceID.
// If secondaryDeviceID != kAudioObjectUnknown, the same audio is mirrored to that
// device via an in-process fan-out ring.  Returns NULL on failure.
AudioBridgeRef AudioBridgeCreate(AudioDeviceID primaryDeviceID,
                                  AudioDeviceID secondaryDeviceID);

// Set volumes (0.0 to 1.0 or higher for gain)
void AudioBridgeSetVolumes(AudioBridgeRef bridge, float primaryVol, float secondaryVol);

// Set manual delay offset in sample frames (max ~88200 frames)
void AudioBridgeSetDelays(AudioBridgeRef bridge, uint32_t primaryDelayFrames, uint32_t secondaryDelayFrames);

// Start audio playback. Returns noErr (0) on success.
OSStatus AudioBridgeStart(AudioBridgeRef bridge);

// Stop audio playback.
void AudioBridgeStop(AudioBridgeRef bridge);

// Stop (if running) and free all resources.
void AudioBridgeDestroy(AudioBridgeRef bridge);
