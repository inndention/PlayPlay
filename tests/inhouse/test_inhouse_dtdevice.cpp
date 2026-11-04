// Lane inhouse-mux, DEP-17 (docs/plans/inhouse-deps.md §6.5 stage 3): the DekTec device tool `PlayPlayMux dektec list|prep`
// (tools/mux/dtdevice.*), which replaces MuxManager's `tsdektec -a -v` (dektecDevices) and `tsdektec --output <port>
// <device>` (start).
//  - prep decisions on a simulated card (SimHardware): nothing re-applied that already matches, input -> ASI output, a port
//    whose channel is attached (or cannot be probed) refused with nothing changed, check mode, force, read-back check,
//    device / port / capability errors, "unsupported" without DTAPI (ppcli exit 64 -> tsdektec fallback);
//  - list JSON and text: the fields MuxManager parses out of `tsdektec -a -v` (its parser is copied below as the oracle and
//    checked on a recorded tsdektec listing of this box's DTA-2154), and no hardware = devices [] with exit 0;
//  - the CLI end to end with --sim / PLAYPLAYMUX_DEKTEC_SIM (prep persists the new state, a refusal leaves the file alone);
//  - with DTAPI and a card: `list` equals `tsdektec -a -v` (read-only), `prep --check` on input ports changes nothing.
//    Attaching a channel to prove the refusal on real hardware runs only with PLAYPLAY_DEKTEC_HWTEST=1 and only on a free
//    input port, asking for the direction it already has (=2 also re-applies it with --force: the real SetIoConfig path,
//    same value). Output ports (the mux's on-air port) are never prepared by this test.
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "dtdevice.h"
#include "ppcli.h"

#ifdef HAS_DEKTEC
#define _NO_USING_NAMESPACE_DTAPI
#include <DTAPI.h>
#endif

using namespace tskit::dt;

namespace {

QString dataFile(const QString& name) { return QStringLiteral(DTDEVICE_DATA_DIR) + "/" + name; }
QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool writeAll(const QString& path, const QByteArray& b) { QFile f(path); return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(b) == b.size(); }
QString lf(QString s) { s.remove('\r'); return s; }

// MuxManager::dektecDevices' parser of `tsdektec -a -v` (playout/supervisor/muxmanager.cpp, dektecDevices), copied verbatim:
// the shape the Supervisor and the UI read today, which `list` has to reproduce.
QJsonObject parseTsdektec(const QString& text) {
    QJsonArray devices;
    const QStringList lines = text.split('\n');
    static const QRegularExpression rxDev("^\\*?\\s*(?:Device )?(\\d+): (\\S+) \\((.*)\\)\\s*$"), rxPorts("^\\s+Physical ports: (\\d+)"), rxChans("^\\s+Channels: input: (\\d+), output: (\\d+)"),
        rxChan("^\\s+(Input|Output) (\\d+): \\S+ port (\\d+)(?: \\(([^)]*)\\))?, ([^(]*)"), rxSerial("^\\s+Serial number: (\\d+)\\s*$"), rxFw("^\\s+Firmware version: (\\d+)"), rxDtapi("^DTAPI: ([\\d.]+), Dta: ([\\d.]+)");
    QJsonObject cur; QJsonArray chans; QString dtapi, dta;
    auto flush = [&]() { if (!cur.isEmpty()) { cur["channels"] = chans; devices.append(cur); } cur = QJsonObject(); chans = QJsonArray(); };
    for (const QString& l0 : lines) {
        const QString l = QString(l0).remove('\r');
        if (auto m = rxDtapi.match(l); m.hasMatch()) { dtapi = m.captured(1); dta = m.captured(2); continue; }
        if (auto m = rxDev.match(l); m.hasMatch()) { flush(); cur["index"] = m.captured(1).toInt(); cur["model"] = m.captured(2); cur["description"] = m.captured(3); continue; }
        if (cur.isEmpty()) continue;
        if (auto m = rxPorts.match(l); m.hasMatch()) cur["ports"] = m.captured(1).toInt();
        else if (auto m = rxChans.match(l); m.hasMatch()) { cur["inputs"] = m.captured(1).toInt(); cur["outputs"] = m.captured(2).toInt(); }
        else if (auto m = rxChan.match(l); m.hasMatch()) { QJsonObject c; c["kind"] = m.captured(1).toLower(); c["index"] = m.captured(2).toInt(); c["port"] = m.captured(3).toInt(); if (!m.captured(4).isEmpty()) c["label"] = m.captured(4); c["standards"] = m.captured(5).trimmed().remove(QRegularExpression(",\\s*$")); chans.append(c); }
        else if (auto m = rxSerial.match(l); m.hasMatch()) cur["serial"] = m.captured(1);
        else if (auto m = rxFw.match(l); m.hasMatch()) cur["firmware"] = m.captured(1).toInt();
    }
    flush();
    return {{"devices", devices}, {"dtapi", dtapi}, {"dta", dta}};
}

// The fields MuxManager's parser yields, taken from a `list` device: what must be equal.
QJsonObject parsedFields(const QJsonObject& d) {
    QJsonObject o;
    for (const char* k : {"index", "model", "description", "ports", "inputs", "outputs", "serial", "firmware"}) o[k] = d.value(k);
    QJsonArray chans;
    for (const QJsonValue& v : d.value("channels").toArray()) {
        const QJsonObject c = v.toObject();
        QJsonObject x{{"kind", c.value("kind")}, {"index", c.value("index")}, {"port", c.value("port")}, {"standards", c.value("standards")}};
        if (c.contains("label")) x["label"] = c.value("label");
        chans.append(x);
    }
    o["channels"] = chans;
    return o;
}

QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

QJsonObject card() { return QJsonDocument::fromJson(readAll(dataFile("dta2154_sim.json"))).object(); }

// The card with `patch` merged into port `port` of device 0 (keys with a null value are removed).
QJsonObject cardWith(int port, const QJsonObject& patch) {
    QJsonObject c = card();
    QJsonArray devs = c.value("devices").toArray();
    QJsonObject d = devs.at(0).toObject();
    QJsonArray ports = d.value("ports").toArray();
    for (int i = 0; i < ports.size(); ++i) {
        QJsonObject p = ports.at(i).toObject();
        if (p.value("port").toInt() != port) continue;
        for (auto it = patch.begin(); it != patch.end(); ++it) { if (it.value().isNull()) p.remove(it.key()); else p[it.key()] = it.value(); }
        ports[i] = p;
    }
    d["ports"] = ports;
    devs[0] = d;
    c["devices"] = devs;
    return c;
}

QJsonObject simPort(const SimHardware& sim, int port) {
    for (const QJsonValue& v : sim.state().value("devices").toArray().at(0).toObject().value("ports").toArray())
        if (v.toObject().value("port").toInt() == port) return v.toObject();
    return {};
}

QStringList setCalls(const SimHardware& sim) {
    QStringList out;
    for (const QString& c : sim.calls) if (c.startsWith("set ")) out << c;
    return out;
}

struct Run { int code = -1; QJsonObject json; QString out, err; };

Run runMux(const QStringList& args, const QString& envSim = QString()) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("PLAYPLAYMUX_DEKTEC_SIM");
    if (!envSim.isEmpty()) env.insert("PLAYPLAYMUX_DEKTEC_SIM", envSim);
    p.setProcessEnvironment(env);
    p.start(QStringLiteral(PLAYPLAYMUX_EXE), args);
    Run r;
    if (!p.waitForFinished(60000)) { p.kill(); return r; }
    r.code = p.exitCode();
    const QByteArray out = p.readAllStandardOutput();
    r.out = QString::fromUtf8(out);
    r.err = QString::fromUtf8(p.readAllStandardError());
    r.json = ppcli::parseResult(out);
    return r;
}

QJsonObject deviceOf(const QJsonObject& listJson, int i = 0) { return listJson.value("devices").toArray().at(i).toObject(); }

}   // namespace

class TestDtDevice : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

    QString simFile(const QString& name, const QJsonObject& state) {
        const QString path = m_tmp.filePath(name);
        writeAll(path, QJsonDocument(state).toJson());
        return path;
    }

private slots:
    void ioCodes() {
        QCOMPARE(ioName(kOutput), QString("output"));
        QCOMPARE(ioName(kInput), QString("input"));
        QCOMPARE(ioName(kAsi), QString("asi"));
        QCOMPARE(ioName(kHdSdi), QString("hdsdi"));
        QCOMPARE(ioName(-1), QString());
        QCOMPARE(ioName(70), QString("70"));               // a video standard subvalue: left numeric
        QCOMPARE(ioCode("ASI"), int(kAsi));
        QCOMPARE(ioCode(" output "), int(kOutput));
        QCOMPARE(ioCode("dblbuf"), int(kDblBuf));
        QCOMPARE(ioCode("47"), 47);
        QCOMPARE(ioCode("sideways"), -1);
        QCOMPARE(ioCode(QString()), -1);
    }

    // The design's "without hardware: list -> [], exit 0" (no card, no DTAPI, or a scan that fails).
    void listWithoutHardware() {
        SimHardware none(QJsonObject{{"devices", QJsonArray()}});
        QJsonObject r = listResult(none, false);
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(r.value("devices").toArray().size(), 0);
        QCOMPARE(r.value("count").toInt(), 0);
        QCOMPARE(ppcli::exitCodeFor(r), 0);
        QVERIFY(listText({}, Versions(), false).isEmpty());

        auto unavailable = unavailableHardware();
        r = listResult(*unavailable, true);
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(r.value("available").toBool(), false);
        QCOMPARE(r.value("devices").toArray().size(), 0);
        QCOMPARE(r.value("backend").toString(), QString("none"));
        QCOMPARE(ppcli::exitCodeFor(r), 0);

        SimHardware broken(QJsonObject{{"scanError", "DtapiDeviceScan: DTAPI_E_DRIVER_INCOMP"}, {"devices", QJsonArray()}});
        r = listResult(broken, false);
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(r.value("devices").toArray().size(), 0);
        QVERIFY(r.value("scanError").toString().contains("DRIVER_INCOMP"));
    }

    void listJsonShape() {
        SimHardware sim(card());
        const QJsonObject r = listResult(sim, true);
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(r.value("dta").toString(), QString("4.28.14.293"));
        QCOMPARE(r.value("count").toInt(), 1);
        const QJsonObject d = deviceOf(r);
        QCOMPARE(d.value("index").toInt(), 0);
        QCOMPARE(d.value("model").toString(), QString("DTA-2154"));
        QCOMPARE(d.value("description").toString(), QString("Quad HD-SDI/ASI in+out"));
        QCOMPARE(d.value("serial").toString(), QString("2154000576"));           // a string, as MuxManager's parser gives it
        QCOMPARE(d.value("typeNumber").toInt(), 2154);
        QCOMPARE(d.value("ports").toInt(), 4);
        QCOMPARE(d.value("inputs").toInt(), 3);
        QCOMPARE(d.value("outputs").toInt(), 1);
        QCOMPARE(d.value("firmware").toInt(), 10);
        QCOMPARE(d.value("pciBus").toInt(), 3);
        const QJsonArray ch = d.value("channels").toArray();
        QCOMPARE(ch.size(), 4);
        // inputs first, numbered per kind, as tsdektec lists them
        const QList<QStringList> want = {{"input", "0", "1", "top"}, {"input", "1", "3", ""}, {"input", "2", "4", ""}, {"output", "0", "2", ""}};
        for (int i = 0; i < 4; ++i) {
            const QJsonObject c = ch.at(i).toObject();
            QCOMPARE(c.value("kind").toString(), want[i][0]);
            QCOMPARE(c.value("index").toInt(), want[i][1].toInt());
            QCOMPARE(c.value("port").toInt(), want[i][2].toInt());
            QCOMPARE(c.value("label").toString(), want[i][3]);
            QCOMPARE(c.contains("label"), !want[i][3].isEmpty());
            QCOMPARE(c.value("standards").toString(), QString("ASI, SDI"));
            QCOMPARE(c.value("inUse").toBool(), c.value("port").toInt() == 2);    // probed: port 2 carries the (simulated) mux
        }
        QCOMPARE(ch.at(0).toObject().value("name").toString(), QString("DTA-2154 port 1 (top)"));
        const QJsonArray pc = d.value("portConfig").toArray();
        QCOMPARE(pc.size(), 4);
        QCOMPARE(pc.at(1).toObject().value("ioDir").toString(), QString("output"));
        QCOMPARE(pc.at(1).toObject().value("ioStd").toString(), QString("asi"));
        QCOMPARE(pc.at(2).toObject().value("ioStd").toString(), QString("sdi"));
        QVERIFY(pc.at(2).toObject().value("canOutput").toBool());
        QCOMPARE(d.value("vpd").toObject().value("PD").toString(), QString("2015.03"));

        // Without --probe nothing says "inUse" and no probe call is made.
        SimHardware quiet(card());
        const QJsonObject q = listResult(quiet, false);
        for (const QJsonValue& v : deviceOf(q).value("channels").toArray()) QVERIFY(!v.toObject().contains("inUse"));
        QCOMPARE(quiet.calls, QStringList{"scan noprobe"});
    }

    // MuxManager's own parser reads the verbose text of `list` as it reads tsdektec, with the same result as the JSON.
    void listTextReadByMuxManagerParser() {
        SimHardware sim(card());
        QList<Device> devices;
        Versions versions;
        const QJsonObject r = listResult(sim, true, &devices, &versions);
        const QString text = listText(devices, versions, true);
        const QJsonObject parsed = parseTsdektec(text);
        QCOMPARE(parsed.value("devices").toArray().size(), 1);
        QCOMPARE(compact(parsed.value("devices").toArray().at(0).toObject()), compact(parsedFields(deviceOf(r))));
        QCOMPARE(parsed.value("dta").toString(), QString("4.28.14.293"));
        QVERIFY(text.contains("\n* Device 0: DTA-2154 (Quad HD-SDI/ASI in+out)\n"));
        QVERIFY(text.contains("  Input 0: DTA-2154 port 1 (top), ASI, SDI (input, output, asi, sdi, hdsdi)\n"));
        QVERIFY(text.contains("  Port 2: output, asi, in use\n"));
        QVERIFY(text.contains("  Port 3: input, sdi, free\n"));
        QVERIFY(text.contains("  Serial number: 00000000806370C0\n"));          // tsdektec's hex line, then the VPD one
        QVERIFY(text.contains("  Production date: 2015.03\n"));
        // tsdektec -a (non-verbose) recorded on this box: identical line
        QCOMPARE(listText(devices, versions, false), lf(QString::fromUtf8(readAll(dataFile("tsdektec_a.txt")))));
    }

    // The oracle itself, on `tsdektec -a -v` as TSDuck 3.41 printed it for this box's DTA-2154 (recorded once).
    void tsdektecFixtureParses() {
        const QString text = QString::fromUtf8(readAll(dataFile("tsdektec_av.txt")));
        QVERIFY(!text.isEmpty());
        const QJsonObject p = parseTsdektec(text);
        QCOMPARE(p.value("dtapi").toString(), QString("6.9.0.237"));
        QCOMPARE(p.value("dta").toString(), QString("4.28.14.293"));
        const QJsonArray devs = p.value("devices").toArray();
        QCOMPARE(devs.size(), 1);
        const QJsonObject d = devs.at(0).toObject();
        QCOMPARE(d.value("model").toString(), QString("DTA-2154"));
        QCOMPARE(d.value("description").toString(), QString("Quad HD-SDI/ASI in+out"));
        QCOMPARE(d.value("ports").toInt(), 8);
        QCOMPARE(d.value("inputs").toInt(), 3);
        QCOMPARE(d.value("outputs").toInt(), 1);
        QCOMPARE(d.value("serial").toString(), QString("2154000576"));
        QCOMPARE(d.value("firmware").toInt(), 10);
        const QJsonArray ch = d.value("channels").toArray();
        QCOMPARE(ch.size(), 4);
        QCOMPARE(compact(ch.at(0).toObject()), compact(QJsonObject{{"kind", "input"}, {"index", 0}, {"port", 1}, {"label", "top"}, {"standards", "ASI, SDI"}}));
        QCOMPARE(compact(ch.at(3).toObject()), compact(QJsonObject{{"kind", "output"}, {"index", 0}, {"port", 2}, {"standards", "ASI, SDI"}}));
    }

    void prepDecisions_data() {
        QTest::addColumn<int>("patchPort");
        QTest::addColumn<QString>("patch");            // JSON merged into that port of the card
        QTest::addColumn<int>("port");
        QTest::addColumn<bool>("output");
        QTest::addColumn<bool>("asi");
        QTest::addColumn<bool>("check");
        QTest::addColumn<bool>("force");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<bool>("refused");
        QTest::addColumn<bool>("changed");
        QTest::addColumn<QStringList>("sets");         // the SetIoConfig calls made, in order
        QTest::addColumn<QString>("error");            // substring of the error ("" when ok)
        QTest::addColumn<QString>("after");            // "ioDir/ioStd" of the port afterwards in the simulated card

        const QString free2 = R"({"inUse": false})";
        QTest::newRow("output already asi: nothing to do") << 2 << free2 << 2 << true << true << false << false
            << true << false << false << QStringList{} << "" << "output/asi";
        QTest::newRow("input sdi -> asi output") << 0 << "{}" << 3 << true << true << false << false
            << true << false << true << QStringList{"set 3 iodir output 25", "set 3 iostd asi -1"} << "" << "output/asi";
        QTest::newRow("keep the standard") << 0 << "{}" << 3 << true << false << false << false
            << true << false << true << QStringList{"set 3 iodir output 25"} << "" << "output/sdi";
        QTest::newRow("output in use: refused") << 0 << "{}" << 2 << true << true << false << false
            << false << true << false << QStringList{} << "is in use: its output channel is attached" << "output/asi";
        QTest::newRow("in use: refused even forced") << 0 << "{}" << 2 << true << true << false << true
            << false << true << false << QStringList{} << "is in use" << "output/asi";
        QTest::newRow("in use: refused in check mode") << 0 << "{}" << 2 << true << true << true << false
            << false << true << false << QStringList{} << "is in use" << "output/asi";
        QTest::newRow("input in use: refused") << 3 << R"({"inUse": true})" << 3 << true << true << false << false
            << false << true << false << QStringList{} << "its input channel is attached" << "input/sdi";
        QTest::newRow("probe fails: refused") << 3 << R"({"probeError": "input channel probe: DTAPI_E_DEVICE"})" << 3 << true << true << false << false
            << false << true << false << QStringList{} << "cannot tell whether port 3" << "input/sdi";
        QTest::newRow("check: plan only") << 0 << "{}" << 3 << true << true << true << false
            << true << false << false << QStringList{} << "" << "input/sdi";
        QTest::newRow("force re-applies, nothing changes") << 2 << free2 << 2 << true << true << false << true
            << true << false << false << QStringList{"set 2 iodir output 25", "set 2 iostd asi -1"} << "" << "output/asi";
        QTest::newRow("to input (tsdektec --input)") << 2 << free2 << 2 << false << true << false << false
            << true << false << true << QStringList{"set 2 iodir input 21"} << "" << "input/asi";
        QTest::newRow("double-buffered copy -> plain output") << 2 << R"({"inUse": false, "ioDirSub": "dblbuf"})" << 2 << true << true << false << false
            << true << false << true << QStringList{"set 2 iodir output 25"} << "" << "output/asi";
        QTest::newRow("disabled port: no channel to probe") << 4 << R"({"ioDir": "disabled"})" << 4 << true << true << false << false
            << true << false << true << QStringList{"set 4 iodir output 25"} << "" << "output/asi";
        QTest::newRow("attached meanwhile (SetIoConfig: ATTACHED)") << 3 << R"({"setResult": "attached"})" << 3 << true << true << false << false
            << false << true << false << QStringList{"set 3 iodir output 25"} << "attached meanwhile" << "input/sdi";
        QTest::newRow("driver failure") << 3 << R"({"setResult": "failed"})" << 3 << true << true << false << false
            << false << false << false << QStringList{"set 3 iodir output 25"} << "DTAPI_E_DEV_DRIVER" << "input/sdi";
        QTest::newRow("read-back does not change") << 3 << R"({"stuck": true})" << 3 << true << true << false << false
            << false << false << false << QStringList{"set 3 iodir output 25", "set 3 iostd asi -1"} << "did not take the configuration" << "input/sdi";
        QTest::newRow("port 0") << 0 << "{}" << 0 << true << true << false << false
            << false << false << false << QStringList{} << "port 0 of DTA-2154 #0 does not exist (ports 1..4)" << "";
        QTest::newRow("port 9") << 0 << "{}" << 9 << true << true << false << false
            << false << false << false << QStringList{} << "does not exist" << "";
        QTest::newRow("input-only port") << 3 << R"({"caps": ["input", "asi"]})" << 3 << true << true << false << false
            << false << false << false << QStringList{} << "cannot be an output" << "input/sdi";
        QTest::newRow("no ASI mode") << 3 << R"({"caps": ["input", "output", "sdi"]})" << 3 << true << true << false << false
            << false << false << false << QStringList{} << "has no DVB-ASI mode" << "input/sdi";
        QTest::newRow("no ASI mode, standard kept") << 3 << R"({"caps": ["input", "output", "sdi"]})" << 3 << true << false << false << false
            << true << false << true << QStringList{"set 3 iodir output 25"} << "" << "output/sdi";
        QTest::newRow("unreadable port") << 3 << R"({"readError": "GetIoConfig(port 3, IODIR): DTAPI_E_DEVICE"})" << 3 << true << true << false << false
            << false << false << false << QStringList{} << "DTAPI_E_DEVICE" << "input/sdi";
    }

    void prepDecisions() {
        QFETCH(int, patchPort); QFETCH(QString, patch); QFETCH(int, port); QFETCH(bool, output); QFETCH(bool, asi);
        QFETCH(bool, check); QFETCH(bool, force); QFETCH(bool, ok); QFETCH(bool, refused); QFETCH(bool, changed);
        QFETCH(QStringList, sets); QFETCH(QString, error); QFETCH(QString, after);
        SimHardware sim(patchPort ? cardWith(patchPort, QJsonDocument::fromJson(patch.toUtf8()).object()) : card());
        PrepRequest req;
        req.port = port; req.output = output; req.asi = asi; req.check = check; req.force = force;
        const QJsonObject r = prepare(sim, req);
        QVERIFY2(r.value("ok").toBool() == ok, qPrintable(compact(r)));
        QCOMPARE(r.value("refused").toBool(), refused);
        QCOMPARE(r.value("changed").toBool(), changed);
        QCOMPARE(setCalls(sim), sets);
        if (error.isEmpty()) QVERIFY2(!r.contains("error"), qPrintable(compact(r)));
        else QVERIFY2(r.value("error").toString().contains(error), qPrintable(r.value("error").toString()));
        QCOMPARE(ppcli::exitCodeFor(r), ok ? 0 : 2);
        if (!after.isEmpty()) {
            const QJsonObject p = simPort(sim, port);
            QCOMPARE(p.value("ioDir").toString() + "/" + p.value("ioStd").toString(), after);
        }
        QCOMPARE(sim.calls.last(), QString("detach"));                    // the device is always let go
        QCOMPARE(r.value("device").toObject().value("serial").toString(), QString("2154000576"));
        if (refused) { QVERIFY(r.contains("inUse")); QVERIFY(!r.contains("after")); }
        if (ok && !check) QCOMPARE(r.value("after").toObject().value("port").toInt(), port);
        if (check) QCOMPARE(r.value("wouldChange").toBool(), port == 3);
    }

    void prepCallOrderAndReport() {
        SimHardware sim(card());
        PrepRequest req;
        req.port = 3;
        const QJsonObject r = prepare(sim, req);
        QCOMPARE(sim.calls, (QStringList{"attach 0 0", "read 3", "probe 3 input", "set 3 iodir output 25", "set 3 iostd asi -1", "read 3", "detach"}));
        QCOMPARE(r.value("actions").toArray(), (QJsonArray{"iodir output", "iostd asi"}));
        QCOMPARE(r.value("before").toObject().value("ioDir").toString(), QString("input"));
        QCOMPARE(r.value("after").toObject().value("ioDir").toString(), QString("output"));
        QCOMPARE(r.value("inUse").toBool(), false);
        QCOMPARE(r.value("direction").toString(), QString("output"));
        QCOMPARE(r.value("backend").toString(), QString("sim"));

        // check mode plans the same two steps and touches nothing
        SimHardware sim2(card());
        req.check = true;
        const QJsonObject c = prepare(sim2, req);
        QCOMPARE(c.value("actions").toArray(), (QJsonArray{"iodir output", "iostd asi"}));
        QCOMPARE(c.value("wouldChange").toBool(), true);
        QCOMPARE(sim2.state(), card());
    }

    void prepDeviceSelection() {
        SimHardware sim(cardWith(2, QJsonObject{{"inUse", false}}));
        PrepRequest req;
        req.port = 2;
        req.device = 3;
        QJsonObject r = prepare(sim, req);
        QCOMPARE(r.value("error").toString(), QString("no DekTec device #3 (1 found)"));
        req.device = 7;                                  // ignored when a serial is given
        req.serial = 2154000576LL;
        r = prepare(sim, req);
        QVERIFY2(r.value("ok").toBool(), qPrintable(compact(r)));
        QCOMPARE(r.value("device").toObject().value("index").toInt(), 0);
        req.serial = 1;
        r = prepare(sim, req);
        QCOMPARE(r.value("error").toString(), QString("no DekTec device with serial 1 (1 found)"));
    }

    void prepWithoutDtapiIsUnsupported() {
        auto hw = unavailableHardware();
        PrepRequest req;
        req.port = 2;
        const QJsonObject r = prepare(*hw, req);
        QCOMPARE(r.value("ok").toBool(), false);
        QCOMPARE(r.value("unsupported").toBool(), true);
        QVERIFY(r.value("error").toString().startsWith("unsupported: "));
        QCOMPARE(ppcli::exitCodeFor(r), 64);              // MuxManager then runs tsdektec as today
    }

    // ---- the CLI, on simulated cards -------------------------------------------------------------------------------------
    void cliListEmpty() {
        const QString f = simFile("empty.json", QJsonObject{{"devices", QJsonArray()}});
        const Run r = runMux({"dektec", "list", "--sim", f});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("ok").toBool(), true);
        QCOMPARE(r.json.value("devices").toArray(), QJsonArray());
        QCOMPARE(r.out.trimmed().count('\n'), 0);         // nothing but the JSON line
        QCOMPARE(r.json.value("simulated").toString(), f);
    }

    void cliListVerbose() {
        const QString f = simFile("list.json", card());
        const Run r = runMux({"dektec", "list", "-v", "--probe", "--sim", f});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("count").toInt(), 1);
        QVERIFY(r.out.contains("* Device 0: DTA-2154 (Quad HD-SDI/ASI in+out)"));
        QVERIFY(r.out.contains("  Port 2: output, asi, in use"));
        const QJsonObject parsed = parseTsdektec(r.out);
        QCOMPARE(compact(parsed.value("devices").toArray().at(0).toObject()), compact(parsedFields(deviceOf(r.json))));
        const Run q = runMux({"dektec", "list", "--quiet", "--sim", f});
        QCOMPARE(q.code, 0);
        QCOMPARE(q.out.trimmed().count('\n'), 0);
    }

    void cliPrepRefusesAttachedPort() {
        const QString f = simFile("refuse.json", card());
        const QByteArray before = readAll(f);
        const Run r = runMux({"dektec", "prep", "--port", "2", "--device", "0", "--sim", f});
        QCOMPARE(r.code, 2);
        QCOMPARE(r.json.value("inUse").toBool(), true);
        QCOMPARE(r.json.value("refused").toBool(), true);
        QVERIFY(r.err.contains("is in use"));
        QCOMPARE(readAll(f), before);                     // untouched
        // tsdektec's own spelling of the same request
        const Run t = runMux({"dektec", "prep", "--output", "2", "0", "--sim", f});
        QCOMPARE(t.code, 2);
        QCOMPARE(readAll(f), before);
    }

    void cliPrepChangesAndPersists() {
        const QString f = simFile("change.json", card());
        const QByteArray before = readAll(f);
        const Run c = runMux({"dektec", "prep", "--output", "3", "--check", "--sim", f});
        QCOMPARE(c.code, 0);
        QCOMPARE(c.json.value("wouldChange").toBool(), true);
        QCOMPARE(readAll(f), before);
        const Run r = runMux({"dektec", "prep", "--output", "3", "0", "--sim", f});   // tsdektec --output 3 0
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("changed").toBool(), true);
        QCOMPARE(r.json.value("after").toObject().value("ioStd").toString(), QString("asi"));
        const Run l = runMux({"dektec", "list", "--sim", f});
        QCOMPARE(deviceOf(l.json).value("outputs").toInt(), 2);
        QCOMPARE(deviceOf(l.json).value("portConfig").toArray().at(2).toObject().value("ioDir").toString(), QString("output"));
        const Run again = runMux({"dektec", "prep", "--port", "3", "--sim", f});
        QCOMPARE(again.code, 0);
        QCOMPARE(again.json.value("changed").toBool(), false);
        QCOMPARE(again.json.value("actions").toArray(), QJsonArray());
        const Run back = runMux({"dektec", "prep", "--input", "3", "--serial", "2154000576", "--sim", f});
        QCOMPARE(back.code, 0);
        QCOMPARE(back.json.value("after").toObject().value("ioDir").toString(), QString("input"));
    }

    void cliEnvironmentSim() {
        const QString f = simFile("env.json", card());
        const Run r = runMux({"dektec", "list"}, f);
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("simulated").toString(), f);
        QCOMPARE(r.json.value("backend").toString(), QString("sim"));
    }

    void cliArguments() {
        const QString f = simFile("args.json", card());
        QCOMPARE(runMux({"dektec", "frob"}).code, 64);
        QCOMPARE(runMux({"dektec", "list", "--bogus", "--sim", f}).code, 64);
        QCOMPARE(runMux({"dektec", "prep", "--sim", f}).code, 2);                                       // no port
        QCOMPARE(runMux({"dektec", "prep", "--port", "x", "--sim", f}).code, 2);
        QCOMPARE(runMux({"dektec", "prep", "--port", "2", "--direction", "sideways", "--sim", f}).code, 64);
        QCOMPARE(runMux({"dektec", "prep", "--port", "2", "--io-standard", "sdi", "--sim", f}).code, 64);
        QCOMPARE(runMux({"dektec", "prep", "--port", "2", "--output", "3", "--sim", f}).code, 2);       // two ports
        QCOMPARE(runMux({"dektec", "prep", "--input", "3", "--direction", "output", "--sim", f}).code, 2);
        QCOMPARE(runMux({"dektec", "prep", "--port", "3", "--device", "0", "1", "--sim", f}).code, 2);  // device twice
        QCOMPARE(runMux({"dektec", "prep", "--port", "3", "--sim", m_tmp.filePath("missing.json")}).code, 2);
        const Run v = runMux({"version"});
        QCOMPARE(v.code, 0);
        QVERIFY(v.json.value("commands").toArray().contains("dektec"));
#ifdef HAS_DEKTEC
        QCOMPARE(v.json.value("dtapi").toBool(), true);
#else
        QCOMPARE(v.json.value("dtapi").toBool(), false);
        QCOMPARE(runMux({"dektec", "prep", "--port", "2"}).code, 64);
        const Run l = runMux({"dektec", "list"});
        QCOMPARE(l.code, 0);
        QCOMPARE(l.json.value("devices").toArray(), QJsonArray());
#endif
    }

    // ---- real hardware (DTAPI) -------------------------------------------------------------------------------------------
    // Read-only: DtapiDeviceScan / HwFuncScan, a shared device attach for VPD and GetIoConfig: what `tsdektec -a -v` does.
    void hwListMatchesTsdektec() {
#ifndef HAS_DEKTEC
        QSKIP("built without DTAPI");
#else
        const Run r = runMux({"dektec", "list", "-v"});
        QCOMPARE(r.code, 0);
        if (r.json.value("count").toInt() == 0) QSKIP("no DekTec device in this machine");
        const QString tsdektec = "C:/Program Files/TSDuck/bin/tsdektec.exe";
        if (!QFile::exists(tsdektec)) QSKIP("TSDuck tsdektec.exe not installed: nothing to compare with");
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(tsdektec, {"-a", "-v"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        const QJsonObject theirs = parseTsdektec(QString::fromUtf8(p.readAll()));
        const QJsonArray devs = theirs.value("devices").toArray();
        QCOMPARE(devs.size(), r.json.value("count").toInt());
        for (int i = 0; i < devs.size(); ++i) {
            const QJsonObject want = devs.at(i).toObject();
            QCOMPARE(compact(parsedFields(deviceOf(r.json, i))), compact(want));
            // and MuxManager's parser reads our text exactly as it reads tsdektec's
            QCOMPARE(compact(parseTsdektec(r.out).value("devices").toArray().at(i).toObject()), compact(want));
        }
        QCOMPARE(r.json.value("dta").toString(), theirs.value("dta").toString());
        qInfo().noquote() << "DTAPI: PlayPlayMux" << r.json.value("dtapi").toString() << "/ tsdektec" << theirs.value("dtapi").toString()
                          << "; Dta" << r.json.value("dta").toString();
#endif
    }

    // prep --check on every input port: probes and reads, never writes. Output ports (the mux's) are not touched.
    void hwPrepCheckIsReadOnly() {
#ifndef HAS_DEKTEC
        QSKIP("built without DTAPI");
#else
        const Run before = runMux({"dektec", "list", "--probe"});
        QCOMPARE(before.code, 0);
        if (before.json.value("count").toInt() == 0) QSKIP("no DekTec device in this machine");
        const QJsonObject d = deviceOf(before.json);
        int checked = 0;
        for (const QJsonValue& v : d.value("channels").toArray()) {
            const QJsonObject c = v.toObject();
            if (c.value("kind").toString() != "input") continue;
            const QString port = QString::number(c.value("port").toInt());
            const Run same = runMux({"dektec", "prep", "--port", port, "--direction", "input", "--check"});
            const Run out = runMux({"dektec", "prep", "--port", port, "--direction", "output", "--check"});
            if (c.value("inUse").toBool()) {                 // someone receives on it: both refused
                QCOMPARE(same.code, 2); QCOMPARE(same.json.value("inUse").toBool(), true);
                QCOMPARE(out.code, 2);
            } else {
                QVERIFY2(same.code == 0, qPrintable(compact(same.json)));
                QCOMPARE(same.json.value("check").toBool(), true);
                QCOMPARE(same.json.value("inUse").toBool(), false);
                QVERIFY2(out.code == 0, qPrintable(compact(out.json)));
                QCOMPARE(out.json.value("wouldChange").toBool(), true);
            }
            ++checked;
        }
        const Run after = runMux({"dektec", "list", "--probe"});
        QCOMPARE(compact(QJsonObject{{"p", deviceOf(after.json).value("portConfig")}}), compact(QJsonObject{{"p", d.value("portConfig")}}));
        qInfo() << "prep --check on" << checked << "input port(s)";
#endif
    }

    // Opt-in (PLAYPLAY_DEKTEC_HWTEST=1): hold a free input port's channel in this process and watch `prep` refuse it, asking for
    // the direction the port already has (so even a broken refusal would change nothing). =2 then also re-applies that same
    // direction with --force once the port is free again (the real SetIoConfig path; the configuration stays identical).
    void hwPrepRefusesAttachedPort() {
#ifndef HAS_DEKTEC
        QSKIP("built without DTAPI");
#else
        const int level = qEnvironmentVariableIntValue("PLAYPLAY_DEKTEC_HWTEST");
        if (level < 1) QSKIP("set PLAYPLAY_DEKTEC_HWTEST=1 to attach a free input port of the card (never an output port)");
        const Run list = runMux({"dektec", "list", "--probe"});
        if (list.json.value("count").toInt() == 0) QSKIP("no DekTec device in this machine");
        const QJsonObject d = deviceOf(list.json);
        // A free input port, preferably one without a special role (genlock reference input, double-buffered output copy).
        int port = 0, plain = 0;
        for (const QJsonValue& v : d.value("channels").toArray()) {
            const QJsonObject c = v.toObject();
            if (c.value("kind").toString() != "input" || !c.contains("inUse") || c.value("inUse").toBool()) continue;
            const QJsonArray caps = c.value("caps").toArray();
            if (!port) port = c.value("port").toInt();
            if (!plain && !caps.contains("genref") && !caps.contains("dblbuf")) plain = c.value("port").toInt();
        }
        if (plain) port = plain;
        if (!port) QSKIP("no free input port on device 0");
        const QString ps = QString::number(port);
        struct Hold {
            Dtapi::DtDevice dev; Dtapi::DtInpChannel ch;
            ~Hold() { if (ch.IsAttached()) ch.Detach(DTAPI_INSTANT_DETACH); if (dev.IsAttached()) dev.Detach(); }
        };
        {
            Hold h;
            QCOMPARE(int(h.dev.AttachToSerial(d.value("serial").toString().toLongLong())), DTAPI_OK);
            QCOMPARE(int(h.ch.AttachToPort(&h.dev, port, /*Exclusive*/ true, /*ProbeOnly*/ false)), DTAPI_OK);
            const Run r = runMux({"dektec", "prep", "--port", ps, "--direction", "input"});
            QCOMPARE(r.code, 2);
            QCOMPARE(r.json.value("inUse").toBool(), true);
            QCOMPARE(r.json.value("refused").toBool(), true);
            QVERIFY(r.json.value("error").toString().contains("its input channel is attached"));
            const Run c = runMux({"dektec", "prep", "--port", ps, "--direction", "output", "--check"});
            QCOMPARE(c.code, 2);
            const Run l = runMux({"dektec", "list", "--probe"});
            for (const QJsonValue& v : deviceOf(l.json).value("channels").toArray())
                if (v.toObject().value("port").toInt() == port) QCOMPARE(v.toObject().value("inUse").toBool(), true);
            qInfo().noquote() << "held input port" << port << "->" << r.json.value("error").toString();
        }
        const Run free = runMux({"dektec", "prep", "--port", ps, "--direction", "input"});
        QVERIFY2(free.code == 0, qPrintable(compact(free.json)));
        QCOMPARE(free.json.value("changed").toBool(), false);
        QCOMPARE(free.json.value("actions").toArray(), QJsonArray());
        if (level >= 2) {
            const Run f = runMux({"dektec", "prep", "--port", ps, "--direction", "input", "--force"});
            QVERIFY2(f.code == 0, qPrintable(compact(f.json)));
            QCOMPARE(f.json.value("actions").toArray(), QJsonArray{"iodir input"});
            QCOMPARE(f.json.value("changed").toBool(), false);
            QCOMPARE(f.json.value("after").toObject().value("ioDir").toString(), QString("input"));
        }
        const Run after = runMux({"dektec", "list"});
        QCOMPARE(compact(QJsonObject{{"p", deviceOf(after.json).value("portConfig")}}), compact(QJsonObject{{"p", d.value("portConfig")}}));
#endif
    }
};

QTEST_GUILESS_MAIN(TestDtDevice)
#include "test_inhouse_dtdevice.moc"
