// TimeAuthority drift reporting (STB-5 = perf-supervisor-app W17): a clock step is reported once and the comparison is
// re-based; drift lines are limited to one a minute and carry what the limit held back. On 10-25 each engine wrote the
// same "Time drift detected" line every second all day (80 421 lines), because the check never re-based.
#include <QtTest>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTimer>
#include "playout/timeauthority.h"
#include <memory>

// Test hook defined in playout/timeauthority.cpp (the header is not this lane's file).
namespace timeauthority_testing { void setClocks(qint64 (*wallMs)(), qint64 (*monoNs)()); }

namespace {

qint64 g_wallMs = 0, g_monoNs = 0;   // the fake wall clock (ms since epoch) and monotonic clock (ns)
qint64 fakeWall() { return g_wallMs; }
qint64 fakeMono() { return g_monoNs; }

struct Line { qint64 atMonoNs; QString text; };
QList<Line> g_lines;
QtMessageHandler g_prev = nullptr;
void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (msg.startsWith(QLatin1String("Time drift"))) { g_lines.append({g_monoNs, msg}); return; }
    if (g_prev) g_prev(type, ctx, msg);
}

// One TimeAuthority on the fake clocks. Its own 1 s timer is stopped, so only tick() checks.
struct Rig {
    std::unique_ptr<TimeAuthority> ta;
    std::unique_ptr<QSignalSpy> spy;
    Rig() {
        g_wallMs = 1790000000000LL; g_monoNs = 5000000000LL; g_lines.clear();
        timeauthority_testing::setClocks(&fakeWall, &fakeMono);
        ta = std::make_unique<TimeAuthority>();
        if (QTimer* t = ta->findChild<QTimer*>()) t->stop();
        spy = std::make_unique<QSignalSpy>(ta.get(), &TimeAuthority::timeDriftDetected);
    }
    ~Rig() { timeauthority_testing::setClocks(nullptr, nullptr); }
    // One healthy interval (both clocks advance by ms), plus an optional wall-clock step, then one check.
    void tick(qint64 wallStepMs = 0, qint64 ms = 1000) {
        g_monoNs += ms * 1000000; g_wallMs += ms + wallStepMs;
        QVERIFY(QMetaObject::invokeMethod(ta.get(), "checkDrift", Qt::DirectConnection));
    }
    void ticks(int n) { for (int i = 0; i < n; ++i) tick(); }
    qint64 signalSum() const { qint64 s = 0; for (const QList<QVariant>& a : *spy) s += a.at(0).toLongLong(); return s; }
};

// Sum of all drift a set of lines accounts for: each line's own correction plus what the rate limit held back.
qint64 reportedSum(const QList<Line>& lines, bool* allParsed) {
    static const QRegularExpression one("^Time drift detected: (-?\\d+) ms \\(re-based(?:; (\\d+) earlier correction\\(s\\), (-?\\d+) ms in all, since the previous line)?\\)$");
    static const QRegularExpression summary("^Time drift detected: (\\d+) correction\\(s\\), (-?\\d+) ms in all, since the previous line \\(re-based each time\\)$");
    qint64 s = 0; *allParsed = true;
    for (const Line& l : lines) {
        const QRegularExpressionMatch m = one.match(l.text), m2 = summary.match(l.text);
        if (m.hasMatch()) s += m.captured(1).toLongLong() + m.captured(3).toLongLong();
        else if (m2.hasMatch()) s += m2.captured(2).toLongLong();
        else { *allParsed = false; qWarning("unparsed: %s", qPrintable(l.text)); }
    }
    return s;
}

} // namespace

class TestTimeAuthority : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { g_prev = qInstallMessageHandler(capture); }
    void cleanupTestCase() { qInstallMessageHandler(g_prev); }

    void steadyClocksGiveNoLine() {
        Rig r;
        r.ticks(3600);
        QCOMPARE(g_lines.size(), 0);
        QCOMPARE(r.spy->count(), 0);
    }

    void stepGivesOneLineAndRebases_data() {
        QTest::addColumn<qint64>("step");
        QTest::newRow("+150 ms") << qint64(150);
        QTest::newRow("-150 ms") << qint64(-150);
        QTest::newRow("+101 ms (just over)") << qint64(101);
        QTest::newRow("+5 s (resume)") << qint64(5000);
    }
    // The 10-25 case: one step, then an hour of healthy seconds. The old check logged it 3 600 times.
    void stepGivesOneLineAndRebases() {
        QFETCH(qint64, step);
        Rig r;
        r.ticks(10);
        r.tick(step);
        QCOMPARE(g_lines.size(), 1);
        QCOMPARE(g_lines.first().text, QString("Time drift detected: %1 ms (re-based)").arg(step));
        QCOMPARE(r.spy->count(), 1);
        QCOMPARE(r.spy->first().at(0).toLongLong(), step);
        r.ticks(3600);
        QCOMPARE(g_lines.size(), 1);   // re-based: the same step is not reported again
        QCOMPARE(r.spy->count(), 1);
        // A new step an hour later is a new correction with its own line.
        r.tick(-step);
        QCOMPARE(g_lines.size(), 2);
        QCOMPARE(g_lines.last().text, QString("Time drift detected: %1 ms (re-based)").arg(-step));
        QCOMPARE(r.spy->count(), 2);
    }

    // Drift at or under the threshold is not a correction, but it keeps adding up against the base.
    void smallDriftAddsUpUntilThreshold() {
        Rig r;
        r.tick(100);                  // exactly the threshold: not over it
        r.ticks(20);
        QCOMPARE(g_lines.size(), 0);
        r.tick(-40);                  // 60 ms against the base
        r.tick(50);                   // 110 ms
        QCOMPARE(g_lines.size(), 1);
        QCOMPARE(g_lines.first().text, QString("Time drift detected: 110 ms (re-based)"));
        r.ticks(600);
        QCOMPARE(g_lines.size(), 1);
        QCOMPARE(r.spy->count(), 1);
    }

    // A clock that steps every second for 10 minutes: every correction is signalled and accounted for, but the log gets
    // one line a minute (lines at 1, 61, ..., 541 s and one summary when the minute after the last line is up).
    void atMostOneLinePerMinute() {
        Rig r;
        for (int i = 0; i < 600; ++i) r.tick(150);
        r.ticks(300);
        QCOMPARE(r.spy->count(), 600);
        QCOMPARE(g_lines.size(), 11);
        for (int i = 1; i < g_lines.size(); ++i)
            QVERIFY2(g_lines[i].atMonoNs - g_lines[i - 1].atMonoNs >= 60000000000LL, qPrintable(g_lines[i].text));
        QCOMPARE(g_lines.first().text, QString("Time drift detected: 150 ms (re-based)"));
        QCOMPARE(g_lines.at(1).text, QString("Time drift detected: 150 ms (re-based; 59 earlier correction(s), 8850 ms in all, since the previous line)"));
        QCOMPARE(g_lines.last().text, QString("Time drift detected: 59 correction(s), 8850 ms in all, since the previous line (re-based each time)"));
        bool parsed = false;
        QCOMPARE(reportedSum(g_lines, &parsed), qint64(600 * 150));
        QVERIFY(parsed);
        QCOMPARE(r.signalSum(), qint64(600 * 150));
    }

    // A wall clock that runs 0.5 ms/s fast for an hour (far worse than w32time allows): a correction about every 200 s,
    // each its own line. The old check logged every second from the first 100 ms on (about 3 400 lines).
    void slowSkewIsReportedEveryFewMinutes() {
        Rig r;
        for (int s = 1; s <= 3600; ++s) r.tick(s % 2 == 0 ? 1 : 0);
        QVERIFY2(g_lines.size() >= 15 && g_lines.size() <= 19, qPrintable(QString::number(g_lines.size())));
        QCOMPARE(g_lines.size(), r.spy->count());
        for (const Line& l : g_lines) QCOMPARE(l.text, QString("Time drift detected: 101 ms (re-based)"));
        for (int i = 1; i < g_lines.size(); ++i) QVERIFY(g_lines[i].atMonoNs - g_lines[i - 1].atMonoNs >= 60000000000LL);
    }

    // Re-basing moves only the drift base: frame numbers, SMPTE and frame times still count from the start time.
    void frameNumbersAreUnaffected() {
        Rig r;
        const qint64 t = QDateTime::currentMSecsSinceEpoch() + 3723500;
        const qint64 f0 = r.ta->timeToFrame(t), ft0 = r.ta->frameToTime(4500);
        const QString s0 = r.ta->toSMPTE(t);
        r.tick(150);
        r.tick(-5000);
        QCOMPARE(g_lines.size(), 1);   // the second correction waits for the minute
        QCOMPARE(r.spy->count(), 2);
        QCOMPARE(r.ta->timeToFrame(t), f0);
        QCOMPARE(r.ta->frameToTime(4500), ft0);
        QCOMPARE(r.ta->toSMPTE(t), s0);
    }

    // Real clocks: the 1 s timer runs the check and a healthy machine reports nothing.
    void realClocksReportNothing() {
        g_lines.clear();
        TimeAuthority ta;
        QTimer* t = ta.findChild<QTimer*>();
        QVERIFY(t);
        QVERIFY(t->isActive());
        QCOMPARE(t->interval(), 1000);
        QSignalSpy spy(&ta, &TimeAuthority::timeDriftDetected);
        QTest::qWait(2300);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(g_lines.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestTimeAuthority)
#include "test_timeauthority.moc"
