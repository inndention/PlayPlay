#ifndef PLAYPLAY_FRAMERATE_H
#define PLAYPLAY_FRAMERATE_H

#include <QString>
#include <QtGlobal>
#include <cstdint>

/**
 * FrameRate — exact rational frame rate (e.g. 30000/1001) with overflow-safe
 * frame <-> nanosecond conversion.
 *
 * Broadcast rates are not integers: 29.97 fps is exactly 30000/1001. Using a
 * double accumulates error over a 24/7 run; using int64 naively overflows
 * (frame * 1e9 * 1001 exceeds 2^63 after ~85 hours). frameToNs() splits the
 * multiplication into a quotient/remainder so it stays exact for ~290 years.
 */
struct FrameRate {
    int num = 30;
    int den = 1;

    constexpr FrameRate() = default;
    constexpr FrameRate(int n, int d) : num(n), den(d > 0 ? d : 1) {}

    static constexpr FrameRate ntsc()      { return {30000, 1001}; } // 29.97
    static constexpr FrameRate ntsc60()    { return {60000, 1001}; } // 59.94
    static constexpr FrameRate film()      { return {24000, 1001}; } // 23.976
    static constexpr FrameRate pal()       { return {25, 1}; }
    static constexpr FrameRate pal50()     { return {50, 1}; }
    static constexpr FrameRate fps30()     { return {30, 1}; }
    static constexpr FrameRate fps60()     { return {60, 1}; }

    bool isValid() const { return num > 0 && den > 0; }
    double fps() const { return isValid() ? double(num) / double(den) : 0.0; }

    /// NTSC-family rates (den == 1001) use drop-frame timecode.
    bool isDropFrame() const { return den == 1001 && (num == 30000 || num == 60000); }

    /// Nominal integer rate used for timecode (30 for 29.97, 60 for 59.94).
    int nominalFps() const { return isValid() ? (num + den / 2) / den : 30; }

    /// Nanoseconds per frame (truncated). Use frameToNs for exact timelines.
    qint64 frameDurationNs() const {
        return isValid() ? (1000000000LL * den) / num : 33333333LL;
    }

    /// Exact start time of `frame` in nanoseconds from timeline origin.
    qint64 frameToNs(qint64 frame) const {
        if (!isValid()) return frame * 33333333LL;
        // frame * 1e9 * den / num, split to avoid overflow.
        const qint64 nsPerDen = 1000000000LL * den;   // <= 1.001e12
        const qint64 q = frame / num;
        const qint64 r = frame % num;                  // < num <= 60000
        return q * nsPerDen + (r * nsPerDen) / num;    // r*nsPerDen <= 6e16, safe
    }

    /// Frame index whose span [frameToNs(f), frameToNs(f+1)) contains `ns`.
    qint64 nsToFrame(qint64 ns) const {
        if (!isValid()) return ns / 33333333LL;
        if (ns < 0) return -1;
        // Estimate ns * num / (1e9 * den) with split arithmetic, then snap to the
        // exact span boundaries produced by frameToNs (which truncates).
        const qint64 qs = ns / 1000000000LL;
        const qint64 rs = ns % 1000000000LL;
        const qint64 whole = (qs * num) / den;
        const qint64 wholeRem = (qs * num) % den;
        qint64 f = whole + (wholeRem * 1000000000LL + rs * num) / (1000000000LL * den);
        while (frameToNs(f + 1) <= ns) ++f;
        while (f > 0 && frameToNs(f) > ns) --f;
        return f;
    }

    /// Audio samples that belong to frame `frame` so that the sample clock
    /// stays locked to the video clock (1601/1602 cadence at 29.97 & 48 kHz).
    qint64 samplesForFrame(qint64 frame, int sampleRate = 48000) const {
        return sampleBoundary(frame + 1, sampleRate) - sampleBoundary(frame, sampleRate);
    }

    /// First sample index of `frame`: floor(frame * sampleRate * den / num).
    qint64 sampleBoundary(qint64 frame, int sampleRate = 48000) const {
        if (!isValid()) return frame * sampleRate / 30;
        const qint64 perDen = qint64(sampleRate) * den;   // <= 4.8e7
        const qint64 q = frame / num;
        const qint64 r = frame % num;
        return q * perDen + (r * perDen) / num;
    }

    QString toString() const {
        if (den == 1) return QString::number(num);
        return QString("%1/%2").arg(num).arg(den);
    }

    /// Parse "30000/1001", "29.97", "29.976", "59.94", "25", "30".
    static FrameRate fromString(const QString& s) {
        const QString t = s.trimmed();
        if (t.contains('/')) {
            const auto parts = t.split('/');
            if (parts.size() == 2) return {parts[0].toInt(), parts[1].toInt()};
        }
        bool ok = false;
        const double d = t.toDouble(&ok);
        if (!ok || d <= 0) return fps30();
        return fromDouble(d);
    }

    /// Snap a floating rate to the nearest well-known broadcast rate.
    static FrameRate fromDouble(double d) {
        struct Known { double v; FrameRate r; };
        static const Known known[] = {
            {23.976, film()}, {24.0, {24, 1}}, {25.0, pal()}, {29.97, ntsc()},
            {30.0, fps30()}, {50.0, pal50()}, {59.94, ntsc60()}, {60.0, fps60()},
        };
        for (const auto& k : known) {
            if (qAbs(d - k.v) < 0.01) return k.r;
        }
        // Unknown: approximate with 1000 denominator.
        return {int(d * 1000.0 + 0.5), 1000};
    }

    bool operator==(const FrameRate& o) const { return num == o.num && den == o.den; }
    bool operator!=(const FrameRate& o) const { return !(*this == o); }
};

/**
 * Timecode — SMPTE 12M timecode formatting with drop-frame support.
 */
namespace Timecode {

/// Frame count since midnight -> "HH:MM:SS:FF" (or "HH:MM:SS;FF" drop-frame).
inline QString fromFrames(qint64 frames, const FrameRate& rate) {
    const int fps = rate.nominalFps();
    if (frames < 0) frames = 0;

    qint64 ff, ss, mm, hh;
    if (rate.isDropFrame()) {
        // Standard DF algorithm: 2 frames dropped per minute except every 10th
        // minute (4 frames at 59.94). Convert a frame count on the real-time
        // (29.97) clock into the labelled 30-fps-nominal timecode.
        const qint64 dropPerMin = (fps == 60) ? 4 : 2;
        const qint64 framesPer10Min = fps * 60 * 10 - dropPerMin * 9;  // 17982 @30
        const qint64 framesPerMin   = fps * 60 - dropPerMin;           // 1798 @30
        const qint64 tenMin = frames / framesPer10Min;
        qint64 rem = frames % framesPer10Min;
        qint64 adjusted = frames + dropPerMin * 9 * tenMin;
        if (rem >= dropPerMin) {
            adjusted += dropPerMin * ((rem - dropPerMin) / framesPerMin);
        }
        ff = adjusted % fps;
        ss = (adjusted / fps) % 60;
        mm = (adjusted / (fps * 60)) % 60;
        hh = (adjusted / (fps * 3600)) % 24;
    } else {
        ff = frames % fps;
        ss = (frames / fps) % 60;
        mm = (frames / (fps * 60)) % 60;
        hh = (frames / (fps * 3600)) % 24;
    }
    const QChar sep = rate.isDropFrame() ? QChar(';') : QChar(':');
    return QString("%1:%2:%3%4%5")
        .arg(hh, 2, 10, QChar('0')).arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0')).arg(sep).arg(ff, 2, 10, QChar('0'));
}

/// Nanoseconds since midnight -> timecode string.
inline QString fromNsOfDay(qint64 nsOfDay, const FrameRate& rate) {
    return fromFrames(rate.nsToFrame(nsOfDay), rate);
}

} // namespace Timecode

#endif // PLAYPLAY_FRAMERATE_H
