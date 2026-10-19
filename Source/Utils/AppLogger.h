#pragma once

#include "../JuceHeader.h"
#include "PlatformPaths.h"

#ifndef PITCHNET_LOGGING_ENABLED
#define PITCHNET_LOGGING_ENABLED JUCE_DEBUG
#endif

/**
 * Simple file logger for debugging.
 * Logs to the Kinu Tune logs folder (see PlatformPaths::getLogsDirectory)
 */
class AppLogger {
public:
  static void init() {
#if PITCHNET_LOGGING_ENABLED
    (void)getSessionId();
    (void)getLogFile();
#endif
  }

  static void log(const juce::String &message) {
#if PITCHNET_LOGGING_ENABLED
    auto logFile = getLogFile();
    auto timestamp =
        juce::Time::getCurrentTime().toString(true, true, true, true);
    logFile.appendText("[" + timestamp + "] " + message + "\n");
#else
    (void)message;
#endif
  }

  static void clear() {
#if PITCHNET_LOGGING_ENABLED
    auto logFile = getLogFile();
    logFile.deleteFile();
#endif
  }

  static juce::File getLogFile() {
#if PITCHNET_LOGGING_ENABLED
    return PlatformPaths::getLogFile("debug_" + getSessionId() + ".log");
#else
    return {};
#endif
  }

  static juce::String getSessionId() {
    static const juce::String sessionId =
        juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    return sessionId;
  }
};

#if PITCHNET_LOGGING_ENABLED
#define LOG(msg) AppLogger::log(msg)
#else
#define LOG(msg) ((void)0)
#endif
