// D-178: a new Kinu Tune editor closes the previous host window, so only one
// editor remains.
#include "../Source/Plugin/SingleWindowPolicy.h"
#include "TestAssert.h"

#include <iostream>

using pitchnet::shouldClosePreviousEditor;

namespace
{
void testSecondEditorClosesPrevious()
{
    CHECK(shouldClosePreviousEditor(2, false));
    CHECK(shouldClosePreviousEditor(3, false));
}

void testSingleEditorDoesNotClose()
{
    CHECK(!shouldClosePreviousEditor(1, false));
    CHECK(!shouldClosePreviousEditor(0, false));
}

void testSharedHostWindowIsNotClosed()
{
    CHECK(!shouldClosePreviousEditor(2, true));
    CHECK(!shouldClosePreviousEditor(3, true));
}
} // namespace

int main()
{
    testSecondEditorClosesPrevious();
    testSingleEditorDoesNotClose();
    testSharedHostWindowIsNotClosed();

    std::cout << "single window policy tests passed\n";
    return 0;
}
