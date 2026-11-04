import QtQuick
import QtQuick.Shapes
import QtTest

// UXB-2: every icon of the generated spec (Play_Play_Native/qml/PlayPlay/Buttons/spec.js, from assets/ui/icons.json) is built
// the way the PlayPlay.Buttons Icon draws it: a stroked ShapePath and a filled + stroked one, round caps and joins, at the
// icon's stroke width. Two checks against the path data as read here in JS (the SVG rules browsers follow):
// - bounds: Shape.boundingRect (Qt 6.6: the QPainterPath bounding box of Qt's PathSvg parse, widened by half the stroke; not
//   stroker output) must equal the JS path bounds widened the same way, to 0.05. A path Qt stops parsing early or reads with
//   other arc flags or numbers changes the box and fails; a difference inside the box does not.
// - ink: Qt's raster, grabbed at 8x, must be inked at every sampled point of every path (each lies on a stroked centre line or
//   a filled part's stroked outline). A subpath Qt drops, such as an arc back to its own start point that Chromium draws as a
//   dot, leaves its points blank and fails.
// The runner uses the offscreen platform and the software renderer, so this exercises Qt's parser, QPainterPath and QPainter
// stroker, not CurveRenderer. Ink that a browser draws but Qt does not (outside the sampled points) and renderer quirks such
// as Chromium's hollow tiny filled dots are not seen here; test_uispec rejects the data that causes them (zero-length segments,
// arcs tighter than half the stroke).
TestCase {
    id: tc
    name: "IconSet"
    when: windowShown
    width: 240; height: 240
    property var spec: null

    // One component per combination: a ShapePath with an empty PathSvg still adds the origin to Shape.boundingRect.
    Component {
        id: strokeAndFill
        Shape {
            property var ic
            width: 24; height: 24
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "white"; fillColor: "transparent"; strokeWidth: ic.sw
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: ic.stroke }
            }
            ShapePath {
                strokeColor: "white"; fillColor: "white"; strokeWidth: ic.sw; fillRule: ShapePath.WindingFill
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: ic.fill }
            }
        }
    }
    Component {
        id: strokeOnly
        Shape {
            property var ic
            width: 24; height: 24
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "white"; fillColor: "transparent"; strokeWidth: ic.sw
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: ic.stroke }
            }
        }
    }
    Component {
        id: fillOnly
        Shape {
            property var ic
            width: 24; height: 24
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: "white"; fillColor: "white"; strokeWidth: ic.sw; fillRule: ShapePath.WindingFill
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: ic.fill }
            }
        }
    }
    Component {
        id: plate
        Rectangle { width: 192; height: 192; color: "black" }
    }

    function initTestCase() {
        spec = Qt.createQmlObject('import QtQuick; import "' + ppQmlDir + '/PlayPlay/Buttons/spec.js" as Spec\n'
            + 'QtObject { readonly property var icons: Spec.icons; readonly property var groups: Spec.iconGroups }', tc, "spec")
        verify(spec !== null && spec.icons !== undefined, "PlayPlay/Buttons/spec.js did not load")
    }

    // Points along an SVG path (absolute and relative M L H V C S Q T A Z): line end points, 32 samples per curve, 48 per arc.
    // Arcs follow SVG F.6.2: identical end points omit the arc (the moveto point stays), a zero radius makes it a line.
    function samplePath(d) {
        const t = d.match(/[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)/g) || []
        const pts = []
        let i = 0, x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0, prev = ""
        const isCmd = k => /^[A-Za-z]$/.test(t[k])
        while (i < t.length) {
            const c = t[i++], C = c.toUpperCase(), rel = c !== C
            if (C === "Z") { x = cx = sx; y = cy = sy; pts.push([x, y]); prev = C; continue }
            let first = true
            while (i < t.length && !isCmd(i)) {
                const n = () => parseFloat(t[i++])
                const p = () => { const a = n(), b = n(); return rel ? [x + a, y + b] : [a, b] }
                const K = (C === "M" && !first) ? "L" : C
                if (K === "M" || K === "L") { [x, y] = p(); cx = x; cy = y; if (K === "M") { sx = x; sy = y } pts.push([x, y]) }
                else if (K === "H") { const v = n(); x = rel ? x + v : v; cx = x; cy = y; pts.push([x, y]) }
                else if (K === "V") { const v = n(); y = rel ? y + v : v; cx = x; cy = y; pts.push([x, y]) }
                else if (K === "C" || K === "S") {
                    const c1 = K === "C" ? p() : ((prev === "C" || prev === "S") ? [2 * x - cx, 2 * y - cy] : [x, y])
                    const c2 = p(), e = p()
                    for (let k = 1; k <= 32; ++k) {
                        const s = k / 32, u = 1 - s
                        pts.push([u*u*u*x + 3*u*u*s*c1[0] + 3*u*s*s*c2[0] + s*s*s*e[0], u*u*u*y + 3*u*u*s*c1[1] + 3*u*s*s*c2[1] + s*s*s*e[1]])
                    }
                    [cx, cy] = c2; [x, y] = e
                } else if (K === "Q" || K === "T") {
                    const c1 = K === "Q" ? p() : ((prev === "Q" || prev === "T") ? [2 * x - cx, 2 * y - cy] : [x, y])
                    const e = p()
                    for (let k = 1; k <= 32; ++k) { const s = k / 32, u = 1 - s; pts.push([u*u*x + 2*u*s*c1[0] + s*s*e[0], u*u*y + 2*u*s*c1[1] + s*s*e[1]]) }
                    [cx, cy] = c1; [x, y] = e
                } else if (K === "A") {   // SVG 1.1 F.6.5
                    let rx = Math.abs(n()), ry = Math.abs(n())
                    const phi = n() * Math.PI / 180, fa = n(), fs = n(), e = p()
                    if (e[0] === x && e[1] === y) { prev = K; first = false; continue }
                    if (rx === 0 || ry === 0) { [x, y] = e; cx = x; cy = y; pts.push([x, y]); prev = K; first = false; continue }
                    const cp = Math.cos(phi), sp = Math.sin(phi), hx = (x - e[0]) / 2, hy = (y - e[1]) / 2
                    const x1 = cp * hx + sp * hy, y1 = -sp * hx + cp * hy
                    const lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
                    if (lam > 1) { rx *= Math.sqrt(lam); ry *= Math.sqrt(lam) }
                    const num = rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1, den = rx*rx*y1*y1 + ry*ry*x1*x1
                    const k = (fa === fs ? -1 : 1) * Math.sqrt(Math.max(0, den > 0 ? num / den : 0))
                    const cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx
                    const ox = cp * cxp - sp * cyp + (x + e[0]) / 2, oy = sp * cxp + cp * cyp + (y + e[1]) / 2
                    const th = Math.atan2((y1 - cyp) / ry, (x1 - cxp) / rx)
                    let dth = Math.atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx) - th
                    if (fs && dth < 0) dth += 2 * Math.PI
                    if (!fs && dth > 0) dth -= 2 * Math.PI
                    for (let s = 1; s <= 48; ++s) {
                        const a = th + dth * s / 48
                        pts.push([ox + rx * Math.cos(a) * cp - ry * Math.sin(a) * sp, oy + rx * Math.cos(a) * sp + ry * Math.sin(a) * cp])
                    }
                    [x, y] = e; cx = x; cy = y
                }
                prev = K; first = false
            }
        }
        return pts
    }

    // {x0, y0, x1, y1} of an icon's ink: every stroke and fill path, widened by half the stroke (round caps and joins)
    function expectedInk(ic) {
        const pts = samplePath(ic.stroke + " " + ic.fill)
        const b = { x0: 1e9, y0: 1e9, x1: -1e9, y1: -1e9 }
        for (const q of pts) { b.x0 = Math.min(b.x0, q[0]); b.y0 = Math.min(b.y0, q[1]); b.x1 = Math.max(b.x1, q[0]); b.y1 = Math.max(b.y1, q[1]) }
        const h = ic.sw / 2
        return { x0: b.x0 - h, y0: b.y0 - h, x1: b.x1 + h, y1: b.y1 + h }
    }

    function test_samplerSelfTest() {
        const near = (b, w) => Math.abs(b.x0 - w[0]) < 0.01 && Math.abs(b.y0 - w[1]) < 0.01 && Math.abs(b.x1 - w[2]) < 0.01 && Math.abs(b.y1 - w[3]) < 0.01
        const cases = [
            ["M18 12 A6 6 0 1 1 6 12 A6 6 0 1 1 18 12 Z", [6, 6, 18, 18]], ["M6 12 A6 6 0 0 1 18 12", [6, 6, 18, 12]],
            ["M6 12 A6 6 0 0 0 18 12", [6, 12, 18, 18]], ["M10 5 A5 2.5 90 0 1 10 15", [10, 5, 12.5, 15]],
            ["M2 2 C2 10 10 10 10 2", [2, 2, 10, 8]], ["M2 2 C2 6 6 6 6 2 S10 -2 10 2", [2, -1, 10, 5]],
            ["M2 2 Q4 6 6 2 T10 2", [2, 0, 10, 4]], ["m4 4 h10 v10 z m2 2 l1 1", [4, 4, 14, 14]], ["M1 1 2 3 4 1", [1, 1, 4, 3]],
            ["M5 5 L7 5 A2 2 0 0 1 7 5", [5, 5, 7, 5]], ["M5 5 A0 2 0 0 1 7 6", [5, 5, 7, 6]]]
        for (const c of cases) {
            const b = expectedInk({ stroke: c[0], fill: "", sw: 0 })
            verify(near(b, c[1]), c[0] + " -> " + JSON.stringify(b))
        }
        compare(samplePath("M5 5 L7 5 A2 2 0 0 1 7 5 L9 5").length, 3, "an arc back to its start adds no points")
    }

    function test_catalogueIsDrawn() {
        let n = 0
        for (const g of spec.groups)
            for (const name of g.icons) { verify(spec.icons[name] !== undefined, name + " is in iconGroups but not drawn"); ++n }
        compare(Object.keys(spec.icons).length, n, "every drawn icon is in exactly one group")
    }

    function test_qtDrawsEveryIconAsSpecified() {
        const names = Object.keys(spec.icons).sort()
        verify(names.length > 100, "the icon set is incomplete: " + names.length)
        // the TestCase item itself is invisible, so the shapes go into the window's content item (an invisible Shape is
        // never processed)
        const stage = tc.Window.contentItem
        verify(stage !== null, "the test window has no content item")
        const shapes = names.map(name => {
            const ic = spec.icons[name]
            const comp = ic.stroke !== "" && ic.fill !== "" ? strokeAndFill : (ic.stroke !== "" ? strokeOnly : fillOnly)
            return createTemporaryObject(comp, stage, { ic: ic })
        })
        for (const s of shapes) verify(s !== null && s.visible, "could not create a visible Shape")
        waitForRendering(stage)
        tryVerify(() => shapes.every(s => s.status === Shape.Ready && s.boundingRect.width > 0), 5000, "every Shape is processed")
        const bad = []
        for (let k = 0; k < names.length; ++k) {
            const r = shapes[k].boundingRect, e = expectedInk(spec.icons[names[k]])
            const d = Math.max(Math.abs(r.x - e.x0), Math.abs(r.y - e.y0), Math.abs(r.x + r.width - e.x1), Math.abs(r.y + r.height - e.y1))
            if (d > 0.05)
                bad.push(names[k] + ": Qt draws (" + [r.x, r.y, r.x + r.width, r.y + r.height].map(v => v.toFixed(2)).join(", ")
                         + ") but the path data spans (" + [e.x0, e.y0, e.x1, e.y1].map(v => v.toFixed(2)).join(", ") + ")")
            if (r.x < 1.5 || r.y < 1.5 || r.x + r.width > 22.5 || r.y + r.height > 22.5)
                bad.push(names[k] + ": ink leaves the 1.5..22.5 safe area")
        }
        compare(bad.join("\n"), "")
    }

    // Each icon white on black at 8x (half the stroke is 7 px, so the pixel holding a sample point lies wholly inside the ink of
    // a piece Qt draws): every sample point of every path must be inked. Blank means Qt dropped or moved that piece.
    function test_qtInksEveryPathPoint() {
        const stage = tc.Window.contentItem
        const S = 8, bad = []
        for (const name of Object.keys(spec.icons).sort()) {
            const ic = spec.icons[name]
            const bg = createTemporaryObject(plate, stage, { x: 0, y: 0 })
            const comp = ic.stroke !== "" && ic.fill !== "" ? strokeAndFill : (ic.stroke !== "" ? strokeOnly : fillOnly)
            verify(comp.createObject(bg, { ic: ic, scale: S, transformOrigin: Item.TopLeft }) !== null, name + ": no Shape")
            waitForRendering(bg)
            const img = grabImage(bg)
            verify(img.width === 192 && img.height === 192, "an 8x grab needs a 192x192 plate at 1 dpr, got " + img.width + "x" + img.height)
            let blank = 0, first = null
            for (const q of samplePath(ic.stroke + " " + ic.fill))
                if (img.red(Math.min(191, Math.floor(q[0] * S)), Math.min(191, Math.floor(q[1] * S))) < 128 && !blank++) first = q
            if (blank)
                bad.push(name + ": " + blank + " path points are blank in Qt's raster, the first at (" + first.map(v => v.toFixed(2)).join(", ") + ")")
            bg.destroy()
        }
        compare(bad.join("\n"), "")
    }
}
