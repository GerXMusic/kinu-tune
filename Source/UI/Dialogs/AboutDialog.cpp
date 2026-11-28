// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
// The "About" window (third-party notices), extracted from MainComponent.cpp
// without changing its text or behaviour.

#include "AboutDialog.h"

#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "BinaryData.h"
#include "../../Utils/Localization.h"
#include "../../Utils/UI/Theme.h"

namespace
{
constexpr float aboutNoticesFontSize = 12.0f;
const juce::Colour updateLogScrollbarTrack(0xFF0D0B0Bu);
const juce::Colour updateLogScrollbarThumb(0xFF565656u);

void styleUpdateButton(juce::TextButton &button)
{
  button.setLookAndFeel(&DarkLookAndFeel::getInstance());
  button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF5B5B5Bu));
  button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3E3E3Eu));
  button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFEFEFEFu));
  button.setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFEFEFEFu));
  button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void styleUpdateLogScrollBars(juce::Component &component)
{
  if (auto *scrollBar = dynamic_cast<juce::ScrollBar *>(&component))
  {
    scrollBar->setLookAndFeel(&DarkLookAndFeel::getInstance());
    scrollBar->setColour(juce::ScrollBar::backgroundColourId, updateLogScrollbarTrack);
    scrollBar->setColour(juce::ScrollBar::trackColourId, updateLogScrollbarTrack);
    scrollBar->setColour(juce::ScrollBar::thumbColourId, updateLogScrollbarThumb);
  }

  for (int i = 0; i < component.getNumChildComponents(); ++i)
    if (auto *child = component.getChildComponent(i))
      styleUpdateLogScrollBars(*child);
}

class AboutContent : public juce::Component
{
public:
  AboutContent()
      : closeButton(TR("dialog.close"))
  {
    noticesEditor.setMultiLine(true);
    noticesEditor.setReadOnly(true);
    noticesEditor.setScrollbarsShown(true);
    noticesEditor.setScrollBarThickness(8);
    noticesEditor.setCaretVisible(false);
    noticesEditor.setPopupMenuEnabled(false);
    noticesEditor.setText(
        juce::String::fromUTF8(BinaryData::THIRD_PARTY_NOTICES_txt,
                               BinaryData::THIRD_PARTY_NOTICES_txtSize),
        juce::dontSendNotification);
    noticesEditor.applyFontToAllText(AppFont::getFont(aboutNoticesFontSize));
    noticesEditor.setColour(juce::TextEditor::backgroundColourId,
                            juce::Colour(0xFF191717u));
    noticesEditor.setColour(juce::TextEditor::textColourId,
                            APP_COLOR_TEXT_PRIMARY);
    noticesEditor.setColour(juce::TextEditor::outlineColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::TextEditor::shadowColourId,
                            juce::Colours::transparentBlack);
    noticesEditor.setColour(juce::ScrollBar::backgroundColourId,
                            updateLogScrollbarTrack);
    noticesEditor.setColour(juce::ScrollBar::trackColourId,
                            updateLogScrollbarTrack);
    noticesEditor.setColour(juce::ScrollBar::thumbColourId,
                            updateLogScrollbarThumb);
    noticesEditor.setLookAndFeel(&DarkLookAndFeel::getInstance());
    styleUpdateLogScrollBars(noticesEditor);
    addAndMakeVisible(noticesEditor);

    styleUpdateButton(closeButton);
    addAndMakeVisible(closeButton);

    setSize(550, 386);
  }

  ~AboutContent() override
  {
    noticesEditor.setLookAndFeel(nullptr);
    closeButton.setLookAndFeel(nullptr);
  }

  void resized() override
  {
    auto bounds = getLocalBounds().reduced(24, 20);
    noticesEditor.setBounds(bounds.removeFromTop(311));
    styleUpdateLogScrollBars(noticesEditor);

    auto buttonRow = getLocalBounds().removeFromBottom(46);
    const int buttonW = 150;
    const int buttonH = 26;
    closeButton.setBounds((getWidth() - buttonW) / 2, buttonRow.getY() + 5,
                          buttonW, buttonH);
  }

  void paint(juce::Graphics &g) override
  {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);
  }

  std::function<void()> onClose;
  juce::TextButton closeButton;

private:
  juce::TextEditor noticesEditor;
};

class AboutDialog : public juce::DialogWindow
{
public:
  explicit AboutDialog(juce::Component *parent)
      : juce::DialogWindow("", APP_COLOR_BACKGROUND, true)
  {
    setOpaque(false);
    setUsingNativeTitleBar(false);
    setTitleBarHeight(0);
    setResizable(false, false);
    setTitleBarButtonsRequired(0, false);

    auto *content = new AboutContent();
    content->onClose = [this] { closeButtonPressed(); };
    content->closeButton.onClick = content->onClose;

    setContentOwned(content, true);
    setSize(550, 386);

    if (parent != nullptr)
      centreAroundComponent(parent, getWidth(), getHeight());
    else
      centreWithSize(getWidth(), getHeight());
  }

  void closeButtonPressed() override
  {
    exitModalState(0);
  }

  void paint(juce::Graphics &g) override
  {
    juce::ignoreUnused(g);
  }
};
} // namespace

void showAboutDialog(juce::Component *parent)
{
  auto *dialog = new AboutDialog(parent);
  dialog->setVisible(true);
  dialog->toFront(true);
  dialog->enterModalState(true, nullptr, true);
}
