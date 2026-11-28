// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "UndoableAction.h"
#include "../UI/PianoRoll/States/NoteEditState.h"

#include <memory>
#include <vector>

class Project;
class Note;

// "Restablecer" de una nota: guarda el estado completo (NoteEditState) de cada
// nota antes y despues, y deja que el timing lo aporte una accion aparte.
// Movida fuera de PianoRollComponent.cpp sin cambiar que se restaura ni el undo.
class ResetNoteEditsAction final : public UndoableAction
{
public:
  ResetNoteEditsAction(Project& project, std::vector<Note*> notes,
                       bool pitch, bool formant, bool amplitude,
                       juce::String name, std::unique_ptr<UndoableAction> timing = {});

  void undo() override;
  void redo() override;
  juce::String getName() const override { return name; }

private:
  void apply(const std::vector<pianoRollEditState::NoteEditState>& states);

  Project& project;
  std::vector<Note*> notes;
  std::vector<pianoRollEditState::NoteEditState> before;
  std::vector<pianoRollEditState::NoteEditState> after;
  juce::String name;
  std::unique_ptr<UndoableAction> timing;
};
