// N12 (document-scope UI, D-354): every item of a track looks the same.
//
// The complaint: clicking a note of another item darkened that item's zone,
// changed the note colours and moved the boundaries. Track mode used to paint
// the active item through NoteRenderer and the others through a second path
// with its own grey gradient, its own opacity and a white wash - so the look of
// an item depended on which one was active.
//
// This test pins the replacement:
//   * the style a track item asks for takes no "active" and is the uniform one
//     (full opacity, no greyscale, no desaturation, steel-blue palette);
//   * a pinned clip of another track is the documented exception (dimmed);
//   * the boundary lines are deduplicated within one pixel, so two contiguous
//     items share one line;
//   * the same item painted as the active one and as a neighbour produces the
//     same pixels in its own box.
#include "TestAssert.h"

#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRoll/RegionNoteStyle.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/RegionPreview.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * SAMPLE_RATE / static_cast<double>(HOP_SIZE)));
}

void setLength(Project& project, double seconds)
{
    const auto frames = static_cast<size_t>(frameFor(seconds));
    project.getAudioData().sampleRate = SAMPLE_RATE;
    project.getAudioData().f0.assign(frames, 220.0f);
    project.getAudioData().basePitch.assign(frames, 60.0f);
    project.getAudioData().deltaPitch.assign(frames, 0.0f);
}

void addNote(Project& project, double startSeconds, double endSeconds, float midi)
{
    project.addNote(Note(frameFor(startSeconds), frameFor(endSeconds), midi));
}

juce::Image paintToImage(PianoRollComponent& roll)
{
    juce::Image image(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    juce::Graphics g(image);
    roll.paint(g);
    return image;
}

// Pixels whose channels differ by more than the given tolerance. Image
// compositing (the neighbour layer is an offscreen image) can round by one.
int differingPixels(const juce::Image& a, const juce::Image& b,
                    juce::Rectangle<int> area, int tolerance)
{
    int different = 0;
    area = area.getIntersection({0, 0, a.getWidth(), a.getHeight()});
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
        {
            const auto pa = a.getPixelAt(x, y);
            const auto pb = b.getPixelAt(x, y);
            const int delta = std::max(
                {std::abs(pa.getRed() - pb.getRed()),
                 std::abs(pa.getGreen() - pb.getGreen()),
                 std::abs(pa.getBlue() - pb.getBlue()),
                 std::abs(pa.getAlpha() - pb.getAlpha())});
            if (delta > tolerance)
                ++different;
        }
    return different;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    // 1. The style of a track item: one struct, no "active" input, uniform.
    const RegionNoteStyle style = trackRegionNoteStyle();
    CHECK(style == trackRegionNoteStyle());
    CHECK(style == trackRegionNoteStyle(true, true));
    CHECK(style.layerOpacity == 1.0f);
    CHECK(style.bodyOpacity == 1.0f);
    CHECK(!style.greyscale);
    CHECK(!style.desaturateBodies);
    CHECK(style.fillBodies);
    CHECK(style.selectionDecoration);
    CHECK(!style.outlineColour.has_value());
    CHECK(style.gradientCentre == regionNoteColours::bodyCentre);
    CHECK(style.gradientSide == regionNoteColours::bodySide);
    CHECK(style.showDeltaPitch);
    CHECK(style.showBasePitch);
    // The pitch switches are a view setting, not an item one: both items in a
    // scene ask for them with the same values.
    CHECK(trackRegionNoteStyle(false, false).showDeltaPitch == false);
    CHECK(trackRegionNoteStyle(false, false) ==
          trackRegionNoteStyle(false, false));

    // 2. Pinned clips of another track: the documented exception, still dimmed.
    const RegionNoteStyle pinned = pinnedRegionNoteStyle(juce::Colours::red);
    CHECK(pinned != style);
    CHECK(pinned.bodyOpacity < 1.0f);
    CHECK(pinned.desaturateBodies);
    CHECK(pinned.outlineColour.has_value());
    CHECK(pinned.layerOpacity == 1.0f); // the layer itself is not dimmed twice

    // 3. Boundaries: deduplicated within one pixel.
    CHECK(dedupeRegionEdges({100.0f, 100.4f}).size() == 1);
    CHECK(dedupeRegionEdges({100.0f, 101.2f}).size() == 2);
    CHECK(dedupeRegionEdges({400.0f, 200.0f, 200.5f, 0.0f}).size() == 3);
    CHECK(dedupeRegionEdges({}).empty());

    // 4. Paint: the same item, once as the active one and once as a neighbour,
    //    comes out the same in its own box. The canvas display offset is 0 in
    //    both scenes (each preview's project zero is the host zero), so the
    //    grid, the ruler and the box geometry are literally the same pixels;
    //    only which item is active changes.
    Project itemA;
    Project itemB;
    setLength(itemA, 0.5);
    setLength(itemB, 1.5);
    addNote(itemA, 0.02, 0.20, 74.0f);
    addNote(itemB, 1.03, 1.22, 72.0f);

    PianoRollComponent roll;
    roll.setSize(900, 520);
    roll.setTrackViewMode(true);
    roll.setScrollX(0);
    roll.setScrollY(0);
    roll.setTimelineDisplayOffset(0.0);
    roll.fitPitchRangeToView(58.0f, 78.0f);

    MainViewRegionPreview previewA;
    previewA.key = "itA";
    previewA.name = "itA";
    previewA.project = &itemA;
    previewA.startSeconds = 0.0;
    previewA.endSeconds = 0.5;
    previewA.sourceStartSeconds = 0.0;
    previewA.sourceEndSeconds = 0.5;
    previewA.projectHostOffsetSeconds = 0.0;
    previewA.hasContent = true;

    MainViewRegionPreview previewB;
    previewB.key = "itB";
    previewB.name = "itB";
    previewB.project = &itemB;
    previewB.startSeconds = 1.0;
    previewB.endSeconds = 1.5;
    previewB.sourceStartSeconds = 1.0;
    previewB.sourceEndSeconds = 1.5;
    previewB.projectHostOffsetSeconds = 0.0;
    previewB.hasContent = true;

    // B as a neighbour of the active A.
    auto aActive = std::make_shared<std::vector<MainViewRegionPreview>>();
    previewA.active = true;
    previewB.active = false;
    aActive->push_back(previewA);
    aActive->push_back(previewB);
    roll.setProject(&itemA);
    roll.setRegionPreviews(aActive);
    const auto bAsNeighbour = paintToImage(roll);

    // B as the active item, A as its neighbour.
    auto bActive = std::make_shared<std::vector<MainViewRegionPreview>>();
    previewA.active = false;
    previewB.active = true;
    bActive->push_back(previewA);
    bActive->push_back(previewB);
    roll.setProject(&itemB);
    roll.setRegionPreviews(bActive);
    const auto bAsActive = paintToImage(roll);

    // B's own box, one pixel inside its cut lines.
    const float pps = roll.getPixelsPerSecond();
    const int left = PianoRollComponent::pianoKeysWidth +
                     static_cast<int>(std::ceil(1.0 * pps - roll.getScrollX())) + 2;
    const int right = PianoRollComponent::pianoKeysWidth +
                      static_cast<int>(std::floor(1.5 * pps - roll.getScrollX())) - 2;
    const juce::Rectangle<int> bBox(
        left, PianoRollComponent::headerHeight, std::max(0, right - left),
        roll.getHeight() - PianoRollComponent::headerHeight);
    CHECK(bBox.getWidth() > 10);

    // Together they occupy the whole sample height, so the old grey gradient
    // and the 3 % wash both land inside this box.
    const int differing = differingPixels(bAsNeighbour, bAsActive, bBox, 2);
    if (differing != 0)
        std::cout << "differing pixels in the item box: " << differing << "\n";
    CHECK(differing == 0);

    std::cout << "Region uniform looks tests passed\n";
    return 0;
}
