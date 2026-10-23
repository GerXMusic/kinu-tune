#pragma once

#include "../JuceHeader.h"

#include <cstdint>
#include <mutex>

/**
 * Device selection shared by every Vocoder in the plug-in.
 *
 * The plug-in keeps several EditorControllers (the composite analysis, the
 * region on the canvas, one per modification rendered in the background) and
 * each one owns its own Vocoder. The preference used to be written into the
 * single instance the settings dialog happened to hold, while the controller
 * that rendered kept the compile-time default - on Windows that is
 * "DirectML" - so a render could run on the GPU with "CPU" selected.
 *
 * One preference for the process fixes that: every Vocoder reads it when it
 * builds a session, and a change is picked up by the next render.
 *
 * `device` stays empty until someone sets it, so a Vocoder keeps its own
 * default (DirectML, CUDA or CPU depending on the build) until the settings
 * are applied.
 */
struct SharedDevicePreference
{
  std::mutex mutex;
  juce::String device;
  int deviceId = 0;
  std::uint64_t generation = 0;
};

inline SharedDevicePreference &sharedDevicePreference()
{
  static SharedDevicePreference preference;
  return preference;
}
