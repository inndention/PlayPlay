#include "source.h"
#include "basicsources.h"
#include "livesources.h"
#include "audioinsource.h"
#ifdef PLAYPLAY_WITH_LIBOBS
#include "libobssource.h"   // cmake/lane-libobs.cmake, docs/libobs-spike.md
#endif
#include <QDebug>

std::unique_ptr<LayerSource> LayerSource::create(const QJsonObject& spec, QString* error) {
    const QString type = spec.value("type").toString("file").toLower();
    std::unique_ptr<LayerSource> s;
    if (type == "file" || type == "capture" || type == "udp" || type == "stream") s = std::make_unique<FileSource>(spec);
    else if (type == "color" || type == "black" || type == "bars" || type == "slate" || type == "tone") s = std::make_unique<GeneratorSource>(spec);
    else if (type == "image" || type == "sequence") s = std::make_unique<ImageSource>(spec);
    else if (type == "template") s = std::make_unique<TemplateSource>(spec);
    else if (type == "ndi") s = std::make_unique<NdiSource>(spec);
    else if (type == "audio_in" || type == "line_in") s = std::make_unique<AudioInSource>(spec);
    else if (type == "dektec_sdi_in" || type == "sdi_in") s = std::make_unique<DekTecSdiInSource>(spec);
    else if (type == "html") s = createHtmlSource(spec, error);
#ifdef PLAYPLAY_WITH_LIBOBS
    else if (type == "libobs") s = std::make_unique<LibObsSource>(spec);
#endif
    else { if (error) *error = "unknown source type: " + type; return nullptr; }
    if (!s && error && error->isEmpty()) *error = "source not available: " + type;
    return s;
}
