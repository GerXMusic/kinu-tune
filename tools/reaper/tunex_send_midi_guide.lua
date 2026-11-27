-- TuneX MIDI guide sender (REAPER ReaScript, F0.3/F3, R10)
--
-- Reads the selected MIDI item's active take, converts every unmuted note to
-- project quarter notes and writes the guide JSON consumed by TuneX at
--   %APPDATA%\TuneX\guides\live\guide_<id>.json   (schema 1, atomic write)
--
-- Why project QN: the guide is a song-level reference. REAPER's
-- MIDI_GetProjQNFromPPQPos already folds the item position, playrate and
-- timebase, so the JSON stays correct when the item moves or the tempo map
-- changes (the quarters do not change, the seconds do).
--
-- Identity (R10, D-363): `source_id` is `reaper:<track GUID>:<take key>`.
-- The item position is GONE: moving or splitting the item keeps the same id
-- (REAPER keeps the take name across both), so re-running the script overwrites
-- the same file instead of leaving the previous guide alive as a second guide.
-- The take key is the take name (or "midi"); ':' is replaced by '_' so the id
-- always has three fields. Old positional ids (`...:<position>:<take>`) are
-- removed here and ignored by TuneX if one survives.
--
-- Usage: select one MIDI item and run the script. It never modifies the
-- project. Run it again after editing the item to refresh the guide.
-- See plan/midi-guia-afinacion-hoja-de-ruta.md (Appendix A).

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function jsonEscape(s)
    s = s:gsub("\\", "\\\\"):gsub("\"", "\\\""):gsub("\n", "\\n")
    s = s:gsub("\r", "\\r"):gsub("\t", "\\t")
    return s
end

local function fmt(x)
    return string.format("%.9f", x)
end

local function writeAtomic(path, text)
    local tmp = path .. ".tmp"
    local f, err = io.open(tmp, "wb")
    if not f then
        return false, err
    end
    f:write(text)
    f:close()
    os.remove(path) -- Windows os.rename does not overwrite
    local ok, rerr = os.rename(tmp, path)
    if not ok then
        os.remove(tmp)
        return false, rerr
    end
    return true
end

-- Reads just the source_id of an already written guide, to decide whether the
-- file belongs to this track. A guide is a few KB; reading the head is enough.
local function readSourceId(path)
    local f = io.open(path, "rb")
    if not f then
        return nil
    end
    local head = f:read(2048) or ""
    f:close()
    return head:match('"source_id"%s*:%s*"([^"]*)"')
end

-- Removes the guides of the SAME track that this run supersedes: the old
-- positional ids (pre-R10) and any other file carrying the same source_id.
-- A guide of another track, or of another take of this track, is left alone.
local function removeStaleTrackGuides(guideDir, trackPrefix, sourceId, keepPath)
    -- Collect first: deleting while enumerating can skip the next name.
    local names = {}
    local index = 0
    while true do
        local name = reaper.EnumerateFiles(guideDir, index)
        if not name then
            break
        end
        index = index + 1
        if name:match("^guide_.*%.json$") then
            names[#names + 1] = name
        end
    end

    for i = 1, #names do
        local full = guideDir .. "\\" .. names[i]
        if full:lower() ~= keepPath:lower() then
            local id = readSourceId(full)
            if id and id:sub(1, #trackPrefix) == trackPrefix then
                local rest = id:sub(#trackPrefix + 1)
                -- Pre-R10 ids always carry the item position as
                -- "<digits>.<digits>:" before the take name.
                local legacyPosition = rest:match("^(%d+%.%d+):")
                if legacyPosition or id == sourceId then
                    os.remove(full)
                end
            end
        end
    end
end

local function main()
    local item = reaper.GetSelectedMediaItem(0, 0)
    if not item then
        return { ok = false, error = "no item selected" }
    end

    local take = reaper.GetActiveTake(item)
    if not take or not reaper.TakeIsMIDI(take) then
        return { ok = false, error = "the selected item is not a MIDI take" }
    end

    local track = reaper.GetMediaItem_Track(item)
    local trackGuid = reaper.GetTrackGUID(track)
    local takeName = trim(reaper.GetTakeName(take))
    local trackName = ({ reaper.GetTrackName(track) })[2] or ""

    -- Stable id: track + take/source, no item position (R10, D-363).
    local takeKey = takeName ~= "" and takeName or "midi"
    takeKey = takeKey:gsub(":", "_")
    local sourceId = string.format("reaper:%s:%s", trackGuid, takeKey)

    local ok, notes, ccs, sysex = reaper.MIDI_CountEvts(take)
    if not ok then
        return { ok = false, error = "MIDI_CountEvts failed" }
    end

    local entries = {}
    local skippedMuted = 0
    for i = 0, notes - 1 do
        local rok, _sel, muted, startppq, endppq, _chan, pitch, vel =
            reaper.MIDI_GetNote(take, i)
        if rok then
            if muted then
                skippedMuted = skippedMuted + 1
            else
                local sqn = reaper.MIDI_GetProjQNFromPPQPos(take, startppq)
                local eqn = reaper.MIDI_GetProjQNFromPPQPos(take, endppq)
                if eqn > sqn then
                    entries[#entries + 1] = {
                        sqn, eqn, pitch, vel / 127.0
                    }
                end
            end
        end
    end

    -- Deterministic order: start, then pitch.
    table.sort(entries, function(a, b)
        if a[1] ~= b[1] then return a[1] < b[1] end
        return a[3] < b[3]
    end)

    local parts = {}
    for i = 1, #entries do
        local e = entries[i]
        parts[#parts + 1] = string.format("[%s,%s,%d,%.5f]",
                                          fmt(e[1]), fmt(e[2]),
                                          math.floor(e[3] + 0.5), e[4])
    end

    local label = trim(trackName .. " " .. takeName)
    local json = string.format(
        '{\n  "schema": 1,\n  "source_id": "%s",\n  "label": "%s",\n' ..
        '  "track_id": "%s",\n  "take": "%s",\n' ..
        '  "kind": "reascript",\n  "host": "REAPER",\n' ..
        '  "created_utc": "%s",\n  "notes": [%s]\n}\n',
        jsonEscape(sourceId),
        jsonEscape(label),
        jsonEscape(trackGuid),
        jsonEscape(takeKey),
        os.date("!%Y-%m-%dT%H:%M:%SZ"),
        table.concat(parts, ","))

    local appdata = os.getenv("APPDATA")
    if not appdata then
        return { ok = false, error = "APPDATA not set" }
    end
    local guideDir = appdata .. "\\TuneX\\guides\\live"
    reaper.RecursiveCreateDirectory(guideDir, 0)

    -- One file per logical source: moving or splitting the item reuses it.
    local slug = sourceId:gsub("[^%w%-_%.]", "_")
    local path = guideDir .. "\\guide_" .. slug .. ".json"
    local wok, werr = writeAtomic(path, json)
    if not wok then
        return { ok = false, error = "write failed: " .. tostring(werr) }
    end

    removeStaleTrackGuides(guideDir, string.format("reaper:%s:", trackGuid),
                           sourceId, path)

    return {
        ok = true,
        path = path,
        notes = #entries,
        muted_skipped = skippedMuted,
        source_id = sourceId,
        label = label,
        first = entries[1],
        last = entries[#entries],
    }
end

local result = main()
if result.ok then
    reaper.ShowConsoleMsg(string.format(
        "TuneX guide: %d notes -> %s\n", result.notes, result.path))
else
    reaper.ShowConsoleMsg("TuneX guide: " .. tostring(result.error) .. "\n")
end
return result
