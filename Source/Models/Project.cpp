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
    if (targetNotes.size() != renderedNotes.size())
        return;
    for (size_t i = 0; i < targetNotes.size(); ++i)
    {
        targetNotes[i].setRenderedEdit(renderedNotes[i].hasRenderedEdit());
        targetNotes[i].setSynthDirty(renderedNotes[i].isSynthDirty());
        targetNotes[i].clearDirty();
    }
}

bool Project::shouldResetNeutralNoteToOriginal(const Note &note) const
{
    constexpr float kGlobalEditEpsilon = 1.0e-6f;
    if (note.isRest() || !note.isDirty())
        return false;
    if (note.getEngine() != NoteEngine::Inherit)
        return false;
    if (std::abs(globalPitchOffset) > kGlobalEditEpsilon ||
        std::abs(formantShift) > kGlobalEditEpsilon)
        return false;
    return note.isNeutralForOriginalWaveform();
}

bool Project::hasRegionEdits() const
{
    if (globalPitchOffset != 0.0f || formantShift != 0.0f)
        return true;

    for (const auto &note : notes)
    {
        if (note.isRest())
            continue;
        if (note.getEngine() != NoteEngine::Inherit)
            return true;
        if (!note.isNeutralForOriginalWaveform())
            return true;
    }

    return false;
}

void Project::clearAllDirty()
{
#define DBG_P1B(msg)                                                          \
  juce::File::getSpecialLocation(juce::File::tempDirectory)                   \
      .getChildFile("pitchcore_p1b_trace.log")                                \
      .appendText("[" + juce::Time::getCurrentTime().toString(true, true, true, true) + \
                  "][tid=" + juce::String::toHexString((juce::int64)(juce::pointer_sized_int)juce::Thread::getCurrentThreadId()) + \
                  "] " + juce::String(msg) + "\n")
    DBG_P1B("clearAllDirty BEFORE this=" +
            juce::String::toHexString((juce::int64)(juce::pointer_sized_int)this) +
            " dirtyNotes=" + juce::String((int)getDirtyNotes().size()) +
            " f0Dirty=" + juce::String(hasF0DirtyRange() ? 1 : 0));
    for (auto &note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
    DBG_P1B("clearAllDirty AFTER this=" +
            juce::String::toHexString((juce::int64)(juce::pointer_sized_int)this) +
            " dirtyNotes=" + juce::String((int)getDirtyNotes().size()) +
            " f0Dirty=" + juce::String(hasF0DirtyRange() ? 1 : 0));
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
        if (!wholeProjectEdited && note.isNeutralForOriginalWaveform())
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

    auto flushBlock = [&]() {
        if (blockNotes.empty())
            return;
        const int rangedStart = std::max(0, blockStart - 60);
        const int rangedEnd = frameCount > 0 ? std::min(frameCount, blockEnd + 60)
                                             : blockEnd + 60;
        if (rangedEnd > rangedStart)
            editedBlockRanges.emplace_back(rangedStart, rangedEnd);
        blockNotes.clear();
    };

    for (int index : selected)
    {
        const auto &note = notes[static_cast<size_t>(index)];
        bool sameBlock = blockNotes.empty();
        if (!sameBlock && !wholeProjectEdited && previousEnd >= 0)
        {
            const bool silence = engineBlocks::findSilenceRun(
                source, audioData.sampleRate, previousEnd * kHop,
                note.getStartFrame() * kHop).has_value();
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
    }
    flushBlock();

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
