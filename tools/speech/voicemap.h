#ifndef VOICEMAP_H
#define VOICEMAP_H

#include <QString>
#include <QStringList>

/**
 * voicemap: how PlayPlay's voice ids (Kokoro names such as "af_heart", or a real SAPI voice name) and speed become the
 * settings of each TTS engine, ported from the scripts so the native tool picks exactly the voice they pick:
 *  - sapiVoiceFor()     = spot_tts.py sapi_voice_for (L229-250), quirks included ("Microsoft Hazel ..." when Hazel is not
 *                         installed starts with "m" and so maps to a male voice, as in Python).
 *  - kokoroPickVoice()  = kokoro_sherpa.js pickVoice (L19-30): the kokoro-en-v0_19 speaker table and the nearest
 *                         speaker for newer Kokoro names (af_heart -> af_bella). Used by the Kokoro engine (DEP-11).
 *  - clampSpeed()       = spot_tts.py `max(0.5, min(2.0, float(a.speed or 1.0)))`, Python's NaN semantics included.
 *  - sapiRate()         = `int(round((speed - 1.0) * 10.0))` (half-even), then clamped to SAPI's -10..10 like the
 *                         PowerShell script did.
 * QtCore only (no COM): the test checks it against the Python function.
 */
namespace voicemap {

QString sapiVoiceFor(const QString& voice, const QStringList& installed);

/// kokoro-en-v0_19 speakers in voices.bin order (index = speaker id).
const QStringList& kokoroVoices();
/// Speaker id of a v0_19 name (case-insensitive, trimmed), -1 when it is not one.
int kokoroSpeakerId(const QString& name);
QString kokoroPickVoice(const QString& want);

double clampSpeed(double speed);
int sapiRate(double speed);

/// Python's str.isspace() for one code point (adds U+001C..U+001F to what QChar::isSpace() knows) and str.strip().
bool isPySpace(char32_t c);
QString pyStrip(const QString& s);

}   // namespace voicemap

#endif // VOICEMAP_H
