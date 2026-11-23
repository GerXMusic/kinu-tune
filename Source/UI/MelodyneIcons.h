#pragma once

#include "../JuceHeader.h"

/** Vector icons for the Melodyne-style editor chrome.
 *
 * They are drawn as paths instead of bitmaps so the rail, the top bar and the
 * floating tool pill stay crisp at every scale and in every host zoom. The
 * set is intentionally small: it covers the Melodyne tool row and the SynthV
 * rail, nothing else.
 */
enum class GuiIcon
{
    Notes,
    Engine,
    Tracks,
    Harmony,
    MidiGuide,
    Settings,
    MainTool,
    PitchTool,
    FormantTool,
    AmplitudeTool,
    SeparationTool,
    TimingTool,
    Menu,
    Close,
    ChevronDown,
    Play,
    Stop,
    Loop,
    Undo,
    Redo,
    Record,
    Audition,
    ChevronLeft,
    ChevronRight
};

void drawGuiIcon(juce::Graphics& g, GuiIcon icon,
                 juce::Rectangle<float> bounds, juce::Colour colour,
                 float strokeWidth = 1.6f);
