// The quality proxy (ST-08): starmedia src/catalog.ts percentileRank / representativePrice / score.
#include "catalog/quality.h"
#include "catalog/pricing.h"

#include <algorithm>
#include <cmath>

namespace studio::catalog {

std::function<double(double)> percentileRank(QList<double> values) {
    std::sort(values.begin(), values.end());            // numbers: the TS (a, b) => a - b
    return [sorted = std::move(values)](double v) {
        if (sorted.size() < 2) return 0.5;
        qsizetype lo = 0;
        while (lo < sorted.size() && sorted[lo] < v) lo++;
        return double(lo) / double(sorted.size() - 1);
    };
}

double representativePrice(const ModelInfo& m) {
    switch (m.kind) {
    case Kind::Text: return 0.3 * m.price.in + 0.7 * m.price.out;
    case Kind::Image: {
        if (!m.image) return 0;
        const ImageSpec& s = *m.image;
        if (s.perImage) return *s.perImage;
        if (s.perMegapixel) return *s.perMegapixel * 1.5;
        return s.perOutputToken.value_or(0) * 1500;
    }
    case Kind::Video: {
        if (!m.video) return 0;
        const QStringList& rs = m.video->resolutions;
        const QString res = rs.contains(QLatin1String("720p")) ? QStringLiteral("720p") : rs.value(0, QStringLiteral("720p"));
        const auto r = videoRate(m, res, false);
        return r ? r->perSec : 0;
    }
    default: return m.price.in + m.price.out;
    }
}

namespace {

double scoreWith(const ModelInfo& m, const std::function<double(double)>& rank, double nowSec, const QList<std::pair<QString, double>>& bonus) {
    const double price = m.free ? 0.05 : rank(std::log10(representativePrice(m) + 1e-9));
    const double ageDays = std::max(0.0, (nowSec - m.created) / 86400);
    const double recency = std::exp(-ageDays / 300);
    const double ctx = std::min(1.0, std::log10(std::max(m.ctx, 1000.0)) / 6.1);
    const double caps = m.kind == Kind::Text
        ? (m.reasoning ? 0.5 : 0) + (m.hasParam(u"structured_outputs") ? 0.25 : 0) + (m.hasParam(u"tools") ? 0.25 : 0)
        : 0.5;
    double q = m.kind == Kind::Text ? 0.5 * price + 0.28 * recency + 0.08 * ctx + 0.14 * caps : 0.62 * price + 0.38 * recency;
    for (const auto& [pat, b] : bonus)
        if (m.id.contains(pat)) q += b;
    return std::max(0.0, std::min(1.0, q));
}

std::function<double(double)> rankFor(Kind kind, const QList<ModelInfo>& peers) {
    QList<double> priced;
    for (const ModelInfo& x : peers)
        if (x.kind == kind && !x.free) priced << std::log10(representativePrice(x) + 1e-9);
    return percentileRank(std::move(priced));
}

} // namespace

void score(QList<ModelInfo>& models, double nowSec, const QList<std::pair<QString, double>>& bonus) {
    QHash<int, std::function<double(double)>> ranks;
    for (ModelInfo& m : models) {
        auto it = ranks.find(int(m.kind));
        if (it == ranks.end()) it = ranks.insert(int(m.kind), rankFor(m.kind, models));
        m.quality = scoreWith(m, it.value(), nowSec, bonus);
    }
}

double scoreOne(const ModelInfo& m, const QList<ModelInfo>& peers, double nowSec, const QList<std::pair<QString, double>>& bonus) {
    return scoreWith(m, rankFor(m.kind, peers), nowSec, bonus);
}

} // namespace studio::catalog
