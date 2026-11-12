#pragma once

// STUDY (cross-mod aliasing, branch study/crossmod-aliasing): experimental, not production.
// One sample of the VCO-2 -> VCO-1 pair with the proposed cross-mod path, shared by
// Voice (study mode 1) and the study tests so both measure the same code.

#include "dsp/FastMath.h"
#include "dsp/Oscillator.h"

namespace vs::dsp::study
{
struct FmState
{
    float prevDt = 0.0f; // VCO-1 increment at the previous sample point
    bool primed = false; // false: no previous sample (note start, cross-mod just switched on)
};

/**
    VCO-2 runs as usual. For a saw/square/pulse VCO-2, the FM uses its naive waveform (no
    band-limiting, no delay) and its edge position inside the sample; the increment of VCO-1 is
    piecewise linear inside the sample and jumps at the edge, and VCO-1 corrects the jump on its
    own output (BLAMP + curvature). Sine/triangle VCO-2 keep the band-limited output (as
    production) with a piecewise-linear increment. Sync: VCO-1 resets at VCO-2's wrap.
    @param o2   out: VCO-2's band-limited output (for the mixer)
    @return     VCO-1's output
*/
inline float crossModPairSample (Oscillator& vco1, Oscillator& vco2, FmState& st, float dt2, float dt1Raw, float width,
                                 float oct, bool sync, float minDt, float maxDt, float& o2) noexcept
{
    const auto w2 = vco2.getWave();
    const float q0 = vco2.getPhase();
    float wrap2 = -1.0f, unused = -1.0f;
    o2 = vco2.process (dt2, width, -1.0f, wrap2);
    Oscillator::FmSeg g;
    float dtB;
    if (w2 == Wave::Saw || w2 == Wave::Square || w2 == Wave::Pulse)
    {
        dtB = clamp (dt1Raw * fast::exp2 (oct * vco2.naiveAt (vco2.getPhase(), width)), minDt, maxDt);
        const float a0 = st.primed ? st.prevDt : dtB;
        float se = 2.0f, pre = 0.0f, post = 0.0f;
        if (wrap2 >= 0.0f)
        {
            se = 1.0f - wrap2;
            pre = w2 == Wave::Saw ? 1.0f : -1.0f;
            post = -pre;
        }
        else if (w2 != Wave::Saw)
        {
            // Falling edge of square/pulse (a fall after a wrap in the same sample needs
            // dt2 > width: not handled in this prototype).
            const float wq = w2 == Wave::Pulse ? width : 0.5f;
            if (q0 < wq && q0 + dt2 >= wq)
            {
                se = (wq - q0) / dt2;
                pre = 1.0f;
                post = -1.0f;
            }
        }
        if (se <= 1.0f)
        {
            const float dPre = clamp (dt1Raw * fast::exp2 (oct * pre), minDt, maxDt);
            const float dPost = clamp (dt1Raw * fast::exp2 (oct * post), minDt, maxDt);
            g.a1 = a0;
            g.b1 = se > 1.0e-6f ? (dPre - a0) / se : 0.0f;
            g.se = se;
            g.b2 = se < 1.0f - 1.0e-6f ? (dtB - dPost) / (1.0f - se) : 0.0f;
            g.a2 = dPost - g.b2 * se;
        }
        else
        {
            g.a1 = a0;
            g.b1 = dtB - a0;
        }
    }
    else
    {
        dtB = clamp (dt1Raw * fast::exp2 (oct * o2), minDt, maxDt);
        const float a0 = st.primed ? st.prevDt : dtB;
        g.a1 = a0;
        g.b1 = dtB - a0;
    }
    st.prevDt = dtB;
    st.primed = true;
    return vco1.processFm (g, width, sync ? wrap2 : -1.0f, unused);
}
} // namespace vs::dsp::study
