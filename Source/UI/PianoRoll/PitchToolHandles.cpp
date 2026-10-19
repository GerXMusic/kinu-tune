#include "PitchToolHandles.h"
#include "BinaryData.h"

#include <algorithm>
#include <limits>

namespace {

juce::Image loadIcon(const char* resourceName)
{
  int size = 0;
  const auto* data = BinaryData::getNamedResource(resourceName, size);
  return data ? juce::ImageFileFormat::loadFrom(data, size) : juce::Image();
}

} // namespace

PitchToolHandles::PitchToolHandles()
    : leftTiltIcon(loadIcon("ltilt_png")),
      vibratoIcon(loadIcon("vibrato_png")),
      driftIcon(loadIcon("drift_png")),
      formantIcon(loadIcon("formant_png")),
      amplitudeIcon(loadIcon("amplitude_png")),
      rightTiltIcon(loadIcon("rtilt_png")) {
  // Initialize (currently empty, but reserve space)
  handles.reserve(20);  // Typical max handles for multi-note selection
}

void PitchToolHandles::updateHandles(const std::vector<Note*>& selectedNotes,
                                     const CoordinateMapper& mapper,
                                     const juce::Rectangle<float>& hoverBounds) {
  juce::ignoreUnused(hoverBounds);
  handles.clear();

  if (selectedNotes.empty() || selectedNotes.front() == nullptr)
    return;

  // VariAudio smart controls sit on the hovered segment's own rectangle
  // (Cubase Pro 15 manual, "Smart Controls"; see docs/design.md 5.2).
  auto* note = selectedNotes.front();
  const float left = mapper.timeToX(mapper.framesToSeconds(note->getStartFrame()));
  const float right = std::max(
      left + 4.0f, mapper.timeToX(mapper.framesToSeconds(note->getEndFrame())));
  const float top = mapper.midiToY(note->getAdjustedMidiNote());
  const float bottom = top + mapper.getPixelsPerSemitone();
  const float centreX = (left + right) * 0.5f;
  const float s = controlSize;
  const float gap = 3.0f;
  if (mapper.getPixelsPerSemitone() < minSemitoneHeight ||
      right - left < minSegmentWidth)
    return;

  const auto addSquare = [this, note, s](HandleType type, float cx, float cy) {
    addHandle(type, cx, cy, note);
    handles.back().bounds = {cx - s * 0.5f, cy - s * 0.5f, s, s};
  };

  // Default set: straighten (top centre), correct pitch (below the bottom
  // centre), glue (outside the bottom corners).
  addSquare(HandleType::Vibrato, centreX, top - s * 0.5f - gap);
  addSquare(HandleType::CorrectPitch, centreX, bottom + s * 0.5f + gap);

  // No split line on the segment body: clicking a segment must only select
  // it (user request, 2026-10-19). Splitting is Alt/Option-click or the
  // Split tool.

  if (options.canGlueLeft)
    addSquare(HandleType::GlueLeft, left - s * 0.5f - 1.0f, bottom - s * 0.5f);
  if (options.canGlueRight)
    addSquare(HandleType::GlueRight, right + s * 0.5f + 1.0f, bottom - s * 0.5f);

  if (!options.showAll || right - left < minSegmentWidthForCorners)
    return;

  // "Show All": tilt at the top corners, formant and volume in the bottom
  // corners, just inside the segment so they do not collide with glue.
  addSquare(HandleType::TiltLeft, left + s * 0.5f, top - s * 0.5f - gap);
  addSquare(HandleType::TiltRight, right - s * 0.5f, top - s * 0.5f - gap);
  addSquare(HandleType::Formant, left + s * 0.5f + 1.0f, bottom - s * 0.5f - 1.0f);
  addSquare(HandleType::Amplitude, right - s * 0.5f - 1.0f, bottom - s * 0.5f - 1.0f);
}

void PitchToolHandles::draw(juce::Graphics& g) const {
  const auto fill = juce::Colour(0xFF4884BFu);
  const auto edge = juce::Colour(0xFF9CC3EAu);
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const auto& handle = handles[i];
    const bool hovered = i == hoveredHandleIndex;
    const auto bounds = handle.bounds;

    g.setColour(hovered ? juce::Colours::white : fill);
    g.fillRect(bounds);
    g.setColour(hovered ? fill : edge);
    g.drawRect(bounds, 1.0f);

    // Glue controls carry a small arrow toward the segment they join.
    if (handle.type == HandleType::GlueLeft || handle.type == HandleType::GlueRight) {
      juce::Path arrow;
      const float cx = bounds.getCentreX();
      const float cy = bounds.getCentreY();
      const float d = handle.type == HandleType::GlueLeft ? -1.5f : 1.5f;
      arrow.addTriangle(cx + d, cy, cx - d, cy - 2.5f, cx - d, cy + 2.5f);
      g.setColour(hovered ? fill : juce::Colours::white);
      g.fillPath(arrow);
    }
  }
}

const juce::Image&
PitchToolHandles::getIconForType(HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
      return leftTiltIcon;
    case HandleType::PitchDrift:
      return driftIcon;
    case HandleType::Vibrato:
      return vibratoIcon;
    case HandleType::Formant:
      return formantIcon;
    case HandleType::Amplitude:
      return amplitudeIcon;
    case HandleType::TiltRight:
      return rightTiltIcon;
    default:
      return vibratoIcon;
  }
}

int PitchToolHandles::hitTest(float worldX, float worldY, float tolerance) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    if (handles[i].bounds.expanded(tolerance * 0.25f).contains(worldX, worldY)) {
      return i;
    }
  }
  return -1;
}

bool PitchToolHandles::containsLayoutPoint(float worldX, float worldY) const
{
  // Only the controls themselves (plus the small gap to the segment) belong to
  // the layout. The segment body must stay free for note and track gestures.
  for (const auto& handle : handles)
    if (handle.bounds.expanded(3.0f).contains(worldX, worldY))
      return true;
  return false;
}

juce::Rectangle<float> PitchToolHandles::getLayoutBounds() const
{
  if (handles.empty())
    return {};

  auto layoutBounds = handles.front().bounds;
  for (size_t i = 1; i < handles.size(); ++i)
    layoutBounds = layoutBounds.getUnion(handles[i].bounds);
  return layoutBounds;
}

void PitchToolHandles::addHandle(HandleType type, float worldX, float worldY, Note* note) {
  Handle h;
  h.type = type;
  h.note = note;
  h.color = getColorForType(type);
  
  h.bounds = juce::Rectangle<float>(worldX, worldY, 0.0f, 0.0f);
  
  handles.push_back(h);
}

juce::Colour PitchToolHandles::getColorForType(HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
    case HandleType::TiltRight:
      return juce::Colours::orange;
      
    case HandleType::PitchDrift:
    case HandleType::Vibrato:
      return juce::Colours::mediumpurple;
      
    case HandleType::SmoothLeft:
    case HandleType::SmoothRight:
      return juce::Colours::cyan; // "Smooth" implies liquid/soft -> cyan/blue
      
    default:
      return juce::Colours::white;
  }
}
