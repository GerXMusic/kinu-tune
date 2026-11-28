// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// The shared editor tooltip window, extracted from MainComponent.cpp.

#include "EditorTooltip.h"

#include "../Components/DarkLookAndFeel.h"
#include "../MainComponent.h"

namespace
{
class EditorTooltipWindow final : public juce::TooltipWindow
{
public:
  juce::String getTipFor(juce::Component &component) override
  {
    for (auto *ancestor = &component; ancestor != nullptr;
         ancestor = ancestor->getParentComponent())
      if (dynamic_cast<MainComponent *>(ancestor) != nullptr)
        return juce::TooltipWindow::getTipFor(component);

    return {};
  }
};
} // namespace

std::shared_ptr<juce::TooltipWindow> getEditorTooltipWindow()
{
  // Share one native window across plugin instances in the host process.
  static std::weak_ptr<juce::TooltipWindow> sharedWindow;
  auto window = sharedWindow.lock();
  if (window == nullptr)
  {
    window = std::make_shared<EditorTooltipWindow>();
    window->setLookAndFeel(&DarkLookAndFeel::getInstance());
    sharedWindow = window;
  }
  return window;
}
