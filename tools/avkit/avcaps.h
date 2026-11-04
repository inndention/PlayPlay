// PlayPlayAvKit capabilities (docs/plans/inhouse-deps.md §6.1, DEP-01): what `ffmpeg -hide_banner -encoders` and
// `ffmpeg -version` report, read in-process from the libav this process links (av_codec_iterate,
// avcodec_configuration) instead of spawning ffmpeg.exe and waiting up to 15 s on the caller's thread.
// More correct than the spawn as well: an ffmpeg.exe found elsewhere can be a different build than the libav the
// engines actually encode with.
//
// Listed is not usable: a hardware encoder (h264_qsv, *_nvenc, *_amf, *_mf) is listed whenever the build has it, also on
// a machine without that GPU/driver. The engine already finds out by opening the encoder
// (EncoderStage::actualVideoCodec); nothing here opens encoders.
//
// Thread-safe; the encoder list is built once per process (about a millisecond) and cached.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace avkit {

/// "path" of the capabilities object: there is no ffmpeg.exe behind it.
inline constexpr const char* kInProcessPath = "(in-process libav)";

struct Encoder {
    QString name;             // "mpeg2video", "libx264", "h264_qsv"
    QString longName;         // AVCodec::long_name ("" when the codec has none)
    QString codec;            // codec descriptor name: "h264" for libx264 / h264_qsv
    QChar type;               // 'V' video, 'A' audio, 'S' subtitle, 'D' data, 'T' attachment, '?' other
    QString flags;            // the 6-character column of `ffmpeg -encoders`: type + F S X B D, e.g. "V.S..D"
    bool hardware = false;    // AV_CODEC_CAP_HARDWARE or AV_CODEC_CAP_HYBRID (listed, not necessarily usable)
    bool experimental = false;
    /// The description `ffmpeg -encoders` prints: long name, plus " (codec <descriptor>)" when the names differ.
    QString description() const;
};

/// Every encoder in `ffmpeg -encoders` order: codec descriptors sorted by media type then name, the encoders of each
/// descriptor in av_codec_iterate order; descriptors named "*_deprecated" are skipped (as fftools does).
const QList<Encoder>& encoders();
/// Byte for byte what `ffmpeg -hide_banner -encoders` prints (legend included, '\n' line ends).
QByteArray encodersText();
bool hasEncoder(const QString& name);

/// avcodec_configuration(): the `configuration:` value of `ffmpeg -version` ("--prefix=... --enable-libsrt ...").
QString configuration();
/// av_version_info(), e.g. "n9.0.2-33-g4cf298e66a-20261022".
QString versionInfo();
/// First line of the capabilities "version": "ffmpeg version <versionInfo()> (in-process libav)".
QString versionLine();
/// {ffmpeg, license, libavutil, libavcodec, libavformat, libswscale, libswresample, libavfilter, libavdevice}: "61.3.100"
/// style. avutil/avcodec/avformat are the loaded DLLs (runtime); the other four come from the headers, so linking AvKit
/// adds no DLL dependency beyond avutil/avcodec/avformat.
QJsonObject versions();

/// The encoders MuxManager always reports in has{} (false when absent), so callers can test without contains().
QStringList watchedEncoders();
/// MuxManager's build{} flags derived from a configuration string and a has{} object:
/// {srt, rist, ndi, qsv, nvenc, x264, x265}.
QJsonObject buildFlags(const QString& configuration, const QJsonObject& has);
/// Exactly the "ffmpeg" object MuxManager::capabilities() builds from the two spawns:
/// {path: kInProcessPath, found: true, version, video[{name, description}], audio[...], has{name: bool}, build{}}.
/// video/audio/has follow MuxManager::parseFfmpegEncoders (V/A rows into the arrays, V/A/S names into has).
QJsonObject ffmpegCapabilities();

} // namespace avkit
