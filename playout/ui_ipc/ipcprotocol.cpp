#include "ipcprotocol.h"
#include <QDateTime>
#include <cstring>

namespace IpcProtocol {

namespace {
// A JSON string literal (quotes included), escaped exactly as QJsonDocument does it.
QByteArray jsonString(const QString& s) {
    const QByteArray a = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);   // ["..."]
    return a.mid(1, a.size() - 2);
}
QString keyOf(const Message& m) { return m.binaryKey.isEmpty() ? QStringLiteral("binary") : m.binaryKey; }
const QLatin1String kJpegKey("jpegBase64");
}

QByteArray Message::toJson() const {
    if (!binary.isEmpty()) {
        // The pre-PS-5 shape, built around the payload's own serialization: the ~100 KB of base64 is appended once
        // instead of going through a QString and QJsonDocument. The top-level keys come out in QJsonObject's sorted
        // order; the base64 key is appended last inside payload (not sorted), which no reader depends on.
        const QString key = keyOf(*this);
        QJsonObject p = payload;
        p.remove(key);
        const QByteArray pj = QJsonDocument(p).toJson(QJsonDocument::Compact);   // "{...}"
        const QByteArray b64 = binary.toBase64();
        const QByteArray cid = jsonString(channelId), k = jsonString(key);
        QByteArray out;
        out.reserve(pj.size() + b64.size() + cid.size() + k.size() + 80);
        out += "{\"channelId\":"; out += cid;
        out += ",\"payload\":"; out.append(pj.constData(), pj.size() - 1);   // without its closing brace
        if (pj.size() > 2) out += ',';
        out += k; out += ":\""; out += b64; out += "\"}";
        out += ",\"timestampNs\":"; out += QByteArray::number(timestampNs);
        out += ",\"type\":"; out += QByteArray::number(int(type));
        out += '}';
        return out;
    }
    QJsonObject obj;
    obj["type"] = static_cast<int>(type);
    obj["channelId"] = channelId;
    obj["timestampNs"] = timestampNs;
    obj["payload"] = payload;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

Message Message::fromJson(const QByteArray& data) {
    Message msg;
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return msg;
    QJsonObject obj = doc.object();
    msg.type = static_cast<MessageType>(obj["type"].toInt(0));
    msg.channelId = obj["channelId"].toString();
    msg.timestampNs = obj["timestampNs"].toInteger(0);   // exact: a ns clock (~1.8e18) is above 2^53, so a double rounds it
    msg.payload = obj["payload"].toObject();
    return msg;
}

QByteArray binaryBody(const Message& msg) {
    QByteArray f = encodeFrame(msg, true);
    return f.mid(4);
}

QByteArray encodeFrame(const Message& msg, bool binaryOk) {
    QByteArray framed;
    if (!binaryOk || msg.binary.isEmpty()) {
        const QByteArray data = msg.toJson();
        framed.reserve(4 + data.size());
        const quint32 len = static_cast<quint32>(data.size());
        framed.append(reinterpret_cast<const char*>(&len), 4);
        framed.append(data);
        return framed;
    }
    const QString key = keyOf(msg);
    QJsonObject h;
    h["type"] = static_cast<int>(msg.type);
    h["channelId"] = msg.channelId;
    h["timestampNs"] = msg.timestampNs;
    QJsonObject p = msg.payload;
    p.remove(key);
    h["payload"] = p;
    h["binaryKey"] = key;
    const QByteArray head = QJsonDocument(h).toJson(QJsonDocument::Compact);
    const qint64 body = 5 + qint64(head.size()) + msg.binary.size();
    framed.reserve(qsizetype(4 + body));
    const quint32 len = static_cast<quint32>(body & kFrameLengthMask) | kBinaryFrameFlag;
    framed.append(reinterpret_cast<const char*>(&len), 4);
    framed.append(char(kBinaryFrameVersion));
    const quint32 hl = static_cast<quint32>(head.size());
    framed.append(reinterpret_cast<const char*>(&hl), 4);
    framed.append(head);
    framed.append(msg.binary);   // the JPEG as it came: no base64, no JSON escaping
    return framed;
}

BinaryDecode decodeBinaryBody(const char* data, qint64 len, Message* out) {
    if (!data || len < 5) return BinaryDecode::Malformed;
    if (static_cast<unsigned char>(data[0]) != kBinaryFrameVersion) return BinaryDecode::UnknownVersion;
    quint32 hl = 0;
    memcpy(&hl, data + 1, 4);
    if (qint64(hl) > len - 5) return BinaryDecode::Malformed;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(data + 5, qsizetype(hl)), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return BinaryDecode::Malformed;
    const QJsonObject obj = doc.object();
    Message m;
    m.type = static_cast<MessageType>(obj["type"].toInt(0));
    m.channelId = obj["channelId"].toString();
    m.timestampNs = obj["timestampNs"].toInteger(0);   // exact (see fromJson)
    m.payload = obj["payload"].toObject();
    m.binaryKey = obj["binaryKey"].toString();
    const qint64 off = 5 + qint64(hl);
    m.binary = QByteArray(data + off, qsizetype(len - off));
    if (out) *out = std::move(m);
    return BinaryDecode::Ok;
}

void inlineBinary(Message& msg) {
    if (msg.binary.isEmpty()) return;
    msg.payload[keyOf(msg)] = QString::fromLatin1(msg.binary.toBase64());
    msg.binary.clear();
    msg.binaryKey.clear();
}

QByteArray previewJpeg(const Message& msg) {
    if (!msg.binary.isEmpty()) return msg.binary;
    return QByteArray::fromBase64(msg.payload.value(kJpegKey).toString().toLatin1());
}

QByteArray liftPreview(Message& msg) {
    if (msg.binary.isEmpty() && msg.payload.contains(kJpegKey)) {
        msg.binary = QByteArray::fromBase64(msg.payload.value(kJpegKey).toString().toLatin1());
        msg.payload.remove(kJpegKey);
        msg.binaryKey = kJpegKey;
    }
    return msg.binary;
}

Message makeTimeSync(qint64 frameNumber, qint64 wallClockNs, qint64 masterOffsetNs) {
    Message msg;
    msg.type = MSG_TIME_SYNC;
    msg.timestampNs = wallClockNs;
    msg.payload["frameNumber"] = frameNumber;
    msg.payload["wallClockNs"] = wallClockNs;
    msg.payload["masterOffsetNs"] = masterOffsetNs;
    return msg;
}

Message makeHeartbeat(const QString& channelId, qint64 frameNumber, int bufferDepth, bool isHealthy) {
    Message msg;
    msg.type = MSG_HEARTBEAT;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload["frameNumber"] = frameNumber;
    msg.payload["bufferDepth"] = bufferDepth;
    msg.payload["isHealthy"] = isHealthy;
    return msg;
}

Message makePlaylistPush(const QString& channelId, const QJsonArray& playlist, bool replaceCurrent) {
    Message msg;
    msg.type = MSG_PLAYLIST_PUSH;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload["playlist"] = playlist;
    msg.payload["replaceCurrent"] = replaceCurrent;
    return msg;
}

Message make(MessageType type, const QString& channelId, const QJsonObject& payload) {
    Message msg;
    msg.type = type;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload = payload;
    return msg;
}

Message makeEngineHello(const QString& channelId, const QJsonObject& facts) {
    Message msg = make(MSG_ENGINE_HELLO, channelId, facts);
    msg.payload["channelId"] = channelId;
    return msg;
}

Message makeSupAlive(qint64 seq, double loopLagMs, qint64 supervisorPid, const QString& channelId) {
    QJsonObject p;
    p["seq"] = double(seq);
    p["loopLagMs"] = loopLagMs;
    p["pid"] = double(supervisorPid);
    return make(MSG_SUP_ALIVE, channelId, p);
}

Message makeTransportCommand(const QString& channelId, const QString& action, const QJsonObject& params) {
    Message msg;
    msg.type = MSG_TRANSPORT_COMMAND;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload["action"] = action;
    msg.payload["params"] = params;
    return msg;
}

Message makeStateChange(const QString& channelId, const QString& state, const QString& reason) {
    Message msg;
    msg.type = MSG_STATE_CHANGE;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload["state"] = state;
    msg.payload["reason"] = reason;
    return msg;
}

Message makeAsRunLog(const QString& channelId, const QString& eventType, const QString& assetId, qint64 frameNumber) {
    Message msg;
    msg.type = MSG_ASRUN_LOG;
    msg.channelId = channelId;
    msg.timestampNs = QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL;
    msg.payload["eventType"] = eventType;
    msg.payload["assetId"] = assetId;
    msg.payload["frameNumber"] = frameNumber;
    return msg;
}

Message makePreviewFrame(const QString& channelId, const QByteArray& jpeg, const QJsonObject& payload) {
    Message msg = make(MSG_PREVIEW_FRAME, channelId, payload);
    msg.binary = jpeg;
    msg.binaryKey = kJpegKey;
    if (jpeg.isEmpty()) msg.payload[kJpegKey] = QString();   // the old shape had the key even when empty
    return msg;
}

Message makeIpcCaps(int binaryFrames) {
    QJsonObject p;
    p["binaryFrames"] = binaryFrames;
    p["transport"] = 1;
    return make(MSG_IPC_CAPS, QString(), p);
}

Message makePreviewSubscribe(bool all, const QStringList& keys, double maxFps) {
    QJsonObject p;
    p["all"] = all;
    p["keys"] = QJsonArray::fromStringList(keys);
    p["maxFps"] = maxFps;
    return make(MSG_UI_PREVIEW_SUBSCRIBE, QString(), p);
}

} // namespace IpcProtocol
