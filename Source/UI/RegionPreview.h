#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <functional>
#include <vector>

class Project;

/**
 * One ARA playback region of a track, as track mode draws it.
 *
 * N10 (D-350): a preview BORROWS the modification's live Project; it owns no
 * Project and keeps no copy of its notes. The document index, the drawing and
 * the hit-test therefore read the same Note objects, and an edit lands in the
 * one place that holds it.
 *
 * Lifetime: `project` is a non-owning pointer into the processor's region
 * store or the canvas. It is valid while the list it belongs to is published.
 * The publisher republishes the list whenever a region's project pointer, its
 * revision or its window changes, and the processor drops the published list
 * before it destroys or replaces a modification's Project
 * (PitchNetAudioProcessor calls updateRegionPreviews(nullptr) on the two paths
 * that do). Nothing here may outlive that call.
 *
 * All host times are absolute host-timeline seconds. The UI must not depend on
 * the ARA SDK.
 */
struct MainViewRegionPreview {
  juce::String key;
  juce::String name;
  double startSeconds = 0.0; // host: first second of the item
  double endSeconds = 0.0;   // host: one past its last second
  bool active = false;
  // A region of another track the user pinned in the track list. Drawn as
  // notes only (no waveform, pitch curve, boundaries or overview), with the
  // inactive-region body plus an outline in outlineColour; never clickable.
  bool pinned = false;
  juce::Colour outlineColour;
  // False when the region has not been analysed yet: only its range is known.
  bool hasContent = false;

  // The live Project this item shows (the modification's own), borrowed and
  // never written from here. Null when the item has no analysis yet.
  Project *project = nullptr;

  // The item's window inside that Project, in project seconds, taken from the
  // same ARA start/end the document index publishes so the drawn window and
  // the indexed notes cannot disagree.
  double sourceStartSeconds = 0.0;
  double sourceEndSeconds = 0.0;

  // Host time of the Project's zero: host = project + projectHostOffsetSeconds.
  // The one offset the drawing and the hit-test place notes with.
  double projectHostOffsetSeconds = 0.0;

  // The region's own audio (first channel, the one the overview draws),
  // starting at waveformStartSeconds. Shared so republishing previews never
  // copies the audio.
  std::shared_ptr<const std::vector<float>> waveformSamples;
  double waveformSampleRate = 0.0;
  double waveformStartSeconds = 0.0;

  bool contains(double timeSeconds) const {
    return timeSeconds >= startSeconds && timeSeconds < endSeconds;
  }
};

using MainViewRegionPreviewList =
    std::shared_ptr<const std::vector<MainViewRegionPreview>>;

/** The list without pinned-track regions, for views that show only this
    track (background waveform, overview). Returns the same list when nothing
    is pinned, so the common case copies nothing. */
inline MainViewRegionPreviewList
withoutPinnedRegions(const MainViewRegionPreviewList &previews) {
  if (!previews)
    return previews;
  bool anyPinned = false;
  for (const auto &region : *previews)
    anyPinned = anyPinned || region.pinned;
  if (!anyPinned)
    return previews;
  auto filtered = std::make_shared<std::vector<MainViewRegionPreview>>();
  for (const auto &region : *previews)
    if (!region.pinned)
      filtered->push_back(region);
  return filtered;
}

// Editable projects and region windows supplied by the ARA backend for a
// track-wide macro. Projects are deduplicated by audio modification.
struct TrackPitchMacroTarget {
  Project* project = nullptr;
  std::vector<juce::Range<int>> frames;
  std::function<bool()> isValid;
  std::function<void(bool)> onChanged; // false: visual preview; true: render
  std::function<void()> onCommit; // persist settings without another render
};
