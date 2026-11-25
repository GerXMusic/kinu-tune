#include "../Audio/Synthesis/EngineBlocks.h"
#include "../Utils/Constants.h"
#include "Project.h"
#include "../Utils/Constants.h"
#include "../Utils/PitchCurveProcessor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float twoPi = 6.2831853071795864769f;

    int normalizeBeatNumerator(int numerator)
    {
        return juce::jlimit(1, 32, numerator);
    }

    int normalizeBeatDenominator(int denominator)
    {
        denominator = juce::jlimit(1, 32, denominator);
        int normalized = 1;
        while (normalized < denominator)
            normalized <<= 1;
        const int lower = normalized >> 1;
        if (lower >= 1 && (denominator - lower) < (normalized - denominator))
            normalized = lower;
        return juce::jlimit(1, 32, normalized);
    }

    TimelineGridDivision normalizeGridDivision(TimelineGridDivision division)
    {
        switch (division)
        {
        case TimelineGridDivision::Whole:
        case TimelineGridDivision::Half:
        case TimelineGridDivision::Quarter:
        case TimelineGridDivision::Eighth:
        case TimelineGridDivision::Sixteenth:
        case TimelineGridDivision::ThirtySecond:
            return division;
        default:
            return TimelineGridDivision::Quarter;
        }
    }
}

Project::Project()
    : macroParameters(std::make_shared<MacroParameters>())
{
}

void Project::addNote(Note note)
{
    // Timeline consumers (including note preview context) expect neighboring
    // notes to also be neighboring elements. A split creates its tail after
    // all existing notes, so appending it would break that invariant.
    const int startFrame = note.getStartFrame();
    const auto insertAt = std::upper_bound(
        notes.begin(), notes.end(), startFrame,
        [](int start, const Note &existing)
        {
            return start < existing.getStartFrame();
        });
    notes.insert(insertAt, std::move(note));
}

Note *Project::getNoteAtFrame(int frame)
{
    for (auto &note : notes)
    {
        if (note.containsFrame(frame))
            return &note;
    }
    return nullptr;
}

std::vector<Note *> Project::getNotesInRange(int startFrame, int endFrame)
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.getStartFrame() < endFrame && note.getEndFrame() > startFrame)
            result.push_back(&note);
    }
    return result;
}

std::vector<Note *> Project::getSelectedNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isSelected())
            result.push_back(&note);
    }
    return result;
}

bool Project::removeNoteByStartFrame(int startFrame)
{
    for (auto it = notes.begin(); it != notes.end(); ++it)
    {
        if (it->getStartFrame() == startFrame)
        {
            notes.erase(it);
            return true;
        }
    }
    return false;
}

void Project::deselectAllNotes()
{
    for (auto &note : notes)
        note.setSelected(false);
}

void Project::selectAllNotes(bool includeRests)
{
    for (auto &note : notes)
    {
        if (!includeRests && note.isRest())
            continue;
        note.setSelected(true);
    }
}

std::vector<Note *> Project::getDirtyNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isDirty())
            result.push_back(&note);
    }
    return result;
}

void Project::applyRenderedNoteState(const Project &rendered)
{
    auto &targetNotes = notes;
    const auto &renderedNotes = rendered.getNotes();
    // D-292: una lista de notas distinta (split/merge durante el render) no se
    // puede casar por indice: el resultado se descarta.
    if (targetNotes.size() != renderedNotes.size())
        return;
    for (size_t i = 0; i < targetNotes.size(); ++i)
    {
        const auto &renderedNote = renderedNotes[i];
        // D-292: el backend limpia en SU copia solo las notas que commiteo. Una
        // nota que sigue sucia en la copia no la toco este render. Y una nota
        // cuya dirtyRevision avanzo en el editor despues de la copia es una
        // edicion nueva: se conserva su dirty (antes, clearDirty() a ciegas la
        // borraba con el waveform entero y su audio no se publicaba).
        const bool committedByRender = !renderedNote.isDirty();
        if (!committedByRender)
            continue;
        if (targetNotes[i].getDirtyRevision() !=
            renderedNote.getDirtyRevision())
            continue;
        targetNotes[i].setRenderedEdit(renderedNote.hasRenderedEdit());
        targetNotes[i].setSynthDirty(renderedNote.isSynthDirty());
        targetNotes[i].clearDirty();
    }
}

bool Project::shouldResetNeutralNoteToOriginal(const Note &note) const
{
    constexpr float kGlobalEditEpsilon = 1.0e-6f;
    if (note.isRest() || !note.isDirty())
        return false;
    // A muted note is an edit with an audible effect; the render must keep the
    // mute until the reset itself clears it (T6/D-258).
    if (note.isMuted())
        return false;
    // D-256: una nota neutral que revierte una edicion ya renderizada vuelve a
    // la fuente aunque tenga motor explicito (el caso real de la macro Correct
    // Pitch al 0 %: la nota se afino con un motor de bloque y al volver al 0 %
    // quedaba re-sintetizada con ese motor). Un motor explicito en una nota que
    // nunca se ha renderizado conserva su timbre: el primer render de un bloque
    // con motor elegido no se sustituye por la fuente (S2).
    if (note.getEngine() != NoteEngine::Inherit && !note.hasRenderedEdit())
        return false;
    if (std::abs(globalPitchOffset) > kGlobalEditEpsilon ||
        std::abs(formantShift) > kGlobalEditEpsilon)
        return false;
    return note.isNeutralForOriginalWaveform();
}

bool Project::noteHasAudioRelevantEdit(const Note &note)
{
    if (note.isRest())
        return false;
    if (note.isMuted())
        return true;
    if (note.getEngine() != NoteEngine::Inherit)
        return true;
    return !note.isNeutralForOriginalWaveform();
}

bool Project::hasRegionEdits() const
{
    // D-260: con el bypass activo hay que publicar igualmente: el publish es
    // el que sustituye el procesado por el original.
    if (bypassProcessing)
        return true;
    if (globalPitchOffset != 0.0f || formantShift != 0.0f)
        return true;

    for (const auto &note : notes)
    {
        if (noteHasAudioRelevantEdit(note))
            return true;
    }

    return false;
}

juce::AudioBuffer<float> Project::buildPublishAudio() const
{
    const auto &processed = audioData.waveform;
    if (!bypassProcessing)
        return processed;

    const auto &original = audioData.originalWaveform;
    if (original.getNumSamples() <= 0)
        return processed;
    if (original.getNumChannels() < processed.getNumChannels())
        return processed;

    // D-260: the bypass publishes the pristine source, with a 5 ms linear
    // crossfade at both region edges so the handover to whatever plays outside
    // the region is continuous (the edge sample equals the processed one).
    constexpr int kByPassFadeSamples = 240;
    juce::AudioBuffer<float> out;
    out.makeCopyOf(original);
    const int samples = std::min(out.getNumSamples(), processed.getNumSamples());
    const int fade = std::min(kByPassFadeSamples, samples);
    for (int channel = 0; channel < out.getNumChannels(); ++channel)
    {
        auto *data = out.getWritePointer(channel);
        const auto *source = processed.getReadPointer(channel);
        for (int i = 0; i < fade; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(fade);
            data[i] = source[i] + t * (data[i] - source[i]);
            const int index = samples - 1 - i;
            data[index] = source[index] + t * (data[index] - source[index]);
        }
    }
    return out;
}

void Project::clearAllDirty()
{
    for (auto &note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
}

void Project::clearDirtyForPass(
    const std::vector<std::pair<int, std::uint64_t>> &dirtyNotes, int f0Start,
    int f0End, std::uint64_t f0Revision)
{
    for (const auto &[index, revision] : dirtyNotes)
    {
        if (index < 0 || index >= static_cast<int>(notes.size()))
            continue;
        auto &note = notes[static_cast<size_t>(index)];
        // D-253: solo se limpia la nota si su revisión es la que se renderizó.
        // Una segunda edición sobre la misma nota sube la revisión y conserva
        // el dirty para el pase siguiente.
        if (note.getDirtyRevision() == revision)
            note.clearDirty();
    }
    // D-253: el rango F0 se limpia solo si nadie marcó después del snapshot.
    if (f0Start >= 0 && f0End >= 0 && f0DirtyRevision == f0Revision)
    {
        f0DirtyStart = -1;
        f0DirtyEnd = -1;
    }
}

int Project::markEditedNotesDirty()
{
    editedBlockRanges.clear();
    editedBlockCursor = -1;

    const bool wholeProjectEdited =
        globalPitchOffset != 0.0f || formantShift != 0.0f;

    // Contract kept (SynthesisEngineShareTests): mark every edited note and
    // set the F0 dirty range over their whole span, exactly as before.
    int marked = 0;
    int start = -1;
    int end = -1;
    std::vector<int> selected;
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
    {
        auto &note = notes[static_cast<size_t>(i)];
        if (note.isRest())
            continue;
        if (!wholeProjectEdited && !noteHasAudioRelevantEdit(note))
            continue;
        note.markDirty();
        ++marked;
        selected.push_back(i);
        start = start < 0 ? note.getStartFrame() : std::min(start, note.getStartFrame());
        end = end < 0 ? note.getEndFrame() : std::max(end, note.getEndFrame());
    }
    if (marked > 0)
        markNoteEditDirtyRange(start, end);
    if (selected.empty())
        return 0;

    std::stable_sort(selected.begin(), selected.end(), [&](int a, int b) {
        return notes[static_cast<size_t>(a)].getStartFrame() <
               notes[static_cast<size_t>(b)].getStartFrame();
    });

    // D-197: independent walk over the same edited notes, grouped into blocks
    // separated by a D-129 silence. It does not touch dirty flags; the
    // controller pulls one padded range per render.
    const auto &source = audioData.originalWaveform.getNumSamples() > 0
                             ? audioData.originalWaveform
                             : audioData.waveform;
    constexpr int kHop = HOP_SIZE;
    const int frameCount = static_cast<int>(audioData.f0.size());

    std::vector<int> blockNotes;
    int blockStart = -1;
    int blockEnd = -1;
    int previousEnd = -1;
    int previousSourceEnd = -1;

    struct BlockSpan
    {
        int first;
        int last;
    };
    std::vector<BlockSpan> blocks;

    auto flushBlock = [&]() {
        if (blockNotes.empty())
            return;
        blocks.push_back({blockStart, blockEnd});
        blockNotes.clear();
    };

    for (int index : selected)
    {
        const auto &note = notes[static_cast<size_t>(index)];
        bool sameBlock = blockNotes.empty();
        if (!sameBlock && !wholeProjectEdited && previousSourceEnd >= 0)
        {
            // D-262 (T8): the silence that separates blocks is measured on the
            // source span; moving a note in the timeline does not invent or
            // remove silence in the audio.
            const bool silence = engineBlocks::findSilenceRun(
                source, audioData.sampleRate, previousSourceEnd * kHop,
                note.getSrcStartFrame() * kHop).has_value();
            sameBlock = !silence;
        }
        if (!sameBlock)
            flushBlock();
        if (blockNotes.empty())
        {
            blockStart = note.getStartFrame();
            blockEnd = note.getEndFrame();
        }
        else
        {
            blockStart = std::min(blockStart, note.getStartFrame());
            blockEnd = std::max(blockEnd, note.getEndFrame());
        }
        blockNotes.push_back(index);
        previousEnd = note.getEndFrame();
        previousSourceEnd = note.getSrcEndFrame();
    }
    flushBlock();

    // D-248: el padding de 60 frames da contexto dentro del bloque, pero no
    // puede cruzar el silencio hacia el bloque vecino. Antes, un bloque cuyo
    // borde quedaba a menos de 60 frames del siguiente (silencio minimo de
    // D-129 = 20 ms) extendia su rango de commit DENTRO del bloque vecino y
    // reescribia su ataque con otra fase de grano (medido: S6, 0,607 de
    // diferencia en el bloque que no se edito). Se recorta en el punto medio
    // entre bloques, que por construccion cae en silencio.
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        int rangedStart = std::max(0, blocks[i].first - 60);
        int rangedEnd = frameCount > 0 ? std::min(frameCount, blocks[i].last + 60)
                                       : blocks[i].last + 60;
        if (i > 0)
            rangedStart = std::max(
                rangedStart, (blocks[i - 1].last + blocks[i].first) / 2);
        if (i + 1 < blocks.size())
            rangedEnd = std::min(
                rangedEnd, (blocks[i].last + blocks[i + 1].first) / 2);
        if (rangedEnd > rangedStart)
            editedBlockRanges.emplace_back(rangedStart, rangedEnd);
    }

    return marked;
}

bool Project::hasEditedBlockPlan() const
{
    return !editedBlockRanges.empty();
}

bool Project::nextEditedBlockRange(int &startFrame, int &endFrame)
{
    if (editedBlockCursor + 1 >= static_cast<int>(editedBlockRanges.size()))
    {
        editedBlockRanges.clear();
        editedBlockCursor = -1;
        return false;
    }
    ++editedBlockCursor;
    startFrame = editedBlockRanges[static_cast<size_t>(editedBlockCursor)].first;
    endFrame = editedBlockRanges[static_cast<size_t>(editedBlockCursor)].second;
    return true;
}
bool Project::hasDirtyNotes() const
{
    for (const auto &note : notes)
    {
        if (note.isDirty())
            return true;
    }
    return false;
}

void Project::setF0DirtyRange(int startFrame, int endFrame)
{
    if (f0DirtyStart < 0 || startFrame < f0DirtyStart)
        f0DirtyStart = startFrame;
    if (f0DirtyEnd < 0 || endFrame > f0DirtyEnd)
        f0DirtyEnd = endFrame;
    ++f0DirtyRevision;
}

void Project::markNoteEditDirtyRange(int startFrame, int endFrame)
{
    if (startFrame > endFrame)
        return;

    // Covers the base-pitch glide into neighbouring notes (the 80 ms
    // smoothing window reaches ~4 frames past a note) and the splice fades
    // (~1 frame), with plenty of margin. The synthesiser extends the render
    // window to the surrounding voiced phrase on its own.
    constexpr int kPaddingFrames = 60;

    const int frameCount = static_cast<int>(audioData.f0.size());
    setF0DirtyRange(std::max(0, startFrame - kPaddingFrames),
                    std::min(frameCount, endFrame + kPaddingFrames));
}

void Project::clearF0DirtyRange()
{
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
}

bool Project::hasF0DirtyRange() const
{
    return f0DirtyStart >= 0 && f0DirtyEnd >= 0;
}

std::pair<int, int> Project::getF0DirtyRange() const
{
    return {f0DirtyStart, f0DirtyEnd};
}

std::pair<int, int> Project::getDirtyFrameRange() const
{
    int minStart = -1;
    int maxEnd = -1;

    // Check dirty notes
    for (const auto &note : notes)
    {
        if (note.isDirty())
        {
            if (minStart < 0 || note.getStartFrame() < minStart)
                minStart = note.getStartFrame();
            if (maxEnd < 0 || note.getEndFrame() > maxEnd)
                maxEnd = note.getEndFrame();
        }
    }

    // Also include the F0 dirty range from pitch-curve edits
    if (f0DirtyStart >= 0)
    {
        if (minStart < 0 || f0DirtyStart < minStart)
            minStart = f0DirtyStart;
    }
    if (f0DirtyEnd >= 0)
    {
        if (maxEnd < 0 || f0DirtyEnd > maxEnd)
            maxEnd = f0DirtyEnd;
    }

    return {minStart, maxEnd};
}

std::vector<float> Project::getAdjustedF0() const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Compose base + delta as dense curve; UV blending is handled downstream
    // by synthesis masks, so we do not zero F0 here.
    std::vector<float> adjustedF0 = PitchCurveProcessor::composeF0(*this,
                                                                   /*applyUvMask=*/false,
                                                                   globalPitchOffset);

    return adjustedF0;
}

float Project::maxCurveNotesDisagreement() const
{
    const auto &audio = audioData;
    const int frames = static_cast<int>(audio.f0.size());
    if (frames <= 0 || notes.empty())
        return 0.0f;

    // D-286: la referencia es la BASE, no la curva compuesta. La base sale solo
    // de los centros de las notas (BasePitchCurve), asi que la vibracion, el
    // portamento o el drift que viven en el delta no cuentan: son parte de la
    // fuente y no un desfase. Medido en vivo (sesion del usuario, 24-11): la
    // curva compuesta se separaba hasta 0,7489 st del centro de una nota sana y
    // la base quedaba a 0,0000 st en las 42 notas; con la compuesta, el arreglo
    // al cargar se disparaba en un proyecto sano y lo re-renderizaba sin
    // arreglar nada.
    const bool haveBase =
        audio.basePitch.size() == static_cast<size_t>(frames);

    float worst = 0.0f;
    for (const auto &note : notes)
    {
        if (note.isRest())
            continue;
        const int start = std::max(0, note.getStartFrame());
        const int end = std::min(frames, note.getEndFrame());
        if (end <= start)
            continue;
        const int middle = start + (end - start) / 2;

        float referenceMidi = 0.0f;
        if (haveBase)
            referenceMidi = audio.basePitch[static_cast<size_t>(middle)];
        if (!(referenceMidi > 0.0f) || !std::isfinite(referenceMidi))
        {
            // Sin base en ese frame: la compuesta es lo unico que hay.
            const float hz = audio.f0[static_cast<size_t>(middle)];
            if (!(hz > 0.0f) || !std::isfinite(hz))
                continue;
            referenceMidi = freqToMidi(hz);
        }

        worst = std::max(worst, std::abs(referenceMidi - note.getAdjustedMidiNote()));
    }
    return worst;
}

std::vector<float> Project::getAdjustedF0ForRange(int startFrame, int endFrame) const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Clamp range
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(audioData.basePitch.size()));

    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = audioData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta + globalPitchOffset;
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    return adjustedF0;
}

void Project::setLoopRange(double startSeconds, double endSeconds)
{
    if (startSeconds > endSeconds)
        std::swap(startSeconds, endSeconds);

    const double duration = audioData.getDuration();
    if (duration > 0.0)
    {
        startSeconds = juce::jlimit(0.0, duration, startSeconds);
        endSeconds = juce::jlimit(0.0, duration, endSeconds);
    }

    loopRange.startSeconds = startSeconds;
    loopRange.endSeconds = endSeconds;
    loopRange.enabled = loopRange.endSeconds > loopRange.startSeconds;
}

void Project::setLoopEnabled(bool enabled)
{
    if (enabled && loopRange.endSeconds <= loopRange.startSeconds)
        loopRange.enabled = false;
    else
        loopRange.enabled = enabled;
}

void Project::clearLoopRange()
{
    loopRange = {};
}

void Project::setUseProjectKey(bool inherit)
{
    if (useProjectKey == inherit) return;
    if (!inherit) {
        scaleRootNote = getScaleRootNote();
        preferredScaleMode = getPreferredScaleMode();
    }
    useProjectKey = inherit;
    modified = true;
}

void Project::setProjectKey(int root, ScaleMode mode)
{
    macroParameters->projectKeyRoot = juce::jlimit(0, 11, root);
    if (mode >= ScaleMode::Major && mode <= ScaleMode::WholeTone)
        macroParameters->projectKeyMode = mode;
    macroParameters->projectKeyInitialized = true;
    modified = true;
}

void Project::setScaleMode(ScaleMode mode)
{
    bool changed = false;
    if (mode != ScaleMode::None && mode != ScaleMode::Chromatic &&
        preferredScaleMode != mode)
    {
        preferredScaleMode = mode;
        changed = true;
    }

    if (macroParameters->scaleMode != mode)
    {
        macroParameters->scaleMode = mode;
        changed = true;
    }

    if (changed)
        modified = true;
}

void Project::setPreferredScaleMode(ScaleMode mode)
{
    setUseProjectKey(false);
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic ||
        preferredScaleMode == mode)
        return;

    preferredScaleMode = mode;
    modified = true;
}

void Project::setScaleRootNote(int noteInOctave)
{
    setUseProjectKey(false);
    const int normalized = juce::jlimit(-1, 11, noteInOctave);
    if (scaleRootNote == normalized)
        return;

    scaleRootNote = normalized;
    modified = true;
}

void Project::setPitchReferenceHz(int hz)
{
    const int normalized = juce::jlimit(420, 450, hz);
    if (macroParameters->pitchReferenceHz == normalized)
        return;

    macroParameters->pitchReferenceHz = normalized;
    modified = true;
}

void Project::setSnapToSemitones(bool enabled)
{
    if (macroParameters->snapToSemitones == enabled)
        return;

    macroParameters->snapToSemitones = enabled;
    modified = true;
}

void Project::setDragSnapMode(DragSnapMode mode)
{
    if (macroParameters->dragSnapMode == mode)
        return;

    macroParameters->dragSnapMode = mode;
    modified = true;
}

void Project::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    if (macroParameters->timelineDisplayMode == mode)
        return;

    macroParameters->timelineDisplayMode = mode;
    modified = true;
}

void Project::setTimelineBeatSignature(int numerator, int denominator)
{
    const int normalizedNumerator = normalizeBeatNumerator(numerator);
    const int normalizedDenominator = normalizeBeatDenominator(denominator);

    if (macroParameters->timelineBeatNumerator == normalizedNumerator &&
        macroParameters->timelineBeatDenominator == normalizedDenominator)
        return;

    macroParameters->timelineBeatNumerator = normalizedNumerator;
    macroParameters->timelineBeatDenominator = normalizedDenominator;
    modified = true;
}

void Project::setTimelineTempoBpm(double bpm)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    if (std::abs(macroParameters->timelineTempoBpm - normalized) < 1.0e-6)
        return;

    macroParameters->timelineTempoBpm = normalized;
    modified = true;
}

void Project::setTimelineGridDivision(TimelineGridDivision division)
{
    const auto normalized = normalizeGridDivision(division);
    if (macroParameters->timelineGridDivision == normalized)
        return;

    macroParameters->timelineGridDivision = normalized;
    modified = true;
}

void Project::setTimelineSnapCycle(bool enabled)
{
    if (macroParameters->timelineSnapCycle == enabled)
        return;

    macroParameters->timelineSnapCycle = enabled;
    modified = true;
}

void Project::setMacroParameters(std::shared_ptr<MacroParameters> parameters)
{
    if (parameters)
        macroParameters = std::move(parameters);
}
