#include "audiodevices.h"

#include <QJsonObject>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <functiondiscoverykeys_devpkey.h>
#  include <propvarutil.h>

namespace {
struct ComScope {
    bool ok = false;
    ComScope() { const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); ok = SUCCEEDED(hr); }   // S_FALSE = already initialised: still balanced
    ~ComScope() { if (ok) CoUninitialize(); }
};
template <class T> struct Rel { T* p = nullptr; ~Rel() { if (p) p->Release(); } T** operator&() { return &p; } T* operator->() { return p; } };

QString friendlyName(IMMDevice* dev) {
    Rel<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return QString();
    PROPVARIANT v; PropVariantInit(&v);
    QString name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = QString::fromWCharArray(v.pwszVal);
    PropVariantClear(&v);
    return name;
}
QString endpointId(IMMDevice* dev) {
    LPWSTR id = nullptr; QString s;
    if (SUCCEEDED(dev->GetId(&id)) && id) { s = QString::fromWCharArray(id); CoTaskMemFree(id); }
    return s;
}
} // namespace

QJsonArray audiodev::listJson() {
    QJsonArray out;
    ComScope com;
    Rel<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) return out;
    for (const EDataFlow flow : {eCapture, eRender}) {
        QString defId;
        { Rel<IMMDevice> d; if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) defId = endpointId(d.p); }
        Rel<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            Rel<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            QJsonObject o; const QString id = endpointId(d.p);
            o["id"] = id; o["name"] = friendlyName(d.p); o["kind"] = flow == eCapture ? "capture" : "render"; o["isDefault"] = (id == defId);
            out.append(o);
        }
    }
    return out;
}

QString audiodev::resolveId(const QString& device, bool loopback) {
    const QString want = device.trimmed();
    const QJsonArray all = listJson();
    const QString kind = loopback ? "render" : "capture";
    if (want.isEmpty() || want.compare("default", Qt::CaseInsensitive) == 0) {
        for (const QJsonValue& v : all) { const QJsonObject o = v.toObject(); if (o["kind"].toString() == kind && o["isDefault"].toBool()) return o["id"].toString(); }
        return QString();
    }
    for (const QJsonValue& v : all) if (v.toObject()["id"].toString() == want) return want;
    for (const QJsonValue& v : all) {
        const QJsonObject o = v.toObject();
        if (o["kind"].toString() == kind && o["name"].toString().contains(want, Qt::CaseInsensitive)) return o["id"].toString();
    }
    return QString();
}
#else
QJsonArray audiodev::listJson() { return QJsonArray(); }
QString audiodev::resolveId(const QString&, bool) { return QString(); }
#endif
