#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImage>
#include "playout/engine/scenerecorder.h"
#include "playout/engine/compositor/compositor.h"
extern "C" {
#include <libavformat/avformat.h>
}

class SceneRecorderTest : public QObject {
    Q_OBJECT
private slots:
    void captureAndRecompose() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); OutputFormat fmt; fmt.width = 64; fmt.height = 36; fmt.rate = FrameRate(30, 1);
        Compositor comp(fmt); comp.setSlateVisible(false);
        Compositor::LayerSpec layer; layer.id = 30; layer.role = "lower_third";
        layer.source = {{"type", "color"}, {"color", "#ff0000"}}; layer.placement.rect = QRectF(0, 0, 32, 36);
        QString why; QVERIFY2(comp.addLayer(layer, 100, &why), qPrintable(why));
        SceneRecorder rec(fmt); QVERIFY2(rec.begin(dir.filePath("session"), &why), qPrintable(why));
        comp.setCaptureHandler([&](SceneCapture c) { rec.submit(std::move(c)); });
        for (int i = 0; i < 8; ++i) { comp.render(100 + i, 0); QTest::qWait(40); }
        comp.setCaptureHandler({}); rec.finish();
        QVERIFY2(rec.status().value("complete").toBool(), qPrintable(QJsonDocument(rec.status()).toJson()));
        QCOMPARE(rec.status().value("frames").toInt(), 8);
        QFile timeline(dir.filePath("session/timeline.jsonl")); QVERIFY(timeline.open(QIODevice::ReadOnly));
        const auto event = QJsonDocument::fromJson(timeline.readLine()).object();
        QCOMPARE(event.value("frame").toInt(), 100); QVERIFY(!event.value("draws").toArray().isEmpty());
        QVERIFY(!event.value("audio").toObject().isEmpty());
        QVERIFY2(SceneRecorder::recompose(dir.filePath("session"), dir.filePath("export.mkv"), {{"omitLayers", QJsonArray{30}}}, &why), qPrintable(why));
        QVERIFY(QFileInfo(dir.filePath("export.mkv")).size() > 1000);
        for (const auto& file : {dir.filePath("session/program.mkv"), dir.filePath("export.mkv")}) {
            AVFormatContext* input = nullptr; const auto name = file.toUtf8();
            QVERIFY(avformat_open_input(&input, name.constData(), nullptr, nullptr) >= 0);
            QVERIFY(avformat_find_stream_info(input, nullptr) >= 0);
            AVPacket* packet = av_packet_alloc(); int pictures = 0;
            while (av_read_frame(input, packet) >= 0) { if (input->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ++pictures; av_packet_unref(packet); }
            av_packet_free(&packet); avformat_close_input(&input); QCOMPARE(pictures, 8);
        }
        QVERIFY(!SceneRecorder::recompose(dir.filePath("session"), dir.filePath("export.mkv"), {}, &why));
    }
    void alphaAndEdits() {
        auto v = allocVideoFrame(2, 2, AV_PIX_FMT_BGRA); QVERIFY(v);
        QImage img(v->data[0], 2, 2, v->linesize[0], QImage::Format_ARGB32); img.fill(QColor(255, 0, 0, 128));
        std::vector<SceneDraw> draws{{v, QRectF(0, 0, 2, 2), {}, {}, 1.0, 30, false}};
        auto out = SceneRecorder::compose(draws, 4, 4); QImage rendered(out->data[0], 4, 4, out->linesize[0], QImage::Format_ARGB32);
        QVERIFY(qAbs(rendered.pixelColor(0, 0).red() - 128) <= 1); QCOMPARE(rendered.pixelColor(3, 3), QColor(Qt::black));
        out = SceneRecorder::compose(draws, 4, 4, {{"omitLayers", QJsonArray{30}}});
        QImage hidden(out->data[0], 4, 4, out->linesize[0], QImage::Format_ARGB32); QCOMPARE(hidden.pixelColor(0, 0), QColor(Qt::black));
    }
};
QTEST_GUILESS_MAIN(SceneRecorderTest)
#include "test_scenerecorder.moc"
