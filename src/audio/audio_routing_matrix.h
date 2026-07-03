// computeRoutingMatrix — 2×N output-from-input downmix matrix per
// routing mode + source channel count, for SwrContext::swr_set_matrix.
//
// Factored out of audio_decoder.cpp so AudioChunkReader (the shuttle
// grain decoder) applies the same per-clip routing the main decoder
// uses — a 5.1 master shuttles with the same fold the user hears at
// 1x. Internal header (qcv_audio only); pulls the AudioRoutingMode
// enum from media_item.h.
//
// Returns nullopt when no custom matrix is needed (FFmpeg's default
// swr behavior is correct: mono→stereo, stereo passthrough, 6ch
// 5.1→stereo BS.775 fold). When set, the matrix is row-major:
// [L_out_row..., R_out_row...].

#pragma once

#include "project/media_item.h"   // AudioRoutingMode

#include <algorithm>
#include <optional>
#include <vector>

namespace qcv {

inline std::optional<std::vector<double>>
computeRoutingMatrix(int mode, int srcChannels)
{
    if (srcChannels <= 0) return std::nullopt;

    auto pluck = [srcChannels](int leftCh, int rightCh)
        -> std::optional<std::vector<double>>
    {
        if (leftCh  >= srcChannels) leftCh  = std::min(0, srcChannels - 1);
        if (rightCh >= srcChannels) rightCh = std::min(1, srcChannels - 1);
        std::vector<double> m(2 * srcChannels, 0.0);
        m[leftCh]                  = 1.0;   // L_out = ch[leftCh]
        m[srcChannels + rightCh]   = 1.0;   // R_out = ch[rightCh]
        return m;
    };

    // ITU-R BS.775 5.1→stereo coefficients, applied to the first
    // 6 source channels (assumes broadcast layout: L R C LFE Ls Rs).
    // LFE deliberately dropped (not contributing to either output);
    // adding -10 dB LFE is a follow-up if reviewers ask.
    auto downmix5_1 = [srcChannels]()
        -> std::optional<std::vector<double>>
    {
        if (srcChannels < 6) return std::nullopt;
        std::vector<double> m(2 * srcChannels, 0.0);
        // L_out = L + 0.707·C + 0.707·Ls
        m[0]                = 1.0;     // L
        m[2]                = 0.707;   // C
        m[4]                = 0.707;   // Ls
        // R_out = R + 0.707·C + 0.707·Rs
        m[srcChannels + 1]  = 1.0;     // R
        m[srcChannels + 2]  = 0.707;   // C
        m[srcChannels + 5]  = 0.707;   // Rs
        return m;
    };

    const auto routing = static_cast<AudioRoutingMode>(mode);
    switch (routing) {
    case AudioRoutingMode::Downmix5_1:
        return downmix5_1();   // nullopt if source has <6 channels →
                                // falls through to default behavior.
    case AudioRoutingMode::Stereo7_8:
        if (srcChannels >= 8) return pluck(6, 7);
        return std::nullopt;   // UI gates this; defensive fallback.
    case AudioRoutingMode::Auto:
    default:
        // Auto: defer to FFmpeg's defaults for 1/2/6 ch (mono→stereo,
        // direct, BS.775 downmix). For 8ch, default to the broadcast
        // stereo bounce on channels 7-8. For other counts (3,4,5,7,
        // 9+), pluck the first two channels — safest fallback.
        if (srcChannels == 1 || srcChannels == 2 || srcChannels == 6) {
            return std::nullopt;
        }
        if (srcChannels >= 8) return pluck(6, 7);
        return pluck(0, 1);
    }
}

} // namespace qcv
