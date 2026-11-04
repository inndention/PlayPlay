#include "sapitts.h"

#include "wavio.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <windows.h>
#include <sapi.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace sapitts {
namespace {

// OneCore voices live in their own token category; SAPI 5 enumerates it when asked for it by id (System.Speech never does)
const wchar_t kOneCoreVoices[] = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";

QString hrText(HRESULT hr) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, DWORD(hr), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    const QString msg = n && buf ? QString::fromWCharArray(buf, int(n)).trimmed() : QString();   // SPERR_* are not in the system table
    if (buf) LocalFree(buf);
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QChar('0')) + (msg.isEmpty() ? QString() : " (" + msg + ")");
}

bool failed(HRESULT hr, const QString& what, QString* err) {
    if (SUCCEEDED(hr)) return false;
    if (err) *err = what + " failed: " + hrText(hr);
    return true;
}

QString takeString(wchar_t* p) {
    const QString s = p ? QString::fromWCharArray(p) : QString();
    if (p) CoTaskMemFree(p);
    return s;
}

QString keyValue(ISpDataKey* key, const wchar_t* name) {
    wchar_t* v = nullptr;
    return key && SUCCEEDED(key->GetStringValue(name, &v)) ? takeString(v) : QString();
}

const wchar_t* wstr(const QString& s) { return reinterpret_cast<const wchar_t*>(s.utf16()); }

VoiceInfo describe(ISpObjectToken* t, const QString& category) {
    VoiceInfo v; v.category = category;
    wchar_t* id = nullptr;
    if (SUCCEEDED(t->GetId(&id))) v.id = takeString(id);
    v.description = keyValue(t, nullptr);                   // the token key's default value
    ComPtr<ISpDataKey> attrs;
    if (SUCCEEDED(t->OpenKey(L"Attributes", &attrs))) {
        v.name = keyValue(attrs.Get(), L"Name"); v.gender = keyValue(attrs.Get(), L"Gender"); v.age = keyValue(attrs.Get(), L"Age");
        v.language = keyValue(attrs.Get(), L"Language"); v.vendor = keyValue(attrs.Get(), L"Vendor");
    }
    if (v.name.isEmpty()) v.name = v.description.section(QStringLiteral(" - "), 0, 0);
    if (v.category.isEmpty()) v.category = v.id.contains(QStringLiteral("Speech_OneCore"), Qt::CaseInsensitive) ? "onecore" : "desktop";
    return v;
}

struct Token { ComPtr<ISpObjectToken> token; VoiceInfo info; };

/// Appends the voices of one token category. A missing category (no OneCore voices on this edition) is not an error.
bool enumCategory(const wchar_t* categoryId, const QString& label, bool optional, QList<Token>* out, QString* err) {
    ComPtr<ISpObjectTokenCategory> cat;
    if (failed(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cat)), "SpObjectTokenCategory", err)) return false;
    HRESULT hr = cat->SetId(categoryId, FALSE);
    if (FAILED(hr)) { if (optional) return true; return !failed(hr, "voice category " + label, err); }
    ComPtr<IEnumSpObjectTokens> en;
    hr = cat->EnumTokens(nullptr, nullptr, &en);
    if (hr == S_FALSE || (SUCCEEDED(hr) && !en)) return true;   // no tokens
    if (FAILED(hr)) { if (optional) return true; return !failed(hr, "enumerating " + label + " voices", err); }
    ULONG n = 0;
    en->GetCount(&n);
    for (ULONG i = 0; i < n; ++i) {
        ComPtr<ISpObjectToken> t;
        if (FAILED(en->Item(i, &t)) || !t) continue;
        out->append({t, describe(t.Get(), label)});
    }
    return true;
}

QList<Token> enumAll(QString* err) {
    QList<Token> all; QString e;
    const bool desk = enumCategory(SPCAT_VOICES, QStringLiteral("desktop"), false, &all, &e);
    enumCategory(kOneCoreVoices, QStringLiteral("onecore"), true, &all, nullptr);
    QSet<QString> seen; QList<Token> out;
    for (const Token& t : all) if (!seen.contains(t.info.id.toLower())) { seen.insert(t.info.id.toLower()); out.append(t); }
    if (out.isEmpty() && err) *err = desk ? QStringLiteral("no SAPI voices installed") : e;
    return out;
}

QString tempPathFor(const QString& target) {
    static std::atomic<int> counter{0};
    const QFileInfo fi(target);
    return fi.absoluteDir().filePath(QStringLiteral("%1.sapi-%2-%3.part.wav").arg(fi.completeBaseName()).arg(GetCurrentProcessId()).arg(++counter));
}

}   // namespace

// ----------------------------------------------------------------------------- ComScope
ComScope::ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) { m_ok = true; m_uninit = true; }
    else if (hr == RPC_E_CHANGED_MODE) m_ok = true;            // already an STA: COM works, SAPI too; not ours to uninitialise
    else m_error = "CoInitializeEx failed: " + hrText(hr);
}
ComScope::~ComScope() { if (m_uninit) CoUninitialize(); }

// ----------------------------------------------------------------------------- voices
QList<VoiceInfo> installedVoices(QString* err) {
    QList<VoiceInfo> r;
    for (const Token& t : enumAll(err)) r.append(t.info);
    return r;
}

QStringList voiceNames(const QList<VoiceInfo>& voices) {
    QStringList n;
    for (const VoiceInfo& v : voices) n << v.name;
    return n;
}

QString defaultVoiceName(QString* err) {
    ComPtr<ISpVoice> voice;
    if (failed(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&voice)), "SpVoice", err)) return {};
    ComPtr<ISpObjectToken> t;
    if (failed(voice->GetVoice(&t), "ISpVoice::GetVoice", err) || !t) return {};
    return describe(t.Get(), QString()).name;
}

// ----------------------------------------------------------------------------- Speaker
struct Speaker::Impl {
    ComPtr<ISpVoice> voice;
    QList<Token> tokens;
    QList<VoiceInfo> infos;
    int rate = 0;
    bool hung = false;
};

Speaker::Speaker() : d(new Impl) {}
Speaker::~Speaker() = default;

bool Speaker::open(QString* err) {
    if (d->voice) return true;
    if (failed(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d->voice)), "creating the SAPI voice (SpVoice)", err)) return false;
    d->tokens = enumAll(nullptr);
    d->infos.clear();
    for (const Token& t : d->tokens) d->infos.append(t.info);
    return true;
}

const QList<VoiceInfo>& Speaker::voices() const { return d->infos; }

bool Speaker::selectVoice(const QString& name, QString* err) {
    if (!d->voice) { if (err) *err = QStringLiteral("SAPI voice not open"); return false; }
    const Token* hit = nullptr;
    for (const Token& t : d->tokens) if (!hit && t.info.name.compare(name, Qt::CaseInsensitive) == 0) hit = &t;
    for (const Token& t : d->tokens) if (!hit && t.info.description.compare(name, Qt::CaseInsensitive) == 0) hit = &t;
    for (const Token& t : d->tokens) if (!hit && t.info.id.compare(name, Qt::CaseInsensitive) == 0) hit = &t;
    if (!hit) { if (err) *err = "voice not installed: " + name; return false; }
    return !failed(d->voice->SetVoice(hit->token.Get()), "selecting voice " + hit->info.name, err);
}

QString Speaker::voiceName() const {
    if (!d->voice) return {};
    ComPtr<ISpObjectToken> t;
    return SUCCEEDED(d->voice->GetVoice(&t)) && t ? describe(t.Get(), QString()).name : QString();
}

bool Speaker::setRate(int rate, QString* err) {
    if (!d->voice) { if (err) *err = QStringLiteral("SAPI voice not open"); return false; }
    rate = qBound(-10, rate, 10);
    if (failed(d->voice->SetRate(rate), "ISpVoice::SetRate", err)) return false;
    d->rate = rate;
    return true;
}

int Speaker::rate() const { return d->rate; }
bool Speaker::hung() const { return d->hung; }

bool Speaker::speakToFile(const QString& text, const QString& wavPath, int timeoutMs, QString* err) {
    if (!d->voice) { if (err) *err = QStringLiteral("SAPI voice not open"); return false; }
    if (d->hung) { if (err) *err = QStringLiteral("the SAPI engine is still busy with an earlier item"); return false; }
    const QString target = QDir::toNativeSeparators(QFileInfo(wavPath).absoluteFilePath());
    const QString tmp = QDir::toNativeSeparators(tempPathFor(target));
    auto cleanup = [&]() { DeleteFileW(wstr(tmp)); };

    ComPtr<ISpStream> stream;
    if (failed(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&stream)), "SpStream", err)) return false;
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = 1; wfx.nSamplesPerSec = kSampleRate; wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2; wfx.nAvgBytesPerSec = kSampleRate * 2; wfx.cbSize = 0;
    // = SPBindToFile(tmp, SPFM_CREATE_ALWAYS, &stream, &SPDFID_WaveFormatEx, &wfx) without sphelper.h (which needs ATL);
    // no event interest, so the file holds only fmt + data
    if (failed(stream->BindToFile(wstr(tmp), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0), "writing " + tmp, err)) return false;
    if (failed(d->voice->SetOutput(stream.Get(), TRUE), "ISpVoice::SetOutput", err)) { stream->Close(); cleanup(); return false; }
    HRESULT hr = d->voice->Speak(wstr(text), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
    if (failed(hr, "ISpVoice::Speak", err)) { stream->Close(); cleanup(); return false; }
    hr = d->voice->WaitUntilDone(timeoutMs > 0 ? DWORD(timeoutMs) : INFINITE);
    if (hr == S_FALSE) {
        // Past the deadline the engine thread may be wedged, and purging, closing the stream (it is still bound) or
        // releasing the voice can then block as long as it does. Touch nothing: the caller ends the process (hung()).
        d->hung = true;
        cleanup();                                               // fails while the stream still holds the file
        if (err) *err = QStringLiteral("SAPI did not finish within %1 s").arg(timeoutMs / 1000.0, 0, 'f', 1);
        return false;
    }
    if (FAILED(hr)) {
        d->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);   // drop the queued speech; give the engine a moment to stop
        d->voice->WaitUntilDone(5000);
        stream->Close(); cleanup();
        if (err) *err = "ISpVoice::WaitUntilDone failed: " + hrText(hr);
        return false;
    }
    SPVOICESTATUS st = {};
    const bool statusOk = SUCCEEDED(d->voice->GetStatus(&st, nullptr));
    hr = stream->Close();                                       // finalises the RIFF sizes and releases the file
    if (statusOk && FAILED(st.hrLastResult)) { cleanup(); if (err) *err = "SAPI could not speak: " + hrText(st.hrLastResult); return false; }
    if (failed(hr, "closing " + tmp, err)) { cleanup(); return false; }

    wavio::Wav w; QString werr;
    if (!wavio::readFile(tmp, &w, &werr)) { cleanup(); if (err) *err = "SAPI output unreadable: " + werr; return false; }
    if (w.sampleRate != kSampleRate || w.channels != 1 || !w.isPcm16()) {
        cleanup();
        if (err) *err = QStringLiteral("SAPI wrote %1 Hz, %2 channel(s), %3-bit; expected 48000 Hz mono 16-bit").arg(w.sampleRate).arg(w.channels).arg(w.sourceBits);
        return false;
    }
    if (!MoveFileExW(wstr(tmp), wstr(target), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError(); cleanup();
        if (err) *err = "cannot write " + target + ": " + hrText(HRESULT_FROM_WIN32(e));
        return false;
    }
    return true;
}

}   // namespace sapitts
