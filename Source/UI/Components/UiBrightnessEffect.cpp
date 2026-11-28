// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// UI brightness filter and the MainComponent entry point that installs it,
// extracted from MainComponent.cpp without changing its behaviour.

#include "UiBrightnessEffect.h"

#include "../MainComponent.h"

#include <cmath>

void MainComponent::applyUiBrightness(double brightnessPercent)
{
  const double normalized = juce::jlimit(75.0, 200.0, brightnessPercent);
  uiBrightnessEffect->setBrightnessPercent(normalized);
  setComponentEffect(std::abs(normalized - 100.0) < 0.01
                         ? nullptr
                         : uiBrightnessEffect.get());
  repaint();
}
