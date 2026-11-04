#pragma once
// The quality proxy (ST-08): a port of the "Quality proxy" section of starmedia src/catalog.ts. OpenRouter exposes no
// benchmark score, so this is a transparent heuristic: within a kind, a higher list price (a proxy for capability), a
// newer release, a larger context and richer capabilities rank higher. It only decides which model wins *among those
// the budget can afford*; pins (RunOptions::pins) or a bonus table correct it.
//
//   text:  0.5 · price + 0.28 · recency + 0.08 · ctx + 0.14 · caps
//   other: 0.62 · price + 0.38 · recency
// price = percentile rank of log10(representative price) among the paid models of the kind (free: 0.05); recency =
// exp(-ageDays / 300); ctx = min(1, log10(max(ctx, 1000)) / 6.1); caps = reasoning 0.5 + structured_outputs 0.25 +
// tools 0.25 (text) or 0.5. The same arithmetic in the same order as the TS, so the scores equal the TS goldens.
#include "catalog/catalog.h"

#include <QList>
#include <QString>
#include <functional>
#include <utility>

namespace studio::catalog {

/// TS percentileRank(values): v → (count of sorted values below v) / (n - 1); 0.5 when fewer than two values.
std::function<double(double)> percentileRank(QList<double> values);
/// TS representativePrice(m): the one price that stands for a model of its kind (text 0.3·in + 0.7·out; image per
/// image, else 1.5 × per megapixel, else 1500 × per output token; video the 720p rate per second (or the first
/// resolution's); others in + out).
double representativePrice(const ModelInfo& m);
/// TS score(models): sets every model's quality (0..1) from its kind's price percentiles, as of `nowSec`; `bonus`
/// adds b to every model whose id contains the pattern, in the order given.
void score(QList<ModelInfo>& models, double nowSec, const QList<std::pair<QString, double>>& bonus = {});
/// One model's score among `peers` of its kind (the per-kind percentile needs them); peers may include the model.
double scoreOne(const ModelInfo& m, const QList<ModelInfo>& peers, double nowSec, const QList<std::pair<QString, double>>& bonus = {});

} // namespace studio::catalog
