// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"

#include <memory>

// One tooltip window shared by every editor in the host process, so a tip can
// extend beyond its own native window. Extracted from MainComponent.cpp.
std::shared_ptr<juce::TooltipWindow> getEditorTooltipWindow();
