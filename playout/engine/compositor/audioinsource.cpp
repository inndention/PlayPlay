#include "audioinsource.h"
#include "../../core/audiodevices.h"

#include <QDebug>
#include <chrono>
#include <vector>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#  include <functiondiscoverykeys_devpkey.h>
#  include <ks.h>
#  include <ksmedia.h>
#endif

AudioInSource::AudioInSource(const QJsonObject& spec, QObject* parent) : LiveSourceBase(parent), m_spec(spec) {
    m_wantAudio = true;
}

AudioInSource::~AudioInSource() { close(); }

bool AudioInSource::open(const OutputFormat& fmt) {
#ifdef Q_OS_WIN
    if (m_open) return true;
    m_fmt = fmt;
    resetAudio();
    m_stop = false; m_initState = 0;
    m_thread = std::thread([this]() { captureLoop(); });
    std::unique_lock<std::mutex> lk(m_initMutex);
    if (!m_initCv.wait_for(lk, std::chrono::milliseconds(3000), [this]() { return m_initState != 0; })) {
        lk.unlock(); m_stop = true; if (m_thread.joinable()) m_thread.join();
        setError("audio device did not start in time"); return false;
    }
    if (m_initState < 0) { lk.unlock(); if (m_thread.joinable()) m_thread.join(); return false; }
    m_open = true;
    return true;
#else
    Q_UNUSED(fmt); setError("audio inputs need Windows (WASAPI)"); return false;
#endif
}

void AudioInSource::close() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_open = false; setConnected(false);
}

void AudioInSource::finishInit(bool ok, const QString& err) {
    { std::lock_guard<std::mutex> lk(m_initMutex); m_initState = ok ? 1 : -1; if (!ok) setError(err); }
    m_initCv.notify_all();
}

QJsonObject AudioInSource::status() const {
    QJsonObject o = LiveSourceBase::status();
    o["device"] = m_deviceName; o["rate"] = m_devRate.load(); o["channels"] = m_devChannels.load(); o["blocks"] = double(m_blocks.load());
    return o;
}

#ifdef Q_OS_WIN
namespace {
template <class T> struct Rel { T* p = nullptr; ~Rel() { if (p) p->Release(); } T** operator&() { return &p; } T* operator->() { return p; } };
}

void AudioInSource::captureLoop() {
    const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct Guard { bool ok; ~Guard() { if (ok) CoUninitialize(); } } guard{comOk};
    const bool loopback = m_spec.value("loopback").toBool(false);
    const QString id = audiodev::resolveId(m_spec.value("device").toString(), loopback);
    if (id.isEmpty()) { finishInit(false, "audio device not found: " + m_spec.value("device").toString("default")); return; }

    Rel<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) { finishInit(false, "no audio device enumerator"); return; }
    Rel<IMMDevice> dev;
    if (FAILED(en->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &dev))) { finishInit(false, "audio device unavailable"); return; }
    {
        Rel<IPropertyStore> props; PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) m_deviceName = QString::fromWCharArray(v.pwszVal);
        PropVariantClear(&v);
    }
    Rel<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) { finishInit(false, "audio client failed"); return; }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) { finishInit(false, "no mix format"); return; }
    const int ch = mix->nChannels, rate = int(mix->nSamplesPerSec), bits = mix->wBitsPerSample;
    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const int frameBytes = mix->nBlockAlign;
    const REFERENCE_TIME bufDur = 2000000;                                       // 200 ms
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, bufDur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { finishInit(false, QString("audio client init failed (0x%1)").arg(quint32(hr), 0, 16)); return; }
    Rel<IAudioCaptureClient> cap;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))) { finishInit(false, "no capture service"); return; }
    if (FAILED(client->Start())) { finishInit(false, "audio start failed"); return; }
    m_devRate = rate; m_devChannels = ch;
    setConnected(true);
    finishInit(true, QString());

    std::vector<float> conv;
    while (!m_stop) {
        Sleep(8);
        UINT32 packet = 0;
        while (!m_stop && SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (frames > 0) {
                conv.resize(size_t(frames) * size_t(ch));
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(conv.begin(), conv.end(), 0.0f);
                else if (isFloat && bits == 32) memcpy(conv.data(), data, size_t(frames) * size_t(frameBytes));
                else if (bits == 16) { const int16_t* s = reinterpret_cast<const int16_t*>(data); for (size_t i = 0; i < conv.size(); ++i) conv[i] = float(s[i]) / 32768.0f; }
                else if (bits == 32) { const int32_t* s = reinterpret_cast<const int32_t*>(data); for (size_t i = 0; i < conv.size(); ++i) conv[i] = float(double(s[i]) / 2147483648.0); }
                else if (bits == 24) { const uint8_t* s = data; for (size_t i = 0; i < conv.size(); ++i, s += 3) { const int32_t v = (int32_t(s[0]) << 8) | (int32_t(s[1]) << 16) | (int32_t(s[2]) << 24); conv[i] = float(double(v) / 2147483648.0); } }
                else std::fill(conv.begin(), conv.end(), 0.0f);
                const float* planes[1] = { conv.data() };
                publishAudio(planes, false, ch, rate, int(frames));
                ++m_blocks;
            }
            cap->ReleaseBuffer(frames);
        }
    }
    client->Stop();
    setConnected(false);
}
#else
void AudioInSource::captureLoop() { finishInit(false, "audio inputs need Windows (WASAPI)"); }
#endif
