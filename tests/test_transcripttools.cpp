// TranscriptTools: the word list, the cuts around words (in the silence, never into a neighbour), the text
// spoken in a range (midpoint rule) and filling segment text, which TranscriptionManager runs on every save.
#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include "playout/supervisor/transcripttools.h"

using namespace TranscriptTools;

namespace {
QJsonObject word(const char* text, double a, double b) { QJsonObject w; w["word"] = QString::fromUtf8(text); w["start"] = a; w["end"] = b; w["probability"] = 0.9; return w; }
QJsonObject sentence(double a, double b, const char* text, const QJsonArray& words) { QJsonObject s; s["start"] = a; s["end"] = b; s["text"] = QString::fromUtf8(text); s["words"] = words; return s; }
// "Good evening, everyone." 1.0-2.2 | 4 s of silence | "Here is the news." 6.2-7.6 (words 0.05 s apart)
QJsonObject sample() {
    QJsonArray segs;
    segs.append(sentence(1.0, 2.2, " Good evening, everyone.", QJsonArray{word(" Good", 1.0, 1.3), word(" evening,", 1.35, 1.8), word(" everyone.", 1.85, 2.2)}));
    segs.append(sentence(6.2, 7.6, " Here is the news.", QJsonArray{word(" Here", 6.2, 6.5), word(" is", 6.55, 6.7), word(" the", 6.75, 6.9), word(" news.", 6.95, 7.6)}));
    QJsonObject t; t["segments"] = segs; t["language"] = "en"; return t;
}
QJsonObject seg(double a, double b, const char* text = nullptr) { QJsonObject s; s["startTime"] = a; s["endTime"] = b; if (text) s["text"] = QString::fromUtf8(text); return s; }
}

class TestTranscriptTools : public QObject {
    Q_OBJECT
private slots:
    void wordsAreFlatTrimmedAndOrdered() {
        const QVector<Word> w = words(sample());
        QCOMPARE(w.size(), 7);
        QCOMPARE(w[0].text, QString("Good"));
        QCOMPARE(w[3].text, QString("Here"));
        QCOMPARE(w[6].end, 7.6);
    }
    void cutsSitInTheSilence() {
        const QVector<Word> w = words(sample());
        // first word: the full pad before it
        QCOMPARE(cutBefore(w, 0), 0.75);
        // 0.05 s between "Good" and "evening,": the cut is the middle of that gap from both sides
        QVERIFY(qAbs(cutAfter(w, 0) - 1.325) < 1e-9);
        QVERIFY(qAbs(cutBefore(w, 1) - 1.325) < 1e-9);
        // 4 s of silence: at most 0.25 s after "everyone." and before "Here"
        QVERIFY(qAbs(cutAfter(w, 2) - 2.45) < 1e-9);
        QVERIFY(qAbs(cutBefore(w, 3) - 5.95) < 1e-9);
        // last word: the full pad after it
        QVERIFY(qAbs(cutAfter(w, 6) - 7.85) < 1e-9);
        // a word at 0.1 s cannot be cut before 0
        QVector<Word> early = {{QStringLiteral("Hi"), 0.1, 0.4}};
        QCOMPARE(cutBefore(early, 0), 0.0);
    }
    void cutPointsAreSortedAndUnique() {
        const QVector<double> p = cutPoints(words(sample()));
        // 7 words give 14 cuts; the 5 small gaps share one point each
        QCOMPARE(p.size(), 9);
        for (int i = 1; i < p.size(); ++i) QVERIFY(p[i] > p[i - 1]);
        QCOMPARE(p.first(), 0.75);
    }
    void textBetweenUsesWordMidpoints() {
        const QVector<Word> w = words(sample());
        QCOMPARE(textBetween(w, 0.0, 10.0), QString("Good evening, everyone. Here is the news."));
        QCOMPARE(textBetween(w, 5.95, 7.85), QString("Here is the news."));
        // a cut through "evening," (1.35-1.8, midpoint 1.575): the word goes to the side holding its midpoint
        QCOMPARE(textBetween(w, 0.0, 1.6), QString("Good evening,"));
        QCOMPARE(textBetween(w, 1.6, 10.0), QString("everyone. Here is the news."));
        QCOMPARE(textBetween(w, 3.0, 5.0), QString());
    }
    void longTextStopsAtAWordBoundary() {
        const QVector<Word> w = words(sample());
        const QString t = textBetween(w, 0.0, 10.0, 20);
        QCOMPARE(t, QString("Good evening,") + QChar(0x2026));
    }
    void fillSegmentTextSetsAndClears() {
        QJsonArray segs{seg(0.0, 2.45), seg(2.45, 5.95, "stale text from an older range"), seg(5.95, 7.85)};
        QCOMPARE(fillSegmentText(segs, sample()), 2);
        QCOMPARE(segs[0].toObject().value("text").toString(), QString("Good evening, everyone."));
        QVERIFY(!segs[1].toObject().contains("text"));          // only silence in that range now
        QCOMPARE(segs[2].toObject().value("text").toString(), QString("Here is the news."));
        QCOMPARE(segs[0].toObject().value("startTime").toDouble(), 0.0);   // times untouched
    }
    void fillFallsBackToSentencesWithoutWordTimings() {
        QJsonObject t = sample();
        QJsonArray ss = t.value("segments").toArray();
        for (int i = 0; i < ss.size(); ++i) { QJsonObject s = ss[i].toObject(); s.remove("words"); ss[i] = s; }
        t["segments"] = ss;
        QJsonArray segs{seg(0.0, 4.0), seg(4.0, 9.0)};
        QCOMPARE(fillSegmentText(segs, t), 2);
        QCOMPARE(segs[0].toObject().value("text").toString(), QString("Good evening, everyone."));
        QCOMPARE(segs[1].toObject().value("text").toString(), QString("Here is the news."));
    }
    void transcriptWithoutSpeechLeavesSegmentsAlone() {
        QJsonObject t; t["segments"] = QJsonArray();
        QJsonArray segs{seg(0.0, 4.0, "kept")};
        QCOMPARE(fillSegmentText(segs, t), 0);
        QCOMPARE(segs[0].toObject().value("text").toString(), QString("kept"));
    }
};

QTEST_GUILESS_MAIN(TestTranscriptTools)
#include "test_transcripttools.moc"
