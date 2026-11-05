#pragma once

namespace pitchnet
{
// D-178: opening a Kinu Tune editor while another one is open asks the host to
// close the previous window, so only one editor remains. The new editor is
// already part of openEditors when this is evaluated. If both editors share
// the same top-level host window (some hosts reuse it), closing it would close
// the new editor too, so that case is excluded.
inline bool shouldClosePreviousEditor(int openEditors, bool sameTopLevelWindow)
{
    // D-178: a second (or later) editor requests the previous host window to
    // close; a shared host window belongs to the new editor and is excluded.
    return openEditors > 1 && !sameTopLevelWindow;
}
} // namespace pitchnet
