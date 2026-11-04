#define _USE_MATH_DEFINES
#include <cmath>
#include "loudness.h"
#include <algorithm>

LoudnessMeter::LoudnessMeter(int sampleRate, int channels)
    : m_rate(sampleRate), m_channels(channels), m_blockLen(sampleRate / 10)
{
    designFilters();
    reset();
}

void LoudnessMeter::designFilters() {
    // BS.1770-4 pre-filter (high shelf) and RLB high-pass, re-derived for fs.
    const double fs = m_rate;
    auto makeShelf = [&]() {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(3.14159265358979323846 * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad b;
        b.b0 = (Vh + Vb * K / Q + K * K) / a0; b.b1 = 2.0 * (K * K - Vh) / a0; b.b2 = (Vh - Vb * K / Q + K * K) / a0;
        b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0;
        return b;
    };
    auto makeHp = [&]() {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(3.14159265358979323846 * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad b;
        b.b0 = 1.0 / a0; b.b1 = -2.0 / a0; b.b2 = 1.0 / a0;
        b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0;
        return b;
    };
    m_shelf.assign(size_t(m_channels), makeShelf());
    m_hp.assign(size_t(m_channels), makeHp());
    m_chWeight.assign(size_t(m_channels), 1.0);
    if (m_channels >= 6) { m_chWeight[4] = 1.41; m_chWeight[5] = 1.41; m_chWeight[3] = 0.0; }   // 5.1: Ls/Rs +1.5 dB, LFE excluded
}

void LoudnessMeter::reset() {
    for (auto& b : m_shelf) { b.z1 = b.z2 = 0; }
    for (auto& b : m_hp) { b.z1 = b.z2 = 0; }
    m_blockPos = 0; m_blockSum = 0.0; m_recent.clear(); m_blockPowers.clear();
    m_momentary = m_shortTerm = -70.0; m_peakDbfs = -100.0;
}

void LoudnessMeter::process(const float* const* planes, int nSamples) {
    for (int i = 0; i < nSamples; ++i) {
        double sum = 0.0;
        for (int c = 0; c < m_channels; ++c) {
            const double x = planes[c][i];
            const double ax = std::fabs(x);
            if (ax > 0.0) { const double db = 20.0 * std::log10(ax); if (db > m_peakDbfs) m_peakDbfs = db; }
            const double y = m_hp[size_t(c)].run(m_shelf[size_t(c)].run(x));
            sum += m_chWeight[size_t(c)] * y * y;
        }
        m_blockSum += sum;
        if (++m_blockPos >= m_blockLen) finishBlock();
    }
}

void LoudnessMeter::finishBlock() {
    const double power = m_blockSum / m_blockLen;
    m_blockSum = 0.0; m_blockPos = 0;
    m_recent.push_back(power);
    while (m_recent.size() > 30) m_recent.pop_front();
    // Momentary: last 4 blocks (400 ms); short-term: last 30 (3 s).
    double m = 0.0; int n = 0;
    for (auto it = m_recent.rbegin(); it != m_recent.rend() && n < 4; ++it, ++n) m += *it;
    m_momentary = toLufs(n ? m / n : 0.0);
    double s = 0.0; for (double p : m_recent) s += p;
    m_shortTerm = toLufs(m_recent.empty() ? 0.0 : s / m_recent.size());
    // Integrated gating works on 400 ms windows with 75 % overlap: store the momentary power.
    if (n == 4) {
        m_blockPowers.push_back(m / 4.0);
        if (m_blockPowers.size() > 36000) m_blockPowers.erase(m_blockPowers.begin(), m_blockPowers.begin() + 18000);   // keep ~1 h
    }
}

double LoudnessMeter::integratedLufs() const {
    if (m_blockPowers.empty()) return -70.0;
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0.0; int n = 0;
    for (double p : m_blockPowers) if (p > absGate) { sum += p; ++n; }
    if (!n) return -70.0;
    const double relGate = (sum / n) * std::pow(10.0, -10.0 / 10.0);
    sum = 0.0; n = 0;
    for (double p : m_blockPowers) if (p > absGate && p > relGate) { sum += p; ++n; }
    return n ? toLufs(sum / n) : -70.0;
}
