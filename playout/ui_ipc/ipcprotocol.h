#ifndef IPCPROTOCOL_H
#define IPCPROTOCOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QByteArray>
#include <QStringList>

/**
 * Inter-process message protocol between Supervisor, Channel Engines and UIs.
 *
 * Transport: QLocalSocket (named pipes) with 4-byte length framing; payloads
 * are JSON (since PS-5 a frame may also be binary: JSON head + raw bytes, see kBinaryFrameFlag). The Supervisor is the hub: engines and UIs are clients.
 *
 * Numeric values are part of the wire format; append new types at the end.
 */
namespace IpcProtocol {

    enum MessageType {
        // Supervisor -> Engine
        MSG_TIME_SYNC = 0,       // Broadcast corrected time to all engines
        MSG_PLAYLIST_PUSH,       // Push playlist items to engine (payload: playlist[], replaceCurrent)
        MSG_TRANSPORT_COMMAND,   // play / pause / stop / skip / cue / take / goto / seek / hold
        MSG_CONFIG_UPDATE,       // Change engine settings (slate, preroll, outputs)

        // Engine -> Supervisor
        MSG_HEARTBEAT,           // Health status, frame counters, buffer depth
        MSG_STATE_CHANGE,        // ON_AIR, IDLE, ERROR, etc.
        MSG_ASRUN_LOG,           // Frame-accurate as-run event (payload: engine event JSON)
        MSG_DRIFT_REPORT,        // Time drift vs master
        MSG_ENGINE_READY,        // Engine has started and is ready
        MSG_ENGINE_FATAL,        // Engine crash / unrecoverable error

        // UI -> Supervisor
        MSG_UI_COMMAND,          // {channelId, action, params} transport/schedule commands from UI
        MSG_UI_QUERY_STATUS,     // Register as a UI client and request a full snapshot
        MSG_UI_QUERY_TELEMETRY,  // Request real-time telemetry stream

        // Supervisor -> UI
        MSG_UI_STATUS,           // Full snapshot: channels[], schedule summary, alarms
        MSG_UI_TELEMETRY,        // Periodic per-channel telemetry (engine stateJson)
        MSG_UI_ALERT,            // {level, message, channelId}

        // Added in Phase 2
        MSG_PREVIEW_FRAME,       // Engine -> Supervisor -> UI: {channelId, jpegBase64, width, height, frame}
        MSG_OUTPUT_CONFIG,       // Supervisor -> Engine: {outputs[]} rebuild sinks
        MSG_CG_COMMAND,          // Supervisor -> Engine: {action: show|hide|clear|update, layer, ...}
        MSG_UI_SCHEDULE,         // Supervisor -> UI: schedule changed {channelId, slotId}
        MSG_UI_ASRUN,            // Supervisor -> UI: as-run row stored
        MSG_UI_RUNLIST,          // Supervisor -> UI: {channelId, runlist[]} currently pushed to engine

        // Added by perf-supervisor PS-3 (engine supervisor link, playout/engine/supervisorlink.h). Older peers ignore
        // both (unknown types fall through every switch).
        MSG_ENGINE_HELLO,        // Engine -> Supervisor on every (re)connect, before MSG_ENGINE_READY: {channelId, pid,
                                 // createTime (FILETIME ticks as a decimal string), createTimeMs, exe, exeMtime, configHash,
                                 // supervisorIpc, supervisorPid, orphan, state, itemId, frame, outputs[]} (supervisorlink.h)
        MSG_SUP_ALIVE,           // Supervisor -> client, 1 Hz: {seq, loopLagMs, pid}. Without channelId: liveness for UIs.
                                 // With channelId = an engine's channel, sent only to the client bound to that channel:
                                 // "this Supervisor has bound (adopted) you"; it ends the engine's orphan mode and makes
                                 // `pid` its new parent. Droppable (the next one replaces it).

        // Added by perf-supervisor PS-5 (binary preview frames, W8). Both are transport control messages: IpcTransport
        // handles them itself and never hands them to its owner. Older peers ignore them (unknown types).
        MSG_IPC_CAPS,            // either way, JSON: {binaryFrames: highest binary frame version this end accepts (0 =
                                 // none), transport: 1}. A PS-5 client sends it first on every connection; a PS-5 server
                                 // answers only clients that sent one, so an old client never sees anything new.
        MSG_UI_PREVIEW_SUBSCRIBE // UI -> Supervisor: {all, keys[], maxFps}: which preview keys (msg.channelId of a relayed
                                 // MSG_PREVIEW_FRAME: "main", "main:in3", "main:*" = prefix) this client wants, at most
                                 // maxFps per key (0 = no cap). Applied by the server transport; no subscription = all.
    };

    /**
     * Binary frames (PS-5). The 4-byte little-endian length header of a frame is normally the JSON body's length (at most
     * 64 MB, so bit 31 is never set). With bit 31 set (kBinaryFrameFlag) the low 31 bits are the length of a binary body:
     *
     *     u8 version (kBinaryFrameVersion) | u32 LE headLen | head: compact JSON of the message without the bytes,
     *     {type, channelId, timestampNs, payload, binaryKey} | raw bytes (a JPEG) up to the end of the body
     *
     * A sender uses it only towards a peer that announced binaryFrames >= the version in MSG_IPC_CAPS; everyone else
     * gets the JSON form with the bytes as base64 under payload[binaryKey] (exactly the pre-PS-5 shape, e.g.
     * "jpegBase64"), so an old reader never meets the flag. A receiver skips (does not close on) a binary body of a
     * version it does not know: the length is still valid.
     */
    constexpr quint32 kBinaryFrameFlag = 0x80000000u;
    constexpr quint32 kFrameLengthMask = 0x7FFFFFFFu;
    constexpr int kBinaryFrameVersion = 1;

    struct Message {
        MessageType type = MSG_HEARTBEAT;
        QString channelId;       // Empty for broadcasts
        qint64 timestampNs = 0;  // Nanoseconds since epoch
        QJsonObject payload;
        // PS-5: raw bytes carried beside the JSON (a preview JPEG) and the payload key that holds them as base64 in the
        // JSON form. Empty binary = an ordinary JSON message.
        QByteArray binary;
        QString binaryKey;

        // JSON form. With binary set, the bytes are inlined as base64 under payload[binaryKey] (the pre-PS-5 wire shape)
        // without re-serializing them through QJsonDocument.
        QByteArray toJson() const;
        static Message fromJson(const QByteArray& data);
    };

    // Frames (4-byte header included). binaryOk: the peer accepts binary frames; ignored for a message without binary.
    QByteArray encodeFrame(const Message& msg, bool binaryOk);
    QByteArray binaryBody(const Message& msg);   // the body of a binary frame (no header)
    enum class BinaryDecode { Ok, UnknownVersion, Malformed };
    BinaryDecode decodeBinaryBody(const char* data, qint64 len, Message* out);
    // binary -> payload[binaryKey] as base64, binary cleared (for owners that only know the JSON shape).
    void inlineBinary(Message& msg);
    // A preview frame in either form -> its JPEG; liftPreview also moves an old JSON frame's base64 into msg.binary
    // (binaryKey "jpegBase64", key removed from the payload) so a relay sends each peer its own form.
    QByteArray previewJpeg(const Message& msg);
    QByteArray liftPreview(Message& msg);

    // Helper builders
    Message makeTimeSync(qint64 frameNumber, qint64 wallClockNs, qint64 masterOffsetNs);
    Message makeHeartbeat(const QString& channelId, qint64 frameNumber, int bufferDepth, bool isHealthy);
    Message makePlaylistPush(const QString& channelId, const QJsonArray& playlist, bool replaceCurrent = false);
    Message makeTransportCommand(const QString& channelId, const QString& action, const QJsonObject& params);
    Message makeStateChange(const QString& channelId, const QString& state, const QString& reason);
    Message makeAsRunLog(const QString& channelId, const QString& eventType, const QString& assetId, qint64 frameNumber);
    Message make(MessageType type, const QString& channelId, const QJsonObject& payload);
    Message makeEngineHello(const QString& channelId, const QJsonObject& facts);
    // channelId empty = liveness broadcast; set = adoption confirmation for that channel's engine (see MSG_SUP_ALIVE).
    Message makeSupAlive(qint64 seq, double loopLagMs, qint64 supervisorPid, const QString& channelId = QString());
    // PS-5. The JPEG travels as msg.binary (binaryKey "jpegBase64"); payload = width, height, frame, input, meters ...
    Message makePreviewFrame(const QString& channelId, const QByteArray& jpeg, const QJsonObject& payload);
    Message makeIpcCaps(int binaryFrames);
    // all = every key (keys ignored); else only `keys` (empty = none). maxFps 0 = no cap.
    Message makePreviewSubscribe(bool all, const QStringList& keys, double maxFps);

} // namespace IpcProtocol

#endif // IPCPROTOCOL_H
