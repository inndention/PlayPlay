// Pillow's Gaussian blur (libImaging/BoxBlur.c), see blur.h. The arithmetic follows the C source type for type: the
// radius in float with two double steps, the weights from a float division, the window in 32-bit unsigned fixed point.
#include "blur.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace blur {
namespace {

// ImagingLineBoxBlur8/32: one line of C-channel pixels from `in` to `out` (different buffers). acc holds the window
// sum for pixel x; the two pixels just outside it count with weight fw (the fractional radius), the window with ww.
template <int C>
void lineBlur(uchar* out, const uchar* in, int lastx, int radius, int edgeA, int edgeB, quint32 ww, quint32 fw) {
    quint32 acc[C];
    for (int c = 0; c < C; ++c) {
        // the window of pixel -1: radius + 1 copies of the first pixel, then pixels 0 .. radius - 1 (past the end the
        // last pixel again, for a line shorter than the radius)
        acc[c] = quint32(in[c]) * quint32(radius + 1);
        for (int x = 0; x < edgeA - 1; ++x) acc[c] += in[x * C + c];
        acc[c] += quint32(in[lastx * C + c]) * quint32(radius - edgeA + 1);
    }
    auto step = [&](int x, int sub, int add, int left, int right) {
        for (int c = 0; c < C; ++c) {
            acc[c] += quint32(in[add * C + c]) - quint32(in[sub * C + c]);   // wraps like Pillow's UINT32, ends exact
            const quint32 bulk = acc[c] * ww + (quint32(in[left * C + c]) + quint32(in[right * C + c])) * fw;
            out[x * C + c] = uchar((bulk + (1u << 23)) >> 24);
        }
    };
    if (edgeA <= edgeB) {
        for (int x = 0; x < edgeA; ++x) step(x, 0, x + radius, 0, x + radius + 1);
        for (int x = edgeA; x < edgeB; ++x) step(x, x - radius - 1, x + radius, x - radius - 1, x + radius + 1);
        for (int x = edgeB; x <= lastx; ++x) step(x, x - radius - 1, lastx, x - radius - 1, lastx);
    } else {                                                   // the radius reaches past both ends
        for (int x = 0; x < edgeB; ++x) step(x, 0, x + radius, 0, x + radius + 1);
        for (int x = edgeB; x < edgeA; ++x) step(x, 0, lastx, 0, lastx);
        for (int x = edgeA; x <= lastx; ++x) step(x, x - radius - 1, lastx, x - radius - 1, lastx);
    }
}

// The per-pass constants of ImagingHorizontalBoxBlur for a line of n pixels.
struct Pass {
    int radius, edgeA, edgeB; quint32 ww, fw;
    Pass(float floatRadius, int n) {
        radius = int(floatRadius);
        ww = quint32(16777216.0f / (floatRadius * 2 + 1));                           // (UINT32)(1 << 24) / float
        fw = (quint32(1u << 24) - quint32(radius * 2 + 1) * ww) / 2;
        edgeA = std::min(radius + 1, n);
        edgeB = std::max(n - radius - 1, 0);
    }
};

void runLine(int channels, uchar* out, const uchar* in, int n, const Pass& p) {
    switch (channels) {
    case 1: lineBlur<1>(out, in, n - 1, p.radius, p.edgeA, p.edgeB, p.ww, p.fw); break;
    case 2: lineBlur<2>(out, in, n - 1, p.radius, p.edgeA, p.edgeB, p.ww, p.fw); break;
    case 3: lineBlur<3>(out, in, n - 1, p.radius, p.edgeA, p.edgeB, p.ww, p.fw); break;
    default: lineBlur<4>(out, in, n - 1, p.radius, p.edgeA, p.edgeB, p.ww, p.fw); break;
    }
}

int channelsOf(QImage::Format f) {
    switch (f) {
    case QImage::Format_Alpha8: case QImage::Format_Grayscale8: return 1;
    case QImage::Format_RGB888: case QImage::Format_BGR888: return 3;
    case QImage::Format_RGB32: case QImage::Format_ARGB32: case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888: case QImage::Format_RGBA8888: case QImage::Format_RGBA8888_Premultiplied: return 4;
    default: return 0;
    }
}

}   // namespace

float gaussianBoxRadius(float radius, int passes) {
    if (passes < 1) return 0.0f;
    const float sigma2 = radius * radius / passes;
    const float L = float(std::sqrt(12.0 * sigma2 + 1.0));                 // [7] box length
    const float l = float(std::floor((L - 1.0) / 2.0));                    // [11] integer part of the box radius
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);                    // [14] fractional part
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

int spread(float boxRadius, int passes) {
    return boxRadius > 0 ? std::max(passes, 0) * (int(boxRadius) + 1) : 0;
}

void boxBlur(uchar* data, int width, int height, qsizetype stride, int channels, float xr, float yr, int passes) {
    if (!data || width <= 0 || height <= 0 || channels < 1 || channels > 4 || passes < 1) return;
    const size_t rowBytes = size_t(width) * size_t(channels);
    if (xr > 0) {                                              // all horizontal passes first, as ImagingBoxBlur does
        const Pass p(xr, width);
        std::vector<uchar> tmp(rowBytes);
        for (int y = 0; y < height; ++y) {
            uchar* row = data + y * stride;
            for (int i = 0; i < passes; ++i) {
                runLine(channels, tmp.data(), row, width, p);
                std::memcpy(row, tmp.data(), rowBytes);
            }
        }
    }
    if (yr > 0) {                                              // then the vertical ones (Pillow transposes; same result)
        const Pass p(yr, height);
        const size_t colBytes = size_t(height) * size_t(channels);
        std::vector<uchar> a(colBytes), b(colBytes);
        for (int x = 0; x < width; ++x) {
            uchar* base = data + size_t(x) * size_t(channels);
            for (int y = 0; y < height; ++y) std::memcpy(&a[size_t(y) * channels], base + y * stride, size_t(channels));
            for (int i = 0; i < passes; ++i) { runLine(channels, b.data(), a.data(), height, p); a.swap(b); }
            for (int y = 0; y < height; ++y) std::memcpy(base + y * stride, &a[size_t(y) * channels], size_t(channels));
        }
    }
}

bool boxBlur(QImage& img, float xr, float yr, int passes) {
    const int ch = channelsOf(img.format());
    if (!ch) return false;
    if (img.isNull() || (xr <= 0 && yr <= 0)) return true;
    boxBlur(img.bits(), img.width(), img.height(), img.bytesPerLine(), ch, xr, yr, passes);
    return true;
}

bool gaussianBlur(QImage& img, float radius) {
    if (!channelsOf(img.format())) return false;
    if (!(radius > 0)) return true;                            // GaussianBlur(0) returns a copy
    const float r = gaussianBoxRadius(radius, 3);
    return boxBlur(img, r, r, 3);
}

QImage gaussianBlurred(const QImage& img, float radius) {
    QImage out = img;
    if (!channelsOf(out.format())) out = out.convertToFormat(out.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    gaussianBlur(out, radius);
    return out;
}

}   // namespace blur
