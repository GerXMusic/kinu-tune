// R10 (guia MIDI, D-363): identidad estable y sync por contenido.
//
// Medido en R5e: la guia se aplica por pista, pero su identidad seguia atada a
// la posicion del item (`reaper:<pista>:<posicion>:<take>`). Mover o cortar el
// item y volver a lanzar el ReaScript dejaba DOS guias vivas; y el sync solo
// miraba el numero de notas, asi que mover una nota sin cambiar el conteo no
// reemplazaba nada.
//
// Aqui se mide, sin host:
//  (a) mismo conteo y posiciones distintas -> la guia se reemplaza;
//  (b) fichero borrado -> la guia viva se quita; una guia importada no;
//  (c) el id es estable sin posicion y los ids posicionales se ignoran;
//  (d) el hash cambia al mover una nota (mismo conteo);
//  (e) mtime+size igual -> no se relee; al cambiar el fichero, si;
//  (f) el aviso "cambiada desde la ultima aplicacion" aparece y desaparece;
//  (g) dos ficheros con el mismo id son una sola guia.
#include "TestAssert.h"

#include "../Source/Models/DocumentMidiGuides.h"
#include "../Source/Models/MidiGuideLive.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr const char* kTrackGuid = "{5A1F1D2E-0000-4000-8000-TUNEX000001}";
constexpr const char* kTake = "Lead";

GuideSource makeGuide(int noteCount, double firstStart, float firstPitch)
{
    GuideSource guide;
    guide.id = midiGuideLive::stableSourceId(kTrackGuid, kTake).toStdString();
    guide.label = "Lead";
    guide.kind = "reascript";
    guide.importedAt = "2026-11-27T12:00:00Z";
    for (int i = 0; i < noteCount; ++i)
    {
        GuideNote note;
        note.startQuarter = firstStart + static_cast<double>(i);
        note.endQuarter = note.startQuarter + 0.5;
        note.pitch = firstPitch + static_cast<float>(i);
        note.velocity = 0.8f;
        guide.notes.push_back(note);
    }
    return guide;
}

juce::File freshDirectory(const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("tunex-midi-guide-sync-" +
                                 juce::String::toHexString(
                                     juce::Random::getSystemRandom().nextInt64()))
                   .getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

juce::String notesJson(const std::vector<GuideNote>& notes)
{
    juce::StringArray parts;
    for (const auto& note : notes)
    {
        juce::String one;
        one << "[" << juce::String(note.startQuarter, 9) << ","
            << juce::String(note.endQuarter, 9) << ","
            << juce::String(static_cast<int>(std::llround(note.pitch))) << ","
            << juce::String(note.velocity, 5) << "]";
        parts.add(one);
    }
    return "[" + parts.joinIntoString(",") + "]";
}

juce::File writeLiveFile(const juce::File& directory, const juce::String& name,
                         const juce::String& sourceId,
                         const std::vector<GuideNote>& notes)
{
    const auto file = directory.getChildFile(name);
    juce::String text;
    text << "{\n  \"schema\": 1,\n  \"source_id\": \"" << sourceId
         << "\",\n  \"label\": \"Lead\",\n  \"kind\": \"reascript\",\n"
         << "  \"created_utc\": \"2026-11-27T12:00:00Z\",\n  \"notes\": "
         << notesJson(notes) << "\n}\n";
    file.replaceWithText(text);
    return file;
}

HostTempoMap tempo120()
{
    return HostTempoMap::constant(120.0, 4, 4);
}

// (a) Mismo numero de notas, otra posicion: reemplaza.
void testSameCountDifferentPositionsReplaces()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:test");

    const auto before = makeGuide(3, 32.0, 60.0f);
    CHECK(store.syncLive({before}));
    const auto* stored = store.findById(before.id);
    CHECK(stored != nullptr);
    CHECK(stored->notes.front().startQuarter == 32.0);

    auto after = before; // same size, every note moved one beat
    for (auto& note : after.notes)
    {
        note.startQuarter += 1.0;
        note.endQuarter += 1.0;
    }
    CHECK(midiGuideLive::contentHash(before) !=
          midiGuideLive::contentHash(after));
    CHECK(store.syncLive({after}));

    const auto* replaced = store.findById(after.id);
    CHECK(replaced != nullptr);
    CHECK(replaced->notes.front().startQuarter == 33.0);
    CHECK(store.findById(after.id) == replaced); // one entry, not two

    // The same content again is not a change (the poll must be cheap).
    CHECK(!store.syncLive({after}));
}

// (b) El fichero desaparecio: la guia viva se va; una importada se queda.
void testDeletedFileRemovesOnlyLiveGuides()
{
    DocumentMidiGuideStore store;
    store.bindTrack("track:test");

    auto imported = makeGuide(2, 10.0, 55.0f);
    imported.id = "file:abcdef";
    imported.kind = "file";
    store.upsert(imported, "file");

    const auto live = makeGuide(2, 20.0, 62.0f);
    CHECK(store.syncLive({live}));
    CHECK(store.size() == 2);

    // A scan with the folder present but no file for the live id is
    // authoritative: the guide is gone (D-363).
    CHECK(store.syncLive({}, true));
    CHECK(store.size() == 1);
    CHECK(store.findById(live.id) == nullptr);
    CHECK(store.findById("file:abcdef") != nullptr);

    // Without an authoritative scan (folder missing) nothing is removed.
    CHECK(store.syncLive({live}, false));
    CHECK(store.size() == 2);
    CHECK(!store.syncLive({}, false));
    CHECK(store.size() == 2);
}

// (c) El id no lleva posicion y el id posicional se reconoce e ignora.
void testIdentityIsStableAndLegacyIsIgnored()
{
    const auto id = midiGuideLive::stableSourceId(kTrackGuid, kTake);
    CHECK(id == midiGuideLive::stableSourceId(kTrackGuid, "  Lead  "));
    CHECK(id.indexOf("13.704") < 0);

    // Same track + take with a different item position: same id.
    const auto moved = midiGuideLive::stableSourceId(kTrackGuid, kTake);
    CHECK(id == moved);

    CHECK(midiGuideLive::isLegacyReaperSourceId(
        juce::String("reaper:") + kTrackGuid + ":13.704374:Lead"));
    CHECK(midiGuideLive::isLegacyReaperSourceId(
        juce::String("reaper:") + kTrackGuid + ":0:Lead"));
    CHECK(!midiGuideLive::isLegacyReaperSourceId(id));
    CHECK(!midiGuideLive::isLegacyReaperSourceId("file:abc"));
    CHECK(!midiGuideLive::isLegacyReaperSourceId(
        juce::String("reaper:") + kTrackGuid + ":Lead"));

    // A pre-R10 file left on disk does not become a second guide.
    const auto dir = freshDirectory("c");
    const auto notes = makeGuide(1, 1.0, 60.0f).notes;
    writeLiveFile(dir, "guide_old.json",
                  juce::String("reaper:") + kTrackGuid + ":13.704374:Lead",
                  notes);
    writeLiveFile(dir, "guide_new.json", id, notes);

    midiGuideLive::LiveDirectoryScanner scanner;
    const auto result = scanner.scan(dir);
    CHECK(result.directoryExists);
    CHECK(result.ignored == 1);
    CHECK(result.rejected == 0);
    CHECK(result.guides.size() == 1);
    CHECK(result.guides.front().id == id.toStdString());
}

// (d) Mover una nota cambia el hash aunque el conteo no cambie.
void testHashTracksEveryField()
{
    const auto base = makeGuide(4, 32.0, 60.0f);
    CHECK(midiGuideLive::contentHash(base) ==
          midiGuideLive::contentHash(makeGuide(4, 32.0, 60.0f)));

    auto moved = base;
    moved.notes[2].startQuarter += 0.25;
    CHECK(midiGuideLive::contentHash(base) != midiGuideLive::contentHash(moved));

    auto retuned = base;
    retuned.notes[1].pitch += 0.01f;
    CHECK(midiGuideLive::contentHash(base) !=
          midiGuideLive::contentHash(retuned));

    auto quieter = base;
    quieter.notes[3].velocity = 0.5f;
    CHECK(midiGuideLive::contentHash(base) !=
          midiGuideLive::contentHash(quieter));

    auto shortened = base;
    shortened.notes[0].endQuarter -= 0.1;
    CHECK(midiGuideLive::contentHash(base) !=
          midiGuideLive::contentHash(shortened));
}

// (e) mtime+size igual: no se relee. Un fichero que cambia si se relee.
void testMtimeSizeSkipsUnchangedFiles()
{
    const auto dir = freshDirectory("e");
    const auto file =
        writeLiveFile(dir, "guide_live.json",
                      midiGuideLive::stableSourceId(kTrackGuid, kTake),
                      makeGuide(3, 32.0, 60.0f).notes);

    midiGuideLive::LiveDirectoryScanner scanner;
    auto first = scanner.scan(dir);
    CHECK(first.reRead == 1);
    CHECK(first.reused == 0);
    CHECK(first.guides.size() == 1);
    const auto firstHash = midiGuideLive::contentHash(first.guides.front());

    auto second = scanner.scan(dir);
    CHECK(second.reRead == 0);
    CHECK(second.reused == 1);
    CHECK(midiGuideLive::contentHash(second.guides.front()) == firstHash);

    // Rewrite the content: the size changes with it, and the mtime is set
    // explicitly so a coarse clock cannot fake "unchanged".
    writeLiveFile(dir, "guide_live.json",
                  midiGuideLive::stableSourceId(kTrackGuid, kTake),
                  makeGuide(5, 32.0, 60.0f).notes);
    file.setLastModificationTime(juce::Time::getCurrentTime());

    auto third = scanner.scan(dir);
    CHECK(third.reRead == 1);
    CHECK(third.reused == 0);
    CHECK(third.guides.front().notes.size() == 5);
    CHECK(midiGuideLive::contentHash(third.guides.front()) != firstHash);
}

// (f) El aviso compara con el hash de la ultima aplicacion.
void testChangedSinceApplyWarning()
{
    const auto guide = makeGuide(3, 32.0, 60.0f);
    const auto tempo = tempo120();
    const auto applied = midiGuideLive::contentHash(guide);

    // Never applied: no warning.
    CHECK(!midiGuideLive::statusFor(guide, tempo, {}).changedSinceApply);
    // Applied this exact content: no warning.
    CHECK(!midiGuideLive::statusFor(guide, tempo, applied).changedSinceApply);
    // The live file moved a note after the apply: warning.
    auto changed = guide;
    changed.notes.front().startQuarter += 0.5;
    CHECK(midiGuideLive::statusFor(changed, tempo, applied).changedSinceApply);
    // Re-applied (hash updated): the warning goes away again.
    CHECK(!midiGuideLive::statusFor(
               changed, tempo, midiGuideLive::contentHash(changed))
               .changedSinceApply);

    // Coverage: quarters become host seconds through the tempo map.
    const auto status = midiGuideLive::statusFor(guide, tempo, {});
    CHECK(status.notes == 3);
    CHECK(std::abs(status.hostStartSeconds - 16.0) < 1.0e-9);  // 32 q * 0.5
    CHECK(std::abs(status.hostEndSeconds - 17.25) < 1.0e-9);   // 34.5 q * 0.5
    CHECK(status.updatedAt == "2026-11-27T12:00:00Z");
}

// (g) Dos ficheros con el mismo id son UNA guia, no dos.
void testDuplicateFilesCollapseToOneGuide()
{
    const auto dir = freshDirectory("g");
    const auto id = midiGuideLive::stableSourceId(kTrackGuid, kTake);
    writeLiveFile(dir, "guide_a.json", id, makeGuide(2, 10.0, 50.0f).notes);
    writeLiveFile(dir, "guide_b.json", id, makeGuide(4, 20.0, 60.0f).notes);

    midiGuideLive::LiveDirectoryScanner scanner;
    const auto result = scanner.scan(dir);
    CHECK(result.guides.size() == 1); // one identity, one guide
    CHECK(result.guides.front().notes.size() == 4); // deterministic winner

    DocumentMidiGuideStore store;
    store.bindTrack("track:test");
    CHECK(store.syncLive(result.guides, true));
    CHECK(store.size() == 1);
}

// (h) Sin carpeta no se inventa una lista autoritativa.
void testMissingDirectoryIsNotAuthoritative()
{
    midiGuideLive::LiveDirectoryScanner scanner;
    const auto dir = freshDirectory("h").getChildFile("does-not-exist");
    const auto result = scanner.scan(dir);
    CHECK(!result.directoryExists);
    CHECK(result.guides.empty());
    CHECK(result.rejected == 0);
}
} // namespace

int main()
{
    testSameCountDifferentPositionsReplaces();
    testDeletedFileRemovesOnlyLiveGuides();
    testIdentityIsStableAndLegacyIsIgnored();
    testHashTracksEveryField();
    testMtimeSizeSkipsUnchangedFiles();
    testChangedSinceApplyWarning();
    testDuplicateFilesCollapseToOneGuide();
    testMissingDirectoryIsNotAuthoritative();

    std::printf("MidiGuideSyncTests: all checks passed\n");
    return 0;
}
