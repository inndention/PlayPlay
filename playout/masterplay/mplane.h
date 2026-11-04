#pragma once
// MasterPlay lane identity (the lane design notes). PlayPlayMasterplay always holds at least this translation unit,
// so the library target exists and links before the parser, importer and service land; MpService and pp-masterplay
// report it so a client can tell a Supervisor without the lane from one with an older contract.
#include <QJsonObject>

namespace mp {

/// Version of the /api/v1/masterplay contract this build implements (bumped when a route or JSON shape changes).
constexpr int kApiVersion = 1;

/// {lane: "masterplay", apiVersion}
QJsonObject laneInfo();

} // namespace mp
