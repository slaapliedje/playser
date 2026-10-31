// SPDX-License-Identifier: GPL-3.0-or-later
#include "OverlayEncoder.h"

#include <algorithm>
#include <cmath>

namespace {

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// SMPTE ST 2084 inverse EOTF; `nits` absolute
float pqEncode(float nits)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::max(nits, 0.f) / 10000.f, m1);
    return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
}

// ARIB STD-B67 OETF; `e` scene-linear, 1.0 = peak
float hlgEncode(float e)
{
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::max(e, 0.f);
    return e <= 1.f / 12 ? std::sqrt(3 * e) : a * std::log(12 * e - b) + c;
}

// BT.709 -> BT.2020 primaries (BT.2087), linear light
constexpr float kToBt2020[3][3] = {
    { 0.6274f, 0.3293f, 0.0433f },
    { 0.0691f, 0.9195f, 0.0114f },
    { 0.0164f, 0.0880f, 0.8956f },
};

// HLG reference white (BT.2408: 75% signal) as scene-linear light
constexpr float kHlgWhite = 0.2647f;

} // namespace

void OverlayEncoder::buildTables(Mpv::Transfer transfer, float whiteNits)
{
    m_transfer = transfer;
    m_white = whiteNits;
    for (int i = 0; i < 256; i++)
        m_toLinear[i] = srgbToLinear(i / 255.f);
    m_encode.resize(kSteps + 1);
    for (int i = 0; i <= kSteps; i++) {
        const float lin = float(i) / kSteps;
        const float v = transfer == Mpv::Transfer::Pq ? pqEncode(lin * whiteNits)
                                                      : hlgEncode(lin * kHlgWhite);
        m_encode[i] = char(std::clamp(int(std::lround(v * 255)), 0, 255));
    }
}

QImage OverlayEncoder::encode(const QImage &src, Mpv::Transfer transfer, float whiteNits)
{
    if (transfer == Mpv::Transfer::Sdr || src.isNull())
        return src;
    if (transfer != m_transfer || whiteNits != m_white)
        buildTables(transfer, whiteNits);

    Q_ASSERT(src.format() == QImage::Format_ARGB32_Premultiplied);
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    const auto *enc = reinterpret_cast<const uchar *>(m_encode.constData());
    // UI images are mostly flat fills: remember the last conversion
    QRgb lastIn = 0, lastOut = 0;
    for (int y = 0; y < src.height(); y++) {
        const auto *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        auto *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); x++) {
            const QRgb px = in[x];
            if (px == lastIn) {
                o[x] = lastOut;
                continue;
            }
            const int a = qAlpha(px);
            QRgb res = 0;
            if (a) {
                // un-premultiply, convert, re-premultiply on the encoded
                // values (mpv blends in the encoded space)
                const float r = m_toLinear[std::min(255, qRed(px) * 255 / a)];
                const float g = m_toLinear[std::min(255, qGreen(px) * 255 / a)];
                const float b = m_toLinear[std::min(255, qBlue(px) * 255 / a)];
                int c[3];
                for (int i = 0; i < 3; i++) {
                    const float l = kToBt2020[i][0] * r + kToBt2020[i][1] * g + kToBt2020[i][2] * b;
                    const int idx = std::clamp(int(l * kSteps + 0.5f), 0, kSteps);
                    c[i] = (enc[idx] * a + 127) / 255;
                }
                res = qRgba(c[0], c[1], c[2], a);
            }
            lastIn = px;
            lastOut = res;
            o[x] = res;
        }
    }
    return out;
}
