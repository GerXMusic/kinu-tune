// D-180: with the transport stopped, a host seek must move the note view. The
// old rule only followed while playing, which left the canvas at the start.
#include "../Source/UI/CursorFollowPolicy.h"
#include "TestAssert.h"

#include <cmath>
#include <iostream>

using namespace pitchnet;

namespace
{
bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}
} // namespace

int main()
{
    // Red step: stopped seek must be followed.
    CHECK(shouldFollowCursor(false, true));
    CHECK(shouldFollowCursor(true, false));
    CHECK(!shouldFollowCursor(false, false));

    // A play start from stop drops the selection's follow suppression; a
    // selection while already playing keeps it.
    CHECK(shouldClearFollowSuppression(false, true));
    CHECK(!shouldClearFollowSuppression(true, true));
    CHECK(!shouldClearFollowSuppression(false, false));

    // Cursor outside the visible window: scroll to reveal it.
    {
        const double scroll =
            scrollXToRevealCursor(40.0, 0.0, 100.0, 1000, 0.0, 100.0);
        CHECK(near(scroll, 3700.0));
        CHECK(40.0 * 100.0 >= scroll);
        CHECK(40.0 * 100.0 <= scroll + 1000.0);
    }

    // Cursor already visible: the view does not move.
    CHECK(near(scrollXToRevealCursor(5.0, 0.0, 100.0, 1000, 0.0, 100.0),
               0.0));
    CHECK(near(scrollXToRevealCursor(0.0, 0.0, 100.0, 1000, 0.0, 100.0),
               0.0));

    // Near the timeline end the scroll clamps so the cursor stays visible.
    {
        const double scroll =
            scrollXToRevealCursor(95.0, 0.0, 100.0, 1000, 0.0, 100.0);
        CHECK(near(scroll, 9000.0));
        CHECK(95.0 * 100.0 >= scroll);
        CHECK(95.0 * 100.0 <= scroll + 1000.0);
    }

    // Negative timeline start (Track view): the clamp uses it as the floor.
    {
        const double scroll =
            scrollXToRevealCursor(5.0, -1000.0, 100.0, 1000, -10.0, 100.0);
        CHECK(near(scroll, 200.0));
        CHECK(5.0 * 100.0 >= scroll);
        CHECK(5.0 * 100.0 <= scroll + 1000.0);
    }

    // No canvas yet: leave the scroll untouched.
    CHECK(near(scrollXToRevealCursor(40.0, 123.0, 100.0, 0, 0.0, 100.0),
               123.0));

    std::cout << "Cursor follow policy tests passed\n";
    return 0;
}
