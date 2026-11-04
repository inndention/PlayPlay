#ifndef AUDIODEVICES_H
#define AUDIODEVICES_H

#include <QJsonArray>
#include <QString>

/**
 * Windows audio endpoints (WASAPI / MMDevice): the sound cards' line inputs and microphones (capture) and the playback devices
 * (render, usable as loopback capture). Shared by the Supervisor (GET /api/v1/audio/devices) and the engine's line-in source.
 * Other platforms report no devices.
 */
namespace audiodev {

/// Active endpoints: [{id, name, kind:"capture"|"render", isDefault}] (capture first).
QJsonArray listJson();

/// The endpoint id a source spec names: `device` is an exact id, or a case-insensitive part of the friendly name; empty or
/// "default" = the default capture endpoint (the default render endpoint when `loopback`). Empty result = not found.
QString resolveId(const QString& device, bool loopback);

} // namespace audiodev

#endif // AUDIODEVICES_H
