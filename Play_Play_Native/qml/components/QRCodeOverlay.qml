import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: qrOverlay
    width: 300
    height: 300
    color: "white"
    radius: 8
    border.color: "#3b82f6"
    border.width: 2
    
    property string qrData: ""
    property string title: "Scan QR Code"
    
    Column {
        anchors.centerIn: parent
        spacing: 20
        
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: title
            font.pixelSize: 18
            font.bold: true
        }
        
        Rectangle {
            width: 200
            height: 200
            anchors.horizontalCenter: parent.horizontalCenter
            color: "white"
            border.color: "#e5e7eb"
            border.width: 1
            
            Text {
                anchors.centerIn: parent
                text: "QR Code\n(To be implemented)"
                color: "#6b7280"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
            }
            
            // TODO: Implement QR code generation using a library
        }
        
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qrData || "No data"
            color: "#6b7280"
            font.pixelSize: 12
            maximumLineCount: 2
            elide: Text.ElideMiddle
        }
    }
}

