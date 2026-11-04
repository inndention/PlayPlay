#pragma once
#include "compositor/source.h"
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonObject>
#include <QRectF>
#include <deque>
#include <vector>

struct SceneDraw {
    FramePtr video;
    QRectF dst, src, clip;
    double opacity = 1.0;
    int layer = 0; // 0 = program bus; compositor layers retain their ids
    bool premultiplied = false;
};
struct SceneCapture {
    qint64 frame = 0;
    std::vector<SceneDraw> draws;
    FramePtr audio;
    std::vector<std::pair<QString, FramePtr>> tracks;
    QJsonObject controls;
};

// Opt-in lossless layer archive + a conventional program MKV. Disk work and encoding
// never run on the compositor thread. An overrun fails the recording explicitly.
class SceneRecorder : public QThread {
public:
    explicit SceneRecorder(const OutputFormat& format);
    ~SceneRecorder() override;
    bool begin(const QString& directory, QString* error);
    void submit(SceneCapture capture);
    void finish();
    QJsonObject status() const;
    static bool recompose(const QString& session, const QString& output,
                          const QJsonObject& edits, QString* error);
    static FramePtr compose(const std::vector<SceneDraw>& draws, int width, int height,
                            const QJsonObject& edits = {});
protected:
    void run() override;
private:
    OutputFormat m_format;
    QString m_directory, m_error;
    mutable QMutex m_mutex;
    QWaitCondition m_ready;
    std::deque<SceneCapture> m_queue;
    bool m_stopping = false, m_active = false, m_complete = false;
    qint64 m_frames = 0, m_queuedBytes = 0;
    void fail(const QString& message);
};
