// N12 (document-scope UI, D-354): the overview strip is view state.
//
// The complaint: moving from one item to another made the bottom overview
// strip appear or disappear. It was derived from the project being activated
// (`PianoRollWorkspaceView::setProject` copied Project::thumbnailVisible into
// the view), so every item carried its own idea of the window with it.
//
// This test pins the replacement:
//   * changing item (setProject) never touches the strip;
//   * the user's toggle does, and it survives the next item change;
//   * the JSON field stays for compatibility (FORMAT_VERSION 5): it still reads
//     and writes, it just no longer drives the view.
#include "TestAssert.h"

#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/PianoRollWorkspaceView.h"

#include <iostream>

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    PianoRollComponent roll;
    roll.setSize(900, 520);
    PianoRollWorkspaceView view(roll);
    view.setSize(900, 620);

    Project itemA;
    Project itemB;
    itemA.thumbnailVisible = false;
    itemB.thumbnailVisible = false;

    // The view starts from the settings manager in production; here the
    // explicit value plays that part.
    view.setOverviewVisible(true);
    CHECK(view.isOverviewVisible());

    // Changing item does not change the strip, whichever way the item's own
    // JSON field points.
    view.setProject(&itemA);
    CHECK(view.isOverviewVisible());
    view.setProject(&itemB);
    CHECK(view.isOverviewVisible());
    itemA.thumbnailVisible = true;
    view.setProject(&itemA);
    CHECK(view.isOverviewVisible());

    // The user's toggle does change it...
    view.setOverviewVisible(false);
    CHECK(!view.isOverviewVisible());

    // ...and the next item change respects the user, not the project field.
    itemB.thumbnailVisible = true;
    view.setProject(&itemB);
    CHECK(!view.isOverviewVisible());

    view.setOverviewVisible(true);
    itemB.thumbnailVisible = false;
    view.setProject(&itemB);
    CHECK(view.isOverviewVisible());

    // A null project (document closed) leaves the view state alone too.
    view.setProject(nullptr);
    CHECK(view.isOverviewVisible());

    // Compatibility: the field still round-trips through the JSON.
    Project saved;
    saved.thumbnailVisible = true;
    const auto json = ProjectSerializer::toJson(saved);
    Project loaded;
    loaded.thumbnailVisible = false;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.thumbnailVisible);
    CHECK(ProjectSerializer::FORMAT_VERSION == 5);

    std::cout << "Overview independence tests passed\n";
    return 0;
}
