import QtQuick
import ".."

/// Live picture of one channel's program output or of a switcher input monitor (the Supervisor relays engine preview frames over IPC;
/// image://preview/<channel> or image://preview/<channel>:in<N>). Repaints only when that id's own frame arrives.
/// `hasFrame` is true while pictures keep arriving (none for 3 s = false), so a caller can fall back to a card.
///
/// Double-buffered: a new frame loads into the hidden image and is swapped in only once it is ready. A single Image is empty while
/// its next source loads, which showed the black background between frames (a flash on every update at 10-15 pictures a second).
///
/// Decoded at tile size (perf-app PA-2 / W11): each request carries this tile's size as sourceSize, so the image provider decodes
/// the JPEG straight to that size (DCT scaling) on the loader thread instead of decoding 1080p and shrinking it; the texture is
/// then drawn at about 1:1, which is why the tiles no longer ask for mipmaps (a mipmap chain per frame, 10-15 times a second).
/// The size is taken when a frame is requested, never bound: a resize must not reload the picture on screen (it would blank it).
Rectangle {
    id: pi
    property string channelId: ""
    property bool live: true
    property int serial: 0
    property real lastFrameMs: 0
    property bool hasFrame: false
    property int front: 0                  // which image is shown (0 / 1)
    property int loading: -1               // which image is loading the next frame (-1 = none)
    color: "#000"
    clip: true

    function url() { return "image://preview/" + pi.channelId + "?" + pi.serial }
    function nextFrame() {
        if (!pi.live || !pi.channelId) return
        pi.serial++
        const back = pi.front === 0 ? img1 : img0
        pi.loading = pi.front === 0 ? 1 : 0
        // The tile's size at request time (0x0 before layout = full size). Set only when it changed (reqW/reqH remember
        // what was asked; Image.sourceSize reads back the loaded size): a sourceSize change is itself a reload.
        const w = Math.round(pi.width), h = Math.round(pi.height)
        if (back.reqW !== w || back.reqH !== h) { back.reqW = w; back.reqH = h; back.sourceSize = Qt.size(w, h) }
        back.source = pi.url()            // a newer frame while one is loading simply replaces the request
    }
    function swapIfReady(which, img) {
        if (img.status === Image.Ready && pi.loading === which) { pi.front = which; pi.loading = -1 }
    }

    Image {
        id: img0
        property int reqW: -1; property int reqH: -1   // the sourceSize last asked for
        anchors.fill: parent; visible: pi.front === 0 && pi.live
        cache: false; asynchronous: true; fillMode: Image.PreserveAspectFit
        smooth: true                          // decoded at this size: no mipmap chain per frame
        onStatusChanged: pi.swapIfReady(0, img0)
    }
    Image {
        id: img1
        property int reqW: -1; property int reqH: -1
        anchors.fill: parent; visible: pi.front === 1 && pi.live
        cache: false; asynchronous: true; fillMode: Image.PreserveAspectFit
        smooth: true
        onStatusChanged: pi.swapIfReady(1, img1)
    }
    onChannelIdChanged: { hasFrame = false; lastFrameMs = 0; img0.source = ""; img1.source = ""; front = 0; loading = -1; nextFrame() }
    onLiveChanged: { if (live) nextFrame() }
    Component.onCompleted: nextFrame()
    Connections { target: supervisor; function onPreviewChanged(id) { if (id === pi.channelId) { pi.lastFrameMs = Date.now(); pi.hasFrame = true; pi.nextFrame() } } }
    Timer { interval: 1000; repeat: true; running: pi.hasFrame; onTriggered: { if (Date.now() - pi.lastFrameMs > 3000) pi.hasFrame = false } }
}
