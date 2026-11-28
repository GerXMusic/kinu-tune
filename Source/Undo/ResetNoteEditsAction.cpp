// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "ResetNoteEditsAction.h"

#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"

#include <algorithm>
#include <limits>

ResetNoteEditsAction::ResetNoteEditsAction(Project& project, std::vector<Note*> notes,
                                           bool pitch, bool formant, bool amplitude,
                                           juce::String name,
                                           std::unique_ptr<UndoableAction> timing)
    : project(project), notes(std::move(notes)), name(std::move(name)),
      timing(std::move(timing))
{
  before.reserve(this->notes.size());
  after.reserve(this->notes.size());
  for (const auto* note : this->notes)
  {
    before.push_back(pianoRollEditState::NoteEditState::capture(*note));
    auto state = pitch ? pianoRollEditState::NoteEditState::defaultsFor(*note)
                       : before.back();
    state.formantShift = formant ? 0.0f : before.back().formantShift;
    state.volumeDb = amplitude ? 0.0f : before.back().volumeDb;
    after.push_back(std::move(state));
  }
}

void ResetNoteEditsAction::undo()
{
  if (timing)
    timing->undo();
  apply(before);
}

void ResetNoteEditsAction::redo()
{
  apply(after);
  if (timing)
    timing->redo();
}

void ResetNoteEditsAction::apply(const std::vector<pianoRollEditState::NoteEditState>& states)
{
  int dirtyStart = std::numeric_limits<int>::max();
  int dirtyEnd = std::numeric_limits<int>::min();
  for (size_t i = 0; i < notes.size() && i < states.size(); ++i)
  {
    if (!notes[i])
      continue;
    states[i].applyTo(*notes[i]);
    dirtyStart = std::min(dirtyStart, notes[i]->getStartFrame());
    dirtyEnd = std::max(dirtyEnd, notes[i]->getEndFrame());
  }
  project.setModified(true);
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  // Same range the edit and its undo mark (Project::markNoteEditDirtyRange).
  project.markNoteEditDirtyRange(dirtyStart, dirtyEnd);
}
