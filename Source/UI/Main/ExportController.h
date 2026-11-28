// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"

class MainComponent;

// Audio/MIDI export dialogs and the export itself. Moved out of
// MainComponent.cpp without changing the dialogs, the settings or the output.
class ExportController
{
public:
  explicit ExportController(MainComponent &owner) : owner(owner) {}

  void exportFile();
  void exportMidiFile();

private:
  MainComponent &owner;

  JUCE_DECLARE_NON_COPYABLE(ExportController)
};
