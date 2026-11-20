#include "Theme.h"

// Premium dark theme palette — refined depth with warm accent highlights
// Background hierarchy: BACKGROUND < SURFACE < SURFACE_ALT < SURFACE_RAISED
// Each layer is subtly brighter, creating natural elevation perception.

// ── Core backgrounds ──────────────────────────────────────────────
// Neutral graphite, closer to a Melodyne/SynthV editor than to the old
// blue-black. The hierarchy still goes BACKGROUND < SURFACE < ALT < RAISED.
const juce::Colour APP_COLOR_BACKGROUND          = juce::Colour(0xFF1B1D21u);  // Editor base
const juce::Colour APP_COLOR_SURFACE             = juce::Colour(0xFF17191Du);  // Primary surface
const juce::Colour APP_COLOR_SURFACE_ALT         = juce::Colour(0xFF101114u);  // Recessed/inset surface (controls, inputs)
const juce::Colour APP_COLOR_SURFACE_RAISED      = juce::Colour(0xFF23262Cu);  // Elevated cards/popups

// ── Borders ───────────────────────────────────────────────────────
const juce::Colour APP_COLOR_BORDER              = juce::Colour(0xFF2C3038u);  // Standard border
const juce::Colour APP_COLOR_BORDER_SUBTLE       = juce::Colour(0xFF22252Bu);  // Barely visible divider
const juce::Colour APP_COLOR_BORDER_HIGHLIGHT    = juce::Colour(0xFF3E454Fu);  // Focus / active border

// ── Piano roll grid & timeline ────────────────────────────────────
const juce::Colour APP_COLOR_GRID                = juce::Colour(0xFF22252Bu);  // Subdivision lines
const juce::Colour APP_COLOR_GRID_BAR            = juce::Colour(0xFF313640u);  // Bar / beat lines
const juce::Colour APP_COLOR_TIMELINE            = juce::Colour(0xFF1E2127u);  // Timeline ruler background

// ── Piano keys ────────────────────────────────────────────────────
const juce::Colour APP_COLOR_PIANO_WHITE         = juce::Colour(0xFF30343Cu);  // White key face
const juce::Colour APP_COLOR_PIANO_BLACK         = juce::Colour(0xFF1C1F25u);  // Black key face
const juce::Colour APP_COLOR_PIANO_TEXT          = juce::Colour(0xFFD5D9E0u);  // Active note label
const juce::Colour APP_COLOR_PIANO_TEXT_DIM      = juce::Colour(0xFF818994u);  // Inactive note label

// ── Text ──────────────────────────────────────────────────────────
const juce::Colour APP_COLOR_TEXT_PRIMARY         = juce::Colour(0xFFE8EBF0u);  // High-contrast primary text
const juce::Colour APP_COLOR_TEXT_MUTED           = juce::Colour(0xFF8A929Eu);  // Secondary / caption text

// ── Pitch & notes ─────────────────────────────────────────────────
const juce::Colour APP_COLOR_PITCH_CURVE          = juce::Colour(0xFFEAF6FFu);  // Pitch contour line
const juce::Colour APP_COLOR_NOTE_NORMAL          = juce::Colour(0xFF2E6E9Eu);  // Default note block
const juce::Colour APP_COLOR_NOTE_SELECTED        = juce::Colour(0xFF78C4F0u);  // Selected note (brighter)
const juce::Colour APP_COLOR_NOTE_HOVER           = juce::Colour(0xFF3E85B8u);  // Hovered note

// ── Accent colours ────────────────────────────────────────────────
const juce::Colour APP_COLOR_PRIMARY              = juce::Colour(0xFF2E8FD6u);  // Melodyne-style blue
const juce::Colour APP_COLOR_PRIMARY_GLOW         = juce::Colour(0xFF8ED2FFu);  // Glow / highlight ring
const juce::Colour APP_COLOR_SECONDARY            = juce::Colour(0xFF59C3D8u);  // Teal-cyan complement

// ── Waveform ──────────────────────────────────────────────────────
const juce::Colour APP_COLOR_WAVEFORM             = juce::Colour(0xFF232830u);  // Waveform background tint

// ── Knobs & shadows ───────────────────────────────────────────────
const juce::Colour APP_COLOR_KNOB_SHADOW          = juce::Colour(0xFF08090Du);  // Deep knob drop-shadow

// ── Alerts ────────────────────────────────────────────────────────
const juce::Colour APP_COLOR_ALERT_WARNING        = juce::Colour(0xFFFFB626u);  // Warm amber warning
const juce::Colour APP_COLOR_ALERT_ERROR          = juce::Colour(0xFFFF4D5Au);  // Softer red error

// ── Overlays & selections ─────────────────────────────────────────
const juce::Colour APP_COLOR_OVERLAY_DIM          = juce::Colour(0xC0000008u);  // Modal backdrop (slightly blue-black)
const juce::Colour APP_COLOR_OVERLAY_SHADOW       = juce::Colour(0xA0000008u);  // Drop-shadow fill
const juce::Colour APP_COLOR_SELECTION_OVERLAY    = juce::Colour(0x18000000u);  // Rubber-band fill
const juce::Colour APP_COLOR_SELECTION_HIGHLIGHT  = juce::Colour(0x406C5AFFu);  // Selection tint (matches primary)
const juce::Colour APP_COLOR_SELECTION_HIGHLIGHT_STRONG = juce::Colour(0xC06C5AFFu); // Strong selection

// ── Title bar (macOS traffic lights & Windows close hover) ────────
const juce::Colour APP_COLOR_TITLEBAR_CLOSE_MAC    = juce::Colour(0xFFFF5F57u);
const juce::Colour APP_COLOR_TITLEBAR_MINIMIZE_MAC = juce::Colour(0xFFFEBC2Eu);
const juce::Colour APP_COLOR_TITLEBAR_MAXIMIZE_MAC = juce::Colour(0xFF28C840u);
const juce::Colour APP_COLOR_TITLEBAR_CLOSE_HOVER  = juce::Colour(0xFFE81123u);

// ── Scrollbars ────────────────────────────────────────────────────
const juce::Colour APP_COLOR_SCROLLBAR_THUMB      = juce::Colour(0xFF565656u);  // Thumb
const juce::Colour APP_COLOR_SCROLLBAR_TRACK      = juce::Colour(0xFF0D0B0Bu);  // Recessed gutter

// ── Melodyne-style editor chrome ──────────────────────────────────
const juce::Colour APP_COLOR_RAIL                 = juce::Colour(0xFF141619u);
const juce::Colour APP_COLOR_RAIL_ACTIVE          = juce::Colour(0xFF23272Eu);
const juce::Colour APP_COLOR_RAIL_ICON            = juce::Colour(0xFFA2AAB6u);
const juce::Colour APP_COLOR_DRAWER               = juce::Colour(0xFF191C21u);
const juce::Colour APP_COLOR_DRAWER_HEADER        = juce::Colour(0xFF20242Au);
const juce::Colour APP_COLOR_CANVAS_ROW_WHITE     = juce::Colour(0xFF202329u);
const juce::Colour APP_COLOR_CANVAS_ROW_BLACK     = juce::Colour(0xFF1A1D22u);
const juce::Colour APP_COLOR_NOTE_WAVE            = juce::Colour(0xFFDCEFFF);
const juce::Colour APP_COLOR_NOTE_IN_TUNE         = juce::Colour(0xFF2E6E9Eu);
const juce::Colour APP_COLOR_NOTE_OUT_OF_TUNE     = juce::Colour(0xFFC47A2Eu);
const juce::Colour APP_COLOR_PLAYHEAD             = juce::Colour(0xFFE8EFF7u);
const juce::Colour APP_COLOR_ACCENT_WARM          = juce::Colour(0xFFE0A34Cu);
const juce::Colour APP_COLOR_ACCENT_SELECT        = juce::Colour(0xFFFFD166u);
const juce::Colour APP_COLOR_TOOL_PILL            = juce::Colour(0xF01D2127u);
const juce::Colour APP_COLOR_TIMELINE_TEXT        = juce::Colour(0xFF9AA3AFu);

void styleCanvasScrollBar(juce::ScrollBar& scrollBar)
{
    // Colours only - the default look and feel does the drawing, which is what
    // keeps this identical to the piano-roll bars. That renderer paints the
    // thumb and nothing else, so the gutter behind the bar is the host
    // component's job (see PianoRollComponent::paint, DraggablePanel::paint).
    scrollBar.setColour(juce::ScrollBar::thumbColourId, APP_COLOR_SCROLLBAR_THUMB);
    scrollBar.setColour(juce::ScrollBar::trackColourId, APP_COLOR_SCROLLBAR_TRACK);
    scrollBar.setColour(juce::ScrollBar::backgroundColourId, APP_COLOR_SCROLLBAR_TRACK);
}
