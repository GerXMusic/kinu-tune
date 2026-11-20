#pragma once

#include "../JuceHeader.h"
#include "MelodyneIcons.h"

/** Basic / Advanced editor modes (gui-decision.md §1).
 *
 * Basic keeps the everyday Melodyne tools; Advanced adds the formant and
 * amplitude tools, the block engine selector and the analysis views. The
 * canvas is the same in both; only the surrounding controls change.
 */
enum class EditorUiMode
{
    Basic = 0,
    Advanced = 1
};

/** The tool row shared by the top bar and the floating tool pill. */
enum class EditorTool
{
    Main = 0,
    Pitch = 1,
    Formant = 2,
    Amplitude = 3,
    Separation = 4,
    Timing = 5
};

inline GuiIcon iconForTool(EditorTool tool)
{
    switch (tool)
    {
    case EditorTool::Pitch:      return GuiIcon::PitchTool;
    case EditorTool::Formant:    return GuiIcon::FormantTool;
    case EditorTool::Amplitude:  return GuiIcon::AmplitudeTool;
    case EditorTool::Separation: return GuiIcon::SeparationTool;
    case EditorTool::Timing:     return GuiIcon::TimingTool;
    case EditorTool::Main:
    default:                     return GuiIcon::MainTool;
    }
}

inline const char* toolLabelKey(EditorTool tool)
{
    switch (tool)
    {
    case EditorTool::Pitch:      return "tool.pitch";
    case EditorTool::Formant:    return "tool.formant";
    case EditorTool::Amplitude:  return "tool.amplitude";
    case EditorTool::Separation: return "tool.separation";
    case EditorTool::Timing:     return "tool.timing";
    case EditorTool::Main:
    default:                     return "tool.main";
    }
}

/** Tools that Basic hides (gui-decision.md §5). */
inline bool isAdvancedOnlyTool(EditorTool tool)
{
    return tool == EditorTool::Formant || tool == EditorTool::Amplitude;
}
