#pragma once
// Identifiers for the Studio engine (ST-02). Shapes follow the StarMedia TS so ids look the same on both sides:
//   random("job", 8)          "job_3f9c0a1b2d4e5f60"           (mcp/core/util.ts randomId)
//   timeOrdered("run", clock) "run_20261026140312_a1b2c3"      (pub lane newRunId: UTC seconds, sorts by creation)
//   approvalToken()           "apv_" + 24 hex                  (mcp/core/spend.ts L213; the log scrubber hides it)
//   idemKey(node, hash, n)    64 hex                           (§4.3: one stable key per provider call attempt)
// Random parts come from the OS CSPRNG (QRandomGenerator::system()): approval tokens and API ids must not be guessable.
#include <QString>
#include <QStringView>

namespace studio {
class Clock;
}

namespace studio::ids {

/// `bytes` random bytes as lower-case hex (2 × bytes chars), 1..64 bytes.
QString randomHex(int bytes);
/// "<prefix>_<hex>". `prefix` must itself be safe (isSafe) — a programming error otherwise (asserts, then "id").
QString random(QStringView prefix, int bytes = 6);
/// "<prefix>_<yyyyMMddHHmmss UTC>_<hex>": plain string order is creation order to the second.
QString timeOrdered(QStringView prefix, const Clock& clock, int bytes = 3);
/// A single-use spend approval token: "apv_" + 24 hex chars (96 random bits), the MCP format.
QString approvalToken();
/// Idempotency key of one provider call: sha256 hex of the stable JSON [nodeId, inputHash, attempt]. Equal inputs give
/// the same key across restarts, so a resumed node is recognised by the provider-side dedupe and by the DB.
QString idemKey(QStringView nodeId, QStringView inputHash, int attempt);
/// True when `id` can be used as a file name and URL path segment as is: 1..128 chars of [A-Za-z0-9_.-], starting
/// with a letter or digit, without "..", and not a Windows device name (con, nul, com1 ...). Ids from requests are
/// checked with this before they reach the disk or the DB.
bool isSafe(QStringView id);

} // namespace studio::ids
