#pragma once

#include "../Audio/SynthesisEngineType.h"
#include "../JuceHeader.h"
#include <optional>
#include <vector>

/**
 * Engine requested for one note (M2 fase 1, D-125).
 *
 * Inherit is the default and keeps the behaviour every project had before the
 * field existed: the note is rendered by the engine selected in SettingsManager
 * (the global preference). The other three name a specific engine for the note
 * and travel in the project. Fase 1 only stores the value: nothing reads it
 * during rendering yet, so the audio of an existing edit does not change.
 */
enum class NoteEngine
{
    Inherit = 0, // Use the global engine (SettingsManager)
    Vocoder,     // Neural vocoder (mel + F0 -> waveform)
    Psola,       // Classic time-domain PSOLA of the fork
    Pitchcore    // Our own classic engine (M2 step 1, D-096)
};

/// The concrete engine named by a note, or nothing when the note inherits.
inline std::optional<SynthesisEngineType> noteEngineToSynthesisEngine(NoteEngine engine)
{
    switch (engine)
    {
        case NoteEngine::Vocoder:   return SynthesisEngineType::Vocoder;
        case NoteEngine::Psola:     return SynthesisEngineType::Psola;
        case NoteEngine::Pitchcore: return SynthesisEngineType::Pitchcore;
        case NoteEngine::Inherit:
        default:                    return std::nullopt;
    }
}

/// Note field for a concrete engine. Every SynthesisEngineType has one.
inline NoteEngine noteEngineFromSynthesisEngine(SynthesisEngineType type)
{
    switch (type)
    {
        case SynthesisEngineType::Vocoder:   return NoteEngine::Vocoder;
        case SynthesisEngineType::Psola:     return NoteEngine::Psola;
        case SynthesisEngineType::Pitchcore: return NoteEngine::Pitchcore;
        default:                             return NoteEngine::Psola;
    }
}

/// Serialised form, matching the names SettingsManager already writes.
inline const char* noteEngineToString(NoteEngine engine)
{
    switch (engine)
    {
        case NoteEngine::Vocoder:   return "Vocoder";
        case NoteEngine::Psola:     return "PSOLA";
        case NoteEngine::Pitchcore: return "Pitchcore";
        case NoteEngine::Inherit:
        default:                    return "Inherit";
    }
}

/// Unknown or missing text loads as Inherit, never as a different engine.
inline NoteEngine stringToNoteEngine(const juce::String& text)
{
    if (text.equalsIgnoreCase("PSOLA"))
        return NoteEngine::Psola;
    if (text.equalsIgnoreCase("Vocoder"))
        return NoteEngine::Vocoder;
    if (text.equalsIgnoreCase("Pitchcore"))
        return NoteEngine::Pitchcore;
    return NoteEngine::Inherit;
}

/**
 * Represents a single note/pitch segment.
 *
 * Timing model:
 * - srcStartFrame/srcEndFrame: Position in original waveform (fixed after detection)
 * - startFrame/endFrame: Position in output timeline
 *
 * Pitch model:
 * - midiNote: The base pitch of the note (can be changed by dragging)
 * - deltaPitch: Per-frame deviation from base pitch (preserved during drag)
 * - f0Values: Original F0 values from detection (for reference)
 *
 * When dragging a note up/down:
 * - midiNote changes
 * - deltaPitch stays the same
 * - Actual pitch = midiNote + deltaPitch[frame]
 *
 */
class Note
{
public:
    Note() = default;
    Note(int startFrame, int endFrame, float midiNote);

    // Source frame range (position in original waveform, fixed after detection)
    int getSrcStartFrame() const { return srcStartFrame; }
    int getSrcEndFrame() const { return srcEndFrame; }
    void setSrcStartFrame(int frame) { srcStartFrame = frame; }
    void setSrcEndFrame(int frame) { srcEndFrame = frame; }
    int getSrcDurationFrames() const { return srcEndFrame - srcStartFrame; }

    // Destination frame range (position in output timeline, can be changed)
    int getStartFrame() const { return startFrame; }
    int getEndFrame() const { return endFrame; }
    void setStartFrame(int frame) { startFrame = frame; }
    void setEndFrame(int frame) { endFrame = frame; }
    int getDurationFrames() const { return endFrame - startFrame; }

    // Sub-frame timing preview used while dragging timing boundaries. These
    // values are deliberately transient: only mouse-up commits integer frame
    // positions to the project and undo history.
    float getVisualStartFrame() const
    {
        return timingPreviewStartActive
                   ? timingPreviewStartFrame
                   : static_cast<float>(startFrame);
    }
    float getVisualEndFrame() const
    {
        return timingPreviewEndActive
                   ? timingPreviewEndFrame
                   : static_cast<float>(endFrame);
    }
    float getVisualDurationFrames() const
    {
        return getVisualEndFrame() - getVisualStartFrame();
    }
    void setTimingPreviewStartFrame(float frame)
    {
        timingPreviewStartFrame = frame;
        timingPreviewStartActive = true;
    }
    void setTimingPreviewEndFrame(float frame)
    {
        timingPreviewEndFrame = frame;
        timingPreviewEndActive = true;
    }
    void clearTimingPreview()
    {
        timingPreviewStartActive = false;
        timingPreviewEndActive = false;
    }
    bool hasTimingPreview() const
    {
        return timingPreviewStartActive || timingPreviewEndActive;
    }

    // Pitch
    float getMidiNote() const { return midiNote; }
    // Regular pitch edits become the source for the next Pitch Center macro
    // pass. The macro uses the dedicated setter below so that repeatedly
    // previewing/applying correction never compounds its own output.
    void setMidiNote(float note)
    {
        midiNote = note;
        lastNonMacroMidiNote = note;
    }
    float getLastNonMacroMidiNote() const { return lastNonMacroMidiNote; }
    void setLastNonMacroMidiNote(float note) { lastNonMacroMidiNote = note; }
    // Pitch moves keep this reference through
    // ScaleUtils::carriedCorrectionSource (see there).
    void setMidiNoteFromPitchCorrection(float note) { midiNote = note; }
    float getOriginalMidiNote() const { return originalMidiNote; }
    void setOriginalMidiNote(float note) { originalMidiNote = note; }
    float getPitchOffset() const { return pitchOffset; }
    void setPitchOffset(float offset) { pitchOffset = offset; }
    // Centro visible/editable: lo que la nota muestra y lo que editan los
    // gestos de altura.
    float getAdjustedMidiNote() const { return midiNote + pitchOffset; }
    // D-303: centro de RENDER, separado del visible. Al cortar una nota cada
    // mitad se recentra en la media local de la curva blanca (centro visible),
    // pero el motor tiene que seguir construyendo su base desde el centro que
    // tenia la nota original; si no, el corte moveria el audio (el escalon de
    // 14,6 c de D-285). En una nota normal vale 0 y todo queda como hoy.
    float getRenderCenterOffset() const { return renderCenterOffset; }
    void setRenderCenterOffset(float offset) { renderCenterOffset = offset; }
    float getRenderMidiNote() const { return midiNote + renderCenterOffset; }
    float getRenderAdjustedMidiNote() const
    {
        return midiNote + pitchOffset + renderCenterOffset;
    }
    float getFormantShift() const { return formantShift; }
    void setFormantShift(float st) { formantShift = juce::jlimit(-12.0f, 12.0f, st); }
    float getVolumeDb() const { return volumeDb; }
    void setVolumeDb(float db) { volumeDb = db; }

    // M5 v1 / D-245: silenciar la nota sin tocar su ganancia guardada. El
    // render la trata como ganancia 0 (fade suave del mismo NoteGainCurve).
    bool isMuted() const { return muted; }
    void setMuted(bool m) { muted = m; }

    // M5 v2 / D-299: nota eliminada. Queda como lapida interna (deleted + rest)
    // para que su bloque D-129 se re-renderice y el silencio sobreviva a
    // guardar/reabrir; la UI la oculta por la misma via que un rest. El render
    // aplica el silencio al final del pase, no por ganancia.
    bool isDeleted() const { return deleted; }
    void setDeleted(bool d) { deleted = d; }

    // RMS of pristine source audio; gain edits are applied separately.
    float getSourceAmplitude() const { return sourceAmplitude; }
    void setSourceAmplitude(float rms) { sourceAmplitude = rms; }

    // Delta pitch (per-frame deviation from base pitch in semitones)
    const std::vector<float>& getDeltaPitch() const { return deltaPitch; }
    void setDeltaPitch(std::vector<float> delta) { deltaPitch = std::move(delta); }
    bool hasDeltaPitch() const { return !deltaPitch.empty(); }

    // Original delta pitch (pristine curve from analysis, never modified)
    const std::vector<float>& getOriginalDeltaPitch() const { return originalDeltaPitch; }
    void setOriginalDeltaPitch(std::vector<float> delta) { originalDeltaPitch = std::move(delta); }
    bool hasOriginalDeltaPitch() const { return !originalDeltaPitch.empty(); }

    // Optional committed per-frame contour. This is the editable source used
    // by pitch tools after a destructive/baked curve operation, while
    // originalDeltaPitch remains the immutable analysis result.
    const std::vector<float>& getBakedDeltaPitch() const { return bakedDeltaPitch; }
    void setBakedDeltaPitch(std::vector<float> delta) { bakedDeltaPitch = std::move(delta); }
    void clearBakedDeltaPitch() { bakedDeltaPitch.clear(); }
    bool hasBakedDeltaPitch() const { return !bakedDeltaPitch.empty(); }
    const std::vector<float>& getActiveDeltaPitch() const
    {
        if (hasBakedDeltaPitch())
            return bakedDeltaPitch;
        if (hasOriginalDeltaPitch())
            return originalDeltaPitch;
        return deltaPitch;
    }

    // Pitch tool transformation parameters (non-destructive)
    float getTiltLeft() const { return tiltLeft; }
    void setTiltLeft(float tilt) { tiltLeft = tilt; }
    float getTiltRight() const { return tiltRight; }
    void setTiltRight(float tilt) { tiltRight = tilt; }
    // Scales deviations from the note center: 1.0 preserves the recorded
    // contour, 0.0 flattens it, and negative values invert it. The range is
    // limited to -10.0 through 10.0 (-1000% through 1000%).
    float getVibrato() const { return vibrato; }
    void setVibrato(float scale) { vibrato = juce::jlimit(-10.0f, 10.0f, scale); }
    // Slow pitch-trend scale: 100% original, 0% removed, negative inverted.
    float getPitchDrift() const { return pitchDrift; }
    void setPitchDrift(float scale) { pitchDrift = juce::jlimit(-10.0f, 10.0f, scale); }
    int getSmoothLeftFrames() const { return smoothLeftFrames; }
    void setSmoothLeftFrames(int frames) { smoothLeftFrames = frames; }
    int getSmoothRightFrames() const { return smoothRightFrames; }
    void setSmoothRightFrames(int frames) { smoothRightFrames = frames; }
    // D-225: length (ms) of the pitch transition out of this note, into the
    // next one, at a legato union. The selected note owns the union with the
    // note after it (Melodyne: the control drags the tail of the note).
    // 0 = the existing fixed join smoother.
    int getTransitionOutMs() const { return transitionOutMs; }
    void setTransitionOutMs(int ms)
    {
        transitionOutMs = juce::jlimit(0, 250, ms);
    }
    // D-223: how much of the transition replaces the natural contour is an
    // engine law, not a per-note control (Melodyne exposes one duration).

    // Delta scale/offset (post-transformation, from delta control handles)
    float getDeltaScale() const { return deltaScale; }
    void setDeltaScale(float scale) { deltaScale = scale; }
    float getDeltaOffset() const { return deltaOffset; }
    void setDeltaOffset(float offset) { deltaOffset = offset; }

    // F0 values (original detected values)
    const std::vector<float>& getF0Values() const { return f0Values; }
    void setF0Values(std::vector<float> values) { f0Values = std::move(values); }
    std::vector<float> getAdjustedF0() const;

    // Get F0 values based on current midiNote + deltaPitch
    std::vector<float> computeF0FromDelta() const;

    // True after this note has contributed to the rendered composite waveform.
    // This lightweight state replaces the former per-note audio cache and lets
    // incremental synthesis refresh connected edited-note clusters.
    bool hasRenderedEdit() const { return renderedEdit; }
    void setRenderedEdit(bool rendered)
    {
        renderedEdit = rendered;
        renderedStartFrame = startFrame;
        renderedEndFrame = endFrame;
    }
    int getRenderedStartFrame() const { return renderedStartFrame; }
    int getRenderedEndFrame() const { return renderedEndFrame; }
    // Relocate the rendered bounds together with the note. Anything that moves
    // a note's timeline coordinates wholesale (non-ARA capture padding/merge)
    // must move these too, or the synthesizer mistakes the relocation for a
    // pending timing edit and clears audio before the region's first note.
    void setRenderedFrames(int start, int end)
    {
        renderedStartFrame = start;
        renderedEndFrame = end;
    }

    // Synth dirty flag (needs re-synthesis; separate from display dirty flag)
    bool isSynthDirty() const { return synthDirty; }
    void setSynthDirty(bool d) { synthDirty = d; }
    void markSynthDirty() { synthDirty = true; }

    // Selection
    bool isSelected() const { return selected; }
    void setSelected(bool sel) { selected = sel; }

    // Dirty flag (for incremental synthesis)
    bool isDirty() const { return dirty; }
    void setDirty(bool d) { dirty = d; }
    // D-253: cada marca sube la revisión. El cierre de un render limpia una
    // nota solo si su revisión es la del snapshot: una segunda edición sobre la
    // misma nota mientras el render está en vuelo no se pierde.
    void markDirty() {
        dirty = true;
        ++dirtyRevision;
    }
    void clearDirty() { dirty = false; }
    std::uint64_t getDirtyRevision() const { return dirtyRevision; }

    // Rest note (no pitch, just a placeholder for silence)
    bool isRest() const { return rest; }
    void setRest(bool r) { rest = r; }

    // Per-note engine (M2 fase 1, D-125). Inherit keeps the global engine.
    NoteEngine getEngine() const { return engine; }
    void setEngine(NoteEngine value) { engine = value; }

    // D-258: motor que tenia la nota antes de aplicar un motor a todo su bloque
    // (solo se guarda al elegir Vocoder). Permite volver al reparto anterior sin
    // depender del undo. Inherit = "no hay override guardado".
    NoteEngine getBlockEngineOverride() const { return blockEngineOverride; }
    void setBlockEngineOverride(NoteEngine value) {
        blockEngineOverride = value;
    }

    // Lyric (character/syllable for this note)
    juce::String getLyric() const { return lyric; }
    void setLyric(const juce::String& text) { lyric = text; }
    bool hasLyric() const { return lyric.isNotEmpty(); }

    // Phoneme (pronunciation for this note)
    juce::String getPhoneme() const { return phoneme; }
    void setPhoneme(const juce::String& ph) { phoneme = ph; }
    bool hasPhoneme() const { return phoneme.isNotEmpty(); }

    // Check if frame is within note
    bool containsFrame(int frame) const;
    bool isNeutralForOriginalWaveform() const;

private:
    // Source position (in original waveform, fixed after detection)
    int srcStartFrame = 0;
    int srcEndFrame = 0;

    // Destination position in the output timeline
    int startFrame = 0;
    int endFrame = 0;
    float timingPreviewStartFrame = 0.0f;
    float timingPreviewEndFrame = 0.0f;
    bool timingPreviewStartActive = false;
    bool timingPreviewEndActive = false;

    float midiNote = 60.0f;
    float lastNonMacroMidiNote = 60.0f;
    float originalMidiNote = 60.0f;
    float pitchOffset = 0.0f;
    // D-303: separa el centro visible (midiNote) del centro que usa el motor
    // para construir la base. Solo el corte lo pone distinto de 0.
    float renderCenterOffset = 0.0f;
    float formantShift = 0.0f; // Semitones, independent of F0
    float sourceAmplitude = 0.0f;
    float volumeDb = 0.0f; // Per-note gain in dB (0 = unity)
    bool muted = false;    // M5 v1: nota silenciada (no toca volumeDb)
    bool deleted = false;  // M5 v2: lapida silenciosa (D-299)

    std::vector<float> deltaPitch;  // Per-frame deviation from midiNote in semitones
    std::vector<float> originalDeltaPitch;  // Pristine curve from analysis (never modified)
    std::vector<float> bakedDeltaPitch; // Optional committed editable contour

    // Pitch tool transformation parameters (non-destructive, stored as parameters)
    float tiltLeft = 0.0f;           // Tilt amount at left edge (semitones)
    float tiltRight = 0.0f;          // Tilt amount at right edge (semitones)
    float vibrato = 1.0f;            // 1.0=original, 0.0=flat, >1.0=amplify, <0.0=invert
    float pitchDrift = 1.0f;
    int smoothLeftFrames = 0;        // Smoothing transition length at left boundary
    int smoothRightFrames = 0;       // Smoothing transition length at right boundary
    int transitionOutMs = 0;         // D-225: union transition out to the next note

    // Post-transformation scale/offset from delta control handles
    float deltaScale = 1.0f;        // Applied after all other transformations (1.0=unchanged)
    float deltaOffset = 0.0f;       // Added after scale (0.0=unchanged)

    std::vector<float> f0Values;
    bool renderedEdit = false; // Audio for this note is present in the composite
    // Timeline bounds represented by the current composite waveform. These can
    // differ from both the immutable source bounds and the next pending edit.
    int renderedStartFrame = 0;
    int renderedEndFrame = 0;
    bool selected = false;
    bool dirty = false;       // For incremental synthesis (display/trigger)
    // D-253: revisión monotónica de la marca sucia (identidad por edición).
    std::uint64_t dirtyRevision = 0;
    bool synthDirty = true;   // Needs re-synthesis (separate from display dirty)
    bool rest = false;        // Rest note (silence placeholder)
    // Per-note engine; Inherit means "use the global engine" (D-125).
    NoteEngine engine = NoteEngine::Inherit;
    // D-258: motor previo guardado al aplicar un motor a todo el bloque.
    NoteEngine blockEngineOverride = NoteEngine::Inherit;

    juce::String lyric;   // Lyric text (e.g., "a", "SP" for silence)
    juce::String phoneme; // Phoneme (e.g., "a", "sp", for pronunciation)
};
