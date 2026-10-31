// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

QtObject {
    // 10-foot UI: sized for a TV at couch distance, scaled from 1080p
    property real scale: 1.0
    // controller whose buttons the hints draw: "xbox", "playstation", "nintendo"
    property string buttonStyle: "xbox"

    readonly property color background: "#0b0f17"
    readonly property color surface: "#151b27"
    readonly property color surfaceRaised: "#1e2635"
    readonly property color accent: "#f5b942"
    readonly property color text: "#eef1f6"
    readonly property color textDim: "#8a94a6"
    readonly property color warning: "#ff6b6b"
    readonly property color focusRing: accent

    readonly property int fontHuge: 64 * scale
    readonly property int fontLarge: 40 * scale
    readonly property int fontMedium: 30 * scale
    readonly property int fontSmall: 24 * scale

    readonly property int margin: 64 * scale
    readonly property int spacing: 24 * scale
    readonly property int radius: 18 * scale

    // a disc's volume label as a title: "BACK_TO_FUTURE_1" -> "Back To Future 1"
    function titleFromLabel(label) {
        let t = String(label || "").replace(/_/g, " ").replace(/\s+/g, " ").trim()
        if (t === t.toUpperCase())
            t = t.toLowerCase().replace(/(^|\s)(\S)/g, (m, sp, c) => sp + c.toUpperCase())
        return t
    }

    function formatTime(secs) {
        if (!isFinite(secs) || secs < 0)
            secs = 0
        const h = Math.floor(secs / 3600)
        const m = Math.floor(secs % 3600 / 60)
        const s = Math.floor(secs % 60)
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + mm + ":" + (s < 10 ? "0" : "") + s
    }
}
