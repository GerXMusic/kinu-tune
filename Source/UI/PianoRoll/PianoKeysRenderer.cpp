#include "PianoKeysRenderer.h"
#include "PianoRollViewHelpers.h"
#include "../Components/AppFont.h"
#include "../../Utils/Constants.h"
#include "../../Utils/UI/Theme.h"

void PianoKeysRenderer::draw(juce::Graphics &g,
                             int componentHeight,
                             int scrollBarSize,
                             ScaleMode activeScaleMode,
                             int activeScaleRootNote,
                             float pitchAxisOffsetSemitones)
{
  if (!coordMapper)
    return;

  const int pianoKeysWidth = CoordinateMapper::pianoKeysWidth;
  const int headerHeight = CoordinateMapper::headerHeight;

  auto keyArea = juce::Rectangle<int>(
      0, headerHeight, pianoKeysWidth,
      juce::jmax(0, componentHeight - headerHeight - scrollBarSize));

  juce::Graphics::ScopedSaveState savedState(g);
  g.reduceClipRegion(keyArea);

  // Background
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(keyArea);

  static const char *noteNames[] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};
  const bool showScaleOverlay =
      activeScaleMode != ScaleMode::None &&
      activeScaleMode != ScaleMode::Chromatic &&
      activeScaleRootNote >= 0;

  const float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();
  // Use truncated scrollY to match grid origin (which uses static_cast<int>(scrollY))
  const int scrollYInt = static_cast<int>(coordMapper->getScrollY());

  for (int midi = MIN_MIDI_NOTE; midi <= MAX_MIDI_NOTE; ++midi)
  {
    float y = coordMapper->midiToY(
                  static_cast<float>(midi) + pitchAxisOffsetSemitones) -
              static_cast<float>(scrollYInt) + headerHeight;
    int noteInOctave = (midi % 12 + 12) % 12;

    const auto toneState = pianoRollView::getScaleToneState(
        activeScaleMode, noteInOctave, activeScaleRootNote);

    juce::Colour keyFill = pianoRollView::isBlackKey(noteInOctave)
                               ? juce::Colour(0xFF20242Au)
                               : juce::Colour(0xFF3E444Eu);
    if (showScaleOverlay)
      keyFill = toneState == pianoRollView::ScaleToneState::OutOfScale
                    ? keyFill.darker(0.25f)
                    : keyFill.brighter(0.05f);

    g.setColour(keyFill);
    g.fillRect(0.0f, y, static_cast<float>(pianoKeysWidth),
               pixelsPerSemitone - 1);
    // A small black-key tongue on the left, so the strip reads as a keyboard
    // instead of a list of labels.
    if (pianoRollView::isBlackKey(noteInOctave))
    {
      g.setColour(juce::Colour(0xFF111317u));
      g.fillRect(0.0f, y + 1.0f, pianoKeysWidth * 0.62f,
                 juce::jmax(1.0f, pixelsPerSemitone - 2.0f));
    }
    g.setColour(juce::Colour(0xFF111317u).withAlpha(0.9f));
    g.fillRect(0.0f, y + pixelsPerSemitone - 1.0f,
               static_cast<float>(pianoKeysWidth), 1.0f);

    int octave = midi / 12 - 1;
    juce::String noteName =
        juce::String(noteNames[noteInOctave]) + juce::String(octave);

    // C labels carry the octave; the rest stay unlabelled like Melodyne's
    // keyboard. Small strips still show every row through the colour.
    if (noteInOctave != 0)
      noteName.clear();

    juce::Colour textColour = pianoRollView::isBlackKey(noteInOctave)
                                  ? juce::Colour(0xFFD7DBE2u)
                                  : juce::Colour(0xFF15171Bu);
    if (showScaleOverlay)
    {
      if (toneState == pianoRollView::ScaleToneState::OutOfScale)
        textColour = textColour.withMultipliedAlpha(0.72f);
    }
    g.setColour(textColour);
    g.setFont(AppFont::getBoldFont(10.5f));
    g.drawText(noteName, 0, static_cast<int>(y), pianoKeysWidth,
               static_cast<int>(pixelsPerSemitone),
               juce::Justification::centred);
  }
}
