#include "compose/filtergraph.h"

#include <QHash>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace studio::compose {

namespace {

// node's path.basename / path.extname over both separators (compose.ts names staged files and warnings with them)
QString baseName(const QString& p) {
    const qsizetype i = std::max(p.lastIndexOf(QLatin1Char('/')), p.lastIndexOf(QLatin1Char('\\')));
    return p.mid(i + 1);
}
QString extName(const QString& p) {
    const QString b = baseName(p);
    const qsizetype i = b.lastIndexOf(QLatin1Char('.'));
    return i <= 0 ? QString() : b.mid(i);
}

const std::array<KenBurns, 6> kAutoKb{KenBurns::In, KenBurns::Right, KenBurns::Out, KenBurns::Left, KenBurns::In, KenBurns::Up};

struct Ctx {
    int W = 0, H = 0;
    double fps = 30;
    Quality quality = Quality::Final;
    Aspect aspect = Aspect::Vertical;
    QString font, workDir;
    ProbeFn probe;
    bool fileOnly = true;
    bool limited = true;                                           // PlanOptions::limitedRange
    QStringList warnings;
    int textN = 0;
    QVector<TextFile>* texts = nullptr;                            // the step being built
    QHash<QString, SourceInfo> probed;                             // a clip is asked about twice (picture, then sound)

    QString out(const QString& name) const { return workDir.isEmpty() ? name : workDir + QLatin1Char('/') + name; }
    SourceInfo info(const QString& path) {
        if (!probe) return {};
        if (const auto it = probed.constFind(path); it != probed.cend()) return *it;
        return *probed.insert(path, probe(path));
    }
    /// "-i path", behind a file-only protocol whitelist unless the caller compares with compose.ts
    QStringList input(const QString& path) const {
        return fileOnly ? QStringList{QStringLiteral("-protocol_whitelist"), QStringLiteral("file"), QStringLiteral("-i"), path}
                        : QStringList{QStringLiteral("-i"), path};
    }
};

/// A drawtext text file (work-dir relative), numbered across the whole plan in compose.ts call order.
QString textFile(Ctx& c, const QString& s) {
    const QString name = QStringLiteral("t%1.txt").arg(c.textN++);
    c.texts->push_back({name, s});
    return name;
}

struct DrawOpts {
    QString color;                   // "" = white
    QString box;                     // "" = no box
    bool border = false;
    bool hasEnable = false;
    double enableFrom = 0, enableTo = 0;
    double fadeIn = 0;
    std::optional<double> boxPad;
};

/// drawtext filters for a block of lines; y0 = top of the block.
QStringList drawLines(Ctx& c, const QStringList& lines, int size, double y0, const DrawOpts& o) {
    const double lh = jsRound(size * 1.28);
    QStringList out;
    for (qsizetype j = 0; j < lines.size(); ++j) {
        QStringList parts{QStringLiteral("drawtext=") + c.font, QStringLiteral("textfile=") + textFile(c, lines[j]),
                          QStringLiteral("expansion=none"), QStringLiteral("fontsize=") + num(size),
                          QStringLiteral("fontcolor=") + (o.color.isEmpty() ? QStringLiteral("white") : o.color),
                          QStringLiteral("x=(w-text_w)/2"), QStringLiteral("y=") + num(jsRound(y0 + double(j) * lh))};
        if (o.border) parts << QStringLiteral("borderw=") + num(std::max(1.0, jsRound(size * 0.07))) << QStringLiteral("bordercolor=black@0.7");
        if (!o.box.isEmpty())
            parts << QStringLiteral("box=1") << QStringLiteral("boxcolor=") + o.box
                  << QStringLiteral("boxborderw=") + num(std::max(2.0, jsRound(o.boxPad ? *o.boxPad : size * 0.28)));
        if (o.fadeIn != 0) parts << QStringLiteral("alpha='min(1,t/%1)'").arg(num(o.fadeIn));
        if (o.hasEnable) parts << QStringLiteral("enable='between(t,%1,%2)'").arg(num(r3(o.enableFrom)), num(r3(o.enableTo)));
        out << parts.join(QLatin1Char(':'));
    }
    return out;
}

double blockHeight(qsizetype n, double size) { return double(n - 1) * jsRound(size * 1.28) + size; }

/// The headline overlay for a clip (on the clip's own timeline).
QStringList headlineFilters(Ctx& c, const QString& text, TextPosition pos, bool onCard, const QString& cardText) {
    const SafeArea sa = safeArea(c.aspect);
    const double minSide = std::min(c.W, c.H);
    const double width = c.W * (1 - 2 * sa.marginX);
    const double base = onCard ? minSide * 0.095 : minSide * 0.072;
    const Fitted fit = fitText(text, width, base, onCard ? 4 : 3);
    if (fit.truncated) c.warnings << QStringLiteral("on-screen text was shortened to fit: \"%1\"").arg(cleanText(text).left(60));
    const double bh = blockHeight(fit.lines.size(), fit.size);
    // 'bottom' is a lower third ABOVE the two-line caption band, so headlines and captions never overlap
    const double y0 = pos == TextPosition::Center ? (c.H - bh) / 2
                      : pos == TextPosition::Top  ? c.H * sa.headTop
                                                  : c.H * (sa.capBottom - 0.16) - bh;
    DrawOpts o;
    o.fadeIn = 0.35;
    if (onCard) o.color = cardText.isEmpty() ? QStringLiteral("white") : cardText;
    else o.box = QStringLiteral("black@0.42");
    return drawLines(c, fit.lines, fit.size, y0, o);
}

QStringList segEncode(const Ctx& c, double L, const QString& out) {
    const bool draft = c.quality == Quality::Draft;
    return {QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), draft ? QStringLiteral("ultrafast") : QStringLiteral("veryfast"),
            QStringLiteral("-crf"), draft ? QStringLiteral("23") : QStringLiteral("16"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-r"), num(c.fps), QStringLiteral("-an"), QStringLiteral("-t"), num(r3(L)), out};
}

/// A segment's last filter: yuv420p, in limited range unless the plan is compared with compose.ts (see limitedRange).
QString yuvOut(const Ctx& c) { return c.limited ? QStringLiteral("scale=out_range=tv,format=yuv420p") : QStringLiteral("format=yuv420p"); }

QString tail(const Ctx& c, const QStringList& extra) {
    QStringList t;
    for (const QString& e : extra) if (!e.isEmpty()) t << e;
    t << yuvOut(c);
    return t.join(QLatin1Char(','));
}

QString zoompan(const ZoomExpr& e, int d, const Ctx& c) {
    return QStringLiteral("zoompan=z='%1':x='%2':y='%3':d=%4:s=%5x%6:fps=%7")
        .arg(e.z, e.x, e.y, num(d), num(c.W), num(c.H), num(c.fps));
}

QString seqName(int i) { return QStringLiteral("seg%1.mp4").arg(i, 3, 10, QLatin1Char('0')); }

/// What a clip turned out to be once its files were probed (compose.ts falls back to a still or a colour card).
struct Resolved { ClipKind kind; QString src; };

Step renderSegment(Ctx& c, int i, const Clip& sc, double L, Resolved* res) {
    Step st;
    st.kind = Step::Kind::Segment;
    st.slot = i;
    st.output = c.out(seqName(i));
    st.seconds = r3(L);
    c.texts = &st.texts;
    ClipKind kind = sc.kind;
    QString src = sc.src;
    if (kind == ClipKind::Video && !src.isEmpty() && !c.info(src).video) {
        c.warnings << QStringLiteral("scene %1: the video clip could not be read (%2); %3").arg(i + 1).arg(baseName(src),
            sc.fallbackImage.isEmpty() ? QStringLiteral("used a colour card instead") : QStringLiteral("used its still image instead"));
        if (!sc.fallbackImage.isEmpty()) { kind = ClipKind::Image; src = sc.fallbackImage; }
        else { kind = ClipKind::Color; src.clear(); }
    }
    if (kind == ClipKind::Image && !src.isEmpty() && !c.info(src).video) {
        c.warnings << QStringLiteral("scene %1: the image could not be read (%2); used a colour card instead").arg(i + 1).arg(baseName(src));
        kind = ClipKind::Color;
        src.clear();
    }
    *res = {kind, src};
    const QString cardBg = hexColor(sc.color, QStringLiteral("0x111827"));
    QStringList head;
    if (!sc.text.isEmpty()) {
        const TextPosition pos = sc.textPosition != TextPosition::Auto ? sc.textPosition
                                 : kind == ClipKind::Color ? TextPosition::Center : TextPosition::Top;
        head = headlineFilters(c, sc.text, pos, kind == ClipKind::Color, textOn(cardBg));
    }
    const double up = c.quality == Quality::Draft ? 1.5 : 2;      // zoompan on an upscaled frame moves in sub-pixel steps
    const int W2 = int(jsRound((c.W * up) / 2) * 2), H2 = int(jsRound((c.H * up) / 2) * 2);
    const int n = int(std::max(1.0, jsRound(L * c.fps)));

    if (kind == ClipKind::Image && !src.isEmpty()) {
        // stage the still under a plain name (image2 would read "%d" in a path as a sequence pattern)
        const QString ext = extName(src).toLower();
        const QString staged = QStringLiteral("img%1%2").arg(i).arg(ext.isEmpty() ? QStringLiteral(".png") : ext);
        st.staged.push_back({src, staged});
        const KenBurns kb = sc.kenburns == KenBurns::Auto ? kAutoKb[size_t(i) % kAutoKb.size()] : sc.kenburns;
        const ZoomExpr e = sc.move ? moveExpr(*sc.move, n) : kenburnsExpr(kb, n);
        st.graph = fitChain(QStringLiteral("[0:v]"), W2, H2, sc.fit, QStringLiteral("[fit]"), sc.focusX, sc.focusY)
                   + QStringLiteral(";[fit]") + zoompan(e, n, c) + QStringLiteral(",setsar=1,") + tail(c, head) + QStringLiteral("[v]");
        st.args = c.input(staged) + QStringList{QStringLiteral("-filter_complex"), st.graph, QStringLiteral("-map"), QStringLiteral("[v]"),
                                                QStringLiteral("-frames:v"), num(n)} + segEncode(c, L, st.output);
        return st;
    }
    if (kind == ClipKind::Video && !src.isEmpty()) {
        QString in = QStringLiteral("[0:v]");
        if (sc.speed != 1) in += QStringLiteral("setpts=(PTS-STARTPTS)/%1,").arg(num(sc.speed));
        // a clip that runs out is looped (compose.ts), but -stream_loop seeks back to the FILE start, not to -ss: a clip
        // with an in-point holds its last frame instead (an ads clip trimmed to its master's end reads (slot + overlap) *
        // speed seconds, past that end, during the transition into the next clip)
        const bool hold = sc.srcStart > 0;
        const QString fpsTo = QStringLiteral(";[fit]fps=") + num(c.fps) + QLatin1Char(',')
                              + (hold ? QStringLiteral("tpad=stop_mode=clone:stop_duration=%1,").arg(num(std::ceil(L))) : QString());
        if (sc.move) {   // a camera move across the clip: the same upscaled zoompan, one output frame per input frame
            st.graph = fitChain(in, W2, H2, sc.fit, QStringLiteral("[fit]"), sc.focusX, sc.focusY) + fpsTo
                       + zoompan(moveExpr(*sc.move, n), 1, c) + QStringLiteral(",setsar=1,") + tail(c, head) + QStringLiteral("[v]");
        } else {
            st.graph = fitChain(in, c.W, c.H, sc.fit, QStringLiteral("[fit]"), sc.focusX, sc.focusY) + fpsTo + tail(c, head) + QStringLiteral("[v]");
        }
        if (hold) st.args << QStringLiteral("-ss") << num(r3(sc.srcStart));
        else st.args << QStringLiteral("-stream_loop") << QStringLiteral("-1");
        st.args += c.input(src) + QStringList{QStringLiteral("-filter_complex"), st.graph, QStringLiteral("-map"), QStringLiteral("[v]")}
                   + segEncode(c, L, st.output);
        return st;
    }
    // colour card
    const QString accent = sc.accent.isEmpty() ? QString() : hexColor(sc.accent, QString());
    QStringList extra;
    if (!accent.isEmpty()) extra << QStringLiteral("drawbox=x=0:y=ih*0.955:w=iw:h=ih*0.045:color=%1:t=fill").arg(accent);
    extra += head;
    st.graph = QStringLiteral("color=c=%1:s=%2x%3:r=%4:d=%5,setsar=1,").arg(cardBg, num(c.W), num(c.H), num(c.fps), num(r3(L)))
               + tail(c, extra) + QStringLiteral("[v]");
    st.args = QStringList{QStringLiteral("-filter_complex"), st.graph, QStringLiteral("-map"), QStringLiteral("[v]")} + segEncode(c, L, st.output);
    return st;
}

Step renderEndCard(Ctx& c, const EndCard& e, double L, int idx) {
    Step st;
    st.kind = Step::Kind::EndCard;
    st.slot = idx;
    st.output = c.out(seqName(idx));
    st.seconds = r3(L);
    c.texts = &st.texts;
    const QString bg = hexColor(e.colors.value(0), QStringLiteral("0x111827"));
    const QString accent = hexColor(e.colors.value(1), QStringLiteral("0xF59E0B"));
    const QString fg = !e.colors.value(2).isEmpty() ? hexColor(e.colors.value(2), textOn(bg)) : textOn(bg);
    const double minSide = std::min(c.W, c.H);
    const SafeArea sa = safeArea(c.aspect);
    const double width = c.W * (1 - 2 * sa.marginX);
    const bool vertical = c.H > c.W * 1.1;
    bool logoOk = false;
    if (!e.logo.isEmpty()) {
        if (c.info(e.logo).video) logoOk = true;
        else c.warnings << QStringLiteral("end card: the logo could not be read (%1); left it out").arg(baseName(e.logo));
    }
    // vertical stack: logo, brand, headline, CTA pill, URL; centred, inside the safe area
    enum class K { Brand, Head, Cta, Url };
    struct Item { QString text; double size; K kind; int lines; };
    QVector<Item> items;
    if (!e.brand.isEmpty()) items.push_back({e.brand, minSide * (vertical ? 0.11 : 0.1), K::Brand, 2});
    if (!e.headline.isEmpty()) items.push_back({e.headline, minSide * 0.062, K::Head, 3});
    if (!e.cta.isEmpty()) items.push_back({e.cta, minSide * 0.066, K::Cta, 2});
    if (!e.url.isEmpty()) items.push_back({e.url, minSide * 0.05, K::Url, 1});
    struct Fit2 { K kind; Fitted f; };
    QVector<Fit2> fitted;
    for (const Item& it : std::as_const(items)) fitted.push_back({it.kind, fitText(it.text, it.kind == K::Cta ? width * 0.85 : width, it.size, it.lines)});
    const double gap = minSide * 0.045;
    const double logoH = logoOk ? jsRound(c.H * (vertical ? 0.16 : 0.24)) : 0;
    double sum = 0;
    for (const Fit2& f : std::as_const(fitted)) sum = sum + blockHeight(f.f.lines.size(), f.f.size) + (f.kind == K::Cta ? f.f.size * 0.6 : 0);
    const double total = (logoOk ? logoH + gap : 0) + sum + gap * std::max<qsizetype>(0, fitted.size() - 1);
    double y = std::max(c.H * sa.headTop, (c.H - total) / 2 - (vertical ? c.H * 0.03 : 0));
    QStringList filters{QStringLiteral("drawbox=x=0:y=ih*0.955:w=iw:h=ih*0.045:color=%1:t=fill").arg(accent)};
    double logoY = 0;
    if (logoOk) { logoY = jsRound(y); y += logoH + gap; }
    for (qsizetype k = 0; k < fitted.size(); ++k) {
        const Fit2& f = fitted[k];
        const double t0 = 0.15 + double(k) * 0.12;
        DrawOpts o;
        o.fadeIn = t0 + 0.3;
        if (f.kind == K::Cta) {
            y += f.f.size * 0.3;
            o.color = textOn(accent);
            o.box = accent;
            o.boxPad = f.f.size * 0.45;
            filters += drawLines(c, f.f.lines, f.f.size, y, o);
            y += f.f.size * 0.3;
        } else {
            o.color = f.kind == K::Url ? fg + QStringLiteral("@0.85") : fg;
            filters += drawLines(c, f.f.lines, f.f.size, y, o);
        }
        y += blockHeight(f.f.lines.size(), f.f.size) + gap;
    }
    const QString base = QStringLiteral("color=c=%1:s=%2x%3:r=%4:d=%5,setsar=1").arg(bg, num(c.W), num(c.H), num(c.fps), num(r3(L)));
    QStringList args;
    if (logoOk) {
        const QString ext = extName(e.logo).toLower();
        const QString staged = QStringLiteral("logo") + (ext.isEmpty() ? QStringLiteral(".png") : ext);
        st.staged.push_back({e.logo, staged});
        args << QStringLiteral("-loop") << QStringLiteral("1") << QStringLiteral("-framerate") << num(c.fps)
             << QStringLiteral("-t") << num(r3(L));
        args += c.input(staged);
        st.graph = base + QStringLiteral("[bg];[0:v]scale=%1:%2:force_original_aspect_ratio=decrease,format=rgba,fade=t=in:st=0:d=0.4:alpha=1[lg];")
                              .arg(num(jsRound(c.W * 0.6)), num(logoH))
                   + QStringLiteral("[bg][lg]overlay=x=(W-w)/2:y=%1:eof_action=repeat,").arg(num(logoY)) + filters.join(QLatin1Char(','))
                   + QLatin1Char(',') + yuvOut(c) + QStringLiteral("[v]");
    } else {
        st.graph = base + QLatin1Char(',') + filters.join(QLatin1Char(',')) + QLatin1Char(',') + yuvOut(c) + QStringLiteral("[v]");
    }
    st.args = args + QStringList{QStringLiteral("-filter_complex"), st.graph, QStringLiteral("-map"), QStringLiteral("[v]")} + segEncode(c, L, st.output);
    return st;
}

/// Caption drawtext filters on absolute output times for the cues that show inside [from, to) (the single join passes
/// the whole line: -inf..inf).
QStringList captionFilters(Ctx& c, const Timeline& t, double total, double from, double to) {
    QStringList vf;
    if (t.captions.isEmpty()) return vf;
    const SafeArea sa = safeArea(t.aspect);
    const double minSide = std::min(c.W, c.H);
    const double size = jsRound(minSide * (t.aspect == Aspect::Landscape ? 0.052 : 0.058));
    const double width = c.W * (1 - 2 * sa.marginX);
    for (const CaptionCue& cue : t.captions) {
        if (!(cue.end > cue.start) || cue.start >= total) continue;
        if (cue.start >= to || std::min(cue.end, total) <= from) continue;   // another part's window
        const Fitted fit = fitText(cue.text, width, size, 2);
        if (fit.lines.isEmpty()) continue;
        const double y0 = c.H * sa.capBottom - blockHeight(fit.lines.size(), fit.size);
        DrawOpts d;
        d.box = QStringLiteral("black@0.5");
        d.border = true;
        d.hasEnable = true;
        d.enableFrom = cue.start;
        d.enableTo = std::min(cue.end, total);
        vf += drawLines(c, fit.lines, fit.size, y0, d);
    }
    return vf;
}

/// The programme's sound up to one label: every placed part, the clips' own sound, the music bed (looped, faded,
/// ducked under the voice). Inputs are numbered from `firstInput` (after the segments in the single join, 0 alone).
struct Sound {
    QStringList inputs;              // the -i arguments (and the music's -stream_loop)
    QStringList lines;               // graph lines that end in `mix`
    QString mix;                     // "[mix]" or "[vo]"
    bool hasSound = false;           // a voice part or music (else `mix` is silence of the exact length)
    int count = 0;                   // sound files opened
};

Sound buildSound(Ctx& c, const Timeline& t, const Plan& plan, const QVector<Resolved>& resolved, qsizetype firstInput, double total) {
    Sound s;
    QStringList& g = s.lines;
    qsizetype nIn = firstInput;
    QStringList voParts;
    const auto addAudio = [&](const AudioPlacement& a, double absStart) {
        s.inputs += c.input(a.path);
        const QString lbl = QStringLiteral("[vo%1]").arg(voParts.size());
        g << audioChain(QStringLiteral("[%1:a]").arg(nIn), a, absStart, lbl);
        voParts << lbl;
        ++nIn;
    };
    if (t.audio.voiceover) addAudio(*t.audio.voiceover, t.audio.voiceover->start.value_or(0));
    for (const AudioPlacement& e : t.audio.events) addAudio(e, e.start.value_or(0));
    const qsizetype N = plan.spans.size();
    for (qsizetype i = 0; i < N; ++i) {
        const Slot& sl = plan.spans[i];
        const bool isCard = i >= t.clips.size();
        const std::optional<AudioPlacement>& a = isCard ? t.endCard->audio : t.clips[i].audio;
        if (a) addAudio(*a, sl.start + a->start.value_or(0.15));
        if (isCard) continue;
        const Clip& cl = t.clips[i];
        if (cl.audioGain > 0 && resolved[i].kind == ClipKind::Video) {   // the clip's own sound under its slot
            if (cl.speed != 1) c.warnings << QStringLiteral("scene %1: the clip's own sound is left out at speed %2").arg(i + 1).arg(num(cl.speed));
            else if (!c.info(resolved[i].src).audio) c.warnings << QStringLiteral("scene %1: the clip has no sound (%2)").arg(i + 1).arg(baseName(resolved[i].src));
            else {
                AudioPlacement nat;
                nat.path = resolved[i].src;
                nat.from = cl.srcStart;
                nat.maxSeconds = sl.duration;
                nat.volume = cl.audioGain;
                addAudio(nat, sl.start);
            }
        }
    }
    if (voParts.size() == 1) g << voParts[0] + QStringLiteral("anull[vo]");
    else if (voParts.size() > 1) g << voParts.join(QString()) + QStringLiteral("amix=inputs=%1:normalize=0:duration=longest:dropout_transition=0[vo]").arg(voParts.size());
    if (t.audio.music) {
        const MusicBed& m = *t.audio.music;
        s.inputs << QStringLiteral("-stream_loop") << QStringLiteral("-1");
        s.inputs += c.input(m.path);
        const double vol = std::max(0.0, std::min(1.0, m.volume.value_or(0.22)));
        const double fadeOut = std::min(1.5, total / 4);
        g << QStringLiteral("[%1:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,atrim=0:%2,asetpts=PTS-STARTPTS,volume=%3,afade=t=in:d=0.3,afade=t=out:st=%4:d=%5[mus]")
                 .arg(nIn).arg(num(total), num(r3(vol)), num(r3(total - fadeOut)), num(r3(fadeOut)));
        ++nIn;
        if (!voParts.isEmpty() && m.duck) {
            g << QStringLiteral("[vo]asplit=2[vo1][vosc]")
              << QStringLiteral("[mus][vosc]sidechaincompress=threshold=0.015:ratio=12:attack=20:release=400[duck]")
              << QStringLiteral("[vo1][duck]amix=inputs=2:normalize=0:duration=longest[mix]");
        } else if (!voParts.isEmpty()) {
            g << QStringLiteral("[vo][mus]amix=inputs=2:normalize=0:duration=longest[mix]");
        } else {
            g << QStringLiteral("[mus]anull[mix]");
        }
        s.mix = QStringLiteral("[mix]");
    } else if (!voParts.isEmpty()) {
        s.mix = QStringLiteral("[vo]");
    } else {
        g << QStringLiteral("anullsrc=r=48000:cl=stereo,atrim=0:%1[mix]").arg(num(total));
        s.mix = QStringLiteral("[mix]");
    }
    s.hasSound = t.audio.music || !voParts.isEmpty();
    s.count = int(nIn - firstInput);
    return s;
}

/// The end of the sound chain. Compose = compose.ts (one loudnorm pass between the trim and the pad, master fades
/// last); Pre = the same without loudnorm (mix.wav for two passes: the fades go in before the measurement, and a
/// linear gain commutes with them); Apply = the second pass on mix.wav, then the same exact-length pad / trim.
enum class Tail { Compose, Pre, Apply };
QString soundTail(const Timeline& t, double total, bool loud, const LoudnessTarget& target, Tail k) {
    // trim before loudness normalisation (no work on overruns), pad + trim AFTER it: loudnorm works in 100 ms frames
    // and can drop the partial last one, which left the audio stream ~0.1 s shorter than the picture
    QStringList af;
    if (k == Tail::Apply) {
        af << QString::fromUtf16(kLoudnormToken);
    } else {
        af << QStringLiteral("apad") << QStringLiteral("atrim=0:%1").arg(num(total));
        if (k == Tail::Compose && loud) af << loudnormFilter(target);
    }
    af << QStringLiteral("aresample=48000") << QStringLiteral("asetpts=N/SR/TB") << QStringLiteral("apad=whole_dur=%1").arg(num(total))
       << QStringLiteral("atrim=0:%1").arg(num(total)) << QStringLiteral("asetpts=PTS-STARTPTS");
    if (k != Tail::Apply) {
        if (t.audio.fadeIn > 0) af << QStringLiteral("afade=t=in:d=%1").arg(num(r3(std::min(t.audio.fadeIn, total))));
        if (t.audio.fadeOut > 0) {
            const double fo = std::min(t.audio.fadeOut, total);
            af << QStringLiteral("afade=t=out:st=%1:d=%2").arg(num(r3(total - fo)), num(r3(fo)));
        }
    }
    return af.join(QLatin1Char(','));
}

/// Characters of a command line built from `args` (a space and quotes each).
qsizetype commandChars(const QStringList& args) {
    qsizetype chars = 0;
    for (const QString& a : args) chars += a.size() + 3;
    return chars;
}

/// One window of the segment-parallel join: output frames [start, end), segments first..last.
struct Window { qint64 start = 0, end = 0; int first = 0, last = 0; };

/// Cut the output frames into windows of about partSeconds at slot boundaries. A boundary is the frame where the
/// transition INTO a slot is over (its start for a cut), so a window never splits a transition: the window before
/// it also joins that slot's segment to blend into it, the window after starts with that segment alone. Each window
/// is 0.5..1.5 x partSeconds when the slots allow it (a long slot makes a longer window); the last is never shorter
/// than half of it. No windows (one join) when there is nothing to cut.
QVector<Window> partWindows(const Plan& p, double partSeconds) {
    const double F = p.fps;
    const qint64 TF = p.frames;
    const qsizetype N = p.spans.size();
    struct Cand { qint64 frame; int slot; };
    QVector<Cand> cands;
    for (qsizetype i = 1; i < N; ++i) {
        const qint64 f = qint64(jsRound((p.spans[i].start + p.spans[i - 1].overlap) * F));
        if (f > 0 && f < TF && (cands.isEmpty() || f > cands.back().frame)) cands.push_back({f, int(i)});
    }
    const double P = std::max(1.0, partSeconds * F);
    QVector<Cand> cuts;
    qint64 prev = 0;
    while (double(TF - prev) > 1.5 * P) {
        int best = -1;
        for (qsizetype j = 0; j < cands.size(); ++j) {
            const double d = double(cands[j].frame - prev);
            if (d < 0.5 * P) continue;
            if (double(TF - cands[j].frame) < 0.5 * P) break;          // no sliver at the end
            if (d > 1.5 * P) { if (best < 0) best = int(j); break; }    // only a long slot left: take its end
            if (best < 0 || std::fabs(d - P) < std::fabs(double(cands[best].frame - prev) - P)) best = int(j);
        }
        if (best < 0) break;
        cuts.push_back(cands[best]);
        prev = cands[best].frame;
    }
    QVector<Window> w;
    if (cuts.isEmpty()) return w;
    qint64 start = 0;
    int first = 0;
    for (const Cand& cut : std::as_const(cuts)) {
        w.push_back({start, cut.frame, first, p.spans[cut.slot - 1].overlap > 0 ? cut.slot : cut.slot - 1});
        start = cut.frame;
        first = cut.slot;
    }
    w.push_back({start, TF, first, int(N - 1)});
    return w;
}

// Poses are written with 6 decimals so 1.2 → 1 reads "1.2-0.2*(...)" and not "-0.19999999999999996".
double r6(double x) { return jsRound(x * 1e6) / 1e6; }

QString lerpExpr(double a, double b, const QString& e) {
    a = r6(a);
    b = r6(b);
    if (a == b) return num(a);
    return num(a) + (b > a ? QLatin1Char('+') : QLatin1Char('-')) + num(r6(std::fabs(b - a))) + QStringLiteral("*(") + e + QLatin1Char(')');
}

} // namespace

// ---------------------------------------------------------------- pieces

QString xfadeName(Transition t) {
    switch (t) {
    case Transition::Fade: return QStringLiteral("fade");
    case Transition::Dissolve: return QStringLiteral("dissolve");
    case Transition::Dip: return QStringLiteral("fadeblack");
    case Transition::Slide: return QStringLiteral("slideleft");
    case Transition::Wipe: return QStringLiteral("wipeleft");
    case Transition::Zoom: return QStringLiteral("zoomin");
    default: return {};
    }
}

ZoomExpr kenburnsExpr(KenBurns kb, int n) {
    const QString p = QStringLiteral("on/") + num(std::max(1, n - 1));
    const QString cx = QStringLiteral("iw/2-(iw/zoom/2)"), cy = QStringLiteral("ih/2-(ih/zoom/2)");
    switch (kb) {
    case KenBurns::In: return {QStringLiteral("1+0.12*") + p, cx, cy};
    case KenBurns::Out: return {QStringLiteral("1.12-0.12*") + p, cx, cy};
    case KenBurns::Left: return {QStringLiteral("1.12"), QStringLiteral("(iw-iw/zoom)*(1-%1)").arg(p), cy};
    case KenBurns::Right: return {QStringLiteral("1.12"), QStringLiteral("(iw-iw/zoom)*%1").arg(p), cy};
    case KenBurns::Up: return {QStringLiteral("1.12"), cx, QStringLiteral("(ih-ih/zoom)*(1-%1)").arg(p)};
    case KenBurns::Down: return {QStringLiteral("1.12"), cx, QStringLiteral("(ih-ih/zoom)*%1").arg(p)};
    default: return {QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("0")};
    }
}

ZoomExpr moveExpr(const ClipMove& m, int n) {
    const QString p = QStringLiteral("on/") + num(std::max(1, n - 1));
    QString e;
    switch (m.ease) {
    case Ease::EaseIn: e = QStringLiteral("(%1)*(%1)").arg(p); break;
    case Ease::EaseOut: e = QStringLiteral("(%1)*(2-%1)").arg(p); break;
    case Ease::EaseInOut: e = QStringLiteral("if(lt(%1,0.5),2*(%1)*(%1),1-2*(1-%1)*(1-%1))").arg(p); break;
    default: e = p; break;
    }
    const auto unit = [](double v) { return std::clamp(v, 0.0, 1.0); };
    const auto zoom = [](double v) { return std::clamp(v, 1.0, 10.0); };   // zoompan's own range
    return {lerpExpr(zoom(m.from.z), zoom(m.to.z), e),
            QStringLiteral("clip((%1)*iw-iw/zoom/2,0,iw-iw/zoom)").arg(lerpExpr(unit(m.from.x), unit(m.to.x), e)),
            QStringLiteral("clip((%1)*ih-ih/zoom/2,0,ih-ih/zoom)").arg(lerpExpr(unit(m.from.y), unit(m.to.y), e))};
}

QString fitChain(const QString& in, int W, int H, Fit fit, const QString& out, double fx, double fy) {
    const QString w = num(W), h = num(H);
    if (fit == Fit::Cover) {
        QString crop = QStringLiteral("crop=%1:%2").arg(w, h);
        if (fx != 0.5 || fy != 0.5)   // centre the crop on the subject, kept inside the picture
            crop += QStringLiteral(":'clip(iw*%1-ow/2,0,iw-ow)':'clip(ih*%2-oh/2,0,ih-oh)'").arg(num(std::clamp(fx, 0.0, 1.0)), num(std::clamp(fy, 0.0, 1.0)));
        return QStringLiteral("%1scale=%2:%3:force_original_aspect_ratio=increase,%4,setsar=1%5").arg(in, w, h, crop, out);
    }
    return QStringLiteral("%1split=2[fa][fb];[fa]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3,gblur=sigma=%4[fbg];")
               .arg(in, w, h, num(std::max(8.0, jsRound(W / 40.0))))
           + QStringLiteral("[fb]scale=%1:%2:force_original_aspect_ratio=decrease[ffg];[fbg][ffg]overlay=(W-w)/2:(H-h)/2,setsar=1%3").arg(w, h, out);
}

QString audioChain(const QString& in, const AudioPlacement& a, double absStart, const QString& label) {
    QStringList f{in + QStringLiteral("aresample=48000"), QStringLiteral("aformat=sample_fmts=fltp:channel_layouts=stereo")};
    if (a.from > 0) f << QStringLiteral("atrim=start=%1").arg(num(r3(a.from))) << QStringLiteral("asetpts=PTS-STARTPTS");
    const double tempo = std::max(1.0, std::min(2.0, a.tempo));
    // atempo leaves its timestamps offset from the sample count, so the final trim cut the mix ~0.15 s short of the
    // picture; re-stamp by sample count
    if (tempo > 1.001) f << QStringLiteral("atempo=%1").arg(num(r3(tempo))) << QStringLiteral("asetpts=N/SR/TB");
    if (a.maxSeconds > 0.2)
        f << QStringLiteral("atrim=0:%1").arg(num(r3(a.maxSeconds))) << QStringLiteral("afade=t=out:st=%1:d=0.15").arg(num(r3(std::max(0.0, a.maxSeconds - 0.15))));
    if (a.volume != 1) f << QStringLiteral("volume=%1").arg(num(r3(a.volume)));
    const double ms = std::max(0.0, jsRound(absStart * 1000));
    if (ms > 0) f << QStringLiteral("adelay=%1:all=1").arg(num(ms));
    return f.join(QLatin1Char(',')) + label;
}

// ---------------------------------------------------------------- the plan

QJsonArray Plan::timelineJson() const {
    QJsonArray a;
    for (const Slot& s : spans) {
        QJsonObject o{{"start", r2(s.start)}, {"end", r2(std::min(total, s.start + s.duration))}, {"kind", s.kind}};
        if (!s.label.isEmpty()) o.insert(QStringLiteral("label"), cleanText(s.label).left(60));
        a.append(o);
    }
    return a;
}

QVector<const Step*> Plan::steps() const {
    QVector<const Step*> v;
    if (hasMix()) v << &mix;
    if (twoPass()) v << &measure;
    for (const Step& s : segments) v << &s;
    for (const Step& s : parts) v << &s;
    if (ok) v << &join << &poster;
    return v;
}

// ---------------------------------------------------------------- loudness

QString loudnormFilter(const LoudnessTarget& t) {
    return QStringLiteral("loudnorm=I=%1:TP=%2:LRA=%3").arg(num(t.i), num(t.tp), num(t.lra));
}

QString loudnormMeasureFilter(const LoudnessTarget& t) { return loudnormFilter(t) + QStringLiteral(":print_format=json"); }

QJsonObject LoudnessMeasure::toJson() const {
    const auto v = [](double x) { return std::isfinite(x) ? QJsonValue(x) : QJsonValue(); };
    return {{"i", v(i)}, {"tp", v(tp)}, {"lra", v(lra)}, {"thresh", v(thresh)}, {"offset", v(offset)}};
}

LoudnessMeasure parseLoudnormLog(QStringView log) {
    LoudnessMeasure m;
    const qsizetype key = log.lastIndexOf(u"\"input_i\"");
    const qsizetype open = key < 0 ? -1 : log.left(key).lastIndexOf(u'{');
    const qsizetype close = key < 0 ? -1 : log.indexOf(u'}', key);
    if (open < 0 || close < 0) { m.error = QStringLiteral("no loudness measurement in the ffmpeg log"); return m; }
    QJsonParseError pe;
    const QJsonObject o = QJsonDocument::fromJson(log.mid(open, close - open + 1).toUtf8(), &pe).object();
    if (pe.error != QJsonParseError::NoError) { m.error = QStringLiteral("unreadable loudness measurement: ") + pe.errorString(); return m; }
    // the values are strings: "-27.61", "-inf" (silence), "inf"
    const auto val = [&o](const char* k, double* out) {
        const QJsonValue v = o.value(QLatin1StringView(k));
        if (v.isDouble()) { *out = v.toDouble(); return true; }
        const QString s = v.toString().trimmed().toLower();
        if (s == u"-inf") { *out = -std::numeric_limits<double>::infinity(); return true; }
        if (s == u"inf" || s == u"+inf") { *out = std::numeric_limits<double>::infinity(); return true; }
        bool ok = false;
        const double d = s.toDouble(&ok);
        if (!ok || std::isnan(d)) return false;
        *out = d;
        return true;
    };
    if (!val("input_i", &m.i) || !val("input_tp", &m.tp) || !val("input_lra", &m.lra) || !val("input_thresh", &m.thresh)) {
        m.error = QStringLiteral("unreadable loudness measurement");
        return m;
    }
    if (!val("target_offset", &m.offset)) m.offset = 0;
    m.ok = true;
    return m;
}

QString loudnormSecondPass(const LoudnessTarget& t, const LoudnessMeasure& m) {
    if (!m.ok) return loudnormFilter(t);                           // no measurement: one dynamic pass, as compose.ts
    if (!std::isfinite(m.i)) return QStringLiteral("anull");      // silence: nothing to normalise
    // loudnorm's option ranges: measured_I -99..0, measured_TP -99..99, measured_LRA 0..99, measured_thresh -99..0,
    // offset -99..99
    const auto cl = [](double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : (v < 0 ? lo : hi); };
    return loudnormFilter(t)
           + QStringLiteral(":measured_I=%1:measured_TP=%2:measured_LRA=%3:measured_thresh=%4:offset=%5:linear=true")
                 .arg(num(r2(cl(m.i, -99, 0))), num(r2(cl(m.tp, -99, 99))), num(r2(cl(m.lra, 0, 99))), num(r2(cl(m.thresh, -99, 0))),
                      num(r2(cl(m.offset, -99, 99))));
}

Plan buildPlan(const Timeline& t, const PlanOptions& o) {
    Plan plan;
    const FrameSize fs = frameSize(t.aspect, t.quality);
    Ctx c;
    c.W = fs.w;
    c.H = fs.h;
    c.fps = t.fps;
    c.quality = t.quality;
    c.aspect = t.aspect;
    c.font = o.font;
    c.workDir = o.workDir;
    c.probe = o.probe;
    c.fileOnly = o.fileOnly;
    c.limited = o.limitedRange;
    plan.width = c.W;
    plan.height = c.H;
    plan.fps = c.fps;
    if (t.clips.isEmpty() && !t.endCard) { plan.error = QStringLiteral("nothing to render: no scenes"); return plan; }
    const auto clampT = [](double s) { return std::max(0.2, std::min(1.5, s)); };
    const double T = clampT(t.transitionSeconds);

    // timeline: slots (clip durations), end card last; transition i is INTO slot i+1 and borrows from both sides
    QVector<double> slotT;                                         // each slot's transition length (per clip or the timeline's)
    for (const Clip& cl : t.clips) {
        Slot s;
        s.kind = clipKindName(cl.kind);
        s.duration = std::max(0.3, cl.duration);
        s.transition = cl.transition ? *cl.transition : t.transition;
        s.label = cl.text;
        plan.spans.push_back(s);
        slotT.push_back(cl.transitionSeconds ? clampT(*cl.transitionSeconds) : T);
    }
    if (t.endCard) {
        Slot s;
        s.kind = QStringLiteral("endcard");
        s.duration = std::max(0.5, t.endCard->duration);
        s.transition = Transition::Cut;
        s.label = t.endCard->brand.isEmpty() ? t.endCard->cta : t.endCard->brand;
        plan.spans.push_back(s);
        slotT.push_back(T);
    }
    double natural = 0;
    for (Slot& s : plan.spans) { s.start = natural; natural += s.duration; }
    const qsizetype N = plan.spans.size();
    plan.natural = natural;
    const double total = r3(t.targetSeconds ? *t.targetSeconds : natural);
    plan.total = total;
    // overlap per boundary (never longer than 40 % of either neighbour)
    for (qsizetype i = 0; i < N; ++i) {
        Slot& s = plan.spans[i];
        s.overlap = (i < N - 1 && s.transition != Transition::Cut) ? r3(std::min({slotT[i], s.duration * 0.4, plan.spans[i + 1].duration * 0.4})) : 0;
    }

    // 1) segments
    QVector<Resolved> resolved(N, Resolved{ClipKind::Color, QString()});
    for (qsizetype i = 0; i < N; ++i) {
        const Slot& s = plan.spans[i];
        const double L = s.duration + s.overlap;
        if (i < t.clips.size()) plan.segments.push_back(renderSegment(c, int(i), t.clips[i], L, &resolved[i]));
        else plan.segments.push_back(renderEndCard(c, *t.endCard, L, int(i)));
    }

    // 2) the sound (alone in mix.wav for a split join or two loudness passes), then the join or the parts + concat
    plan.frames = qint64(std::ceil(total * c.fps - 1e-6));        // frames trim=duration=total keeps (pts < total)
    plan.loudness = t.audio.loudnessTarget();
    const QVector<Window> windows = o.partSeconds > 0 ? partWindows(plan, o.partSeconds) : QVector<Window>();
    const bool split = !windows.isEmpty();
    const bool wantTwo = o.loudnessPasses >= 2 && t.audio.loudness != Loudness::Off;
    const Sound snd = buildSound(c, t, plan, resolved, split || wantTwo ? 0 : N, total);
    plan.loudnessOn = snd.hasSound && t.audio.loudness != Loudness::Off;
    const bool two = wantTwo && plan.loudnessOn;
    plan.loudnessPasses = two ? 2 : 1;
    const bool useMix = split || two;
    const QString F = num(c.fps);
    const bool draft = t.quality == Quality::Draft;
    const auto refuse = [&](const QString& why) {
        plan.error = why;
        plan.segments.clear();
        plan.parts.clear();
        plan.join = plan.mix = plan.measure = Step();
        return plan;
    };
    if (useMix) {
        Step& m = plan.mix;
        m.kind = Step::Kind::Mix;
        m.output = c.out(QStringLiteral("mix.wav"));
        m.graphFile = QStringLiteral("mix.txt");
        m.seconds = total;
        m.progress = true;
        QStringList mg = snd.lines;
        mg << snd.mix + soundTail(t, total, plan.loudnessOn, plan.loudness, two ? Tail::Pre : Tail::Compose) + QStringLiteral("[aout]");
        m.graph = mg.join(QStringLiteral(";\n"));
        // float WAV keeps the headroom of an unnormalised mix; rf64 past 4 GB (about 3 h at 48 kHz stereo float)
        m.args = snd.inputs + QStringList{QStringLiteral("-/filter_complex"), m.graphFile, QStringLiteral("-map"), QStringLiteral("[aout]"),
                                          QStringLiteral("-c:a"), QStringLiteral("pcm_f32le"), QStringLiteral("-ar"), QStringLiteral("48000"),
                                          QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-rf64"), QStringLiteral("auto"),
                                          QStringLiteral("-t"), num(total), m.output};
        if (snd.count > kMaxJoinInputs || commandChars(m.args) > kMaxJoinCommandChars)
            return refuse(QStringLiteral("too many sound inputs for one mix (%1 files, %2 characters of arguments; at most %3 and %4): "
                                         "mix the dialogue into stems first").arg(snd.count).arg(commandChars(m.args)).arg(kMaxJoinInputs).arg(kMaxJoinCommandChars));
    }
    if (two) {
        Step& me = plan.measure;
        me.kind = Step::Kind::Measure;
        me.logLevel = QStringLiteral("info");
        me.seconds = total;
        me.args = c.input(baseName(plan.mix.output))
                  + QStringList{QStringLiteral("-af"), loudnormMeasureFilter(plan.loudness), QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")};
    }
    const QStringList videoEncode{
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), draft ? QStringLiteral("veryfast") : QStringLiteral("medium"),
        QStringLiteral("-crf"), draft ? QStringLiteral("26") : QStringLiteral("19"), QStringLiteral("-profile:v"), QStringLiteral("high"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-r"), F, QStringLiteral("-g"), num(c.fps * 2)};
    const QStringList audioEncode{
        QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"), draft ? QStringLiteral("128k") : QStringLiteral("192k"),
        QStringLiteral("-ar"), QStringLiteral("48000"), QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-movflags"), QStringLiteral("+faststart")};
    // one time base everywhere (fps= switches to 1/fps, concat outputs AVTB): xfade refuses inputs whose time bases
    // differ, which broke a cut followed by a fade. Segments first..last, labels numbered from 0, offsets from `first`.
    const auto joinChain = [&](QStringList& g, int first, int last) {
        for (int i = first; i <= last; ++i)
            g << QStringLiteral("[%1:v]settb=AVTB,setpts=PTS-STARTPTS,fps=%2,settb=AVTB,format=yuv420p[s%1]").arg(i - first).arg(F);
        QString acc = QStringLiteral("s0");
        for (int i = first + 1; i <= last; ++i) {
            const int l = i - first;
            const Transition tr = plan.spans[i - 1].transition;
            const double ov = plan.spans[i - 1].overlap;
            const QString outL = QStringLiteral("j%1").arg(l);
            if (tr == Transition::Cut || ov <= 0) g << QStringLiteral("[%1][s%2]concat=n=2:v=1:a=0,settb=AVTB[%3]").arg(acc).arg(l).arg(outL);
            else g << QStringLiteral("[%1][s%2]xfade=transition=%3:duration=%4:offset=%5[%6]").arg(acc).arg(l)
                          .arg(xfadeName(tr), num(ov), num(r3(plan.spans[i].start - plan.spans[first].start)), outL);
            acc = outL;
        }
        return acc;
    };

    Step& j = plan.join;
    if (!split) {
        // join + captions + audio in one pass (compose.ts); with two loudness passes the sound is mix.wav's
        j.kind = Step::Kind::Join;
        j.graphFile = QStringLiteral("graph.txt");
        j.output = c.out(QStringLiteral("out.mp4"));
        j.seconds = total;
        j.progress = true;
        c.texts = &j.texts;
        QStringList g;
        const QString acc = joinChain(g, 0, int(N - 1));
        QStringList vf = captionFilters(c, t, total, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        vf << QStringLiteral("tpad=stop_mode=clone:stop_duration=%1").arg(num(std::ceil(std::max(1.0, total - natural + 1))))
           << QStringLiteral("trim=duration=%1").arg(num(total)) << QStringLiteral("setpts=PTS-STARTPTS") << QStringLiteral("format=yuv420p");
        g << QStringLiteral("[%1]").arg(acc) + vf.join(QLatin1Char(',')) + QStringLiteral("[vout]");
        // audio inputs follow the segments
        QStringList inputs;
        for (const Step& s : std::as_const(plan.segments)) inputs += c.input(baseName(s.output));
        qsizetype nIn = N;
        if (useMix) {
            inputs += c.input(baseName(plan.mix.output));
            g << QStringLiteral("[%1:a]").arg(N) + soundTail(t, total, true, plan.loudness, Tail::Apply) + QStringLiteral("[aout]");
            j.loudnessToken = true;
            ++nIn;
        } else {
            inputs += snd.inputs;
            g += snd.lines;
            g << snd.mix + soundTail(t, total, plan.loudnessOn, plan.loudness, Tail::Compose) + QStringLiteral("[aout]");
            nIn += snd.count;
        }
        j.graph = g.join(QStringLiteral(";\n"));
        j.args = inputs + QStringList{QStringLiteral("-/filter_complex"), j.graphFile, QStringLiteral("-map"), QStringLiteral("[vout]"),
                                      QStringLiteral("-map"), QStringLiteral("[aout]")}
                 + videoEncode + audioEncode + QStringList{QStringLiteral("-t"), num(total), j.output};
        // one join opens every segment and sound at once: past these sizes the command line no longer fits CreateProcess
        // and ffmpeg would hold hundreds of decoders open, so the caller splits the timeline into parts instead
        const qsizetype chars = commandChars(j.args);
        if (nIn > kMaxJoinInputs || chars > kMaxJoinCommandChars)
            return refuse(QStringLiteral("too many inputs for one join (%1 files, %2 characters of arguments; at most %3 and %4): "
                                         "split the timeline into shorter parts").arg(nIn).arg(chars).arg(kMaxJoinInputs).arg(kMaxJoinCommandChars));
    } else {
        // the segment-parallel join: one part per window, each the single join's chain over its own segments
        const double ceilPad = std::max(1.0, total - natural + 1);
        for (qsizetype k = 0; k < windows.size(); ++k) {
            const Window& w = windows[k];
            Step pt;
            pt.kind = Step::Kind::Part;
            pt.slot = int(k);
            pt.first = w.first;
            pt.last = w.last;
            pt.frameStart = w.start;
            pt.frames = w.end - w.start;
            pt.seconds = r3(double(pt.frames) / c.fps);
            pt.progress = true;
            pt.output = c.out(QStringLiteral("part%1.mp4").arg(k, 3, 10, QLatin1Char('0')));
            pt.graphFile = QStringLiteral("part%1.txt").arg(k, 3, 10, QLatin1Char('0'));
            c.texts = &pt.texts;
            QStringList g, inputs;
            for (int i = w.first; i <= w.last; ++i) inputs += c.input(baseName(plan.segments[i].output));
            const QString acc = joinChain(g, w.first, w.last);
            // the part's first frame is its first segment's: skip to the window (past the transition into that slot,
            // which the window before blended), clone-pad against segments a frame short, keep exactly the window's
            // frames, put them on absolute times for the captions' enable windows, then start the file at 0
            const qint64 skip = std::max<qint64>(0, qint64(jsRound(double(w.start) - plan.spans[w.first].start * c.fps)));
            QStringList vf{QStringLiteral("tpad=stop_mode=clone:stop_duration=%1").arg(num(std::ceil(ceilPad + double(w.last - w.first + 1) / c.fps))),
                           QStringLiteral("trim=start_frame=%1:end_frame=%2").arg(skip).arg(skip + pt.frames),
                           QStringLiteral("setpts=(N+%1)/(%2*TB)").arg(w.start).arg(F)};
            vf += captionFilters(c, t, total, double(w.start) / c.fps, double(w.end) / c.fps);
            vf << QStringLiteral("setpts=PTS-STARTPTS") << QStringLiteral("format=yuv420p");
            g << QStringLiteral("[%1]").arg(acc) + vf.join(QLatin1Char(',')) + QStringLiteral("[vout]");
            pt.graph = g.join(QStringLiteral(";\n"));
            pt.args = inputs + QStringList{QStringLiteral("-/filter_complex"), pt.graphFile, QStringLiteral("-map"), QStringLiteral("[vout]")}
                      + videoEncode + QStringList{QStringLiteral("-an"), QStringLiteral("-frames:v"), QString::number(pt.frames), pt.output};
            const int nIn = w.last - w.first + 1;
            const qsizetype chars = commandChars(pt.args);
            if (nIn > kMaxJoinInputs || chars > kMaxJoinCommandChars)
                return refuse(QStringLiteral("too many inputs for one part (%1 files, %2 characters of arguments; at most %3 and %4): "
                                             "use shorter parts").arg(nIn).arg(chars).arg(kMaxJoinInputs).arg(kMaxJoinCommandChars));
            plan.parts.push_back(pt);
        }
        // concat: the parts' pictures by stream copy (same encoder settings, each opens on an IDR frame) + the sound
        j.kind = Step::Kind::Concat;
        j.output = c.out(QStringLiteral("out.mp4"));
        j.seconds = total;
        j.progress = true;
        QString list = QStringLiteral("ffconcat version 1.0\n");
        for (const Step& p : std::as_const(plan.parts)) list += QStringLiteral("file '%1'\n").arg(baseName(p.output));
        j.texts = {{QStringLiteral("parts.txt"), list}};
        j.args = QStringList{QStringLiteral("-f"), QStringLiteral("concat")} + c.input(QStringLiteral("parts.txt")) + c.input(baseName(plan.mix.output));
        if (two) {
            j.graphFile = QStringLiteral("mux.txt");
            j.graph = QStringLiteral("[1:a]") + soundTail(t, total, true, plan.loudness, Tail::Apply) + QStringLiteral("[aout]");
            j.loudnessToken = true;
            j.args += QStringList{QStringLiteral("-/filter_complex"), j.graphFile, QStringLiteral("-map"), QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("[aout]")};
        } else {
            j.args += QStringList{QStringLiteral("-map"), QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("1:a")};
        }
        j.args += QStringList{QStringLiteral("-c:v"), QStringLiteral("copy")} + audioEncode + QStringList{QStringLiteral("-t"), num(total), j.output};
    }

    // 3) poster
    Step& p = plan.poster;
    p.kind = Step::Kind::Poster;
    const QString outFile = o.outFile.isEmpty() ? j.output : o.outFile;
    p.output = o.posterFile.isEmpty() ? c.out(QStringLiteral("poster.png")) : o.posterFile;
    const double posterAt = std::min(total - 0.05, std::max(0.0, t.posterAt ? *t.posterAt : std::max(0.5, (N ? plan.spans[0].duration : 1) * 0.4)));
    p.args = QStringList{QStringLiteral("-ss"), num(r3(posterAt))} + c.input(outFile)
             + QStringList{QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-update"), QStringLiteral("1"), p.output};
    p.seconds = 0;

    QSet<QString> seen;
    for (const QString& w : std::as_const(c.warnings))
        if (!seen.contains(w)) { seen.insert(w); plan.warnings << w; }
    plan.ok = true;
    return plan;
}

} // namespace studio::compose
