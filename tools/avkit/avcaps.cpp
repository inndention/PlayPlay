// PlayPlayAvKit capabilities: fftools' print_codecs(encoder=1) and `-version`, in-process. See avcaps.h.
#include "avcaps.h"

#include <QJsonArray>

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavdevice/version.h>
#include <libavfilter/version.h>
#include <libswresample/version.h>
#include <libswscale/version.h>
}

namespace avkit {
namespace {

QChar mediaTypeChar(AVMediaType t) {   // fftools get_media_type_char
    switch (t) {
    case AVMEDIA_TYPE_VIDEO: return 'V';
    case AVMEDIA_TYPE_AUDIO: return 'A';
    case AVMEDIA_TYPE_DATA: return 'D';
    case AVMEDIA_TYPE_SUBTITLE: return 'S';
    case AVMEDIA_TYPE_ATTACHMENT: return 'T';
    default: return '?';
    }
}

QString versionString(unsigned v) { return QString("%1.%2.%3").arg(AV_VERSION_MAJOR(v)).arg(AV_VERSION_MINOR(v)).arg(AV_VERSION_MICRO(v)); }

QList<Encoder> buildEncoders() {
    // fftools get_codecs_sorted: every descriptor, sorted by media type then name (names are unique, so qsort's
    // instability never shows).
    std::vector<const AVCodecDescriptor*> descs;
    for (const AVCodecDescriptor* d = nullptr; (d = avcodec_descriptor_next(d));) descs.push_back(d);
    std::sort(descs.begin(), descs.end(), [](const AVCodecDescriptor* a, const AVCodecDescriptor* b) {
        return a->type != b->type ? a->type < b->type : std::strcmp(a->name, b->name) < 0;
    });
    QList<Encoder> out;
    for (const AVCodecDescriptor* d : descs) {
        if (std::strstr(d->name, "_deprecated")) continue;
        void* it = nullptr;
        for (const AVCodec* c; (c = av_codec_iterate(&it));) {       // next_codec_for_id(id, encoder=1)
            if (c->id != d->id || !av_codec_is_encoder(c)) continue;
            Encoder e;
            e.name = QString::fromUtf8(c->name);
            e.longName = c->long_name ? QString::fromUtf8(c->long_name) : QString();
            e.codec = QString::fromUtf8(d->name);
            e.type = mediaTypeChar(d->type);
            const int caps = c->capabilities;
            const char f[7] = {char(e.type.toLatin1()), (caps & AV_CODEC_CAP_FRAME_THREADS) ? 'F' : '.', (caps & AV_CODEC_CAP_SLICE_THREADS) ? 'S' : '.',
                               (caps & AV_CODEC_CAP_EXPERIMENTAL) ? 'X' : '.', (caps & AV_CODEC_CAP_DRAW_HORIZ_BAND) ? 'B' : '.',
                               (caps & AV_CODEC_CAP_DR1) ? 'D' : '.', 0};
            e.flags = QString::fromLatin1(f);
            e.hardware = (caps & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) != 0;
            e.experimental = (caps & AV_CODEC_CAP_EXPERIMENTAL) != 0;
            out.append(e);
        }
    }
    return out;
}

} // namespace

QString Encoder::description() const { return name != codec ? longName + " (codec " + codec + ")" : longName; }

const QList<Encoder>& encoders() {
    static const QList<Encoder> list = buildEncoders();     // C++11 static init: thread-safe, built once
    return list;
}

QByteArray encodersText() {
    QByteArray t = "Encoders:\n"
                   " V..... = Video\n"
                   " A..... = Audio\n"
                   " S..... = Subtitle\n"
                   " .F.... = Frame-level multithreading\n"
                   " ..S... = Slice-level multithreading\n"
                   " ...X.. = Codec is experimental\n"
                   " ....B. = Supports draw_horiz_band\n"
                   " .....D = Supports direct rendering method 1\n"
                   " ------\n";
    for (const Encoder& e : encoders()) {
        // printf(" %c%c%c%c%c%c", ...); printf(" %-20s %s", name, long_name); " (codec %s)" when the names differ
        t += ' '; t += e.flags.toLatin1();
        t += ' '; t += e.name.toUtf8().leftJustified(20, ' ');
        t += ' '; t += e.longName.toUtf8();
        if (e.name != e.codec) { t += " (codec "; t += e.codec.toUtf8(); t += ')'; }
        t += '\n';
    }
    return t;
}

bool hasEncoder(const QString& name) {
    for (const Encoder& e : encoders()) if (e.name == name) return true;
    return false;
}

QString configuration() { return QString::fromUtf8(avcodec_configuration()); }
QString versionInfo() { return QString::fromUtf8(av_version_info()); }
QString versionLine() { return QString("ffmpeg version %1 %2").arg(versionInfo(), kInProcessPath); }

QJsonObject versions() {
    QJsonObject v;
    v["ffmpeg"] = versionInfo();
    v["license"] = QString::fromUtf8(avcodec_license());
    v["libavutil"] = versionString(avutil_version());
    v["libavcodec"] = versionString(avcodec_version());
    v["libavformat"] = versionString(avformat_version());
    // header values: calling swscale_version() & co. would make every program linking AvKit import those DLLs too
    v["libavfilter"] = versionString(LIBAVFILTER_VERSION_INT);
    v["libavdevice"] = versionString(LIBAVDEVICE_VERSION_INT);
    v["libswscale"] = versionString(LIBSWSCALE_VERSION_INT);
    v["libswresample"] = versionString(LIBSWRESAMPLE_VERSION_INT);
    return v;
}

QStringList watchedEncoders() {
    return {"libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf",
            "h264_mf", "hevc_mf", "ac3", "eac3", "aac", "mp2", "libopus"};
}

QJsonObject buildFlags(const QString& conf, const QJsonObject& has) {
    // the same tests MuxManager::capabilities applies to the `configuration:` line of `ffmpeg -version`
    QJsonObject b;
    b["srt"] = conf.contains("--enable-libsrt");
    b["rist"] = conf.contains("--enable-librist");
    b["ndi"] = conf.contains("libndi");
    b["qsv"] = conf.contains("--enable-libvpl") || conf.contains("--enable-libmfx");
    b["nvenc"] = conf.contains("--enable-nvenc") || conf.contains("--enable-ffnvcodec") || has.value("h264_nvenc").toBool();
    b["x264"] = conf.contains("--enable-libx264");
    b["x265"] = conf.contains("--enable-libx265");
    return b;
}

QJsonObject ffmpegCapabilities() {
    QJsonArray video, audio; QJsonObject has;
    for (const Encoder& e : encoders()) {
        if (e.type != 'V' && e.type != 'A' && e.type != 'S') continue;   // parseFfmpegEncoders matches ^ ([VAS])
        if (e.type != 'S') {
            QJsonObject o; o["name"] = e.name; o["description"] = e.description().trimmed();
            (e.type == 'V' ? video : audio).append(o);
        }
        has[e.name] = true;
    }
    for (const QString& n : watchedEncoders()) if (!has.contains(n)) has[n] = false;
    QJsonObject ff;
    ff["path"] = QString::fromLatin1(kInProcessPath);
    ff["found"] = true;
    ff["version"] = versionLine();
    ff["video"] = video; ff["audio"] = audio; ff["has"] = has;
    ff["build"] = buildFlags(configuration(), has);
    return ff;
}

} // namespace avkit
