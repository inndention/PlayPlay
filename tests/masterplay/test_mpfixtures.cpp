// MasterPlay lane scaffold (MP-01): the sample fixtures are complete and byte-exact against manifest.tsv, git cannot
// rewrite their line ends, no plant data or credential is committed, and the lane library links. Every sample is
// synthetic: hand-written in the MasterPlay formats, or a log whose titles and paths were replaced by placeholders.
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include "../../playout/masterplay/mplane.h"

namespace {

struct Row { QString file; qint64 bytes = -1; QByteArray sha256; QString source, note; };

QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path, bool* ok = nullptr) {
    QFile f(path); const bool o = f.open(QIODevice::ReadOnly); if (ok) *ok = o;
    return o ? f.readAll() : QByteArray();
}

// manifest.tsv: file, bytes, sha256, source, note (TAB separated, '#' comments). Short rows are kept with what they
// have so the completeness checks name them instead of silently skipping them.
QList<Row> manifest() {
    QList<Row> rows;
    for (const QByteArray& line : readAll(fx("manifest.tsv")).split('\n')) {
        if (line.trimmed().isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> f = line.split('\t');
        Row r; r.file = QString::fromUtf8(f.value(0));
        if (f.size() >= 5) { r.bytes = f[1].toLongLong(); r.sha256 = f[2]; r.source = QString::fromUtf8(f[3]); r.note = QString::fromUtf8(f[4]); }
        rows << r;
    }
    return rows;
}

const QStringList kSamples = {"FirstPlaylist.in", "OSD-Instructions.in", "Saturday_0200.in", "Testfiles.in", "sample.in",
                              "DefaultCountDown.osd", "DefaultCountUp.osd", "DefaultCrawl.osd", "DefaultLogo.osd",
                              "DefaultRealtimeClock.osd", "Masterplay Logo.osd", "LogFile.log", "mplay_sample.ini"};

} // namespace

class TestMpFixtures : public QObject {
    Q_OBJECT
private slots:
    void manifestListsEverySample() {
        QVERIFY2(QDir(QStringLiteral(MP_FIXTURES)).exists(), MP_FIXTURES);
        const QList<Row> rows = manifest();
        QStringList listed;
        for (const Row& r : rows) {
            QVERIFY2(r.bytes >= 0 && r.sha256.size() == 64 && !r.source.isEmpty(), qPrintable("short manifest row: " + r.file));
            listed << r.file;
        }
        for (const QString& s : kSamples) QVERIFY2(listed.contains(s), qPrintable("not in manifest.tsv: " + s));
        QCOMPARE(listed.size(), QSet<QString>(listed.begin(), listed.end()).size());   // no duplicate rows
    }

    void bytesMatchManifest_data() {
        QTest::addColumn<QString>("file"); QTest::addColumn<qint64>("bytes"); QTest::addColumn<QByteArray>("sha256");
        for (const Row& r : manifest()) QTest::newRow(qPrintable(r.file)) << r.file << r.bytes << r.sha256;
    }
    void bytesMatchManifest() {
        QFETCH(QString, file); QFETCH(qint64, bytes); QFETCH(QByteArray, sha256);
        bool ok = false; const QByteArray b = readAll(fx(file), &ok);
        QVERIFY2(ok, qPrintable("missing fixture: " + file));
        QCOMPARE(qint64(b.size()), bytes);
        QCOMPARE(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(), sha256);
    }

    // The properties the byte-exact parser tests depend on, checked by name so a bad checkout (autocrlf, an editor
    // that saved a fixture) fails here with a reason rather than as a hash mismatch further down the lane.
    void lineEndsSurvive() {
        QVERIFY(readAll(fx(".gitattributes")).contains("* -text"));
        const QByteArray bom("\xEF\xBB\xBF");
        QVERIFY(readAll(fx("FirstPlaylist.in")).startsWith(bom));
        QVERIFY(readAll(fx("sample.in")).startsWith(bom));
        QVERIFY(!readAll(fx("Testfiles.in")).startsWith(bom));
        const QByteArray sat = readAll(fx("Saturday_0200.in"));
        QVERIFY(sat.contains("PlayMode=2\r\n"));
        QVERIFY2(!sat.endsWith('\n'), "Saturday_0200.in has no final newline");
        QVERIFY2(readAll(fx("Testfiles.in")).endsWith("\\TestCards\t\r\n"), "Testfiles.in keeps its trailing TAB");
        QVERIFY2(readAll(fx("OSD-Instructions.in")).contains("!ShowCrawl Sample Crawl Text\tcfg="), "space after the instruction, TAB before fields");
        const QByteArray log = readAll(fx("LogFile.log"));
        QVERIFY(log.endsWith("\r\n"));
        QCOMPARE(log.count("\r\n"), log.count('\n'));                   // CRLF only
        QCOMPARE(int(log.count('\n')), 883);
        const QByteArray ini = readAll(fx("mplay_sample.ini"));
        QCOMPARE(ini.count("\r\n"), ini.count('\n'));
    }

    // MP-04 asserts these counts on the LogFile.log copy: 621 played (000) and 262 missed (118).
    void asRunCodeCounts() {
        int played = 0, missed = 0, other = 0;
        for (const QByteArray& line : readAll(fx("LogFile.log")).split('\n')) {
            if (line.isEmpty()) continue;
            const QByteArray code = line.mid(20, 4);                      // "2025-06-19T18:21:16 000 ..."
            if (code == "000 ") ++played; else if (code == "118 ") ++missed; else ++other;
        }
        QCOMPARE(played, 621); QCOMPARE(missed, 262); QCOMPARE(other, 0);
    }

    // The plant playlist (plant_playlist*.in) and plant logs are business data: never in the fixture folder. The sample
    // INI only carries placeholder secrets.
    void noPlantDataOrCredentials() {
        const QStringList names = QDir(QStringLiteral(MP_FIXTURES)).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const QString& n : names) QVERIFY2(!n.startsWith("plant_playlist", Qt::CaseInsensitive), qPrintable("plant file in fixtures: " + n));
        QVERIFY(readAll(fx(".gitignore")).contains("plant_playlist*"));
        const QByteArray ini = readAll(fx("mplay_sample.ini"));
        QVERIFY(ini.contains("User0=admin\tredacted\t"));                  // placeholders, not a stored password
    }

    void laneLibraryLinks() {
        const QJsonObject info = mp::laneInfo();
        QCOMPARE(info.value("lane").toString(), QStringLiteral("masterplay"));
        QCOMPARE(info.value("apiVersion").toInt(), mp::kApiVersion);
    }
};

QTEST_GUILESS_MAIN(TestMpFixtures)
#include "test_mpfixtures.moc"
