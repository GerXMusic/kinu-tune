// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "UndoableAction.h"

#include <functional>

// Un arrastre que mueve las notas de varias pistas como un solo paso de undo:
// guarda la lambda que aplica el estado anterior (redo = false) o el posterior
// (redo = true). Movida fuera de PianoRollComponent.cpp, que la definia dentro
// del gesto; el undo y el redo siguen llamando a la misma lambda.
class TrackDragAction final : public UndoableAction
{
public:
  explicit TrackDragAction(std::function<void(bool)> apply)
      : apply(std::move(apply)) {}

  void undo() override;
  void redo() override;
  juce::String getName() const override;
  bool ownsProjectContext() const override { return true; }
  bool requiresAudioResynthesis() const override { return false; }

private:
  std::function<void(bool)> apply;
};
