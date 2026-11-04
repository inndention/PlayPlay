#ifndef SAPITTS_H
#define SAPITTS_H

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

/**
 * sapitts: Windows SAPI 5 text-to-speech through COM, replacing spot_tts.py's PowerShell + System.Speech engine
 * (L212-316). No PowerShell, no .NET, no temporary script: ISpVoice speaks into an ISpStream bound to a WAV file
 * (what sphelper's SPBindToFile does) at 48 kHz 16-bit mono, the format the script asked System.Speech for.
 *  - Voices come from two token categories: the desktop SAPI voices (SPCAT_VOICES, "Microsoft Zira Desktop") and the
 *    OneCore voices (HKLM\SOFTWARE\Microsoft\Speech_OneCore\Voices, e.g. "Microsoft Mark", which System.Speech never
 *    lists). Desktop voices come first, so a Kokoro id maps onto the same voice the script picked.
 *  - Text is spoken with SPF_IS_NOT_XML: narration from web sources can never carry SAPI XML (voice / volume changes).
 *  - Each speak has a deadline (async speak + WaitUntilDone). Past it the engine thread may be wedged, so nothing more is
 *    asked of it: the speaker reports hung() and the caller ends the process (ttscli prints the failure, then _Exit(2))
 *    instead of purging, closing or releasing, any of which could wait on that thread forever.
 *  - Output is written to a temporary file next to the target and renamed over it when complete, so a failed or killed
 *    run never leaves a half-written WAV under the expected name.
 * COM must be initialised on the calling thread (ComScope); everything here runs on that one thread.
 */
namespace sapitts {

struct VoiceInfo {
    QString name;          // Attributes\Name ("Microsoft Zira Desktop"): what System.Speech reports as VoiceInfo.Name
    QString description;   // the token's default value ("Microsoft Zira Desktop - English (United States)")
    QString id;            // full token id (registry path)
    QString category;      // "desktop" | "onecore"
    QString gender, age, language, vendor;
};

/// CoInitializeEx(COINIT_MULTITHREADED) for this scope; ok() is false when COM could not be initialised at all.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool ok() const { return m_ok; }
    QString error() const { return m_error; }
private:
    bool m_ok = false, m_uninit = false;
    QString m_error;
};

/// Desktop voices, then OneCore voices (duplicates by token id dropped). Empty + err when SAPI is unusable.
QList<VoiceInfo> installedVoices(QString* err = nullptr);
QStringList voiceNames(const QList<VoiceInfo>& voices);
/// The voice a fresh ISpVoice speaks with when none is selected (the user's default voice).
QString defaultVoiceName(QString* err = nullptr);

/// One SAPI voice, reused for every item of a batch (as the script reused one SpeechSynthesizer).
class Speaker {
public:
    Speaker();
    ~Speaker();
    Speaker(const Speaker&) = delete;
    Speaker& operator=(const Speaker&) = delete;
    /// Creates the voice and enumerates the tokens it can switch to.
    bool open(QString* err);
    const QList<VoiceInfo>& voices() const;
    /// Selects by Name (case-insensitive), then description, then token id. False (voice unchanged) when not installed.
    bool selectVoice(const QString& name, QString* err = nullptr);
    /// Name of the voice currently selected (the default voice until selectVoice succeeds).
    QString voiceName() const;
    /// SAPI rate, clamped to -10..10.
    bool setRate(int rate, QString* err = nullptr);
    int rate() const;
    /// Speaks `text` into a 48 kHz 16-bit mono PCM WAV at `wavPath` (replaced atomically). timeoutMs <= 0: no deadline.
    bool speakToFile(const QString& text, const QString& wavPath, int timeoutMs, QString* err);
    /// A speak ran past its deadline and was left as it was. Do not release this object: end the process (its temporary
    /// .part.wav may stay behind, never under the target name).
    bool hung() const;
    static constexpr int kSampleRate = 48000;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}   // namespace sapitts

#endif // SAPITTS_H
