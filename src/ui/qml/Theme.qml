pragma Singleton

import QtQuick

// Central palette + font-family + dimension singleton. Access from
// any QML file via:
//
//   import Qcv
//   ...
//   color: Theme.bg
//
// Visual language mirrors the sister app (MinRender): squared
// corners, single cobalt-blue accent, minimal elevation, instant
// hover color swaps (no animations), Phosphor icons, Inter sans +
// JetBrainsMono.
QtObject {
    // --- Surfaces ---
    readonly property color bg:           "#161616"
    readonly property color bgAlt:        "#181818"
    readonly property color surface:      "#1a1a1a"
    readonly property color surfaceAlt:   "#1d1d1d"
    readonly property color surfaceHover: "#252525"
    readonly property color toolbar:      "#1f1f1f"
    readonly property color toolbarAlt:   "#262626"
    readonly property color border:       "#2a2a2a"
    readonly property color borderStrong: "#333333"
    readonly property color divider:      "#2a2a2a"
    readonly property color selection:    accentMuted

    // --- Text ---
    readonly property color textPrimary:   "#dddddd"
    readonly property color textSecondary: "#888888"
    readonly property color textMuted:     "#666666"
    readonly property color textBright:    "#ffffff"
    readonly property color textInverted:  "#111111"

    // --- Semantic / accent ---
    // Cobalt blue, locked. Predictable branding > per-user OS
    // accent on a pro review tool.
    readonly property color accent:         "#0189f1"
    readonly property color accentHover:    "#1b95f1"
    readonly property color accentMuted:    "#10395b"
    readonly property color accentSelected: accent
    readonly property color success:        "#4cb050"
    readonly property color warn:           "#f5a623"
    readonly property color error:          "#c04040"
    readonly property color info:           "#9cc9ff"

    // --- Fonts ---
    // Family names resolved by main.cpp at startup and exposed
    // through WindowManager. Falls back to a generic family if
    // the bundled .ttf didn't load.
    readonly property string fontFamily: WindowManager
                                         ? WindowManager.interFamily
                                         : "sans-serif"
    readonly property string monoFamily: WindowManager
                                         ? WindowManager.monoFamily
                                         : "monospace"
    readonly property string iconFamily: WindowManager
                                         ? WindowManager.phosphorFamily
                                         : "Phosphor"

    readonly property int fontSizeTiny:   10
    readonly property int fontSizeSmall:  11
    readonly property int fontSizeBase:   12
    readonly property int fontSizeMedium: 13
    readonly property int fontSizeLarge:  14
    readonly property int fontSizeXL:     16

    // --- Icon sizes ---
    readonly property int iconSizeSmall:   12
    readonly property int iconSizeToolbar: 16
    readonly property int iconSizeMedium:  18
    readonly property int iconSizeLarge:   24

    // --- Dimensions ---
    readonly property int rowHeight:        24
    readonly property int rowHeightDense:   22
    readonly property int rowHeightTall:    28
    readonly property int toolStripHeight:  32
    readonly property int headerHeight:     28
    // Track-header / transport gutter — narrow inset on the
    // left of the timeline holding edit-mode toggles. Same
    // width is used as the TransportBar's left margin so the
    // transport buttons align with the start of the track
    // area below.
    readonly property int gutterWidth:      28

    // --- Padding / spacing ---
    readonly property int paddingTight:  4
    readonly property int padding:       8
    readonly property int paddingLoose: 12

    readonly property int spacingTight:  2
    readonly property int spacing:       4
    readonly property int spacingLoose:  8

    // --- Borders / radii ---
    readonly property int borderWidth:  1
    readonly property int dividerWidth: 1
    // Squared corners by default. radiusSmall/Base/Large stay
    // available for chips/badges that want rounding.
    readonly property int radius:       0
    readonly property int radiusSmall:  2
    readonly property int radiusBase:   3
    readonly property int radiusLarge:  6
    readonly property int radiusPill:  10
}
