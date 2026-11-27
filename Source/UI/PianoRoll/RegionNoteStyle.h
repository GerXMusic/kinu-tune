#pragma once

#include "../../JuceHeader.h"

#include <optional>
#include <vector>

class NoteRenderer;

/**
 * N12 (D-354) - the ONE description of how a track item's notes are painted.
 *
 * Track mode used to paint the active item through NoteRenderer and the other
 * items through a second path with its own gradient, its own opacity and its
 * own rules. The two drifted, and the user saw an item darken only because the
 * selection had moved to another one. This struct is now the only thing that
 * decides that look: the active item and every other item of the track ask for
 * the same style, and the factory takes no "active" argument on purpose.
 *
 * Pinned clips of OTHER tracks are the documented exception
 * (pinnedRegionNoteStyle): they are a dimmed reference, not an item of this
 * document.
 *
 * Selection is not a field here: Note::selected is derived from the document
 * selection (D-349), so the one body renderer draws it the same for every item.
 */
struct RegionNoteStyle
{
  juce::Colour gradientCentre;
  juce::Colour gradientSide;
  // Alpha of the item's note layer over the grid. 1.0 for every item of this
  // track: no item is painted "more" than another.
  float layerOpacity = 1.0f;
  // Opacity of the bodies inside that layer, and whether they are desaturated
  // (pinned reference only).
  float bodyOpacity = 1.0f;
  bool desaturateBodies = false;
  bool greyscale = false;
  bool fillBodies = true;
  bool selectionDecoration = true;
  std::optional<juce::Colour> outlineColour;
  bool showDeltaPitch = true;
  bool showBasePitch = true;

  bool operator==(const RegionNoteStyle &other) const
  {
    return gradientCentre == other.gradientCentre &&
           gradientSide == other.gradientSide &&
           layerOpacity == other.layerOpacity &&
           bodyOpacity == other.bodyOpacity &&
           desaturateBodies == other.desaturateBodies &&
           greyscale == other.greyscale && fillBodies == other.fillBodies &&
           selectionDecoration == other.selectionDecoration &&
           outlineColour == other.outlineColour &&
           showDeltaPitch == other.showDeltaPitch &&
           showBasePitch == other.showBasePitch;
  }
  bool operator!=(const RegionNoteStyle &other) const
  {
    return !(*this == other);
  }
};

/** The canonical note palette. NoteRenderer's defaults come from here, so the
    style and the renderer cannot drift apart. */
namespace regionNoteColours
{
inline const juce::Colour bodyCentre(0xFF5E93D6u);
inline const juce::Colour bodySide(0xFF304861u);
} // namespace regionNoteColours

/** The style of an item of this track: identical for the active one and for
    the others. Deliberately takes no "active". The two pitch-curve flags are
    per-view settings, never per-item. */
RegionNoteStyle trackRegionNoteStyle(bool showDeltaPitch = true,
                                     bool showBasePitch = true);

/** A clip of another track, pinned as a reference: dimmed, desaturated and
    outlined in the track's colour. Never equal to trackRegionNoteStyle(). */
RegionNoteStyle pinnedRegionNoteStyle(juce::Colour outline);

/** Turns a style into NoteRenderer settings. The only writer of those settings
    on the item painting paths. */
void applyRegionNoteStyle(NoteRenderer &renderer, const RegionNoteStyle &style);

/** The x positions that get a cut line between items: the edges deduplicated
    within one pixel, so two contiguous items share their line (N12/D-354). */
std::vector<float> dedupeRegionEdges(std::vector<float> edges);
