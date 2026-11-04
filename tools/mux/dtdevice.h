#ifndef PP_DTDEVICE_H
#define PP_DTDEVICE_H

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <memory>

/**
 * PlayPlayMux part 6 (docs/plans/inhouse-deps.md §6.5 stage 3, DEP-17): DekTec devices through DTAPI, replacing the two
 * tsdektec calls of MuxManager:
 *  - `tsdektec -a -v` (dektecDevices): `list` gives the same device / channel fields MuxManager parses from that text
 *    (index, model, description, ports, inputs, outputs, serial, firmware, channels {kind, index, port, label, standards},
 *    dtapi, dta) plus each port's current I/O direction and standard and, with `probe`, whether a channel is attached;
 *  - `tsdektec --output <port> <device>` (start): `prepare` makes the mux port an output (and DVB-ASI), the calls
 *    DekTecAsiSink::openHardware makes (SetIoConfig IODIR / IOSTD).
 *
 * prepare() is stricter than tsdektec, which always re-applies the direction: it changes nothing that already matches,
 * and it refuses a port whose channel is attached by another process (an engine's DekTec ASI sink or a running tsp may own
 * it). DTAPI itself refuses SetIoConfig on an attached port, but the probe also refuses when nothing needs changing, so a
 * mux start reports "port in use" up front instead of tsp failing on the attach later.
 *
 * The hardware sits behind an interface: the DTAPI backend (only where the SDK is compiled in, PLAYPLAY_HAVE_DEKTEC; else
 * a backend that reports itself unavailable: `list` answers no devices, `prep` is "unsupported" so callers fall back to
 * tsdektec), and SimHardware, a card described in JSON for the unit tests and for `PlayPlayMux dektec --sim F` /
 * PLAYPLAYMUX_DEKTEC_SIM (a test Supervisor can then run the real tool without ever touching the on-air card).
 * Not part of PlayPlayTsKit (that library is QtCore only); static library PlayPlayDtDevice.
 */
namespace tskit::dt {

/// DTAPI I/O configuration codes (DTAPI.h DTAPI_IOCONFIG_*; dtdevice.cpp static_asserts them when DTAPI is compiled in).
enum IoCode : int {
    kIoDir = 0, kIoStd = 1,                                        // groups
    kDisabled = 20, kInput = 21, kIntInput = 22, kIntOutput = 23, kMonitor = 24, kOutput = 25,   // IODIR values
    kSharedAnt = 26, kDblBuf = 27, kLoopS2L3 = 28, kLoopS2Ts = 29, kLoopThr = 30,                 // IODIR subvalues
    k12gSdi = 31, k3gSdi = 32, k6gSdi = 33, kAsi = 34, kAvEnc = 35, kDekTecSt = 36, kDemod = 37, // IOSTD values
    kGpsTime = 38, kHdmi = 39, kHdSdi = 40, kIfAdc = 41, kIp = 42, kMod = 43, kPhaseNoise = 44, kRs422 = 45,
    kSdiRx = 46, kSdi = 47, kSpi = 48, kSpiSdi = 49
};
/// "output", "asi", "hdsdi", ...; "" for -1 (unknown); the decimal number for anything else.
QString ioName(int code);
/// Inverse of ioName for the codes it names; -1 when unknown.
int ioCode(const QString& name);

struct PortConfig {
    int port = 0;                                  // physical port, 1-based (tsdektec "port N")
    int ioDir = -1, ioDirSub = -1;                 // IODIR value / subvalue, -1 = unknown
    int ioStd = -1, ioStdSub = -1;                 // IOSTD value / subvalue
    bool canInput = false, canOutput = false, canAsi = false, canSdi = false;
    QJsonObject toJson() const;
};

struct Channel {
    QString kind;                                  // "input" | "output"
    int index = 0;                                 // per kind, in DtapiHwFuncScan order ("Input 0", "Output 0")
    int port = 0;
    QString name;                                  // as tsdektec prints it: "DTA-2154 port 1 (top)"
    QString label;                                 // its parenthesised tail ("top"), else empty
    QString standards;                             // tsdektec's stream types from the caps: "ASI, SDI"
    QString interfaceType;                         // DTAPI's own interface text (DTAPI 6.13: "ASI/SDI")
    QStringList caps;                              // short capability names: input, output, dblbuf, asi, sdi, ...
    int inUse = -1;                                // list --probe: 1 attached by some process, 0 free, -1 not probed
    QString probeError;
    QJsonObject toJson() const;
};

struct Device {
    int index = 0;                                 // DtapiDeviceScan order = tsdektec's device number
    QString model;                                 // "DTA-2154"
    QString description;                           // VPD identification ("Quad HD-SDI/ASI in+out"); model if none
    int typeNumber = 0;
    qint64 serial = 0;
    int ports = 0;                                 // physical ports (DtDeviceDesc::m_NumPorts)
    int firmware = -1, firmwareVariant = -1, hardwareRevision = -1, pciBus = -1, slot = -1;
    QList<Channel> channels;                       // inputs first, then outputs (tsdektec's order)
    QList<PortConfig> portConfig;                  // ports whose I/O configuration could be read
    QList<QPair<QString, QString>> vpd;            // VPD items tsdektec prints: (keyword "CL" / "EC" / "PD" / "PN" / "SN" ..., value)
    int inputs() const;
    int outputs() const;
    QJsonObject toJson() const;
};

struct Versions { QString dtapi, dta, dtaNw, service; };

class Hardware {
public:
    enum Probe { ProbeUnknown = -1, ProbeFree = 0, ProbeInUse = 1 };
    enum SetResult { SetOk, SetAttached, SetFailed };
    virtual ~Hardware() = default;
    virtual QString backend() const = 0;           // "dtapi" | "sim" | "none"
    virtual bool available() const = 0;            // false: built without DTAPI
    virtual Versions versions() = 0;
    /// Every device; `probe` also asks whether each channel is attached (ProbeOnly attach, nothing is held).
    virtual QList<Device> scan(bool probe, QString* error) = 0;
    // prepare() primitives on one attached device (by scan index, or by serial when serial > 0)
    virtual bool attach(int index, qint64 serial, Device* info, QString* error) = 0;
    virtual void detach() = 0;
    virtual bool readPort(int port, PortConfig* cfg, QString* error) = 0;
    /// Is the port's input (output = false) or output channel attached by anyone? Never attaches it.
    virtual Probe probe(int port, bool output, QString* detail) = 0;
    virtual SetResult setIoConfig(int port, int group, int value, int subValue, QString* error) = 0;
};

/// The DTAPI backend; without the SDK compiled in, the same object as unavailableHardware().
std::unique_ptr<Hardware> openDtapi();
/// available() == false: what a build without DTAPI has.
std::unique_ptr<Hardware> unavailableHardware();

/**
 * A card described in JSON (tests, test Supervisors):
 *   {"versions": {"dtapi": "sim", "dta": "4.28.14.293"}, "scanError": "...",
 *    "devices": [{"model": "DTA-2154", "description": "...", "typeNumber": 2154, "serial": 2154000576, "firmware": 10,
 *                 "ports": [{"port": 1, "ioDir": "input", "ioStd": "asi", "caps": ["input","output","asi","sdi"],
 *                            "label": "top", "standards": "ASI, SDI", "inUse": false}, ...]}]}
 * Channels follow the ports' current direction (as DtapiHwFuncScan does after SetIoConfig). Fault injection per port:
 * "probeError" (probe answers unknown), "setResult": "attached" | "failed", "stuck": true (SetIoConfig "succeeds" but
 * the read-back never changes), "readError". setIoConfig() updates the state; `calls` records every hardware call.
 */
class SimHardware final : public Hardware {
public:
    explicit SimHardware(const QJsonObject& state);
    QJsonObject state() const { return m_state; }
    QStringList calls;

    QString backend() const override { return QStringLiteral("sim"); }
    bool available() const override { return true; }
    Versions versions() override;
    QList<Device> scan(bool probe, QString* error) override;
    bool attach(int index, qint64 serial, Device* info, QString* error) override;
    void detach() override;
    bool readPort(int port, PortConfig* cfg, QString* error) override;
    Probe probe(int port, bool output, QString* detail) override;
    SetResult setIoConfig(int port, int group, int value, int subValue, QString* error) override;

private:
    Device describe(int index, bool probe) const;
    QJsonObject portObject(int device, int port) const;
    void setPortObject(int device, int port, const QJsonObject& p);
    QJsonObject m_state;
    int m_attached = -1;
};

/// SimHardware from a JSON file (nullptr + error when unreadable); saveSim writes the state back atomically.
std::unique_ptr<SimHardware> openSim(const QString& path, QString* error);
bool saveSim(const SimHardware& sim, const QString& path, QString* error);

/// `dektec list`: {"ok": true, "devices": [...], "count", "backend", "dtapi", "dta", ["dtaNw", "service"]}. No hardware or
/// no DTAPI is ok with devices [] ("available": false without DTAPI); a failed DtapiDeviceScan is ok too, with "scanError"
/// (a missing driver means no usable card, not a broken tool).
QJsonObject listResult(Hardware& hw, bool probe, QList<Device>* devices = nullptr, Versions* versions = nullptr);
/// Text for stdout before the JSON line: tsdektec -a ("0: DTA-2154 (Quad HD-SDI/ASI in+out)") or, verbose, the layout of
/// tsdektec -a -v (versions line, "* Device", "Physical ports", "Channels", "Input 0: <name>, <standards>", ...) plus one
/// "Port N: <direction>, <standard>" line per port; MuxManager's tsdektec parser reads it unchanged.
QString listText(const QList<Device>& devices, const Versions& versions, bool verbose);

struct PrepRequest {
    int device = 0;                                // DtapiDeviceScan index (tsdektec's device number)
    qint64 serial = 0;                             // > 0: the device with this serial instead
    int port = 0;                                  // physical port, 1-based
    bool output = true;                            // target direction (false = input, tsdektec --input)
    bool asi = true;                               // output only: also make the I/O standard DVB-ASI (the mux sends ASI)
    bool check = false;                            // report what would change; change nothing
    bool force = false;                            // re-apply even what already matches (what tsdektec always does)
};

/**
 * `dektec prep`: {"ok", "device" {index, model, serial}, "port", "direction", "before" / "after" (PortConfig), "actions"
 * (applied, or planned with check), "changed", "wouldChange", "inUse", "check"} or ok:false with "error"; a port whose
 * channel is attached (or whose state cannot be probed) is refused with "refused": true and nothing changed. Without DTAPI:
 * {"ok": false, "unsupported": true} (ppcli exit 64: the caller runs tsdektec instead).
 */
QJsonObject prepare(Hardware& hw, const PrepRequest& req);

}   // namespace tskit::dt

#endif // PP_DTDEVICE_H
