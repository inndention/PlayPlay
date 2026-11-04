#pragma once
// Caption sidecars (ST-18b): SRT and WebVTT files next to a rendered video, for players, platforms and NLEs that want
// the captions as text rather than burned into the picture.
//
// toSrt / toVtt are ports of StarMedia src/media.ts (the subtitle writer of the transcription tool, the harness's
// captions export and the film's), byte for byte: test_studio_compose compares them with golden/captions-golden.json,
// recorded from the real media.ts (golden/gen-captions-golden.mjs). That includes the TS arithmetic of its ts():
// hours, minutes and seconds are floored from sec / 3600, (sec % 3600) / 60 and sec % 60 separately, and the
// milliseconds are floor((s - floor(s)) * 1000) of the double, so 2.3 s prints ",299" and 1.001 s ",000" (the fraction
// lands a hair under the whole millisecond); negative and huge times print as the TS prints them. A 1 ms early stamp
// is invisible to a viewer; matching the TS keeps one caption file for both engines.
//
// sidecarCues() is what the composer burns in (compose/filtergraph.h): the same cues in the same window, so the
// sidecar and the picture agree. Pure functions apart from writeSidecars(); safe on any thread.
#include "compose/timeline.h"

#include <QString>
#include <QVector>

namespace studio::compose {

/// media.ts ts(): "HH:MM:SS,mmm" (sep ',', SRT) or "HH:MM:SS.mmm" (sep '.', WebVTT), with its arithmetic.
QString subtitleTime(double seconds, QChar sep);
/// media.ts toSrt: "1\n00:00:01,500 --> 00:00:03,250\ntext\n" per cue, joined by "\n" ("" for no cues).
QString toSrt(const QVector<CaptionCue>& cues);
/// media.ts toVtt: "WEBVTT\n\n" + "start --> end\ntext\n" per cue joined by "\n"; &, < and > escaped in the text.
QString toVtt(const QVector<CaptionCue>& cues);

/// The captions of `t` as the composer burns them in: cues that end after they start and start before the end of the
/// video, whose text is not empty once cleaned for drawing (an emoji-only cue is not drawn, so it is not listed), the
/// end clipped to the total length, in start order (stable). The text keeps its emoji (subtitle renderers have the
/// fonts) but each line is trimmed with its whitespace collapsed, blank lines are dropped (a blank line ends an SRT
/// cue) and "-->" becomes "->" (WebVTT forbids it in cue text).
QVector<CaptionCue> sidecarCues(const Timeline& t);
/// The cues moved by `offset` seconds (a sequence's captions placed on the film's timeline), keeping only what still
/// shows inside [from, to) and clipping to it; cues that end up empty are dropped.
QVector<CaptionCue> shiftCues(const QVector<CaptionCue>& cues, double offset, double from = 0, double to = 1e300);

/// Writes the SRT and / or VTT file (an empty path skips it) atomically, UTF-8 without BOM. False with `error` when a
/// file could not be written (neither is left half-written).
bool writeSidecars(const QVector<CaptionCue>& cues, const QString& srtPath, const QString& vttPath, QString* error = nullptr);

} // namespace studio::compose
