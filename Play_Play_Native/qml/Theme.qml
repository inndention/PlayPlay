pragma Singleton
import QtQuick 2.15

/**
 * Theme — PlayPlayClean's :root tokens (docs/reference-feature-inventory.md §UI)
 * carried over to QML. Surfaces step from near-black to the raised panels;
 * one cyan accent, purple secondary; status colours are the broadcast set
 * (on-air red, standby amber, ready green, cued indigo).
 */
QtObject {
    property bool darkMode: true

    // --- surfaces -------------------------------------------------------------
    readonly property color bg0: "#05050a"        // window
    readonly property color bg1: "#0a0a12"        // rails, top bar
    readonly property color bg2: "#0f0f18"        // panels
    readonly property color bg3: "#141420"        // raised rows
    readonly property color bg4: "#1a1a28"        // inputs, hover
    readonly property color bg5: "#22223a"        // selected
    readonly property color border0: "#1f1f35"
    readonly property color border1: "#2a2a40"
    readonly property color border2: "#3f3f60"

    // --- text ----------------------------------------------------------------------
    readonly property color text0: "#f0f0f8"
    readonly property color text1: "#a0a0b8"
    readonly property color text2: "#707088"
    readonly property color text3: "#505068"

    // --- accent + status ------------------------------------------------------------
    readonly property color accent0: "#00d4ff"
    readonly property color accentHover0: "#33dfff"
    readonly property color accent2: "#a855f7"
    readonly property color onAir: "#ff2d55"
    readonly property color standby: "#ff9500"
    readonly property color ready: "#30d158"
    readonly property color cued: "#5e5ce6"
    readonly property color warn: "#ffd60a"
    readonly property color danger: "#ff453a"

    // --- timeline / content classes ---------------------------------------------------
    readonly property color clsVideo: "#5856d6"
    readonly property color clsAudio: "#30d158"
    readonly property color clsGraphics: "#ff9f0a"
    readonly property color clsEffects: "#bf5af2"
    readonly property color clsLive: "#ff2d55"
    readonly property color clsPreroll: "#64d2ff"
    readonly property color clsAd: "#ff375f"
    readonly property color clsBumper: "#ac8e68"

    function typeColor(t) {
        switch (String(t || "").toLowerCase()) {
        case "ad": case "commercial": return clsAd
        case "promo": return clsEffects
        case "station_id": case "id": return clsGraphics
        case "bumper": return clsBumper
        case "live": return clsLive
        case "cg": case "image": return clsGraphics
        case "gap": return text3
        case "filler": case "psa": return clsPreroll
        default: return clsVideo
        }
    }
    function stateColor(s) {
        switch (String(s || "")) {
        case "ON_AIR": return onAir
        case "PREROLL": return cued
        case "PAUSED": return warn
        case "HOLD": case "IDLE": return standby
        case "ERROR": case "OFFLINE": return danger
        default: return text2
        }
    }

    // --- typography -----------------------------------------------------------------------
    readonly property string fontDisplay: "Segoe UI Semibold"
    readonly property string fontSans: "Segoe UI"
    readonly property string fontMono: "Cascadia Mono, Consolas"
    // Text size boost: every fs* token scales with Theme.scale (persisted by main.qml, Ctrl+= / Ctrl+- / Ctrl+0).
    property real scale: 1.0   // legacy; the UI zoom is uiScale (C++), applied to the window root
    readonly property int fs10: 10
    readonly property int fs11: 11
    readonly property int fs12: 12
    readonly property int fs13: 13
    readonly property int fs14: 14
    readonly property int fs16: 16
    readonly property int fs20: 20
    readonly property int fs28: 28
    readonly property int fs40: 40

    // --- geometry ---------------------------------------------------------------------------
    readonly property int r3: 3
    readonly property int r5: 5
    readonly property int r8: 8
    readonly property int r12: 12
    readonly property int s4: 4
    readonly property int s8: 8
    readonly property int s12: 12
    readonly property int s16: 16
    readonly property int s24: 24
    readonly property int topBarH: 40
    readonly property int railW: 48
    readonly property int columnW: 400
    readonly property int tickerH: 46

    // --- compatibility aliases for the pre-Phase-5 pages ------------------------------------
    property color background: bg0
    property color surface: bg2
    property color surfaceHighlight: bg3
    property color surfaceSelected: bg5
    property color accent: accent0
    property color accentHover: accentHover0
    property color accentPressed: "#00a8cc"
    property color accentAlt: accent2
    property color textPrimary: text0
    property color textSecondary: text1
    property color textDisabled: text3
    property color error: danger
    property color success: ready
    property color warning: warn
    property color live: onAir
    property color border: border1
    property color glow: "#00d4ff40"
    property string fontFamily: fontSans
    property int fontSizeTiny: 11
    property int fontSizeSmall: 12
    property int fontSizeNormal: 13
    property int fontSizeLarge: 18
    property int fontSizeHeader: 24
    property int radius: 8
    property int radiusLg: 12
    property int paddingXS: 6
    property int paddingSmall: 8
    property int paddingNormal: 12
    property int paddingLarge: 18

    // --- helpers ---------------------------------------------------------------------------------
    function fmtClock(sec) {
        sec = Math.max(0, Math.floor(sec || 0))
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? (h + ":" + (m < 10 ? "0" : "") + m) : m) + ":" + (s < 10 ? "0" : "") + s
    }
    function fmtDur(sec) {
        sec = Math.max(0, Math.round(sec || 0))
        var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
        return (h > 0 ? h + "h " : "") + m + "m " + (s < 10 ? "0" : "") + s + "s"
    }
    function fmtTime(ms, withSec) {
        if (!ms) return "--:--"
        var d = new Date(ms)
        return Qt.formatTime(d, withSec ? "h:mm:ss AP" : "h:mm AP")
    }
    function pad2(n) { return (n < 10 ? "0" : "") + n }
}
