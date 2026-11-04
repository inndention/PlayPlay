import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: cgCanvas
    color: "#000000"

    property var cgElements: []
    property bool isPreview: false

    Repeater {
        model: cgElements
        delegate: Item {
            id: elementItem
            anchors.fill: parent

            property var element: modelData

            // Text element
            Text {
                id: textElement
                visible: element && element.type === "text"
                anchors.centerIn: parent
                text: element ? element.text : ""
                color: element ? element.color : "white"
                font.pixelSize: element ? element.fontSize : 24
                font.bold: element ? element.bold : false
                font.italic: element ? element.italic : false
                
                // Animation
                opacity: element && element.visible ? 1.0 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: element ? element.animationDuration : 300 }
                }
            }

            // Image element
            Image {
                id: imageElement
                visible: element && element.type === "image"
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                source: element ? element.source : ""
                
                opacity: element && element.visible ? 1.0 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: element ? element.animationDuration : 300 }
                }
            }

            // Video element
            MediaPlayer {
                id: videoElement
                visible: element && element.type === "video"
                anchors.fill: parent
                source: element ? element.source : ""
                playing: element && element.visible && element.playing
                
                opacity: element && element.visible ? 1.0 : 0.0
                Behavior on opacity {
                    NumberAnimation { duration: element ? element.animationDuration : 300 }
                }
            }
        }
    }

    // Preview overlay
    Rectangle {
        visible: isPreview
        anchors.fill: parent
        color: "#40000000"
        border.color: "#3b82f6"
        border.width: 2
        
        Text {
            anchors.centerIn: parent
            text: "PREVIEW"
            color: "#3b82f6"
            font.pixelSize: 24
            font.bold: true
        }
    }

    function addElement(element) {
        var newElements = cgElements.slice()
        newElements.push(element)
        cgElements = newElements
    }

    function removeElement(elementId) {
        var newElements = cgElements.filter(function(e) {
            return e.id !== elementId
        })
        cgElements = newElements
    }

    function updateElement(elementId, updates) {
        var newElements = cgElements.map(function(e) {
            if (e.id === elementId) {
                return Object.assign({}, e, updates)
            }
            return e
        })
        cgElements = newElements
    }

    function showElement(elementId) {
        updateElement(elementId, { visible: true })
    }

    function hideElement(elementId) {
        updateElement(elementId, { visible: false })
    }
}

