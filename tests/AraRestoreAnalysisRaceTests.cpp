// D-204: veto del re-analisis cuando existe un archivo ARA restaurado valido.
// Rojo antes de la politica: este archivo no compila sin
// Source/Plugin/AraRestoreAnalysisPolicy.h.
#include "AraRestoreAnalysisPolicy.h"
#include "TestAssert.h"

#include <iostream>

using namespace ara_restore;

namespace
{
void testT1ValidArchiveVetoesAnalysis()
{
    CHECK(shouldVetoAnalysis(true, false));
    CHECK(!shouldVetoAnalysis(true, true));
    CHECK(!shouldVetoAnalysis(false, false));
}

void testT2HydrateWithoutRegeneration()
{
    CHECK(decideRestoreAction(true, true, false) == Decision::Hydrate);
    CHECK(decideRestoreAction(true, false, false) == Decision::Pending);
    CHECK(decideRestoreAction(false, false, false) == Decision::Analyze);
    CHECK(decideRestoreAction(true, false, true) == Decision::Analyze);
}

void testT3NoArchiveAnalyses()
{
    CHECK(!shouldVetoAnalysis(false, false));
    CHECK(decideRestoreAction(false, false, false) == Decision::Analyze);
}

void testT4RestoredArchiveIsNotOverwritten()
{
    CHECK(!shouldOverwriteArchive(true, false));
    CHECK(shouldOverwriteArchive(true, true));
    CHECK(shouldOverwriteArchive(false, false));
}

void testT6HydrationFailureWarnsAndPends()
{
    CHECK(shouldWarnHydrationFailure(true, false, false));
    CHECK(!shouldWarnHydrationFailure(true, true, false));
    CHECK(!shouldWarnHydrationFailure(false, false, false));
    CHECK(!shouldWarnHydrationFailure(true, false, true));
}

void testArchivedIdMatchesAbsoluteToRelative()
{
    CHECK(archivedIdMatches(
        "C:\\Users\\GumiPC\\AppData\\Local\\Temp\\opencode\\Media\\1-Original.wav",
        "Media\\1-Original.wav"));
    CHECK(archivedIdMatches("Media\\1-Original render 001.wav",
                            "Media\\1-Original render 001.wav"));
    CHECK(!archivedIdMatches("Media\\1-Original.wav",
                             "Media\\1-Original render 001.wav"));
    CHECK(archivedIdMatches("C:\\X\\A.WAV", "c:/x/a.wav"));
    CHECK(!archivedIdMatches("", "Media\\1-Original.wav"));
}

void testArchivedIdMatchesBySourceName()
{
    CHECK(archivedIdMatches("C:\\X\\1-Original.wav", "guid-1234",
                            "1-Original.wav"));
    CHECK(!archivedIdMatches("C:\\X\\1-Original.wav", "guid-1234",
                             "1-Original render 001.wav"));
}
} // namespace

int main()
{
    testT1ValidArchiveVetoesAnalysis();
    testT2HydrateWithoutRegeneration();
    testT3NoArchiveAnalyses();
    testT4RestoredArchiveIsNotOverwritten();
    testT6HydrationFailureWarnsAndPends();
    testArchivedIdMatchesAbsoluteToRelative();
    testArchivedIdMatchesBySourceName();

    std::cout << "ARA restore analysis race tests passed\n";
    return 0;
}
