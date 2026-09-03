// fv2_limiter.hpp -- the soft ceiling applied to everything the engine renders.
//
// This lives in a header shared by the host and by engine_test on purpose. The
// voices are trimmed to a target RMS rather than a target peak, because these
// voices have peaks 15 to 20 dB above their RMS and holding the peak down left
// them several dB quieter than a SAPI voice is expected to be. So peaks above
// the ceiling are expected, and the limiter is what makes that safe -- which
// means a test that checks levels has to apply the *same* limiter the product
// applies, not a copy of it that can drift.
//
// Measured on a demanding sentence at the shipping trims, this touches between
// 0.01% and 0.11% of samples.

#pragma once

#include <vector>

namespace fv2 {

// Above this, samples are compressed rather than clipped.
const int kLimiterKnee = 29000;
// The hard ceiling the compressed samples land under.
const int kLimiterCeiling = 32700;

inline int limitSample(int v)
{
    const int a = v < 0 ? -v : v;
    if (a <= kLimiterKnee) return v;
    const int over = a - kLimiterKnee;
    const int comp = kLimiterKnee +
                     (over * (kLimiterCeiling - kLimiterKnee)) / (32768 - kLimiterKnee);
    return v < 0 ? -comp : comp;
}

inline void limit(std::vector<unsigned char>& pcm)
{
    if (pcm.size() < 2) return;
    short* s = reinterpret_cast<short*>(&pcm[0]);
    const size_t n = pcm.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        s[i] = static_cast<short>(limitSample(s[i]));
    }
}

}  // namespace fv2
