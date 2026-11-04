#include "dtdevice.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>

#include <vector>

#ifdef HAS_DEKTEC
#define _NO_USING_NAMESPACE_DTAPI    // DTAPI.h otherwise ends in `using namespace Dtapi;` (its Dt* names next to ours)
#include <DTAPI.h>
#endif

namespace tskit::dt {

namespace {

struct IoNameRow { int code; const char* name; };
const IoNameRow kIoNames[] = {
    {kDisabled, "disabled"}, {kInput, "input"}, {kIntInput, "intinput"}, {kIntOutput, "intoutput"}, {kMonitor, "monitor"},
    {kOutput, "output"}, {kSharedAnt, "sharedant"}, {kDblBuf, "dblbuf"}, {kLoopS2L3, "loops2l3"}, {kLoopS2Ts, "loops2ts"},
    {kLoopThr, "loopthr"}, {k12gSdi, "12gsdi"}, {k3gSdi, "3gsdi"}, {k6gSdi, "6gsdi"}, {kAsi, "asi"}, {kAvEnc, "avenc"},
    {kDekTecSt, "dektecst"}, {kDemod, "demod"}, {kGpsTime, "gpstime"}, {kHdmi, "hdmi"}, {kHdSdi, "hdsdi"}, {kIfAdc, "ifadc"},
    {kIp, "ip"}, {kMod, "mod"}, {kPhaseNoise, "phasenoise"}, {kRs422, "rs422"}, {kSdiRx, "sdirx"}, {kSdi, "sdi"},
    {kSpi, "spi"}, {kSpiSdi, "spisdi"}};

// tsdektec's labels for the VPD items it prints, in its order.
struct VpdRow { const char* keyword; const char* label; };
const VpdRow kVpd[] = {{"CL", "Customer id"}, {"EC", "Engineering change level"}, {"MN", "Manufacture id"},
                       {"PD", "Production date"}, {"PN", "Part number"}, {"SN", "Serial number"},
                       {"XT", "Crystal stability"}, {"BO", "Bitrate offset"}};

QString noDeviceError(int index, qint64 serial, int count) {
    return serial > 0 ? QString("no DekTec device with serial %1 (%2 found)").arg(serial).arg(count)
                      : QString("no DekTec device #%1 (%2 found)").arg(index).arg(count);
}

QJsonObject deviceRef(const Device& d) {
    return {{"index", d.index}, {"model", d.model}, {"serial", QString::number(d.serial)}};
}

// Number channels per kind and put inputs first, as tsdektec lists them.
QList<Channel> orderChannels(const QList<Channel>& in) {
    QList<Channel> out;
    for (const char* kind : {"input", "output"}) {
        int n = 0;
        for (Channel c : in) if (c.kind == QLatin1String(kind)) { c.index = n++; out.append(c); }
    }
    return out;
}

// ---- backend without DTAPI ---------------------------------------------------------------------------------------------
class NoHardware final : public Hardware {
public:
    QString backend() const override { return QStringLiteral("none"); }
    bool available() const override { return false; }
    Versions versions() override { return {}; }
    QList<Device> scan(bool, QString*) override { return {}; }
    bool attach(int, qint64, Device*, QString* e) override { if (e) *e = "DekTec support (DTAPI) is not compiled in"; return false; }
    void detach() override {}
    bool readPort(int, PortConfig*, QString* e) override { if (e) *e = "no DTAPI"; return false; }
    Probe probe(int, bool, QString* d) override { if (d) *d = "no DTAPI"; return ProbeUnknown; }
    SetResult setIoConfig(int, int, int, int, QString* e) override { if (e) *e = "no DTAPI"; return SetFailed; }
};

// ---- DTAPI backend -----------------------------------------------------------------------------------------------------
#ifdef HAS_DEKTEC
static_assert(DTAPI_IOCONFIG_IODIR == kIoDir && DTAPI_IOCONFIG_IOSTD == kIoStd && DTAPI_IOCONFIG_DISABLED == kDisabled
              && DTAPI_IOCONFIG_INPUT == kInput && DTAPI_IOCONFIG_OUTPUT == kOutput && DTAPI_IOCONFIG_DBLBUF == kDblBuf
              && DTAPI_IOCONFIG_LOOPTHR == kLoopThr && DTAPI_IOCONFIG_ASI == kAsi && DTAPI_IOCONFIG_HDSDI == kHdSdi
              && DTAPI_IOCONFIG_3GSDI == k3gSdi && DTAPI_IOCONFIG_SDI == kSdi && DTAPI_IOCONFIG_SPISDI == kSpiSdi,
              "tskit::dt::IoCode must mirror DTAPI_IOCONFIG_*");

QString dtErr(Dtapi::DTAPI_RESULT r) { return QString::fromLatin1(Dtapi::DtapiResult2Str(r)); }

// "DTA-2154 port 1 (top)" -> "top"
QString labelOf(const QString& name) {
    static const QRegularExpression rx(QStringLiteral("\\(([^()]*)\\)\\s*$"));
    const auto m = rx.match(name);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

// The VPD id string is "<model> <description>" ("DTA-2154 Quad HD-SDI/ASI in+out"); tsdektec shows the description part.
QString descriptionOf(const QString& vpdId, const QString& model) {
    QString d = vpdId.trimmed();
    if (!model.isEmpty() && d.startsWith(model)) d = d.mid(model.size()).trimmed();
    return d.isEmpty() ? model : d;
}

QString hwfString(Dtapi::DtHwFuncDesc& f, int type) {
    char s[256] = {};
    return Dtapi::DtapiDtHwFuncDesc2String(&f, type, s, int(sizeof s) - 1) == DTAPI_OK ? QString::fromUtf8(s).trimmed() : QString();
}

QString dvcString(const Dtapi::DtDeviceDesc& d, int type) {
    char s[256] = {};
    return Dtapi::DtapiDtDeviceDesc2String(&d, type, s, int(sizeof s) - 1) == DTAPI_OK ? QString::fromUtf8(s).trimmed() : QString();
}

QString vpdItem(Dtapi::DtDevice& dev, const char* keyword) {
    char s[256] = {};                                    // DTAPI caps an item at 63 characters
    return dev.VpdRead(keyword, s) == DTAPI_OK ? QString::fromLatin1(s).trimmed() : QString();
}

QString versionString(int a, int b, int c, int d) { return QString("%1.%2.%3.%4").arg(a).arg(b).arg(c).arg(d); }

// tsdektec composes the port text itself (TSDuck 3.41, verified on this box's DTA-2154 against its DLL's string table):
// "<model> port <n>", " (top)" on port 1 of a multi-port card, then the stream types from the capability flags in this
// order. DTAPI's own HWF2STR strings differ by version (6.13: "DTA-2154 port 1" / "ASI/SDI"), so name / standards follow
// tsdektec (what MuxManager parses today); DTAPI's interface text is only reported as "interface".
QString tsdektecStandards(const Dtapi::DtCaps& f) {
    QStringList t;
    if (f.HasCaps(DTAPI_CAP_ASI)) t << "ASI";
    if (f.HasCaps(DTAPI_CAP_SDI)) t << "SDI";
    if (f.HasCaps(DTAPI_CAP_MOD)) t << "Modulator";
    if (f.HasCaps(DTAPI_CAP_IP)) t << "TS-over-IP";
    if (f.HasCaps(DTAPI_CAP_SPISDI)) t << "SPI_SDI";
    if (f.HasCaps(DTAPI_CAP_VIRTUAL)) t << "Virtual Stream";
    return t.join(", ");
}

QStringList capNames(const Dtapi::DtCaps& f) {
    static const struct { Dtapi::DtCaps cap; const char* name; } kCaps[] = {
        {DTAPI_CAP_DISABLED, "disabled"}, {DTAPI_CAP_INPUT, "input"}, {DTAPI_CAP_OUTPUT, "output"},
        {DTAPI_CAP_INTINPUT, "intinput"}, {DTAPI_CAP_INTOUTPUT, "intoutput"}, {DTAPI_CAP_MONITOR, "monitor"},
        {DTAPI_CAP_DBLBUF, "dblbuf"}, {DTAPI_CAP_LOOPTHR, "loopthr"}, {DTAPI_CAP_FAILSAFE, "failsafe"},
        {DTAPI_CAP_GENLOCKED, "genlocked"}, {DTAPI_CAP_GENREF, "genref"}, {DTAPI_CAP_AUTOBFGEN, "autobfgen"},
        {DTAPI_CAP_ASIPOL, "asipol"}, {DTAPI_CAP_MATRIX, "matrix"}, {DTAPI_CAP_MATRIX2, "matrix2"},
        {DTAPI_CAP_TS, "ts"}, {DTAPI_CAP_ASI, "asi"}, {DTAPI_CAP_SDI, "sdi"}, {DTAPI_CAP_HDSDI, "hdsdi"},
        {DTAPI_CAP_3GSDI, "3gsdi"}, {DTAPI_CAP_6GSDI, "6gsdi"}, {DTAPI_CAP_12GSDI, "12gsdi"}, {DTAPI_CAP_IP, "ip"},
        {DTAPI_CAP_MOD, "mod"}, {DTAPI_CAP_DEMOD, "demod"}, {DTAPI_CAP_HDMI, "hdmi"}};
    QStringList out;
    for (const auto& c : kCaps) if (f.HasCaps(c.cap)) out << QString::fromLatin1(c.name);
    return out;
}

bool readPortOn(Dtapi::DtDevice& dev, int port, PortConfig* c, QString* error) {
    *c = PortConfig();
    c->port = port;
    int v = -1, s = -1;
    const Dtapi::DTAPI_RESULT r = dev.GetIoConfig(port, DTAPI_IOCONFIG_IODIR, v, s);
    if (r != DTAPI_OK) { if (error) *error = QString("GetIoConfig(port %1, IODIR): %2").arg(port).arg(dtErr(r)); return false; }
    c->ioDir = v; c->ioDirSub = s;
    v = s = -1;
    if (dev.GetIoConfig(port, DTAPI_IOCONFIG_IOSTD, v, s) == DTAPI_OK) { c->ioStd = v; c->ioStdSub = s; }
    c->canInput = dev.HasCaps(port, DTAPI_CAP_INPUT);
    c->canOutput = dev.HasCaps(port, DTAPI_CAP_OUTPUT);
    c->canAsi = dev.HasCaps(port, DTAPI_CAP_ASI);
    c->canSdi = dev.HasCaps(port, DTAPI_CAP_SDI) || dev.HasCaps(port, DTAPI_CAP_HDSDI);
    return true;
}

// ProbeOnly: DTAPI checks the exclusive-access claim without attaching (nothing is held, nothing is configured).
Hardware::Probe probeOn(Dtapi::DtDevice& dev, int port, bool output, QString* detail) {
    Dtapi::DTAPI_RESULT r;
    if (output) {
        Dtapi::DtOutpChannel ch;
        r = ch.AttachToPort(&dev, port, /*ProbeOnly*/ true);
        if (ch.IsAttached()) ch.Detach(DTAPI_INSTANT_DETACH);
    } else {
        Dtapi::DtInpChannel ch;
        r = ch.AttachToPort(&dev, port, /*Exclusive*/ true, /*ProbeOnly*/ true);
        if (ch.IsAttached()) ch.Detach(DTAPI_INSTANT_DETACH);
    }
    if (r == DTAPI_OK || r == DTAPI_OK_FAILSAFE) return Hardware::ProbeFree;
    if (r == DTAPI_E_IN_USE) return Hardware::ProbeInUse;
    if (detail) *detail = QString("%1 channel probe: %2").arg(output ? "output" : "input", dtErr(r));
    return Hardware::ProbeUnknown;
}

bool scanDescs(std::vector<Dtapi::DtDeviceDesc>* devs, std::vector<Dtapi::DtHwFuncDesc>* funcs, QString* error) {
    int n = 0;
    devs->resize(16);
    Dtapi::DTAPI_RESULT r = Dtapi::DtapiDeviceScan(int(devs->size()), n, devs->data());
    if (r == DTAPI_E_BUF_TOO_SMALL && n > 0) { devs->resize(size_t(n)); r = Dtapi::DtapiDeviceScan(n, n, devs->data()); }
    if (r != DTAPI_OK) { devs->clear(); funcs->clear(); if (error) *error = "DtapiDeviceScan: " + dtErr(r); return false; }
    devs->resize(size_t(qMax(n, 0)));
    n = 0;
    funcs->resize(64);
    r = Dtapi::DtapiHwFuncScan(int(funcs->size()), n, funcs->data());
    if (r == DTAPI_E_BUF_TOO_SMALL && n > 0) { funcs->resize(size_t(n)); r = Dtapi::DtapiHwFuncScan(n, n, funcs->data()); }
    if (r != DTAPI_OK) { funcs->clear(); if (error) *error = "DtapiHwFuncScan: " + dtErr(r); return false; }   // devices without channels
    funcs->resize(size_t(qMax(n, 0)));
    return true;
}

Device describeDesc(int index, const Dtapi::DtDeviceDesc& d, std::vector<Dtapi::DtHwFuncDesc>& funcs, bool full, bool probe) {
    Device x;
    x.index = index;
    x.model = dvcString(d, DTAPI_DVC2STR_TYPE_NMB);
    if (x.model.isEmpty()) x.model = QString("DTx-%1").arg(d.m_TypeNumber);
    x.typeNumber = d.m_TypeNumber;
    x.serial = d.m_Serial;
    x.ports = d.m_NumPorts;
    x.firmware = d.m_FirmwareVersion;
    x.firmwareVariant = d.m_FirmwareVariant;
    x.hardwareRevision = d.m_HardwareRevision;
    if (d.m_Category == DTAPI_CAT_PCI) { x.pciBus = d.m_PciBusNumber; x.slot = d.m_SlotNumber; }
    x.description = x.model;
    if (!full) return x;

    // Shared (non-exclusive) device attach: VPD, I/O configuration and channel probes are reads.
    Dtapi::DtDevice dev;
    const bool attached = dev.AttachToSerial(d.m_Serial) == DTAPI_OK;
    if (attached) {
        x.description = descriptionOf(vpdItem(dev, "VPDID"), x.model);
        for (const VpdRow& v : kVpd) { const QString s = vpdItem(dev, v.keyword); if (!s.isEmpty()) x.vpd.append({QString::fromLatin1(v.keyword), s}); }
        for (int p = 1; p <= x.ports; ++p) { PortConfig c; if (readPortOn(dev, p, &c, nullptr)) x.portConfig.append(c); }
    }
    QList<Channel> chans;
    for (Dtapi::DtHwFuncDesc& f : funcs) {
        if (f.m_DvcDesc.m_Serial != d.m_Serial) continue;
        for (const char* kind : {"input", "output"}) {
            const bool out = kind[0] == 'o';
            if (!(f.m_ChanType & (out ? DTAPI_CHAN_OUTPUT : DTAPI_CHAN_INPUT))) continue;
            Channel c;
            c.kind = QString::fromLatin1(kind);
            c.port = f.m_Port;
            c.name = x.model + (f.m_Port > 0 ? QString(" port %1").arg(f.m_Port) : QString())
                   + (f.m_Port == 1 && d.m_NumPorts > 1 ? QStringLiteral(" (top)") : QString());
            c.label = labelOf(c.name);
            c.standards = tsdektecStandards(f.m_Flags);
            c.interfaceType = hwfString(f, DTAPI_HWF2STR_ITF_TYPE);
            c.caps = capNames(f.m_Flags);
            if (probe && attached) {
                QString detail;
                const Hardware::Probe pr = probeOn(dev, f.m_Port, out, &detail);
                c.inUse = pr == Hardware::ProbeInUse ? 1 : pr == Hardware::ProbeFree ? 0 : -1;
                c.probeError = detail;
            }
            chans.append(c);
        }
    }
    x.channels = orderChannels(chans);
    if (attached) dev.Detach();
    return x;
}

class DtapiHardware final : public Hardware {
public:
    ~DtapiHardware() override { detach(); }
    QString backend() const override { return QStringLiteral("dtapi"); }
    bool available() const override { return true; }

    Versions versions() override {
        Versions v;
        int a = 0, b = 0, c = 0, d = 0;
        if (Dtapi::DtapiGetVersion(a, b, c, d) == DTAPI_OK) v.dtapi = versionString(a, b, c, d);
        a = b = c = d = -1;
        if (Dtapi::DtapiGetDeviceDriverVersion(DTAPI_CAT_PCI, a, b, c, d) == DTAPI_OK && a >= 0) v.dta = versionString(a, b, c, d);
        a = b = c = d = -1;
        if (Dtapi::DtapiGetDeviceDriverVersion(DTAPI_CAT_NW, a, b, c, d) == DTAPI_OK && a >= 0) v.dtaNw = versionString(a, b, c, d);
        a = b = c = d = -1;
        if (Dtapi::DtapiGetDtapiServiceVersion(a, b, c, d) == DTAPI_OK && a >= 0) v.service = versionString(a, b, c, d);
        return v;
    }

    QList<Device> scan(bool probe, QString* error) override {
        std::vector<Dtapi::DtDeviceDesc> devs;
        std::vector<Dtapi::DtHwFuncDesc> funcs;
        scanDescs(&devs, &funcs, error);
        QList<Device> out;
        for (size_t i = 0; i < devs.size(); ++i) out.append(describeDesc(int(i), devs[i], funcs, true, probe));
        return out;
    }

    bool attach(int index, qint64 serial, Device* info, QString* error) override {
        detach();
        std::vector<Dtapi::DtDeviceDesc> devs;
        std::vector<Dtapi::DtHwFuncDesc> funcs;
        if (!scanDescs(&devs, &funcs, error) && devs.empty()) return false;
        int i = -1;
        if (serial > 0) { for (size_t k = 0; k < devs.size(); ++k) if (devs[k].m_Serial == serial) i = int(k); }
        else i = index;
        if (i < 0 || i >= int(devs.size())) { if (error) *error = noDeviceError(index, serial, int(devs.size())); return false; }
        *info = describeDesc(i, devs[size_t(i)], funcs, false, false);
        const Dtapi::DTAPI_RESULT r = m_dev.AttachToSerial(devs[size_t(i)].m_Serial);
        if (r != DTAPI_OK) { if (error) *error = QString("attach %1 #%2: %3").arg(info->model).arg(i).arg(dtErr(r)); return false; }
        return true;
    }

    void detach() override { if (m_dev.IsAttached()) m_dev.Detach(); }
    bool readPort(int port, PortConfig* cfg, QString* error) override { return readPortOn(m_dev, port, cfg, error); }
    Probe probe(int port, bool output, QString* detail) override { return probeOn(m_dev, port, output, detail); }

    SetResult setIoConfig(int port, int group, int value, int subValue, QString* error) override {
        const Dtapi::DTAPI_RESULT r = m_dev.SetIoConfig(port, group, value, subValue);
        if (r == DTAPI_OK) return SetOk;
        if (error) *error = QString("SetIoConfig(port %1, %2 = %3): %4").arg(port).arg(group == kIoDir ? "IODIR" : "IOSTD", ioName(value), dtErr(r));
        return r == DTAPI_E_ATTACHED || r == DTAPI_E_IN_USE ? SetAttached : SetFailed;
    }

private:
    Dtapi::DtDevice m_dev;
};
#endif   // HAS_DEKTEC

// ---- SimHardware helpers -------------------------------------------------------------------------------------------------
qint64 jsonSerial(const QJsonValue& v) { return v.isString() ? v.toString().toLongLong() : qint64(v.toDouble()); }

QStringList jsonStrings(const QJsonValue& v, const QStringList& def) {
    if (!v.isArray()) return def;
    QStringList out;
    for (const QJsonValue& x : v.toArray()) out << x.toString().toLower();
    return out;
}

PortConfig simPortConfig(const QJsonObject& p) {
    PortConfig c;
    c.port = p.value("port").toInt();
    c.ioDir = ioCode(p.value("ioDir").toString());
    c.ioDirSub = p.contains("ioDirSub") ? ioCode(p.value("ioDirSub").toString()) : c.ioDir;   // DTAPI: sub = value for a plain direction
    c.ioStd = ioCode(p.value("ioStd").toString());
    const QStringList caps = jsonStrings(p.value("caps"), {"input", "output", "asi", "sdi"});
    c.canInput = caps.contains("input");
    c.canOutput = caps.contains("output");
    c.canAsi = caps.contains("asi");
    c.canSdi = caps.contains("sdi") || caps.contains("hdsdi");
    return c;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------------
QString ioName(int code) {
    if (code < 0) return {};
    for (const IoNameRow& r : kIoNames) if (r.code == code) return QString::fromLatin1(r.name);
    return QString::number(code);
}

int ioCode(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n.isEmpty()) return -1;
    for (const IoNameRow& r : kIoNames) if (n == QLatin1String(r.name)) return r.code;
    bool ok = false;
    const int v = n.toInt(&ok);
    return ok ? v : -1;
}

QJsonObject PortConfig::toJson() const {
    QJsonObject o{{"port", port}, {"ioDir", ioName(ioDir)}, {"ioStd", ioName(ioStd)}, {"canInput", canInput},
                  {"canOutput", canOutput}, {"canAsi", canAsi}, {"canSdi", canSdi}};
    if (ioDirSub >= 0 && ioDirSub != ioDir) o["ioDirSub"] = ioName(ioDirSub);      // dblbuf, loopthr, ...
    if (ioStdSub >= 0) o["ioStdSub"] = ioStdSub;                                    // a video standard code (DTAPI_IOCONFIG_1080I59_94 ...)
    return o;
}

QJsonObject Channel::toJson() const {
    QJsonObject o{{"kind", kind}, {"index", index}, {"port", port}, {"name", name}, {"standards", standards}};
    if (!label.isEmpty()) o["label"] = label;
    if (!interfaceType.isEmpty()) o["interface"] = interfaceType;
    if (!caps.isEmpty()) o["caps"] = QJsonArray::fromStringList(caps);
    if (inUse >= 0) o["inUse"] = inUse == 1;
    if (!probeError.isEmpty()) o["probeError"] = probeError;
    return o;
}

int Device::inputs() const { int n = 0; for (const Channel& c : channels) n += c.kind == QLatin1String("input"); return n; }
int Device::outputs() const { int n = 0; for (const Channel& c : channels) n += c.kind == QLatin1String("output"); return n; }

QJsonObject Device::toJson() const {
    QJsonObject o{{"index", index}, {"model", model}, {"description", description}, {"typeNumber", typeNumber},
                  {"serial", QString::number(serial)}, {"ports", ports}, {"inputs", inputs()}, {"outputs", outputs()}};
    if (firmware >= 0) o["firmware"] = firmware;
    if (firmwareVariant >= 0) o["firmwareVariant"] = firmwareVariant;
    if (hardwareRevision > 0) o["hardwareRevision"] = hardwareRevision;
    if (pciBus >= 0) { o["pciBus"] = pciBus; o["slot"] = slot; }
    QJsonArray ch;
    for (const Channel& c : channels) ch.append(c.toJson());
    o["channels"] = ch;
    QJsonArray pc;
    for (const PortConfig& p : portConfig) pc.append(p.toJson());
    o["portConfig"] = pc;
    if (!vpd.isEmpty()) { QJsonObject v; for (const auto& kv : vpd) v[kv.first] = kv.second; o["vpd"] = v; }
    return o;
}

std::unique_ptr<Hardware> unavailableHardware() { return std::make_unique<NoHardware>(); }

std::unique_ptr<Hardware> openDtapi() {
#ifdef HAS_DEKTEC
    return std::make_unique<DtapiHardware>();
#else
    return unavailableHardware();
#endif
}

// ---- SimHardware ---------------------------------------------------------------------------------------------------------
SimHardware::SimHardware(const QJsonObject& state) : m_state(state) {}

Versions SimHardware::versions() {
    const QJsonObject v = m_state.value("versions").toObject();
    return {v.value("dtapi").toString("sim"), v.value("dta").toString(), v.value("dtaNw").toString(), v.value("service").toString()};
}

Device SimHardware::describe(int index, bool probe) const {
    const QJsonObject o = m_state.value("devices").toArray().at(index).toObject();
    Device d;
    d.index = index;
    d.model = o.value("model").toString("DTA-2154");
    d.description = o.value("description").toString(d.model);
    d.typeNumber = o.value("typeNumber").toInt(d.model.section('-', 1).toInt());
    d.serial = jsonSerial(o.value("serial"));
    d.firmware = o.value("firmware").toInt(-1);
    d.firmwareVariant = o.value("firmwareVariant").toInt(-1);
    d.pciBus = o.value("pciBus").toInt(-1);
    d.slot = o.value("slot").toInt(-1);
    const QJsonObject vpd = o.value("vpd").toObject();
    for (const VpdRow& v : kVpd) if (vpd.contains(v.keyword)) d.vpd.append({QString::fromLatin1(v.keyword), vpd.value(v.keyword).toString()});
    const QJsonArray ports = o.value("ports").toArray();
    d.ports = o.value("numPorts").toInt(int(ports.size()));
    QList<Channel> chans;
    for (const QJsonValue& v : ports) {
        const QJsonObject p = v.toObject();
        const PortConfig c = simPortConfig(p);
        if (!p.contains("readError")) d.portConfig.append(c);
        if (c.ioDir != kInput && c.ioDir != kOutput) continue;
        Channel ch;
        ch.kind = c.ioDir == kInput ? QStringLiteral("input") : QStringLiteral("output");
        ch.port = c.port;
        ch.label = p.value("label").toString();
        ch.name = QString("%1 port %2").arg(d.model).arg(c.port) + (ch.label.isEmpty() ? QString() : " (" + ch.label + ")");
        ch.standards = p.value("standards").toString("ASI, SDI");
        ch.caps = jsonStrings(p.value("caps"), {"input", "output", "asi", "sdi"});
        if (probe) {
            ch.probeError = p.value("probeError").toString();
            ch.inUse = !ch.probeError.isEmpty() ? -1 : p.value("inUse").toBool() ? 1 : 0;
        }
        chans.append(ch);
    }
    d.channels = orderChannels(chans);
    return d;
}

QList<Device> SimHardware::scan(bool probe, QString* error) {
    calls << QString("scan %1").arg(probe ? "probe" : "noprobe");
    if (m_state.contains("scanError")) { if (error) *error = m_state.value("scanError").toString(); return {}; }
    QList<Device> out;
    for (int i = 0; i < m_state.value("devices").toArray().size(); ++i) out.append(describe(i, probe));
    return out;
}

bool SimHardware::attach(int index, qint64 serial, Device* info, QString* error) {
    calls << QString("attach %1 %2").arg(index).arg(serial);
    const int count = int(m_state.value("devices").toArray().size());
    int i = -1;
    if (serial > 0) { for (int k = 0; k < count; ++k) if (describe(k, false).serial == serial) i = k; }
    else i = index;
    if (i < 0 || i >= count) { if (error) *error = noDeviceError(index, serial, count); return false; }
    m_attached = i;
    *info = describe(i, false);
    return true;
}

void SimHardware::detach() { if (m_attached >= 0) calls << "detach"; m_attached = -1; }

QJsonObject SimHardware::portObject(int device, int port) const {
    for (const QJsonValue& v : m_state.value("devices").toArray().at(device).toObject().value("ports").toArray())
        if (v.toObject().value("port").toInt() == port) return v.toObject();
    return {};
}

void SimHardware::setPortObject(int device, int port, const QJsonObject& p) {
    QJsonArray devs = m_state.value("devices").toArray();
    QJsonObject d = devs.at(device).toObject();
    QJsonArray ports = d.value("ports").toArray();
    for (int i = 0; i < ports.size(); ++i) if (ports.at(i).toObject().value("port").toInt() == port) ports[i] = p;
    d["ports"] = ports;
    devs[device] = d;
    m_state["devices"] = devs;
}

bool SimHardware::readPort(int port, PortConfig* cfg, QString* error) {
    calls << QString("read %1").arg(port);
    const QJsonObject p = m_attached >= 0 ? portObject(m_attached, port) : QJsonObject();
    if (p.isEmpty()) { if (error) *error = QString("GetIoConfig(port %1, IODIR): DTAPI_E_NO_SUCH_PORT").arg(port); return false; }
    if (p.contains("readError")) { if (error) *error = p.value("readError").toString(); return false; }
    *cfg = simPortConfig(p);
    return true;
}

Hardware::Probe SimHardware::probe(int port, bool output, QString* detail) {
    calls << QString("probe %1 %2").arg(port).arg(output ? "output" : "input");
    const QJsonObject p = m_attached >= 0 ? portObject(m_attached, port) : QJsonObject();
    if (p.contains("probeError")) { if (detail) *detail = p.value("probeError").toString(); return ProbeUnknown; }
    const int dir = ioCode(p.value("ioDir").toString());
    if (dir != (output ? kOutput : kInput)) {      // DTAPI: DTAPI_E_NO_DT_OUTPUT / _INPUT
        if (detail) *detail = QString("%1 channel probe: port is not an %1").arg(output ? "output" : "input");
        return ProbeUnknown;
    }
    return p.value("inUse").toBool() ? ProbeInUse : ProbeFree;
}

Hardware::SetResult SimHardware::setIoConfig(int port, int group, int value, int subValue, QString* error) {
    calls << QString("set %1 %2 %3 %4").arg(port).arg(group == kIoDir ? "iodir" : "iostd", ioName(value)).arg(subValue);
    QJsonObject p = m_attached >= 0 ? portObject(m_attached, port) : QJsonObject();
    const QString what = QString("SetIoConfig(port %1, %2 = %3)").arg(port).arg(group == kIoDir ? "IODIR" : "IOSTD", ioName(value));
    if (p.isEmpty()) { if (error) *error = what + ": DTAPI_E_NO_SUCH_PORT"; return SetFailed; }
    const QString forced = p.value("setResult").toString();
    if (forced == "attached" || p.value("inUse").toBool()) { if (error) *error = what + ": DTAPI_E_ATTACHED"; return SetAttached; }
    if (forced == "failed") { if (error) *error = what + ": DTAPI_E_DEV_DRIVER"; return SetFailed; }
    const PortConfig c = simPortConfig(p);
    if ((group == kIoDir && ((value == kOutput && !c.canOutput) || (value == kInput && !c.canInput))) || (group == kIoStd && value == kAsi && !c.canAsi)) {
        if (error) *error = what + ": DTAPI_E_NOT_SUPPORTED";
        return SetFailed;
    }
    if (p.value("stuck").toBool()) return SetOk;                    // "accepted", but the port keeps its old configuration
    if (group == kIoDir) {
        p["ioDir"] = ioName(value);
        if (subValue >= 0 && subValue != value) p["ioDirSub"] = ioName(subValue); else p.remove("ioDirSub");
    } else {
        p["ioStd"] = ioName(value);
    }
    setPortObject(m_attached, port, p);
    return SetOk;
}

std::unique_ptr<SimHardware> openSim(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = "cannot read DekTec simulation " + path; return nullptr; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) { if (error) *error = QString("DekTec simulation %1: %2").arg(path, pe.errorString()); return nullptr; }
    return std::make_unique<SimHardware>(doc.object());
}

bool saveSim(const SimHardware& sim, const QString& path, QString* error) {
    QSaveFile f(path);
    const QByteArray data = QJsonDocument(sim.state()).toJson(QJsonDocument::Indented);
    if (f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit()) return true;
    if (error) *error = "cannot write DekTec simulation " + path;
    return false;
}

// ---- list ------------------------------------------------------------------------------------------------------------------
QJsonObject listResult(Hardware& hw, bool probe, QList<Device>* devicesOut, Versions* versionsOut) {
    QJsonObject r{{"ok", true}, {"backend", hw.backend()}, {"available", hw.available()}, {"probed", probe}};
    QList<Device> devices;
    Versions v;
    if (!hw.available()) {
        r["note"] = "DekTec support (DTAPI) is not compiled into this build";
    } else {
        v = hw.versions();
        r["dtapi"] = v.dtapi;
        r["dta"] = v.dta;
        if (!v.dtaNw.isEmpty()) r["dtaNw"] = v.dtaNw;
        if (!v.service.isEmpty()) r["service"] = v.service;
        QString err;
        devices = hw.scan(probe, &err);
        if (!err.isEmpty()) r["scanError"] = err;
    }
    QJsonArray arr;
    for (const Device& d : devices) arr.append(d.toJson());
    r["devices"] = arr;
    r["count"] = int(arr.size());
    if (devicesOut) *devicesOut = devices;
    if (versionsOut) *versionsOut = v;
    return r;
}

QString listText(const QList<Device>& devices, const Versions& v, bool verbose) {
    QString t;
    if (!verbose) {
        for (const Device& d : devices) t += QString("%1: %2 (%3)\n").arg(d.index).arg(d.model, d.description);
        return t;
    }
    QStringList vs;
    if (!v.dtapi.isEmpty()) vs << "DTAPI: " + v.dtapi;
    if (!v.dta.isEmpty()) vs << "Dta: " + v.dta;
    if (!v.dtaNw.isEmpty()) vs << "DtaNw: " + v.dtaNw;
    if (!v.service.isEmpty()) vs << "Service: " + v.service;
    t += "\n" + vs.join(", ") + "\n";
    for (const Device& d : devices) {
        t += QString("\n* Device %1: %2 (%3)\n").arg(d.index).arg(d.model, d.description);
        t += QString("  Physical ports: %1\n").arg(d.ports);
        t += QString("  Channels: input: %1, output: %2\n").arg(d.inputs()).arg(d.outputs());
        for (const Channel& c : d.channels) {
            QString line = QString("  %1 %2: %3").arg(c.kind == QLatin1String("input") ? "Input" : "Output").arg(c.index).arg(c.name);
            if (!c.standards.isEmpty()) line += ", " + c.standards;
            if (!c.caps.isEmpty()) line += " (" + c.caps.join(", ") + ")";
            t += line + "\n";
        }
        // Current I/O configuration per port (not in tsdektec), with the probe result of the port's channel.
        for (const PortConfig& p : d.portConfig) {
            QString line = QString("  Port %1: %2").arg(p.port).arg(ioName(p.ioDir));
            if (p.ioDirSub >= 0 && p.ioDirSub != p.ioDir) line += " (" + ioName(p.ioDirSub) + ")";
            if (p.ioStd >= 0) line += ", " + ioName(p.ioStd);
            for (const Channel& c : d.channels) {
                if (c.port != p.port) continue;
                if (c.inUse == 1) line += ", in use"; else if (c.inUse == 0) line += ", free"; else if (!c.probeError.isEmpty()) line += ", unknown (" + c.probeError + ")";
            }
            t += line + "\n";
        }
        t += QString("  Serial number: %1\n").arg(QString::number(d.serial, 16).toUpper().rightJustified(16, '0'));
        if (d.firmware >= 0) t += QString("  Firmware version: %1 (0x%2)\n").arg(d.firmware).arg(QString::number(d.firmware, 16).toUpper().rightJustified(8, '0'));
        if (d.firmwareVariant >= 0) t += QString("  Firmware variant: %1 (0x%2)\n").arg(d.firmwareVariant).arg(QString::number(d.firmwareVariant, 16).toUpper().rightJustified(8, '0'));
        if (d.pciBus >= 0) t += QString("  PCI bus: %1, slot: %2\n").arg(d.pciBus).arg(d.slot);
        bool vpdSerial = false;
        for (const auto& kv : d.vpd) {
            QString label = kv.first;
            for (const VpdRow& r : kVpd) if (kv.first == QLatin1String(r.keyword)) label = QString::fromLatin1(r.label);
            vpdSerial = vpdSerial || kv.first == QLatin1String("SN");
            t += QString("  %1: %2\n").arg(label, kv.second);
        }
        if (!vpdSerial) t += QString("  Serial number: %1\n").arg(d.serial);    // the decimal form MuxManager reads
    }
    return t;
}

// ---- prep ------------------------------------------------------------------------------------------------------------------
QJsonObject prepare(Hardware& hw, const PrepRequest& req) {
    const QString dirName = req.output ? QStringLiteral("output") : QStringLiteral("input");
    QJsonObject r{{"ok", false}, {"backend", hw.backend()}, {"port", req.port}, {"direction", dirName}, {"check", req.check}};
    auto fail = [&r](const QString& e) { r["ok"] = false; r["error"] = e; return r; };
    if (!hw.available()) { r["unsupported"] = true; return fail("unsupported: DekTec support (DTAPI) is not compiled into this PlayPlayMux"); }

    Device dev;
    QString err;
    if (!hw.attach(req.device, req.serial, &dev, &err)) return fail(err);
    struct DetachLater { Hardware& h; ~DetachLater() { h.detach(); } } detachLater{hw};
    r["device"] = deviceRef(dev);
    const QString where = QString("port %1 of %2 #%3").arg(req.port).arg(dev.model).arg(dev.index);
    if (req.port < 1 || req.port > dev.ports) return fail(QString("%1 does not exist (ports 1..%2)").arg(where).arg(dev.ports));

    PortConfig before;
    if (!hw.readPort(req.port, &before, &err)) return fail(where + ": " + err);
    r["before"] = before.toJson();
    if (req.output ? !before.canOutput : !before.canInput) return fail(QString("%1 cannot be an %2").arg(where, dirName));
    const bool wantAsi = req.output && req.asi;
    if (wantAsi && !before.canAsi) return fail(where + " has no DVB-ASI mode");

    // Hands off a port whose channel (the one its current direction gives it) is attached by anyone, whatever was asked:
    // an engine's DekTec ASI sink or a running tsp may be transmitting on it.
    QString detail, chanKind;
    Hardware::Probe pr = Hardware::ProbeFree;                       // disabled / monitor / internal: no channel to own
    if (before.ioDir == kOutput) { chanKind = "output"; pr = hw.probe(req.port, true, &detail); }
    else if (before.ioDir == kInput) { chanKind = "input"; pr = hw.probe(req.port, false, &detail); }
    r["inUse"] = pr == Hardware::ProbeInUse ? QJsonValue(true) : pr == Hardware::ProbeFree ? QJsonValue(false) : QJsonValue(QJsonValue::Null);
    if (pr == Hardware::ProbeInUse) {
        r["refused"] = true;
        return fail(QString("%1 is in use: its %2 channel is attached by another process (an engine's DekTec output or a running mux); nothing changed").arg(where, chanKind));
    }
    if (pr == Hardware::ProbeUnknown) {
        r["refused"] = true;
        return fail(QString("cannot tell whether %1 is in use (%2); nothing changed").arg(where, detail));
    }

    const int target = req.output ? kOutput : kInput;
    const bool needDir = before.ioDir != target || (before.ioDirSub >= 0 && before.ioDirSub != target);
    const bool needStd = wantAsi && before.ioStd != kAsi;
    const bool doDir = needDir || req.force, doStd = needStd || (req.force && wantAsi);
    r["wouldChange"] = needDir || needStd;
    if (req.check) {
        QJsonArray planned;
        if (doDir) planned.append("iodir " + dirName);
        if (doStd) planned.append("iostd asi");
        r["ok"] = true; r["changed"] = false; r["actions"] = planned;
        return r;
    }

    QJsonArray applied;
    auto apply = [&](int group, int value, int sub, const QString& what) {
        const Hardware::SetResult s = hw.setIoConfig(req.port, group, value, sub, &err);
        if (s == Hardware::SetOk) { applied.append(what); return true; }
        r["actions"] = applied;
        if (s == Hardware::SetAttached) { r["inUse"] = true; r["refused"] = true; fail(QString("%1 is in use (a channel was attached meanwhile): %2").arg(where, err)); }
        else fail(where + ": " + err);
        return false;
    };
    if (doDir && !apply(kIoDir, target, target, "iodir " + dirName)) return r;     // subvalue = value: plain direction
    if (doStd && !apply(kIoStd, kAsi, -1, "iostd asi")) return r;
    r["actions"] = applied;
    PortConfig after;
    if (!hw.readPort(req.port, &after, &err)) return fail(where + ": read-back: " + err);
    r["after"] = after.toJson();
    r["changed"] = after.ioDir != before.ioDir || after.ioDirSub != before.ioDirSub || after.ioStd != before.ioStd;
    if (after.ioDir != target || (wantAsi && after.ioStd != kAsi))
        return fail(QString("%1 did not take the configuration (now %2, %3)").arg(where, ioName(after.ioDir), ioName(after.ioStd)));
    r["ok"] = true;
    return r;
}

}   // namespace tskit::dt
