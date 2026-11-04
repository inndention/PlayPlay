#ifndef MUSICSYNTH_H
#define MUSICSYNTH_H

#include "wavio.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <array>
#include <vector>

/**
 * musicsynth: `PlayPlaySpotRender music`, the C++ port of scripts/spot_music.py (docs/plans/inhouse-deps.md §6.3, DEP-07).
 * Same command line, same stdout JSON {ok, wav, mid, bpm, key, mood, seconds, tracks, notes}, exit 0 / 2 (64 for an
 * option this tool does not know, so the Supervisor can fall back to the script).
 *  - compose(): the Piece composer (moods, scales, progressions, bass / arpeggio / drum / lead rules) driven by
 *    PyRandom, a bit-exact CPython random.Random, so key, tempo, progression and melody equal the script's for every
 *    seed and midiFile() is byte-identical to write_midi().
 *  - render(): the synthesizer (ADSR, detuned-saw pad, sine/square bass, FM electric piano arpeggio with a dotted-eighth
 *    echo, triangle lead with vibrato, synthetic drums, Haas-widened pad) -> 48 kHz stereo, exactly ceil(seconds * 48000)
 *    frames. Deterministic per seed, but not sample-identical to numpy (double-precision oscillators instead of float32
 *    phases; drum noise from our own generator instead of numpy's PCG64), which does not matter for a music bed.
 *    One intended difference in the levelling: the script scales to -12 dBFS RMS and then applies tanh(0.9 x) / 0.9 * 0.95,
 *    whose ceiling (1.056) sits above full scale, so loud moods hard-clip at 1.0 and the bed lands at -13.0..-13.4 dBFS
 *    (measured on all 108 goldens). Here the soft limiter is 0.95 * tanh(x / 0.95) and the gain is iterated so the bed
 *    lands on -12 dBFS RMS: never over 0.95, never clipped.
 * QtCore only (QSaveFile writes, so a crash never leaves half a WAV for the compose step).
 */
namespace music {

constexpr int kSampleRate = 48000;
constexpr int kPpq = 480;

enum Track { Pad = 0, Bass, Arp, Lead, Drums, TrackCount };
const char* trackName(int track);              // "pad", "bass", "arp", "lead", "drums"

struct NoteEvent {
    int start = 0;                              // 16th steps
    int length = 0;                             // 16th steps
    int note = 0;                               // MIDI note
    int velocity = 0;
    bool operator==(const NoteEvent& o) const { return start == o.start && length == o.length && note == o.note && velocity == o.velocity; }
};

struct Piece {
    QString mood;                               // resolved (pickMood)
    QString scaleName;
    std::vector<int> scale;
    QString key;                                // e.g. "F dorian"
    int energy = 5;                             // clamped 1..10
    int bpm = 0;
    int root = 0;                               // MIDI note of the tonic (48..57)
    int bars = 0;
    double step = 0.0;                          // seconds per 16th
    std::vector<int> progression;               // scale-degree chord roots per bar (looped)
    std::array<int, 4> gm{};                    // General MIDI programs: pad, bass, arp, lead
    std::array<std::vector<NoteEvent>, TrackCount> tracks;
    int noteCount() const;
    QStringList trackNames() const;             // the non-empty tracks, in pad..drums order
};

QStringList moodNames();                        // the 12 moods, in the script's table order
/// pick_mood(): strip + lower, aliases (driving -> news, sad -> solemn ...), anything unknown -> "warm".
QString pickMood(const QString& name);
/// max(3.0, min(900.0, s)) with Python's comparison semantics (NaN -> 900).
double clampSeconds(double s);

Piece compose(double seconds, const QString& mood, int energy, const std::vector<quint32>& seedWords);
Piece compose(double seconds, const QString& mood, int energy, qint64 seed);
/// write_midi(): format 1, 480 PPQ, conductor track (tempo, 4/4, key signature) + one track per non-empty part.
QByteArray midiFile(const Piece& piece);
/// render(): 48 kHz stereo 16-bit, ceil(seconds * 48000) frames, -12 dBFS RMS, peaks <= 0.95, 50 ms fade-in and a
/// min(1.2 s, seconds / 3) fade-out.
wavio::Wav render(const Piece& piece, double seconds);

struct Options {
    QString out;                                // --out (required)
    QString mid;                                // --mid ("" = no MIDI file)
    double seconds = 0.0;                       // --seconds (required), clamped by run()
    QString mood = QStringLiteral("warm");      // --mood
    int energy = 5;                             // --energy (clamped)
    std::vector<quint32> seed{1};               // --seed: abs(seed) as 32-bit words, least significant first
};
/// The script's argparse rules (Python int()/float() value syntax). False = *exitCode / *result hold the error.
bool parseOptions(const QStringList& args, Options* o, QJsonObject* result, int* exitCode, bool* helpShown = nullptr);
/// Composes, writes --mid then --out, and returns the stdout object ({ok: false, error} on failure).
QJsonObject run(const Options& o);
/// `PlayPlaySpotRender music <args>`: parse, run, print the result as the last stdout line; returns the exit code.
int cli(const QStringList& args);
QString usage();

}   // namespace music

#endif // MUSICSYNTH_H
