#include "scte35.h"

#include <QtGlobal>
#include <cmath>

namespace scte35 {

namespace {
QStringList lowerList(const QJsonValue& v) {
    QStringList out;
    if (v.isString()) { for (const QString& s : v.toString().split(',', Qt::SkipEmptyParts)) if (!s.trimmed().isEmpty()) out << s.trimmed().toLower(); }
    else for (const QJsonValue& x : v.toArray()) { const QString s = x.toVariant().toString().trimmed().toLower(); if (!s.isEmpty()) out << s; }
    return out;
}
const char* b(bool v) { return v ? "true" : "false"; }
int parsePid(const QJsonValue& v) {
    if (v.isDouble()) return v.toInt();
    const QString s = v.toString().trimmed();
    bool ok = false; const int n = s.startsWith("0x", Qt::CaseInsensitive) ? s.mid(2).toInt(&ok, 16) : s.toInt(&ok);
    return ok ? n : 0;
}
} // namespace

QJsonObject defaults() {
    QJsonObject d;
    d["enabled"] = false; d["autoBreaks"] = true; d["cueIn"] = true; d["autoReturn"] = true;
    d["breakTypes"] = QJsonArray{"ad", "promo", "psa"}; d["minBreakSec"] = 5; d["offsetMs"] = 0;
    d["basePort"] = 5310; d["injectCount"] = 2; d["heartbeatSec"] = 0;
    return d;
}

QJsonObject normalize(const QJsonObject& in) {
    QJsonObject d = defaults();
    for (auto it = in.begin(); it != in.end(); ++it) if (d.contains(it.key())) d[it.key()] = it.value();
    for (const char* k : {"enabled", "autoBreaks", "cueIn", "autoReturn"}) d[k] = d.value(k).toBool(defaults().value(k).toBool());
    QJsonArray bt; for (const QString& s : lowerList(d.value("breakTypes"))) bt.append(s);
    d["breakTypes"] = bt;
    d["minBreakSec"] = qBound(0.0, d.value("minBreakSec").toDouble(5), 600.0);
    d["offsetMs"] = qBound(0, d.value("offsetMs").toInt(0), 10000);
    d["basePort"] = qBound(1024, d.value("basePort").toInt(5310), 65000);
    d["injectCount"] = qBound(1, d.value("injectCount").toInt(2), 10);
    d["heartbeatSec"] = qBound(0, d.value("heartbeatSec").toInt(0), 3600);
    return d;
}

static QString wrap(const QString& body) {
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <splice_information_table protocol_version=\"0\" pts_adjustment=\"0\" tier=\"0xFFF\">\n")
         + body + QStringLiteral("  </splice_information_table>\n</tsduck>\n");
}

QString spliceInsertXml(const Splice& s) {
    QString x = QString("    <splice_insert splice_event_id=\"%1\" splice_event_cancel=\"false\" out_of_network=\"%2\" splice_immediate=\"true\" unique_program_id=\"%3\" avail_num=\"%4\" avails_expected=\"%5\"")
                    .arg(s.eventId).arg(b(s.outOfNetwork)).arg(qBound(0, s.uniqueProgramId, 65535)).arg(qBound(0, s.availNum, 255)).arg(qBound(0, s.availsExpected, 255));
    if (s.durationSec > 0) {
        const qint64 ticks = qint64(std::llround(s.durationSec * 90000.0)) & 0x1FFFFFFFFLL;       // 33-bit, 90 kHz
        x += QString(">\n      <break_duration auto_return=\"%1\" duration=\"%2\"/>\n    </splice_insert>\n").arg(b(s.autoReturn)).arg(ticks);
    } else x += "/>\n";
    return wrap(x);
}

QString spliceCancelXml(quint32 eventId) { return wrap(QString("    <splice_insert splice_event_id=\"%1\" splice_event_cancel=\"true\"/>\n").arg(eventId)); }
QString spliceNullXml() { return wrap(QStringLiteral("    <splice_null/>\n")); }

int pidFor(const QJsonObject& svc, int index) {
    const int p = parsePid(svc.value("scte35").toObject().value("pid"));
    return p >= 0x20 && p < 0x1FFF ? p : 0x0400 + qMax(0, index - 1);
}

bool serviceEnabled(const QJsonObject& cfg, const QJsonObject& svc) {
    return cfg.value("enabled").toBool(false) && svc.value("scte35").toObject().value("enabled").toBool(true);
}

int portFor(const QJsonObject& cfg, int index) { return cfg.value("basePort").toInt(5310) + qMax(0, index - 1); }

QStringList tspPluginArgs(const QJsonObject& cfg, const QJsonArray& services) {
    QStringList a;
    if (!cfg.value("enabled").toBool(false)) return a;
    for (int i = 0; i < services.size(); ++i) {
        const QJsonObject s = services[i].toObject();
        if (!serviceEnabled(cfg, s)) continue;
        const QString sid = QString::number(s.value("serviceId").toInt());
        const QString pid = QString("0x%1").arg(pidFor(s, i + 1), 4, 16, QChar('0'));
        a << "-P" << "pmt" << "--service" << sid << "--add-pid" << pid + "/0x86" << "--add-registration" << "0x43554549";
        a << "-P" << "spliceinject" << "--service" << sid << "--pid" << pid << "--udp" << QString("127.0.0.1:%1").arg(portFor(cfg, i + 1))
          << "--inject-count" << QString::number(cfg.value("injectCount").toInt(2));
    }
    return a;
}

bool isBreakType(const QJsonObject& cfg, const QString& type) { return !type.isEmpty() && lowerList(cfg.value("breakTypes")).contains(type.toLower()); }

double breakSecondsFrom(const QJsonObject& rl, const QString& runlistId, const QStringList& breakTypes) {
    QJsonArray all;
    for (const char* k : {"current", "next"}) for (const QJsonValue& v : rl.value(k).toObject().value("items").toArray()) all.append(v);
    double total = 0.0; bool found = false;
    for (const QJsonValue& v : all) {
        const QJsonObject it = v.toObject();
        if (!found) { if (it.value("id").toString() != runlistId) continue; found = true; total += it.value("duration").toDouble(); continue; }
        if (!breakTypes.contains(it.value("type").toString().toLower())) break;
        total += it.value("duration").toDouble();
    }
    return found ? total : 0.0;
}

} // namespace scte35
