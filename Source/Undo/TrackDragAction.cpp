// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "TrackDragAction.h"

void TrackDragAction::undo() { apply(false); }

void TrackDragAction::redo() { apply(true); }

juce::String TrackDragAction::getName() const { return "Drag Track Notes"; }
