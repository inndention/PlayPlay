#ifndef TRANSCRIPTTOOLS_H
#define TRANSCRIPTTOOLS_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

/**
 * TranscriptTools: what the segment tools need from a transcript, kept apart from the Supervisor so it can be
 * unit tested (QtCore only). The segment editors (SegmentTimeline.qml, web/segments.js) use the same rules.
 *
 * Transcript JSON (scripts/transcribe.py): {segments: [{start, end, text, words: [{word, start, end, probability}]}]}
 *  - words(): every word in time order, text trimmed (whisper words carry a leading space).
 *  - cutBefore() / cutAfter(): where to cut around a run of words so the cut falls in the silence next to it:
 *    half the gap to the neighbouring word, at most maxPad seconds, never into the neighbour.
 *  - cutPoints(): all of those cuts, sorted and de-duplicated: the snap targets for segment edges.
 *  - textBetween(): the words spoken in [a, b). A word belongs to the range that holds its midpoint, so a word
 *    split by a cut is counted on one side only.
 *  - fillSegmentText(): sets each segment's `text` from the transcript and removes it when the range is silent;
 *    without word timings it uses the whisper sentences' text.
 */
namespace TranscriptTools {

struct Word {
    QString text;
    double start = 0.0;
    double end = 0.0;
};

QVector<Word> words(const QJsonObject& transcript);
double cutBefore(const QVector<Word>& w, int i, double maxPad = 0.25);
double cutAfter(const QVector<Word>& w, int i, double maxPad = 0.25);
QVector<double> cutPoints(const QVector<Word>& w, double maxPad = 0.25);
QString textBetween(const QVector<Word>& w, double a, double b, int maxChars = 400);
QString sentenceTextBetween(const QJsonObject& transcript, double a, double b, int maxChars = 400);
/// Returns how many segments now carry text. Leaves the list untouched when the transcript has no text at all.
int fillSegmentText(QJsonArray& segments, const QJsonObject& transcript, int maxChars = 400);

}   // namespace TranscriptTools

#endif // TRANSCRIPTTOOLS_H
