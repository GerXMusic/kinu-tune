#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "CoordinateMapper.h"

#include <vector>
#include <unordered_map>

class SelectHandler;
class PitchEditor;

/**
 * Draws the F0 / delta / base pitch curves over the grid, and owns the
 * generated-base-pitch cache. Cache is regenerated on demand when notes change
 * (detected by note-count + total-frame mismatch) or when externally
 * invalidated.
 */
class PitchCurveRenderer
{
public:
  struct Params
  {
    bool showDeltaPitch;
    bool showBasePitch;
    bool showUvInterpolationDebug;
    bool showActualF0Debug;
    bool showCleanedF0Debug;
    bool showVocoderF0Debug;
    bool hidePitchCurves = false;
    float pitchCurveAlpha = 1.0f;
    const std::vector<float>* midiCurveOverride = nullptr;
    int componentWidth;
  };

  PitchCurveRenderer() = default;

  void setCoordinateMapper(CoordinateMapper *m) { coordMapper = m; }
  void setProject(Project *p)
  {
    project = p;
    invalidateBasePitchCache();
  }
  void setSelectHandler(SelectHandler *h) { selectHandler = h; }
  void setPitchEditor(PitchEditor *e) { pitchEditor = e; }
  // Inactive regions in Track mode: draw the curves in grey.
  void setGreyscale(bool shouldUseGreyscale) { greyscale = shouldUseGreyscale; }

  void draw(juce::Graphics &g, const Params &params);

  // D-304: la curva compuesta es una sola polilinea mientras las notas sean
  // contiguas y de la misma region. Se corta en un hueco, al cambiar de region
  // o cuando la nota anterior no se dibujo (culling). Regla pura y publica para
  // poder medirla sin dibujar.
  static bool shouldBreakPolyline(bool hasPreviousNote,
                                  float previousVisualEndFrame,
                                  float visualStartFrame,
                                  int previousRegion,
                                  int currentRegion,
                                  float toleranceFrames = 0.5f);

  void beginMacroPreview(const std::vector<Note*>& notes);
  void endMacroPreview() { macroPreview.clear(); macroNotes.clear(); }

  // Public to allow external invalidation (State handlers, undo operations).
  void invalidateBasePitchCache();

private:
  void updateBasePitchCacheIfNeeded();

  CoordinateMapper *coordMapper = nullptr;
  Project *project = nullptr;
  SelectHandler *selectHandler = nullptr;
  PitchEditor *pitchEditor = nullptr;

  struct MacroPreview
  {
    float midi;
    float drift;
    std::vector<float> driftResponse;
    float displayMidi;
    float displayDrift;
  };
  std::unordered_map<const Note*, MacroPreview> macroPreview;
  std::vector<const Note*> macroNotes;
  double lastMacroPreviewUpdate = 0.0;
  float macroOffset(const Note& note, int frame, bool includeDrift) const;
  std::vector<float> cachedBasePitch;
  size_t cachedNoteCount = 0;
  int cachedTotalFrames = 0;
  bool cacheInvalidated = true;
  bool greyscale = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchCurveRenderer)
};
