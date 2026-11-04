#ifndef PYRANDOM_H
#define PYRANDOM_H

#include <QString>
#include <QtGlobal>

#include <array>
#include <cstddef>
#include <vector>

/**
 * pyrandom: a bit-exact replica of the parts of CPython's `random.Random` (Modules/_randommodule.c + Lib/random.py,
 * 3.11) that scripts/spot_music.py uses, so the C++ composer picks the same key, tempo, progression and melody as the
 * Python one for every seed and the MIDI files are byte-identical (docs/plans/inhouse-deps.md §6.3, DEP-07):
 *  - seeding with an int: MT19937 `init_by_array` over the 32-bit words of abs(seed), least significant first
 *    (`random_seed`: keyused = max(1, ceil(bits / 32)) words), so seeds wider than 64 bits work too;
 *  - random() = ((a >> 5) * 2^26 + (b >> 6)) / 2^53;
 *  - getrandbits(k) (k <= 64: words from least significant, the last one shifted right by 32 - k % 32);
 *  - _randbelow(n) = getrandbits(n.bit_length()) retried while >= n (so choice() from a 1-element list still consumes
 *    words, as in Python);
 *  - uniform(a, b) = a + (b - a) * random(); choice(seq) = seq[_randbelow(len(seq))].
 * Plus Python's round(): pyRound(x) is round-half-even to an integer, pyRound(x, nd) the correctly rounded decimal
 * (dtoa mode 3, ties to even) read back as a double.
 * QtCore only (QString for the int parser).
 */
class PyRandom {
public:
    static constexpr int N = 624;

    /// random.Random(seed) for an int seed (abs() is taken, like CPython).
    explicit PyRandom(qint64 seed = 0);
    /// random.Random(n) for n = the magnitude given as 32-bit words, least significant first (sign already dropped).
    explicit PyRandom(const std::vector<quint32>& magnitudeWords);

    void seed(qint64 seed);
    void seedWords(std::vector<quint32> magnitudeWords);

    quint32 genrand();                        // genrand_uint32 (= getrandbits(32))
    double random();
    quint64 getrandbits(int k);               // 0 <= k <= 64 (k outside -> 0)
    quint64 randbelow(quint64 n);             // n >= 1 (n == 0 -> 0)
    double uniform(double a, double b);
    size_t choiceIndex(size_t n) { return size_t(randbelow(quint64(n))); }
    template <typename T> const T& choice(const std::vector<T>& v) { return v[choiceIndex(v.size())]; }

    const std::array<quint32, N>& state() const { return m_mt; }
    int index() const { return m_index; }

    /// The words init_by_array gets for abs(seed): {0} for 0, else the significant 32-bit words, low first.
    static std::vector<quint32> wordsFor(quint64 magnitude);
    /**
     * Python's int(text) for base 10, as argparse `type=int` applies it: surrounding whitespace, an optional sign, ASCII
     * digits with single underscores between them, leading zeros allowed, at most 4300 digits (CPython's
     * int_max_str_digits). Returns false (and err) where Python raises ValueError.
     */
    static bool parseInt(const QString& text, bool* negative, std::vector<quint32>* magnitudeWords, QString* err = nullptr);

private:
    void initGenrand(quint32 s);
    std::array<quint32, N> m_mt{};
    int m_index = N + 1;
};

/// Python round(x) with no ndigits: nearest integer, ties to even (float.__round__). Returns the integral double.
double pyRound(double x);
/// Python round(x, ndigits) for 0 <= ndigits <= 17: the correctly rounded decimal, ties to even, back as a double.
double pyRound(double x, int ndigits);

#endif // PYRANDOM_H
