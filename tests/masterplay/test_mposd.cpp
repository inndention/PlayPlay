// MasterPlay OSD and INI readers (MP-05, playout/masterplay/mposd.h, mpini.h): the profile reader (Windows
// GetPrivateProfileString semantics), the six sample *.osd files as structs, Delphi TColor (BGR) to
// #RRGGBB, the Mplay.INI subset from the synthetic mplay_sample.ini (placeholder secrets), list values (CommaText / ';' / '|'),
// path rules seeded from ExchangeDriveLetters / MasterPlayLocalDriveLetter / ExchangeList / the INI location, and the
// suggested source for POST /masterplay/config/from-ini.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include "../../playout/masterplay/mpini.h"
#include "../../playout/masterplay/mposd.h"

using namespace mp;

namespace {

QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QByteArray crlf(const QStringList& lines) { return (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8(); }
QString diagList(const QList<Diagnostic>& ds) {   // "line:code:col ..."
    QStringList o;
    for (const Diagnostic& d : ds) o << QStringLiteral("%1:%2:%3").arg(d.line).arg(d.code).arg(d.col);
    return o.join(' ');
}
QString hexOf(qint64 v) { return osd::tColorToHex(v).value_or(QStringLiteral("none")); }
QString ruleList(const QList<ini::PathRule>& rules) {   // "from>to(origin[?]) ..."
    QStringList o;
    for (const ini::PathRule& r : rules) o << QStringLiteral("%1>%2(%3%4)").arg(r.from, r.to, r.origin, r.guess ? QStringLiteral("?") : QString());
    return o.join(' ');
}
QByteArray utf16(const QString& s, bool le) {
    QByteArray b = le ? QByteArray("\xFF\xFE", 2) : QByteArray("\xFE\xFF", 2);
    for (QChar c : s) {
        const char lo = char(c.unicode() & 0xFF), hi = char(c.unicode() >> 8);
        b += le ? lo : hi;
        b += le ? hi : lo;
    }
    return b;
}
// Nothing a from-ini reply carries may come from the secret keys of Mplay.INI.
void assertNoSecrets(const QJsonObject& o) {
    const QByteArray j = QJsonDocument(o).toJson(QJsonDocument::Compact);
    for (const char* s : {"redacted", "Password", "Loginname", "ApprovalUsers", "SSL", "admin"})
        QVERIFY2(!j.contains(s), s);
}
const QStringList kOsdFiles = {"DefaultCountDown.osd", "DefaultCountUp.osd", "DefaultCrawl.osd", "DefaultLogo.osd",
                               "DefaultRealtimeClock.osd", "Masterplay Logo.osd"};

} // namespace

class TestMpOsd : public QObject {
    Q_OBJECT
private slots:
    // ---- the six .osd samples ---------------------------------------------------------------------------------------

    void osdSamples_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("kind");        // effective kind
        QTest::addColumn<bool>("kindFromName");
        QTest::addColumn<QList<int>>("pos");      // x, y, trans1, bTrans, dTrans
        QTest::addColumn<QString>("font");        // "name/size" or ""
        QTest::addColumn<QString>("fontColor");   // hex, "" when absent
        QTest::addColumn<QString>("backColor");
        QTest::addColumn<QList<int>>("crawl");    // crawlTime, rollTime, fieldMode
        QTest::addColumn<QString>("anchor");
        QTest::addColumn<int>("lines");
        QTest::newRow("DefaultCountDown") << "DefaultCountDown.osd" << -1 << "clock" << true << QList<int>{50, 50, 2, 2, 0}
                                          << "Verdana/28" << "#FF0000" << "#FFFFFF" << QList<int>{-1, -1, -1} << "top_left" << 10;
        QTest::newRow("DefaultCountUp") << "DefaultCountUp.osd" << 5 << "clock" << false << QList<int>{65, 50, 0, 1, 0}
                                        << "Consolas/36" << "#0000FF" << "#FFFFFF" << QList<int>{-1, -1, -1} << "top_left" << 11;
        QTest::newRow("DefaultCrawl") << "DefaultCrawl.osd" << 6 << "crawl" << false << QList<int>{0, 50, 0, 2, 0}
                                      << "Tahoma/30" << "#FFFF00" << "#000000" << QList<int>{4, 900000, 1} << "bottom" << 14;
        QTest::newRow("DefaultLogo") << "DefaultLogo.osd" << 1 << "logo" << false << QList<int>{55, 45, 0, 0, 1}
                                     << "" << "" << "" << QList<int>{-1, -1, -1} << "top_right" << 7;
        QTest::newRow("DefaultRealtimeClock") << "DefaultRealtimeClock.osd" << 5 << "clock" << false << QList<int>{65, 50, 0, 1, 0}
                                              << "Consolas/36" << "#0000FF" << "#FFFFFF" << QList<int>{-1, -1, -1} << "top_left" << 11;
        QTest::newRow("Masterplay Logo") << "Masterplay Logo.osd" << 1 << "logo" << false << QList<int>{55, 45, 0, 0, 1}
                                         << "" << "" << "" << QList<int>{-1, -1, -1} << "top_right" << 7;
    }
    void osdSamples() {
        QFETCH(QString, file); QFETCH(int, type); QFETCH(QString, kind); QFETCH(bool, kindFromName);
        QFETCH(QList<int>, pos); QFETCH(QString, font); QFETCH(QString, fontColor); QFETCH(QString, backColor);
        QFETCH(QList<int>, crawl); QFETCH(QString, anchor); QFETCH(int, lines);
        const QByteArray bytes = readAll(fx(file));
        QVERIFY2(!bytes.isEmpty(), qPrintable(file));
        QCOMPARE(int(ini::readIni(bytes).physicalLines), lines);
        const osd::OsdConfig c = osd::readOsd(bytes, file);
        QVERIFY2(c.diagnostics.isEmpty(), qPrintable(diagList(c.diagnostics)));
        QCOMPARE(c.name, file);
        QCOMPARE(c.encoding, Encoding::Utf8);        // pure ASCII
        QVERIFY(c.hasGlobal);
        QCOMPARE(c.type, type);
        QCOMPARE(c.kind, osd::kindOfType(type));
        bool fromName = false;
        QCOMPARE(osd::kindName(c.effectiveKind(&fromName)), kind);
        QCOMPARE(fromName, kindFromName);
        QCOMPARE((QList<int>{c.x, c.y, c.trans1, c.bTrans, c.dTrans}), pos);
        QCOMPARE(c.trans2, -1); QCOMPARE(c.trans3, -1);
        QCOMPARE(c.fontName.isEmpty() ? QString() : QStringLiteral("%1/%2").arg(c.fontName).arg(c.fontSize), font);
        QCOMPARE(c.fontColor.hex, fontColor);
        QCOMPARE(c.fontColor.present, !fontColor.isEmpty());
        QCOMPARE(c.backColor.hex, backColor);
        QVERIFY(!c.color1.present && !c.color2.present);
        QCOMPARE((QList<int>{c.crawlTime, c.rollTime, c.fieldMode}), crawl);
        QVERIFY(c.extra.isEmpty());
        QCOMPARE(c.placement().value("anchor").toString(), anchor);
        QCOMPARE(c.placement().value("fromOsd").toBool(true), false);

        const QJsonObject j = c.toJson();
        QCOMPARE(j.value("kind").toString(), kind);
        QCOMPARE(j.value("kindFromName").toBool(), kindFromName);
        QCOMPARE(j.contains("type"), type >= 0);
        QCOMPARE(j.value("fontColor").toString(), fontColor);
        QCOMPARE(j.contains("fontColor"), !fontColor.isEmpty());
        QCOMPARE(j.value("backColor").toString(), backColor);
        QCOMPARE(j.value("x").toInt(), pos[0]);
        QCOMPARE(j.value("placement").toObject().value("anchor").toString(), anchor);
        QVERIFY(j.value("diagnostics").toArray().isEmpty());
    }

    // The same samples read twice give the same struct; identical bytes (the manifest's two duplicate pairs) agree.
    void osdSamplesAgree() {
        QCOMPARE(osd::readOsd(readAll(fx("DefaultLogo.osd"))).toJson().value("dTrans"),
                 osd::readOsd(readAll(fx("Masterplay Logo.osd"))).toJson().value("dTrans"));
        QJsonObject a = osd::readOsd(readAll(fx("DefaultCountUp.osd")), "x").toJson();
        QJsonObject b = osd::readOsd(readAll(fx("DefaultRealtimeClock.osd")), "x").toJson();
        QCOMPARE(a, b);
    }

    // ---- TColor ------------------------------------------------------------------------------------------------------

    void tColorTable_data() {
        QTest::addColumn<qint64>("tcolor"); QTest::addColumn<QString>("hex");
        QTest::newRow("white 16777215") << qint64(16777215) << "#FFFFFF";
        QTest::newRow("teal 8421376") << qint64(8421376) << "#008080";
        QTest::newRow("black 0") << qint64(0) << "#000000";
        QTest::newRow("red 255") << qint64(255) << "#FF0000";
        QTest::newRow("lime 65280") << qint64(65280) << "#00FF00";
        QTest::newRow("blue 16711680") << qint64(16711680) << "#0000FF";
        QTest::newRow("purple 8388736 (Mplay.INI layers)") << qint64(8388736) << "#800080";
        QTest::newRow("gray 8421504 ([DrawGrid])") << qint64(8421504) << "#808080";
        QTest::newRow("mixed 0x00123456") << qint64(0x00123456) << "#563412";
        QTest::newRow("palette-relative 0x02FF8000") << qint64(0x02FF8000) << "#0080FF";
        QTest::newRow("clBtnFace -16777201") << qint64(-16777201) << "none";
        QTest::newRow("clBtnFace 0xFF00000F") << qint64(0xFF00000FLL) << "none";
        QTest::newRow("old system 0x80000005") << qint64(0x80000005LL) << "none";
        QTest::newRow("clNone") << qint64(0x1FFFFFFF) << "none";
        QTest::newRow("clDefault") << qint64(0x20000000) << "none";
        QTest::newRow("palette index 0x01000005") << qint64(0x01000005) << "none";
        QTest::newRow("too big") << qint64(0x100000000LL) << "none";
        QTest::newRow("too small") << qint64(-2147483649LL) << "none";
    }
    void tColorTable() {
        QFETCH(qint64, tcolor); QFETCH(QString, hex);
        QCOMPARE(hexOf(tcolor), hex);
        if (hex != QLatin1String("none") && (tcolor >> 24) == 0) {
            QCOMPARE(osd::hexToTColor(hex).value_or(-1), tcolor);      // the exporter's way back
            QCOMPARE(osd::parseTColor(QString::number(tcolor)).value_or(-1), tcolor);
        }
    }

    void tColorParsing() {
        auto tc = [](const char* s) { return osd::parseTColor(QString::fromLatin1(s)).value_or(-1); };
        QCOMPARE(tc("16777215"), qint64(16777215));
        QCOMPARE(tc(" 8421376 "), qint64(8421376));
        QCOMPARE(tc("$00808000"), qint64(8421376));
        QCOMPARE(tc("0x808000"), qint64(8421376));
        QCOMPARE(tc("clTeal"), qint64(8421376));
        QCOMPARE(tc("CLWHITE"), qint64(16777215));
        QCOMPARE(tc("#008080"), qint64(8421376));
        QCOMPARE(tc("-16777201"), qint64(-16777201));
        QCOMPARE(hexOf(tc("clBtnFace")), QStringLiteral("none"));
        QCOMPARE(hexOf(tc("$FF00000F")), QStringLiteral("none"));
        QCOMPARE(hexOf(tc("clNone")), QStringLiteral("none"));
        QVERIFY(!osd::parseTColor("").has_value());
        QVERIFY(!osd::parseTColor("teal").has_value());
        QVERIFY(!osd::parseTColor("99999999999").has_value());
        QVERIFY(!osd::parseTColor("12abc").has_value());
        QCOMPARE(osd::hexToTColor("008080").value_or(-1), qint64(8421376));
        QCOMPARE(osd::hexToTColor("#ff0000").value_or(-1), qint64(255));
        QVERIFY(!osd::hexToTColor("#00808").has_value());
        QVERIFY(!osd::hexToTColor("#GG0000").has_value());
        QVERIFY(!osd::hexToTColor("").has_value());
    }

    // ---- OSD edge cases ----------------------------------------------------------------------------------------------

    void osdDiagnostics() {
        const QByteArray b = crlf({
            "Stray=1",                    // 1  before any section: MP001
            "[global]",                   // 2  section names ignore case
            "Type = 6",                   // 3  blanks around '='
            "XPos=abc",                   // 4  MP011 at the value (col 6)
            "YPos=  486",                 // 5
            "FontColor=teal-ish",         // 6  MP011 col 11
            "BFarbe=clNavy",              // 7
            "FontName=\"Arial Black\"",   // 8  outer quotes go
            "Shadow=1",                   // 9  not modelled: extra
            "garbage line",               // 10 MP001
            "; comment",                  // 11
            "",                           // 12
            "Type=1",                     // 13 repeated key: the first wins
            "[Global]",                   // 14 repeated section: invisible to lookups
            "Type=5"});
        const osd::OsdConfig c = osd::readOsd(b, "custom.osd");
        QCOMPARE(diagList(c.diagnostics), QStringLiteral("1:MP001:1 4:MP011:6 6:MP011:11 10:MP001:1"));
        for (const Diagnostic& d : c.diagnostics) QCOMPARE(d.severity, diagnosticInfo(d.code).severity);
        QVERIFY(c.hasGlobal);
        QCOMPARE(c.type, 6);
        QCOMPARE(c.kind, osd::Kind::Crawl);
        QCOMPARE(c.x, -1);
        QCOMPARE(c.y, 486);
        QVERIFY(c.fontColor.present && !c.fontColor.valid);
        QCOMPARE(c.fontColor.toJson().toObject().value("raw").toString(), QStringLiteral("teal-ish"));
        QCOMPARE(c.backColor.hex, QStringLiteral("#000080"));
        QCOMPARE(c.fontName, QStringLiteral("Arial Black"));
        QCOMPARE(c.extra.size(), 1);
        QCOMPARE(c.extra[0].name, QStringLiteral("Shadow"));
        QCOMPARE(c.toJson().value("extra").toObject().value("Shadow").toString(), QStringLiteral("1"));

        const ini::IniFile f = ini::readIni(b);
        QCOMPARE(f.comments, 1); QCOMPARE(f.blank, 1); QCOMPARE(f.ignored, 2);
        QCOMPARE(f.sections.size(), 2);
        QCOMPARE(f.value("GLOBAL", "type"), QStringLiteral("6"));

        // A system colour is valid but has no hex: the JSON keeps the raw value and the number.
        const osd::OsdConfig s = osd::readOsd(crlf({"[Global]", "Type=1", "FontColor=-16777201", "BFarbe=$FF00000F"}));
        QVERIFY(s.diagnostics.isEmpty());
        QVERIFY(s.fontColor.valid && s.fontColor.hex.isEmpty());
        QCOMPARE(s.fontColor.toJson().toObject().value("tcolor").toDouble(), -16777201.0);
        QCOMPARE(s.backColor.toJson().toObject().value("tcolor").toDouble(), double(0xFF00000FLL));

        // No [Global]: one MP011 for the file, defaults, the kind guessed from the name.
        const osd::OsdConfig n = osd::readOsd(crlf({"[Other]", "Type=1"}), "DefaultLogo.osd");
        QVERIFY(!n.hasGlobal);
        QCOMPARE(diagList(n.diagnostics), QStringLiteral("0:MP011:0"));
        QCOMPARE(n.type, -1);
        QCOMPARE(n.placement().value("anchor").toString(), QStringLiteral("top_right"));
        QCOMPARE(osd::readOsd(QByteArray(), "x.osd").placement().value("anchor").toString(), QStringLiteral("bottom_left"));
        QCOMPARE(osd::readOsd(crlf({"[Global]", "Type=3"})).kind, osd::Kind::Other);
    }

    void matchOsdNames() {
        const QStringList names = QDir(QStringLiteral(MP_FIXTURES)).entryList({"*.osd"}, QDir::Files);
        QCOMPARE(names.size(), 6);
        QCOMPARE(osd::matchOsdName("D:\\Masterplay\\OSD\\MasterPlay Logo.osd", names), QStringLiteral("Masterplay Logo.osd"));
        QCOMPARE(osd::matchOsdName("D:/x/defaultcrawl.OSD", names), QStringLiteral("DefaultCrawl.osd"));
        QCOMPARE(osd::matchOsdName("DefaultLogo", names), QStringLiteral("DefaultLogo.osd"));
        QCOMPARE(osd::matchOsdName("D:\\OSD\\nothing.osd", names), QString());
        QCOMPARE(osd::matchOsdName("D:\\OSD\\", names), QString());
        // Every cfg= in the sample playlists names one of the six samples.
        int cfgs = 0;
        static const QRegularExpression kCfg(QStringLiteral("\\tcfg=([^\\t\\r\\n]+)"));
        for (const char* pl : {"OSD-Instructions.in", "sample.in"}) {
            const QString text = QString::fromUtf8(readAll(fx(QString::fromLatin1(pl))));
            for (auto it = kCfg.globalMatch(text); it.hasNext();) {
                const QString cfg = it.next().captured(1);
                const QString hit = osd::matchOsdName(cfg, names);
                QVERIFY2(!hit.isEmpty(), qPrintable(cfg));
                QVERIFY(osd::readOsd(readAll(fx(hit)), hit).diagnostics.isEmpty());
                ++cfgs;
            }
        }
        QCOMPARE(cfgs, 6);
    }

    // ---- the profile reader ------------------------------------------------------------------------------------------

    void iniSemantics() {
        QByteArray b = "; head comment\r\n[A]\rk1=v1\nK2 =  spaced value  \r\n"
                       "q1=\"quoted\"\r\nq2='single'\r\nq3=\"open\r\nq4=\"\"\r\ntab=a\tb\t\r\n"
                       "nb=\xC2\xA0x\xC2\xA0\r\n=novalue\r\n[noclose\r\n[ B ] trailing text\r\nk1=b1\r\n"
                       "[a]\r\nk1=second\r\nnum=$1F\r\nplus=+5\r\nsp= 7 \r\nbad=abc\r\nt1=TRUE\r\nt2=off\r\nt3=2\r\n";
        const ini::IniFile f = ini::readIni(b);
        QCOMPARE(f.encoding, Encoding::Utf8);
        QCOMPARE(diagList(f.diagnostics), QStringLiteral("11:MP001:1 12:MP001:1"));   // "=novalue", "[noclose"
        QCOMPARE(f.sectionNames(), (QStringList{"A", "B", "a"}));
        QCOMPARE(f.value("a", "K1"), QStringLiteral("v1"));         // the first [A] answers, case-insensitively
        QCOMPARE(f.value("A", "k2"), QStringLiteral("spaced value"));
        QCOMPARE(f.section("A")->key("k2")->raw, QStringLiteral("  spaced value  "));
        QCOMPARE(f.section("A")->key("k2")->col, 5);
        QCOMPARE(f.value("A", "q1"), QStringLiteral("quoted"));
        QCOMPARE(f.value("A", "q2"), QStringLiteral("single"));
        QCOMPARE(f.value("A", "q3"), QStringLiteral("\"open"));
        QCOMPARE(f.value("A", "q4"), QString());
        QCOMPARE(f.value("A", "tab"), QStringLiteral("a\tb"));      // inner TAB kept, trailing TAB trimmed
        QCOMPARE(f.value("A", "nb"), QString::fromUtf8("\xC2\xA0x\xC2\xA0"));   // U+00A0 is not a blank for Windows
        QCOMPARE(f.value("B", "k1"), QStringLiteral("b1"));
        QCOMPARE(f.value("A", "num"), QString());                   // in the second [a]: invisible
        QCOMPARE(f.sections[2].value("num"), QStringLiteral("$1F"));
        QCOMPARE(f.value("missing", "k", "def"), QStringLiteral("def"));
        QVERIFY(f.has("A", "Q1") && !f.has("A", "num"));
        QCOMPARE(f.physicalLines, 23);                              // "[A]\r" ended its line
        QCOMPARE(f.comments, 1);

        const ini::IniFile g = ini::readIni("[S]\r\nnum=$1F\r\nhex=0x10\r\nplus=+5\r\nsp= 7 \r\nbad=abc\r\nempty=\r\n"
                                            "t1=TRUE\r\nt2=off\r\nt3=2\r\nt4=maybe\r\n");
        bool ok = false;
        QCOMPARE(g.intValue("S", "num", -1, &ok), 31); QVERIFY(ok);
        QCOMPARE(g.intValue("S", "hex"), 16);
        QCOMPARE(g.intValue("S", "plus"), 5);
        QCOMPARE(g.intValue("S", "sp"), 7);
        QCOMPARE(g.intValue("S", "bad", -9, &ok), -9); QVERIFY(!ok);
        QCOMPARE(g.intValue("S", "empty", -9), -9);
        QCOMPARE(g.intValue("S", "absent", 4), 4);
        QVERIFY(g.boolValue("S", "t1") && !g.boolValue("S", "t2", true) && g.boolValue("S", "t3"));
        QVERIFY(g.boolValue("S", "t4", true) && !g.boolValue("S", "t4", false) && g.boolValue("S", "absent", true));
    }

    void iniEncodings() {
        const QString text = QString::fromUtf8("[S]\r\nName=Z\xC3\xBCrich \xE2\x82\xAC\r\n");
        const ini::IniFile bom = ini::readIni("\xEF\xBB\xBF" + text.toUtf8());
        QCOMPARE(bom.encoding, Encoding::Utf8Bom);
        QCOMPARE(bom.value("S", "Name"), QString::fromUtf8("Z\xC3\xBCrich \xE2\x82\xAC"));
        QVERIFY(bom.diagnostics.isEmpty());
        for (bool le : {true, false}) {
            const ini::IniFile u = ini::readIni(utf16(text, le));
            QCOMPARE(u.encoding, le ? Encoding::Utf16LE : Encoding::Utf16BE);
            QCOMPARE(u.value("S", "Name"), QString::fromUtf8("Z\xC3\xBCrich \xE2\x82\xAC"));
            QVERIFY(u.diagnostics.isEmpty());
        }
        // cp1252 (what Delphi writes): not reported, decoded with the Windows table.
        const ini::IniFile ansi = ini::readIni(QByteArray("[S]\r\nAusVerz\xF6gerung=2000\r\nE=\x80\x81\r\n"));
        QCOMPARE(ansi.encoding, Encoding::Ansi1252);
        QVERIFY(ansi.diagnostics.isEmpty());
        QCOMPARE(ansi.value("S", QString::fromUtf8("ausverz\xC3\xB6gerung")), QStringLiteral("2000"));
        QCOMPARE(ansi.value("S", "E"), QString(QChar(0x20AC)) + QChar(0x81));
        // Bad bytes after a BOM: MP010 at their line.
        const ini::IniFile badBom = ini::readIni(QByteArray("\xEF\xBB\xBF[S]\r\nA=1\r\nB=\xFF\r\n"));
        QCOMPARE(diagList(badBom.diagnostics), QStringLiteral("3:MP010:0"));
        const ini::IniFile odd = ini::readIni(utf16("[S]\r\nA=1", true) + QByteArray("\x41", 1));
        QCOMPARE(odd.diagnostics.size(), 1);
        QCOMPARE(odd.diagnostics[0].code, QStringLiteral("MP010"));
        QVERIFY(ini::readIni(QByteArray()).sections.isEmpty());
    }

    void splitListTable_data() {
        QTest::addColumn<QString>("value"); QTest::addColumn<QStringList>("items");
        QTest::newRow("empty") << "" << QStringList{};
        QTest::newRow("one") << "D:\\Media" << QStringList{"D:\\Media"};
        QTest::newRow("spaces kept") << "D:\\My Media" << QStringList{"D:\\My Media"};
        QTest::newRow("semicolon") << "D:\\A;E:\\B" << QStringList{"D:\\A", "E:\\B"};
        QTest::newRow("bar") << "D:\\A|E:\\B" << QStringList{"D:\\A", "E:\\B"};
        QTest::newRow("comma") << "D:\\A,E:\\B" << QStringList{"D:\\A", "E:\\B"};
        QTest::newRow("blanks and empties") << " D:\\A ; ;E:\\B ; " << QStringList{"D:\\A", "E:\\B"};
        QTest::newRow("commatext") << "\"D:\\My Media\",E:\\Clips" << QStringList{"D:\\My Media", "E:\\Clips"};
        QTest::newRow("commatext doubled quote") << "\"D:\\a \"\"q\"\"\",x" << QStringList{"D:\\a \"q\"", "x"};
        QTest::newRow("commatext blank separates") << "\"a b\" c,d" << QStringList{"a b", "c", "d"};
        QTest::newRow("commatext empty items") << "\"a\",,\"b\"," << QStringList{"a", "b"};
        QTest::newRow("extensions") << "*.mpg;*.m2p" << QStringList{"*.mpg", "*.m2p"};
    }
    void splitListTable() {
        QFETCH(QString, value); QFETCH(QStringList, items);
        QCOMPARE(ini::splitList(value), items);
    }

    // ---- Mplay.INI (the synthetic sample) -------------------------------------------------------------------

    void mplaySampleFile() {
        const QByteArray bytes = readAll(fx("mplay_sample.ini"));
        const ini::IniFile f = ini::readIni(bytes);
        QCOMPARE(f.encoding, Encoding::Ansi1252);               // German key names in cp1252
        QVERIFY2(f.diagnostics.isEmpty(), qPrintable(diagList(f.diagnostics)));
        QCOMPARE(f.physicalLines, int(bytes.count('\n')));
        QCOMPARE(f.ignored, 0);
        QCOMPARE(f.sectionNames().first(), QStringLiteral("MPEGPlay"));
        QCOMPARE(f.sectionNames().last(), QStringLiteral("LastUsedOnAirPlaylists0"));
        QVERIFY(f.section("Medialooks 1") && f.section("Laufschrift0") && f.section("CountDown1"));
        QCOMPARE(f.value("MPEGPlay", "Port1"), QStringLiteral("COM3\tNot Defined"));
        QCOMPARE(f.value("MPEGPlay", "LogoOrte"), QStringLiteral("40,40,600,40,40,440,600,440,"));
        QCOMPARE(f.value("PlayListEditor", "Videoformat"), QStringLiteral("23.98"));
        QCOMPARE(f.section("PlayListEditor")->key("Videoformat")->raw, QStringLiteral("23.98 "));
        QCOMPARE(f.value("MPEGPlay", "InFilesDirectory"), f.value("PlayListEditor", "InFilesDirectory"));
        // The reader keeps every value (the sample only has placeholders where a real file keeps stored passwords).
        QCOMPARE(f.value("ApprovalUsers", "User0"), QStringLiteral("admin\tredacted\t0\t1"));
        QCOMPARE(f.intValue("ApprovalUsers", "ItemCount"), 2);
        // Mplay.INI keeps per-layer OSD defaults in the .osd shape.
        const osd::OsdConfig crawl = osd::fromSection(*f.section("Laufschrift0"));
        QVERIFY2(crawl.diagnostics.isEmpty(), qPrintable(diagList(crawl.diagnostics)));
        QCOMPARE(crawl.name, QStringLiteral("Laufschrift0"));
        QCOMPARE(crawl.effectiveKind(), osd::Kind::Crawl);
        QCOMPARE(crawl.x, 0); QCOMPARE(crawl.y, 480);
        QCOMPARE(crawl.fontColor.hex, QStringLiteral("#FFFFFF")); QCOMPARE(crawl.backColor.hex, QStringLiteral("#000000"));
        QCOMPARE(crawl.color1.hex, QStringLiteral("#FFFFFF")); QCOMPARE(crawl.color2.hex, QStringLiteral("#000000"));
        QCOMPARE(crawl.trans2, 2); QCOMPARE(crawl.textFieldWidth, 100); QCOMPARE(crawl.fieldMode, 1); QCOMPARE(crawl.timeSec, 4);
        QVERIFY(crawl.extra.isEmpty());
        const osd::OsdConfig cd1 = osd::fromSection(*f.section("CountDown1"));
        QCOMPARE(cd1.color1.hex, QStringLiteral("#008000"));
        QCOMPARE(cd1.fontName, QStringLiteral("Courier New")); QCOMPARE(cd1.fontSize, 24);
        QCOMPARE(cd1.effectiveKind(), osd::Kind::Clock);
        QVERIFY(osd::fromSection(*f.section("FlipText0")).extra.isEmpty());
    }

    void mplaySubset() {
        const ini::MplayIni m = ini::readMplayIni(readAll(fx("mplay_sample.ini")));
        QVERIFY2(m.diagnostics.isEmpty(), qPrintable(diagList(m.diagnostics)));
        QVERIFY(m.isMplayIni);
        QCOMPARE(m.encoding, Encoding::Ansi1252);
        QCOMPARE(m.iniVersion, QStringLiteral("4.70"));
        QCOMPARE(m.playlistDir, QStringLiteral("D:\\Masterplay\\Playlist"));
        QCOMPARE(m.logDir, QStringLiteral("D:\\Masterplay\\Logfiles"));
        QCOMPARE(m.osdDir, QStringLiteral("D:\\Masterplay\\Osd"));
        QCOMPARE(m.tempDir, QStringLiteral("D:\\Masterplay\\Temp"));
        QCOMPARE(m.asRunLog, QStringLiteral("LogFile.log"));
        QCOMPARE(m.statusLog, QStringLiteral("Status.log"));
        QCOMPARE(m.operatorLog, QStringLiteral("Operator.log"));
        QVERIFY(m.asRunWithDirectories && m.asRunAddInfoFields && m.asRunIsoDateTime);
        QCOMPARE(m.asRunNaming, 0);
        QVERIFY(m.searchPaths.isEmpty());                         // SuchPfade= in both sections
        QVERIFY(!m.searchIncludeSubDirs && !m.searchUseWildChar && !m.searchNewProgInSearchPaths);
        QVERIFY(m.searchExcludedExtensions.isEmpty());
        QVERIFY(m.substitutes.isEmpty());
        QCOMPARE(m.videoExtensions, (QStringList{".mpg", ".m2p", ".mp2", ".avi", ".vob", ".mus", ".ts", ".cry", ".wmv", ".mov"}));
        QCOMPARE(m.videoOnlyExtensions, (QStringList{".mpo", ".mpv", ".m2v"}));
        QCOMPARE(m.audioExtensions, (QStringList{".wav", ".mp3", ".mpa", ".wma"}));
        QVERIFY(!m.dailyPlaylistMode);
        QVERIFY(m.autoReloadPlaylist && m.loadPassedRepeatLoop && !m.eventsSamePriority && !m.osdClearEndOfExact);
        QCOMPARE(m.loadPassedDays, 2);
        QCOMPARE(m.defPictureTimeSec, 10.0);
        QCOMPARE(m.defaultFrameRate, 23.98);
        QCOMPARE(m.stationName, QString());                       // "Noch kein Name" is the German default
        QVERIFY(m.isoDateTimes);
        QCOMPARE(m.editorFramesPerSecond, 25);
        QCOMPARE(m.editorFrameRate, 23.98);
        QCOMPARE(m.suggestedFps(), 24);
        QCOMPARE(m.dayChange, QTime(0, 0));
        QCOMPARE(m.exchangeMode, ini::ExchangeMode::Automatic);   // ExchangeDriveLetters=2
        QCOMPARE(m.localDriveLetter, QChar('D'));
        QVERIFY(m.mappedDriveLetter.isNull());
        QVERIFY(m.exchangeList.isEmpty());
        QVERIFY(!m.generateUuids);
        QCOMPARE(m.templateDir, QStringLiteral("D:\\Masterplay\\Playlist"));
        QCOMPARE(m.commandListsDir, QStringLiteral("D:\\Masterplay\\CommandLists"));
        QCOMPARE(m.defaultSelectionDir, QStringLiteral("D:\\Masterplay"));
        QCOMPARE(m.lastPlaylist, QStringLiteral("D:\\Masterplay\\Playlist\\Saturday_0200.in"));
        QCOMPARE(m.startupPlaylist, QString());
        QCOMPARE(m.recentPlaylists, (QStringList{"D:\\Masterplay\\Playlist\\Saturday_0200.in", "D:\\Masterplay\\Playlist\\sample.in"}));
        QCOMPARE(m.outputFormat, QStringLiteral("1920x1080 HD1080 25p"));
        QCOMPARE(m.outputDevice, QStringLiteral("Sample Output Card Ch 1"));
        QCOMPARE(m.channelName, QString());
        QVERIFY(m.outputNdi);
        QCOMPARE(m.installDir, QStringLiteral("D:\\Masterplay"));

        const QJsonObject j = m.toJson();
        assertNoSecrets(j);
        QCOMPARE(j.value("encoding").toString(), QStringLiteral("cp1252"));
        QCOMPARE(j.value("editor").toObject().value("exchangeDriveLetters").toString(), QStringLiteral("automatic"));
        QCOMPARE(j.value("editor").toObject().value("localDriveLetter").toString(), QStringLiteral("D"));
        QCOMPARE(j.value("editor").toObject().value("dayChange").toString(), QStringLiteral("00:00"));
        QCOMPARE(j.value("player").toObject().value("videoExtensions").toArray().size(), 10);
        QCOMPARE(j.value("onAir").toObject().value("recentPlaylists").toArray().size(), 2);
        QCOMPARE(j.value("suggestedFps").toInt(), 24);
    }

    // POST /masterplay/config/from-ini on the sample: the source record and the seeded path rules.
    void suggestFromSample() {
        const ini::MplayIni m = ini::readMplayIni(readAll(fx("mplay_sample.ini")));

        // No location given (pasted text): ExchangeDriveLetters=2 with no mapped letter gives an identity rule for
        // MasterPlay's local drive, flagged as a guess so the operator sets it.
        const ini::SourceSuggestion s = ini::suggestSource(m);
        QCOMPARE(ruleList(s.pathMap), QStringLiteral("D:\\>D:\\(MasterPlayLocalDriveLetter?)"));
        QCOMPARE(s.source.value("id").toString(), QStringLiteral("masterplay"));
        QCOMPARE(s.source.value("label").toString(), QStringLiteral("MasterPlay"));
        QCOMPARE(s.source.value("read").toString(), QStringLiteral("fs"));
        QCOMPARE(s.source.value("playlistDir").toString(), QStringLiteral("D:\\Masterplay\\Playlist"));
        QCOMPARE(s.source.value("logPath").toString(), QStringLiteral("D:\\Masterplay\\Logfiles\\LogFile.log"));
        QCOMPARE(s.source.value("statusLog").toString(), QStringLiteral("D:\\Masterplay\\Logfiles\\Status.log"));
        QCOMPARE(s.source.value("operatorLog").toString(), QStringLiteral("D:\\Masterplay\\Logfiles\\Operator.log"));
        QCOMPARE(s.source.value("osdDir").toString(), QStringLiteral("D:\\Masterplay\\Osd"));
        QCOMPARE(s.source.value("tz").toString(), QString());
        QCOMPARE(s.source.value("dailyPlaylistMode").toBool(true), false);
        QCOMPARE(s.source.value("lastPlaylist").toString(), QStringLiteral("D:\\Masterplay\\Playlist\\Saturday_0200.in"));
        QCOMPARE(s.source.value("recentPlaylists").toArray().size(), 2);
        QCOMPARE(s.source.value("ini").toObject().value("logPath").toString(), QStringLiteral("D:\\Masterplay\\Logfiles\\LogFile.log"));
        QVERIFY(s.notes.join('\n').contains("tz:"));
        const QJsonObject j = s.toJson(m);
        assertNoSecrets(j);
        QCOMPARE(j.value("pathMap").toArray().size(), 1);
        QCOMPARE(j.value("pathMap").toArray()[0].toObject().value("guess").toBool(), true);
        QCOMPARE(j.value("defaults").toObject().value("fps").toInt(), 24);
        QCOMPARE(j.value("defaults").toObject().value("defPictureTimeSec").toDouble(), 10.0);
        QVERIFY(j.value("searchPaths").toArray().isEmpty());
        QCOMPARE(j.value("ini").toObject().value("installDir").toString(), QStringLiteral("D:\\Masterplay"));

        // Read over a share: the MasterPlay folder is reachable there (iniLocation), MasterPlay's local drive is guessed
        // as the administrative share, and the source folders are mapped.
        const ini::SourceSuggestion u = ini::suggestSource(m, {"\\\\MASTERPLAY-PC\\Masterplay\\Mplay.INI"});
        QCOMPARE(ruleList(u.pathMap), QStringLiteral("D:\\Masterplay\\>\\\\MASTERPLAY-PC\\Masterplay\\(iniLocation) "
                                                     "D:\\>\\\\MASTERPLAY-PC\\D$\\(MasterPlayLocalDriveLetter?)"));
        QCOMPARE(u.source.value("id").toString(), QStringLiteral("masterplay-masterplay-pc"));
        QCOMPARE(u.source.value("label").toString(), QStringLiteral("MasterPlay MASTERPLAY-PC"));
        QCOMPARE(u.source.value("playlistDir").toString(), QStringLiteral("\\\\MASTERPLAY-PC\\Masterplay\\Playlist"));
        QCOMPARE(u.source.value("logPath").toString(), QStringLiteral("\\\\MASTERPLAY-PC\\Masterplay\\Logfiles\\LogFile.log"));
        QCOMPARE(u.source.value("osdDir").toString(), QStringLiteral("\\\\MASTERPLAY-PC\\Masterplay\\Osd"));
        QCOMPARE(u.source.value("lastPlaylist").toString(), QStringLiteral("\\\\MASTERPLAY-PC\\Masterplay\\Playlist\\Saturday_0200.in"));
        QCOMPARE(u.source.value("ini").toObject().value("playlistDir").toString(), QStringLiteral("D:\\Masterplay\\Playlist"));
        QCOMPARE(ini::mapPath("D:\\Media\\News\\x.mpg", u.pathMap), QStringLiteral("\\\\MASTERPLAY-PC\\D$\\Media\\News\\x.mpg"));
        QCOMPARE(ini::mapPath("d:/masterplay/OSD/DefaultLogo.osd", u.pathMap), QStringLiteral("\\\\MASTERPLAY-PC\\Masterplay\\OSD\\DefaultLogo.osd"));
        // The same folder written with forward slashes.
        QCOMPARE(ruleList(ini::suggestSource(m, {"//MASTERPLAY-PC/Masterplay/Mplay.INI"}).pathMap), ruleList(u.pathMap));

        // A local or copied path says nothing about the MasterPlay folder.
        const ini::SourceSuggestion l = ini::suggestSource(m, {"C:\\copies\\Mplay.INI"});
        QCOMPARE(ruleList(l.pathMap), ruleList(s.pathMap));
        QVERIFY(l.notes.join('\n').contains("local or copied path"));

        // A copy kept on another share: the folder is not named like the MasterPlay folder, so the location rule is a
        // guess too, and the notes ask for it to be confirmed.
        const ini::SourceSuggestion c = ini::suggestSource(m, {"\\\\nas\\configs\\station\\Mplay.INI"});
        QCOMPARE(ruleList(c.pathMap), QStringLiteral("D:\\Masterplay\\>\\\\nas\\configs\\station\\(iniLocation?) "
                                                     "D:\\>\\\\nas\\D$\\(MasterPlayLocalDriveLetter?)"));
        QVERIFY(c.pathMap[0].note.startsWith("guess:"));
        QVERIFY(c.notes.join('\n').contains("The INI may be a copy"));
        QVERIFY(!u.notes.join('\n').contains("The INI may be a copy"));
        // The share folder's name decides, in any case.
        QCOMPARE(ruleList(ini::suggestSource(m, {"\\\\h\\MASTERPLAY\\Mplay.INI"}).pathMap).section(' ', 0, 0),
                 QStringLiteral("D:\\Masterplay\\>\\\\h\\MASTERPLAY\\(iniLocation)"));
    }

    // The seeding rules on synthetic INIs: each exchange mode, ExchangeList forms, search paths, substitutes, bad values.
    void suggestVariants() {
        auto mk = [](const QStringList& editor, const QStringList& player = {}) {
            QStringList lines{"[MPEGPlay]", "InFilesDirectory=D:\\MP\\Playlist", "LogFilesDirectory=D:\\MP\\Logfiles"};
            lines << player << "[PlayListEditor]" << editor;
            return ini::readMplayIni(crlf(lines));
        };
        // Enabled with a mapped letter: local -> mapped, stated by the INI (no guess).
        ini::MplayIni m = mk({"ExchangeDriveLetters=1", "MasterPlayLocalDriveLetter=d", "MappedDriveLetter=X:"});
        QCOMPARE(m.mappedDriveLetter, QChar('X'));
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("D:\\>X:\\(ExchangeDriveLetters)"));
        QCOMPARE(ini::suggestSource(m).source.value("playlistDir").toString(), QStringLiteral("X:\\MP\\Playlist"));
        // Automatic with a mapped letter behaves the same; the same letter on both sides is the identity guess.
        m = mk({"ExchangeDriveLetters=2", "MasterPlayLocalDriveLetter=D", "MappedDriveLetter=Y"});
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("D:\\>Y:\\(ExchangeDriveLetters)"));
        m = mk({"ExchangeDriveLetters=1", "MasterPlayLocalDriveLetter=D", "MappedDriveLetter=D"});
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("D:\\>D:\\(MasterPlayLocalDriveLetter?)"));
        // Disabled: no drive rule, said in the notes.
        m = mk({"ExchangeDriveLetters=0", "MasterPlayLocalDriveLetter=D", "MappedDriveLetter=X"});
        ini::SourceSuggestion s = ini::suggestSource(m);
        QVERIFY(s.pathMap.isEmpty());
        QVERIFY(s.notes.join('\n').contains("disabled"));
        QCOMPARE(s.source.value("playlistDir").toString(), QStringLiteral("D:\\MP\\Playlist"));

        // ExchangeList: "editor;playlist" pairs, read in the load direction (playlist path -> editor path).
        m = mk({"ExchangeList=f:\\ReplaceWith;G:\\ReplaceWithThis"});
        QCOMPARE(m.exchangeList.size(), 1);
        QCOMPARE(m.exchangeList[0].editor, QStringLiteral("f:\\ReplaceWith"));
        QCOMPARE(m.exchangeList[0].playlist, QStringLiteral("G:\\ReplaceWithThis"));
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("G:\\ReplaceWithThis\\>f:\\ReplaceWith\\(ExchangeList)"));
        QCOMPARE(ini::mapPath("G:\\ReplaceWithThis\\a\\b.mpg", ini::suggestSource(m).pathMap), QStringLiteral("f:\\ReplaceWith\\a\\b.mpg"));
        m = mk({"ExchangeList=\"F:\\My A;G:\\B\",H:\\C;I:\\D"});
        QCOMPARE(m.exchangeList.size(), 2);
        QCOMPARE(m.exchangeList[0].editor, QStringLiteral("F:\\My A"));
        QCOMPARE(m.exchangeList[1].playlist, QStringLiteral("I:\\D"));
        m = mk({"ExchangeList=F:\\A;G:\\B;H:\\C;I:\\D"});                // only ';': read pairwise
        QCOMPARE(m.exchangeList.size(), 2);
        QCOMPARE(m.exchangeList[1].editor, QStringLiteral("H:\\C"));
        m = mk({"ExchangeList=F:\\A;G:\\B|H:\\C"});                      // odd: the last one is dropped with MP011
        QCOMPARE(m.exchangeList.size(), 1);
        QCOMPARE(diagList(m.diagnostics), QStringLiteral("5:MP011:14"));
        // ExchangeList and drive letters together, sorted longest first; an ExchangeList rule wins a tie on `from`.
        m = mk({"ExchangeDriveLetters=1", "MasterPlayLocalDriveLetter=D", "MappedDriveLetter=X", "ExchangeList=Z:\\;D:\\"});
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("D:\\>Z:\\(ExchangeList)"));
        m = mk({"ExchangeDriveLetters=1", "MasterPlayLocalDriveLetter=D", "MappedDriveLetter=X", "ExchangeList=Y:\\Media;D:\\Media"});
        QCOMPARE(ruleList(ini::suggestSource(m).pathMap), QStringLiteral("D:\\Media\\>Y:\\Media\\(ExchangeList) D:\\>X:\\(ExchangeDriveLetters)"));

        // Search paths: both sections and a [SearchDirs] section (absolute paths only), de-duplicated.
        m = mk({"SuchPfade=d:\\clips\\;\\\\nas\\more"}, {"SuchPfade=D:\\Clips;\\\\nas\\media", "SubstituteTable=%news%=\\\\nas\\news,%promo%=D:\\Promos,bad"});
        QCOMPARE(m.searchPaths, (QStringList{"D:\\Clips", "\\\\nas\\media", "\\\\nas\\more"}));
        QCOMPARE(m.substitutes.size(), 2);
        QCOMPARE(m.substitutes[0].name, QStringLiteral("news"));
        QCOMPARE(m.substitutes[0].value, QStringLiteral("\\\\nas\\news"));
        QCOMPARE(m.substitutes[1].name, QStringLiteral("promo"));
        QCOMPARE(diagList(m.diagnostics), QStringLiteral("5:MP011:17"));
        m = ini::readMplayIni(crlf({"[MPEGPlay]", "SuchPfade=", "[SearchDirs]", "Count=2", "Dir0=E:\\More", "Dir1=relative\\x"}));
        QCOMPARE(m.searchPaths, QStringList{"E:\\More"});
        s = ini::suggestSource(m);
        QCOMPARE(s.toJson(m).value("searchPaths").toArray()[0].toObject().value("mapped").toString(), QStringLiteral("E:\\More"));
        // A [SearchDirs] value holding a list splits when every item is an absolute path; one path holding ',' (its
        // second piece is no path) stays whole, as do paths with spaces; mixed lists with a relative item are skipped.
        m = ini::readMplayIni(crlf({"[SearchDirs]", "SuchPfade=D:\\A;\\\\nas\\B|e:\\a\\", "Dir0=D:\\Clips, old", "Dir1=D:\\My Clips",
                                    "Dir2=F:\\x,G:\\y", "Dir3=rel;H:\\z", "Dir4=d:\\clips, OLD\\"}));
        QCOMPARE(m.searchPaths, (QStringList{"D:\\A", "\\\\nas\\B", "e:\\a\\", "D:\\Clips, old", "D:\\My Clips", "F:\\x", "G:\\y"}));
        QVERIFY(m.diagnostics.isEmpty());

        // Bad values: MP011 at the value, the field left unset.
        m = mk({"ExchangeDriveLetters=7", "MasterPlayLocalDriveLetter=DD", "MappedDriveLetter=1"});
        QCOMPARE(m.exchangeMode, ini::ExchangeMode::Absent);
        QVERIFY(m.localDriveLetter.isNull() && m.mappedDriveLetter.isNull());
        QCOMPARE(diagList(m.diagnostics), QStringLiteral("5:MP011:22 6:MP011:28 7:MP011:19"));
        QVERIFY(ini::suggestSource(m).pathMap.isEmpty());
        // Enabled without a local letter: nothing to seed.
        m = mk({"ExchangeDriveLetters=1", "MappedDriveLetter=X"});
        s = ini::suggestSource(m);
        QVERIFY(s.pathMap.isEmpty());
        QVERIFY(s.notes.join('\n').contains("without MasterPlayLocalDriveLetter"));

        // Not an Mplay.INI.
        m = ini::readMplayIni(crlf({"[Foo]", "A=1"}));
        QVERIFY(!m.isMplayIni);
        s = ini::suggestSource(m, {"\\\\h\\s\\Mplay.INI"});
        QVERIFY(s.pathMap.isEmpty());
        QVERIFY(s.notes.join('\n').contains("not an Mplay.INI"));
        QCOMPARE(s.source.value("playlistDir").toString(), QString());
        QCOMPARE(s.source.value("logPath").toString(), QString());

        // Station name and the INI layout without an editor section; DailyPlaylistMode from either section.
        m = ini::readMplayIni(crlf({"[MPEGPlay]", "KopfStellenName=Star News", "DailyPlaylistMode=1", "InFilesDirectory=C:\\Masterplay\\Masterplay OnAir PE2\\Playlist",
                                    "LogFilesDirectory=C:\\Masterplay\\Masterplay OnAir PE2\\Logfiles", "NamingAsRun=2", "AsRunLogIsoDateTime=0"}));
        QCOMPARE(m.installDir, QStringLiteral("C:\\Masterplay\\Masterplay OnAir PE2"));
        QCOMPARE(m.suggestedFps(), 30);
        s = ini::suggestSource(m, {"\\\\STATION-OBS\\Masterplay OnAir PE2"});
        QCOMPARE(ruleList(s.pathMap), QStringLiteral("C:\\Masterplay\\Masterplay OnAir PE2\\>\\\\STATION-OBS\\Masterplay OnAir PE2\\(iniLocation)"));
        QCOMPARE(s.source.value("id").toString(), QStringLiteral("masterplay-station-obs"));
        QCOMPARE(s.source.value("label").toString(), QStringLiteral("Star News"));
        QCOMPARE(s.source.value("playlistDir").toString(), QStringLiteral("\\\\STATION-OBS\\Masterplay OnAir PE2\\Playlist"));
        QCOMPARE(s.source.value("dailyPlaylistMode").toBool(), true);
        const QString notes = s.notes.join('\n');
        QVERIFY(notes.contains("NamingAsRun=2") && notes.contains("AsRunLogIsoDateTime=0") && notes.contains("DailyPlaylistMode=1"));
    }

    // Pasted or corrupt INIs reach POST /masterplay/config/from-ini, maybe on the Supervisor's event loop: no loop runs to
    // a number taken from the file, every list stops at 256 items with one MP011, and de-duplication is not quadratic.
    // (Before this, ItemCount=2000000000 meant 2e9 key lookups: minutes of CPU.)
    void boundedWork() {
        QElapsedTimer t;
        t.start();
        // ItemCount is no loop bound: decimal, $hex and 0x forms near INT_MAX; Items far below it still count.
        for (const char* count : {"2000000000", "$7FFFFFFF", "0x7fffffff"}) {
            const ini::MplayIni m = ini::readMplayIni(crlf({"[LastUsedOnAirPlaylists0]", QStringLiteral("ItemCount=%1").arg(QLatin1String(count)),
                                                            "Item0=a.in", "Item1=b.in", "Item2000000=c.in"}));
            QCOMPARE(m.recentPlaylists, (QStringList{"a.in", "b.in", "c.in"}));
        }
        // Item<n> as "Item%1" spells it (Item01, Item+1, "Item 1" are other names); the first of a repeated name wins;
        // without ItemCount the run stops at the first gap, an empty value keeps it going.
        ini::MplayIni m = ini::readMplayIni(crlf({"[LastUsedOnAirPlaylists0]", "Item0=a", "Item01=x", "Item+1=x", "Item 1=x", "item1=b", "ITEM1=dup",
                                                  "Item2=", "Item3=d", "Item5=f"}));
        QCOMPARE(m.recentPlaylists, (QStringList{"a", "b", "d"}));
        m = ini::readMplayIni(crlf({"[LastUsedOnAirPlaylists0]", "ItemCount=6", "Item0=a", "Item5=f", "Item6=g"}));
        QCOMPARE(m.recentPlaylists, (QStringList{"a", "f"}));       // with ItemCount: every Item below it, gaps allowed
        m = ini::readMplayIni(crlf({"[LastUsedOnAirPlaylists0]", "ItemCount=-3", "Item0=a"}));
        QVERIFY(m.recentPlaylists.isEmpty());

        // Long lists: a 100k-item SuchPfade, 1000 extensions / substitutes / exchange pairs, 300 recent playlists.
        QStringList paths, exts, subs, pairs;
        for (int i = 0; i < 100000; ++i) paths << QStringLiteral("D:\\Media\\p%1").arg(i);
        for (int i = 0; i < 1000; ++i) {
            exts << QStringLiteral("*.e%1").arg(i);
            subs << QStringLiteral("%s%1%=D:\\S%1").arg(i);
            pairs << QStringLiteral("X:\\E%1;D:\\P%1").arg(i);
        }
        QStringList lines{"[MPEGPlay]", "SuchPfade=" + paths.join(';'), "VideoFileExtensions=" + exts.join(';'), "SubstituteTable=" + subs.join(','),
                          "[PlayListEditor]", "ExchangeList=" + pairs.join('|'), "[LastUsedOnAirPlaylists0]", "ItemCount=2000000000"};
        for (int i = 0; i < 300; ++i) lines << QStringLiteral("Item%1=D:\\Masterplay\\Playlist\\r%1.in").arg(i);
        m = ini::readMplayIni(crlf(lines));
        QCOMPARE(m.searchPaths.size(), 256);
        QCOMPARE(m.searchPaths.last(), QStringLiteral("D:\\Media\\p255"));
        QCOMPARE(m.videoExtensions.size(), 256);
        QCOMPARE(m.substitutes.size(), 256);
        QCOMPARE(m.substitutes.last().name, QStringLiteral("s255"));
        QCOMPARE(m.exchangeList.size(), 256);
        QCOMPARE(m.exchangeList.last().playlist, QStringLiteral("D:\\P255"));
        QCOMPARE(m.recentPlaylists.size(), 256);
        // One MP011 per capped list, at its value: SuchPfade, VideoFileExtensions, SubstituteTable, ExchangeList, Item256.
        QCOMPARE(diagList(m.diagnostics), QStringLiteral("2:MP011:11 3:MP011:21 4:MP011:17 6:MP011:14 265:MP011:9"));
        const ini::SourceSuggestion s = ini::suggestSource(m, {"\\\\h\\Masterplay\\Mplay.INI"});
        QCOMPARE(s.pathMap.size(), 256);
        const QJsonObject j = s.toJson(m);
        QCOMPARE(j.value("searchPaths").toArray().size(), 256);
        QCOMPARE(j.value("source").toObject().value("recentPlaylists").toArray().size(), 256);

        // A [SearchDirs] section with 50k keys: 256 search paths, one MP011 at the first one dropped (Dir256).
        lines = QStringList{"[SearchDirs]"};
        for (int i = 0; i < 50000; ++i) lines << QStringLiteral("Dir%1=E:\\d%1").arg(i);
        m = ini::readMplayIni(crlf(lines));
        QCOMPARE(m.searchPaths.size(), 256);
        QCOMPARE(diagList(m.diagnostics), QStringLiteral("258:MP011:8"));

        // normalizeRules de-duplicates through a set: 20k rules, 5k distinct `from`s, the first of each kept.
        QList<ini::PathRule> many;
        for (int i = 0; i < 20000; ++i) many << ini::PathRule{QStringLiteral("D:\\R%1").arg(i % 5000), QStringLiteral("E:\\T%1").arg(i), "user", "", false};
        const QList<ini::PathRule> norm = ini::normalizeRules(many);
        QCOMPARE(norm.size(), 5000);
        QCOMPARE(ini::mapPath("d:\\r42\\x.mpg", norm), QStringLiteral("E:\\T42\\x.mpg"));

        QVERIFY2(t.elapsed() < 10000, qPrintable(QStringLiteral("took %1 ms").arg(t.elapsed())));
    }

    // ---- path rules ----------------------------------------------------------------------------------------------------

    void pathRules() {
        QCOMPARE(ini::normalizePath("D:"), QStringLiteral("D:\\"));
        QCOMPARE(ini::normalizePath("d:/a//b/"), QStringLiteral("d:\\a\\b\\"));
        QCOMPARE(ini::normalizePath("\\\\\\host\\\\s"), QStringLiteral("\\\\host\\s"));
        QCOMPARE(ini::normalizePath("//nas/S"), QStringLiteral("\\\\nas\\S"));
        QCOMPARE(ini::normalizePath(""), QString());

        const QList<ini::PathRule> rules = ini::normalizeRules({
            {"S:", "//nas/S", "user", "", false},
            {"C:\\Users\\Station\\Sync", "D:\\Sync\\", "user", "", false},
            {"s:\\", "\\\\other\\S", "user", "dup: the first one wins", false},
            {"D:\\Media", "", "user", "empty target: skipped", false},
            {"", "X:\\", "user", "no from: dropped", false}});
        QCOMPARE(ruleList(rules), QStringLiteral("C:\\Users\\Station\\Sync\\>D:\\Sync\\(user) D:\\Media\\>(user) S:\\>\\\\nas\\S\\(user)"));
        int used = -2;
        QCOMPARE(ini::mapPath("s:\\Shows\\a.mpg", rules, &used), QStringLiteral("\\\\nas\\S\\Shows\\a.mpg"));
        QCOMPARE(used, 2);
        QCOMPARE(ini::mapPath("c:/users/station/sync/x/y.mp4", rules), QStringLiteral("D:\\Sync\\x\\y.mp4"));
        QCOMPARE(ini::mapPath("C:\\Users\\Station\\Sync", rules), QStringLiteral("D:\\Sync"));
        QCOMPARE(ini::mapPath("C:\\Users\\Station\\Syncthing\\a", rules, &used), QStringLiteral("C:\\Users\\Station\\Syncthing\\a"));   // whole components
        QCOMPARE(used, -1);
        QCOMPARE(ini::mapPath("D:\\Media\\a.mpg", rules, &used), QStringLiteral("D:\\Media\\a.mpg"));   // empty target skipped
        QCOMPARE(used, -1);
        QCOMPARE(ini::mapPath("S:", rules), QStringLiteral("\\\\nas\\S\\"));               // "S:" is the root "S:\"
        QCOMPARE(ini::mapPath("a/b.mpg", rules), QStringLiteral("a/b.mpg"));                // unmatched: unchanged
        QCOMPARE(ini::mapPath("X:\\a", {{"X:\\", "Y:\\", "user", "", false}}), QStringLiteral("Y:\\a"));
        QCOMPARE(ini::mapPath("X:\\", {{"X:\\", "Y:", "user", "", false}}), QStringLiteral("Y:\\"));
        QCOMPARE(ini::mapPath("X:", {{"X:\\", "Y:\\", "user", "", false}}), QStringLiteral("Y:\\"));

        const ini::PathRule r{"D:\\", "\\\\h\\D$\\", "MasterPlayLocalDriveLetter", "n", true};
        const ini::PathRule back = ini::PathRule::fromJson(r.toJson());
        QCOMPARE(back.toJson(), r.toJson());
        QCOMPARE(ini::PathRule::fromJson(QJsonObject{{"from", "A:\\"}, {"to", "B:\\"}}).origin, QStringLiteral("user"));
        QVERIFY(!QJsonObject(ini::PathRule{"A:\\", "B:\\", "user", "", false}.toJson()).contains("guess"));
    }

    void modeNames() {
        QCOMPARE(ini::exchangeModeName(ini::ExchangeMode::Disabled), QStringLiteral("disabled"));
        QCOMPARE(ini::exchangeModeName(ini::ExchangeMode::Enabled), QStringLiteral("enabled"));
        QCOMPARE(ini::exchangeModeName(ini::ExchangeMode::Automatic), QStringLiteral("automatic"));
        QCOMPARE(ini::exchangeModeName(ini::ExchangeMode::Absent), QStringLiteral("absent"));
        for (int t : {-1, 1, 5, 6, 2}) QVERIFY(!osd::kindName(osd::kindOfType(t)).isEmpty());
        QCOMPARE(osd::kindName(osd::kindOfType(2)), QStringLiteral("other"));
    }
};

QTEST_GUILESS_MAIN(TestMpOsd)
#include "test_mposd.moc"
