import QtQuick
import QtQuick.Shapes

// Icon: one in-house icon of assets/ui/icons.json (generated into spec.js), drawn as vector paths with QtQuick.Shapes, so it
// needs no image, font, Qt Svg or qsvg plugin. The paths sit on the 24-unit grid and are scaled to `size` through the item
// scale, which scales the stroke with them. "curve" (default) uses Shape.CurveRenderer: it antialiases in the shader, so
// icons stay sharp when UiScale scales the window root. "geometry" is the fallback for a GPU where that misbehaves: the
// triangulating renderer inside a 4x multisampled layer rendered at twice the size. The software backend draws either one
// with QPainter.
//
// Each icon is {stroke, fill, sw}: "stroke" paths are stroked only, "fill" paths are filled and stroked (rounded corners).
// Only the Shape an icon has is visible: a ShapePath with an empty PathSvg adds the origin to Shape.boundingRect.
Item {
    id: ic
    property string name: ""
    property real size: 15
    property color color: "#f0f0f8"
    property string renderer: BtnTokens.iconRenderer       // "curve" | "geometry"

    readonly property var paths: BtnTokens.icons[ic.name] !== undefined ? BtnTokens.icons[ic.name] : null
    readonly property bool known: ic.paths !== null
    readonly property int rendererType: ic.renderer === "geometry" ? Shape.GeometryRenderer : Shape.CurveRenderer

    implicitWidth: ic.size
    implicitHeight: ic.size

    function checkName() {
        if (ic.name !== "" && !ic.known) BtnTokens.warnOnce("icon:" + ic.name, "unknown icon \"" + ic.name + "\" (assets/ui/icons.json)")
    }
    // The layer group is created on first access, so curve icons (the default) never touch it.
    function applyLayer() {
        if (ic.renderer === "geometry") {
            ic.layer.samples = 4
            ic.layer.smooth = true
            ic.layer.textureSize = Qt.size(Math.ceil(ic.width * 2), Math.ceil(ic.height * 2))
            ic.layer.enabled = true
        } else if (ic._layered) {
            ic.layer.enabled = false
        }
        ic._layered = ic.renderer === "geometry"
    }
    property bool _layered: false
    onNameChanged: ic.checkName()
    onRendererChanged: ic.applyLayer()
    onWidthChanged: if (ic._layered) ic.applyLayer()
    onHeightChanged: if (ic._layered) ic.applyLayer()
    Component.onCompleted: { ic.checkName(); ic.applyLayer() }

    Item {
        id: grid
        width: 24; height: 24
        scale: Math.min(ic.width, ic.height) / 24
        transformOrigin: Item.TopLeft
        x: (ic.width - 24 * grid.scale) / 2
        y: (ic.height - 24 * grid.scale) / 2

        Shape {
            id: strokeShape
            objectName: "iconStroke"
            anchors.fill: parent
            visible: ic.known && ic.paths.stroke !== ""
            preferredRendererType: ic.rendererType
            ShapePath {
                strokeColor: ic.color; fillColor: "transparent"
                strokeWidth: ic.known ? ic.paths.sw : 1.75
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: strokeShape.visible ? ic.paths.stroke : "" }
            }
        }
        Shape {
            id: fillShape
            objectName: "iconFill"
            anchors.fill: parent
            visible: ic.known && ic.paths.fill !== ""
            preferredRendererType: ic.rendererType
            ShapePath {
                strokeColor: ic.color; fillColor: ic.color; fillRule: ShapePath.WindingFill
                strokeWidth: ic.known ? ic.paths.sw : 1.75
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: fillShape.visible ? ic.paths.fill : "" }
            }
        }
    }
}
