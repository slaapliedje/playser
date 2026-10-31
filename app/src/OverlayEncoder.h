// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>

#include "Mpv.h"

// mpv composites the UI overlay in the display's colorspace (our patched
// mpv, overlay-colorspace=display) or, stock, in the video's (vo_gpu_next,
// "Infer bitmap colorspace from source"); either way an sRGB UI must be
// re-encoded for an HDR one or its white lands at the display's peak
// (docs/spike-results.md, 0d). This converts premultiplied sRGB to
// premultiplied BT.2020 PQ/HLG, with SDR white at `whiteNits`.
class OverlayEncoder
{
public:
    // Returns `src` unchanged for Transfer::Sdr.
    QImage encode(const QImage &src, Mpv::Transfer transfer, float whiteNits = 203.f);

private:
    void buildTables(Mpv::Transfer transfer, float whiteNits);

    Mpv::Transfer m_transfer = Mpv::Transfer::Sdr;
    float m_white = 0;
    float m_toLinear[256];
    // linear BT.2020 (1.0 = SDR white) quantized to kSteps -> 8-bit code
    static constexpr int kSteps = 1 << 16;
    QByteArray m_encode;
};
