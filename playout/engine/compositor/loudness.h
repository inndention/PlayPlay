#ifndef PLAYPLAY_LOUDNESS_H
#define PLAYPLAY_LOUDNESS_H

#include <QtGlobal>
#include <vector>
#include <deque>

/**
 * LoudnessMeter — ITU-R BS.1770 / EBU R128 programme loudness.
 *
 * K-weighting (stage 1 high-shelf, stage 2 high-pass) per channel, mean square
 * over 100 ms blocks, momentary (400 ms) and short-term (3 s) loudness in LUFS,
 * gated integrated loudness (absolute -70 LUFS, relative -10 LU), and sample
 * peak in dBFS. Coefficients are derived for the configured sample rate.
 */
class LoudnessMeter {
public:
    explicit LoudnessMeter(int sampleRate = 48000, int channels = 2);
    void reset();
    /// Planar float input, one call per output frame.
    void process(const float* const* planes, int nSamples);

    double momentaryLufs() const { return m_momentary; }
    double shortTermLufs() const { return m_shortTerm; }
    double integratedLufs() const;
    double peakDbfs() const { return m_peakDbfs; }
    double truePeakApproxDbfs() const { return m_peakDbfs; }   // sample peak (true peak needs oversampling)
    qint64 blocks() const { return qint64(m_blockPowers.size()); }

private:
    struct Biquad { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0; double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    int m_rate, m_channels;
    std::vector<Biquad> m_shelf, m_hp;
    std::vector<double> m_chWeight;
    // 100 ms block accumulation
    int m_blockLen, m_blockPos = 0;
    double m_blockSum = 0.0;
    std::deque<double> m_recent;          // last 30 block powers (3 s)
    std::vector<double> m_blockPowers;    // all 400 ms-window powers for gating (bounded)
    double m_momentary = -70.0, m_shortTerm = -70.0, m_peakDbfs = -100.0;
    void finishBlock();
    static double toLufs(double power) { return power > 1e-12 ? -0.691 + 10.0 * std::log10(power) : -70.0; }
    void designFilters();
};

#endif // PLAYPLAY_LOUDNESS_H
