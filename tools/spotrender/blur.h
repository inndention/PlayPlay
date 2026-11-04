#ifndef SPOTRENDER_BLUR_H
#define SPOTRENDER_BLUR_H

#include <QImage>
#include <QtGlobal>

/**
 * blur: Pillow's ImageFilter.GaussianBlur, ported bit for bit (DEP-08a, docs/plans/inhouse-deps.md §6.3). spot_card.py
 * softens every drop shadow (and the photo backdrops) with it; these few dozen lines keep the card renderer off Qt's
 * private qt_blurImage and give exactly Pillow's pixels (the test compares against Pillow when it is installed):
 *  - gaussianBoxRadius() = BoxBlur.c _gaussian_blur_radius(): the extended-box radius whose `passes`-fold repetition has
 *    the Gaussian's variance (Gwosdek et al., "Theoretical foundations of Gaussian convolution by extended box
 *    filtering"), in the same float / double steps;
 *  - boxBlur() = ImagingBoxBlur(): `passes` horizontal passes over every row, then `passes` vertical passes over every
 *    column. One pass is a running sum over 2r+1 pixels plus the two pixels at distance r+1 weighted by the fractional
 *    part, in 8.24 fixed point with Pillow's rounding; the border pixel is repeated past the edges.
 * In place, 8 bits per channel, every channel on its own. Straight and premultiplied data blur the same way; for the
 * black shadow layers spot_card blurs the two are identical. Blur premultiplied pixels when colour must not bleed out of
 * transparent areas.
 */
namespace blur {

/// _gaussian_blur_radius(radius, passes): 0 for radius 0.
float gaussianBoxRadius(float radius, int passes = 3);
/// How far `passes` extended boxes of radius `boxRadius` spread ink on each side. Ink cropped with spread() + 1 pixels of
/// margin (clamped to the image) blurs to exactly what the whole image would, so a shadow needs only its own box.
int spread(float boxRadius, int passes = 3);
/// One plane of `channels` interleaved bytes per pixel (1..4). xr / yr are box radii (gaussianBoxRadius()); a radius of 0
/// or less skips that direction, as Pillow does.
void boxBlur(uchar* data, int width, int height, qsizetype stride, int channels, float xr, float yr, int passes = 3);
/// 8-bit formats only: Alpha8, Grayscale8, RGB888, BGR888, RGB32, ARGB32, ARGB32_Premultiplied, RGBX8888, RGBA8888,
/// RGBA8888_Premultiplied. Returns false and leaves the image alone for any other format.
bool boxBlur(QImage& img, float xr, float yr, int passes = 3);
/// ImageFilter.GaussianBlur(radius) = boxBlur(gaussianBoxRadius(radius, 3)) both ways; radius <= 0 changes nothing.
bool gaussianBlur(QImage& img, float radius);
QImage gaussianBlurred(const QImage& img, float radius);

}   // namespace blur

#endif // SPOTRENDER_BLUR_H
