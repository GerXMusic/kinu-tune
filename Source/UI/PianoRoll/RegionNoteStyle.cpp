#include "RegionNoteStyle.h"

#include "NoteRenderer.h"

#include <algorithm>
#include <cmath>

RegionNoteStyle trackRegionNoteStyle(bool showDeltaPitch, bool showBasePitch)
{
  // No "active" parameter: this is the whole point of N12 (D-354). Every item
  // of the track - the one the canvas edits and the ones drawn as neighbours -
  // gets these exact values.
  RegionNoteStyle style;
  style.gradientCentre = regionNoteColours::bodyCentre;
  style.gradientSide = regionNoteColours::bodySide;
  style.layerOpacity = 1.0f;
  style.bodyOpacity = 1.0f;
  style.desaturateBodies = false;
  style.greyscale = false;
  style.fillBodies = true;
  style.selectionDecoration = true;
  style.outlineColour = std::nullopt;
  style.showDeltaPitch = showDeltaPitch;
  style.showBasePitch = showBasePitch;
  return style;
}

RegionNoteStyle pinnedRegionNoteStyle(juce::Colour outline)
{
  // A reference of another track: never an item of this document, so it is the
  // documented exception to the uniform look. Values are the ones the pinned
  // layer had before N12; only their home moved.
  RegionNoteStyle style = trackRegionNoteStyle();
  style.bodyOpacity = 0.35f;
  style.desaturateBodies = true;
  style.greyscale = false;
  style.outlineColour = outline;
  return style;
}

void applyRegionNoteStyle(NoteRenderer &renderer, const RegionNoteStyle &style)
{
  renderer.setNoteColours(style.gradientCentre, style.gradientSide);
  renderer.setGreyscale(style.greyscale);
  renderer.setFillBodies(style.fillBodies);
  renderer.setSelectionDecoration(style.selectionDecoration);
  renderer.setOutlineColour(style.outlineColour);
}

std::vector<float> dedupeRegionEdges(std::vector<float> edges)
{
  std::sort(edges.begin(), edges.end());
  std::vector<float> lines;
  for (const float x : edges)
  {
    if (lines.empty() || std::abs(x - lines.back()) > 1.0f)
      lines.push_back(x);
  }
  return lines;
}
