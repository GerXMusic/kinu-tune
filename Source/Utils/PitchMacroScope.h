#pragma once

#include "../Models/Project.h"
#include <algorithm>
#include <vector>

// Clip mode honors the note selection; Track mode covers every voiced note
// intersecting the track's playback windows, once even for overlapping regions.
inline std::vector<Note*> collectPitchMacroNotes(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows)
{
  auto& notes = project.getNotes();
  const bool selected = !trackMode && std::any_of(notes.begin(), notes.end(),
      [](const Note& note) { return !note.isRest() && note.isSelected(); });
  std::vector<Note*> result;
  for (auto& note : notes) {
    const bool inWindow = !trackMode || std::any_of(windows.begin(), windows.end(),
        [&note](juce::Range<int> range) {
          return note.getEndFrame() > range.getStart() && note.getStartFrame() < range.getEnd();
        });
    if (!note.isRest() && inWindow && (!selected || note.isSelected()))
      result.push_back(&note);
  }
  return result;
}

// Notes a pitch macro edits: in the given windows, and only the selected ones
// when onlySelected (the caller decides across all tracks, so a selection in
// one track does not leave another track's notes unselected-but-edited).
inline std::vector<Note*> collectPitchMacroTargetNotes(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows,
    bool onlySelected)
{
  std::vector<Note*> result;
  for (auto* note : collectPitchMacroNotes(project, trackMode, windows))
    if (!onlySelected || note->isSelected())
      result.push_back(note);
  return result;
}

inline bool hasSelectedPitchMacroNotes(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows)
{
  for (auto* note : collectPitchMacroNotes(project, trackMode, windows))
    if (note->isSelected())
      return true;
  return false;
}
