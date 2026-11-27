// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../JuceHeader.h"

/**
 * Stable session identity of one ARA track inside its document (R5d, D-358).
 *
 * The key used to be the ADDRESS of the ARA::PlugIn::RegionSequence. Hosts
 * recreate region sequences when the editor closes and reopens (measured in
 * R5c), so the address changed, DocumentMidiGuideStore::bindTrack saw "another
 * track" and emptied the guide (111 ghosts on the first stop, 0 on the rest).
 *
 * The sequence object is therefore NOT part of the key. What survives the
 * recreation is:
 *  - the document: it lives as long as the host document does, and the guide
 *    store lives in its document controller. documentKey identifies it.
 *  - the track name: stable across the recreation, and across a reorder.
 *  - the ordinal: only a fallback for hosts that send no name, and it is the
 *    position in ARADocument::getRegionSequences().
 *
 * Limitation, deliberate and documented: ARA exposes no persistent track id,
 * so two tracks of one document with the SAME non-empty name are not
 * distinguished. SCOPE=A gives one track (and one document) per plug-in
 * instance, and the host's own track order would have to be lost as well for
 * the fallback to collide.
 */
juce::String documentTrackKey(const juce::String& documentKey,
                              const juce::String& trackName,
                              int trackOrdinal);
