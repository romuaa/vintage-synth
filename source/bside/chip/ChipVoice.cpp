// One chip's whole analog chain (ChipVoice.h; docs/architecture.md 8.19.4). Reachable from the audio thread: no
// allocation, no locks, no exceptions; every loop is bounded (kBlock samples per inner block, kMaxPending writes,
// kMaxConfigs configurations, the half-band filters' pairs).

#include "bside/chip/ChipVoice.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace vs::bside::chip
{
namespace
{
constexpr double kPassHz = ChipVoice::kAntiAliasPassHz;
constexpr double kDeviation = ChipVoice::kAntiAliasDeviation;

bool sameBits (double a, double b) noexcept { return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b); }

/** Model A's output function with blended coefficients (Filter::outputShape's form). */
double shapeWith (const std::array<double, 5>& p, double lp, double lo, double hi) noexcept
{
    const double v = lp < lo ? lo : (lp > hi ? hi : lp);
    return ((((p[4] * v + p[3]) * v + p[2]) * v + p[1]) * v + p[0]) * v;
}

std::array<double, 5> shapeOf (const Filter::Shape& s) noexcept { return { s.p1, s.p2, s.p3, s.p4, s.p5 }; }
} // namespace

ChipVoice::ChipVoice() noexcept
{
    prepare (Config {});
}

void ChipVoice::prepare (const Config& config) noexcept
{
    cfg = config;
    c.prepare (cfg);
    c.renderBendVoices (true); // Model A's voice bend in the renderer ("ALIAS"; no effect on Model B)
    st = OutputStage {};
    st.prepare (cfg.model, cfg.internalRate, cfg.volumeDc);
    const HalfBandDesign pre = designHalfBand (cfg.internalRate, kPassHz, kDeviation);
    const HalfBandDesign twice = designHalfBand (2.0 * cfg.internalRate, kPassHz, ChipVoice::kTwiceDeviation);
    mixFir.prepare (pre);
    preAc.prepare (pre);
    preDc.prepare (pre);
    preDirect.prepare (pre);
    upLin.prepare (twice);
    upLow.prepare (twice);
    down.prepare (twice);
    if (cfg.model == Model::A)
    {
        preDelay = pre.delay();
        postDelay = upLin.pairCount() + down.pairCount() - 1;
        shapeDelay = upLin.pairCount() - 1;
        Filter probe;
        probe.prepare (Model::A, cfg.internalRate, cfg.variation);
        probe.setMode (Filter::kLowPass);
        const Filter::Shape s = probe.outputShapeCoefficients();
        lpMin = s.lpMin;
        lpMax = s.lpMax;
    }
    else
    {
        preDelay = 0;
        postDelay = pre.delay();
        shapeDelay = 0;
        lpMin = -1.0e9;
        lpMax = 1.0e9;
    }
    latency = Chip::kLatencySamples + preDelay + postDelay;
    reset();
}

void ChipVoice::setFilter (Filter* f) noexcept
{
    filter = f;
    if (filter != nullptr)
    {
        filter->setCutoff (cutoffValue);
        filter->setResonance (resonanceNow >> 4);
        filter->setMode (modeVolumeNow);
        refreshStaticDc();
    }
}

void ChipVoice::reset() noexcept
{
    c.reset();
    st.reset();
    paramHead = paramCount = configHead = configCount = 0;
    nConfigs = 0;
    overflows = 0;
    cutoffValue = 0.0f;
    lastCutoffChange = 0;
    cutoffChanged = false;
    moving = false;
    resonanceNow = 0;
    modeVolumeNow = 0;
    lastFv = lastDv = -1.0;
    rounding = render::OutputRounding {};
    mixFir.reset();
    preAc.reset();
    preDc.reset();
    preDirect.reset();
    upLin.reset();
    upLow.reset();
    down.reset();
    freshPre = freshPost = true;
    preDcNow = 0.0;
    postRing = {};
    countRing = {};
    shapeRing = {};
    postPos = countPos = 0;
    lastFilterIn = lastFilterOut = 0.0;
    injected = 0;
    st.setModeVolume (0);
    st.setRouting (0);
    if (filter != nullptr)
    {
        filter->reset();
        setFilter (filter);
    }
}

// ---- inputs ----

void ChipVoice::write (std::uint8_t reg, std::uint8_t value) noexcept
{
    const auto r = static_cast<std::uint8_t> (reg & reg::kAddressMask);
    c.write (reg, value);
    if (r == reg::cutoffLo || r == reg::cutoffHi)
    {
        const auto fc = static_cast<float> ((c.registerValue (reg::cutoffLo) & 7) | (c.registerValue (reg::cutoffHi) << 3));
        queue (reg::cutoffLo, 0, fc);
    }
    else if (r == reg::resonanceRouting || r == reg::modeVolume)
        queue (r, value, 0.0f);
}

void ChipVoice::setOscillator (int n, const OscInput& in) noexcept { c.setOscillator (n, in); }

void ChipVoice::setFilterInput (const FilterInput& in) noexcept
{
    c.setFilter (in);
    queue (reg::cutoffLo, 0, c.cutoffSmooth());
    queue (reg::resonanceRouting, c.registerValue (reg::resonanceRouting), 0.0f);
    queue (reg::modeVolume, c.registerValue (reg::modeVolume), 0.0f);
}

void ChipVoice::powerOnKeepingTime() noexcept
{
    c.powerOnKeepingTime();
    queue (reg::cutoffLo, 0, 0.0f);
    queue (reg::resonanceRouting, 0, 0.0f);
    queue (reg::modeVolume, 0, 0.0f);
}

void ChipVoice::queue (std::uint8_t reg, std::uint8_t value, float cutoffNew) noexcept
{
    // Only what the filter or the stage read: the cutoff, the resonance nibble, the volume and mode bits (the routing
    // and oscillator 3's mute move in the renderer). The last queued value decides whether a write changes anything.
    Pending p;
    p.reg = reg;
    p.value = value;
    p.cutoff = cutoffNew;
    const Chip::EventTime t = c.renderEventTime();
    if (t.beforeStart)
    {
        applyNow (p);
        return;
    }
    p.sample = t.sample;
    c.unitStepTaps (t.base, p.taps, p.first);
    // volume, mode and resonance blend configurations (band-limited at the write's place); the cutoff is a parameter
    const bool config = reg == reg::modeVolume || reg == reg::resonanceRouting;
    auto& q = config ? configs : params;
    int& head = config ? configHead : paramHead;
    int& count = config ? configCount : paramCount;
    if (count == kMaxPending)
    {
        // full (ChipVoice.h "QUEUE"): the oldest entry is applied at once, as if its sample had come; order and values kept
        ++overflows;
        const Pending& oldest = q[static_cast<std::size_t> (head)];
        if (config)
            beginConfig (oldest);
        else
            applyParam (oldest, static_cast<std::int64_t> (oldest.sample));
        head = (head + 1) % kMaxPending;
        --count;
    }
    q[static_cast<std::size_t> ((head + count) % kMaxPending)] = p;
    ++count;
}

void ChipVoice::applyNow (const Pending& p) noexcept
{
    if (p.reg == reg::modeVolume)
    {
        modeVolumeNow = p.value;
        st.setModeVolume (p.value);
        if (filter != nullptr)
            filter->setMode (p.value);
        for (int i = 0; i < nConfigs; ++i)
        {
            auto& o = old[static_cast<std::size_t> (i)];
            o.stage.setModeVolume (p.value);
            if (filter != nullptr)
                o.filter.setMode (p.value); // Shared: the copies are never run
        }
        refreshStaticDc();
        return;
    }
    if (p.reg == reg::resonanceRouting)
    {
        applyParam (p, 0);
        return;
    }
    cutoffValue = p.cutoff; // part of the starting state: not a movement
    if (filter != nullptr)
        filter->setCutoff (cutoffValue);
}

void ChipVoice::applyParam (const Pending& p, std::int64_t q) noexcept
{
    if (p.reg == reg::resonanceRouting)
    {
        const auto r = static_cast<std::uint8_t> (p.value & 0xf0);
        if (r == resonanceNow)
            return;
        resonanceNow = r;
        if (filter != nullptr)
        {
            filter->setResonance (r >> 4);
            for (int i = 0; i < nConfigs; ++i)
                old[static_cast<std::size_t> (i)].filter.setResonance (r >> 4);
        }
        refreshStaticDc();
        return;
    }
    // the cutoff
    if (! (p.cutoff < cutoffValue || p.cutoff > cutoffValue))
        return;
    cutoffValue = p.cutoff;
    if (cutoffChanged && q - lastCutoffChange < kCutoffSettle)
        moving = true;
    lastCutoffChange = q;
    cutoffChanged = true;
    if (! moving && filter != nullptr)
    {
        filter->setCutoff (cutoffValue);
        for (int i = 0; i < nConfigs; ++i)
            old[static_cast<std::size_t> (i)].filter.setCutoff (cutoffValue);
    }
}

void ChipVoice::beginConfig (const Pending& p) noexcept
{
    const bool resonanceWrite = p.reg == reg::resonanceRouting;
    if (resonanceWrite && ((p.value & 0xf0) == resonanceNow || filter == nullptr))
    {
        resonanceNow = static_cast<std::uint8_t> (p.value & 0xf0); // unchanged, or Shared (the owner's filter): nothing to blend
        return;
    }
    if (! resonanceWrite && (p.value & 0x7f) == (modeVolumeNow & 0x7f))
    {
        modeVolumeNow = p.value; // only the mute bit (the renderer's): nothing to blend
        st.setModeVolume (p.value);
        return;
    }
    if (nConfigs == kMaxConfigs)
    {
        // bounded: the oldest configuration is retired at once
        for (int i = 1; i < nConfigs; ++i)
            old[static_cast<std::size_t> (i - 1)] = old[static_cast<std::size_t> (i)];
        --nConfigs;
    }
    Old& o = old[static_cast<std::size_t> (nConfigs)];
    o.stage = st;
    if (filter != nullptr)
        o.filter = *filter;
    o.sample = p.sample;
    o.first = p.first;
    o.taps = p.taps;
    ++nConfigs;
    if (resonanceWrite)
    {
        // the copy keeps the old resonance and runs on beside the filter over the window (its poles differ: the two
        // states part, which the blend's weights confine to the window)
        resonanceNow = static_cast<std::uint8_t> (p.value & 0xf0);
        filter->setResonance (resonanceNow >> 4);
    }
    else
    {
        modeVolumeNow = p.value;
        st.setModeVolume (p.value);
        if (filter != nullptr)
            filter->setMode (p.value);
    }
    refreshStaticDc();
}

void ChipVoice::refreshStaticDc() noexcept
{
    // with volume DC off the stage subtracts what the filter makes of its bus's static DC (OutputStage "volumeDc off"):
    // Model B the voices' DC at the counts, Model A the pre-filtered voices' DC the filter is fed; Shared: the owner's
    // figure, per sample (chainSample)
    if (filter == nullptr)
        return;
    const bool a = cfg.model == Model::A;
    st.setFilterStaticDc (filter->staticOutput (a ? preDcNow : st.staticFilterBusDc()));
    for (int i = 0; i < nConfigs; ++i)
    {
        auto& o = old[static_cast<std::size_t> (i)];
        o.stage.setFilterStaticDc (o.filter.staticOutput (a ? preDcNow : o.stage.staticFilterBusDc()));
    }
}

// ---- per output sample ----

double ChipVoice::weightAt (std::int64_t q, std::uint64_t sample, int first, const std::array<double, render::kKernelTaps>& taps) noexcept
{
    const auto s = static_cast<std::int64_t> (sample);
    const std::int64_t k = q - (s + first);
    double w = q >= s ? 1.0 : 0.0;
    if (k >= 0 && k < render::kKernelTaps)
        w += taps[static_cast<std::size_t> (k)];
    return w;
}

void ChipVoice::advanceTo (std::int64_t q) noexcept
{
    // parameters from their own sample on
    while (paramCount > 0 && static_cast<std::int64_t> (params[static_cast<std::size_t> (paramHead)].sample) <= q)
    {
        applyParam (params[static_cast<std::size_t> (paramHead)], q);
        paramHead = (paramHead + 1) % kMaxPending;
        --paramCount;
    }
    // configurations from the first sample of their window on
    while (configCount > 0)
    {
        const Pending& p = configs[static_cast<std::size_t> (configHead)];
        if (static_cast<std::int64_t> (p.sample) + p.first > q)
            break;
        beginConfig (p);
        configHead = (configHead + 1) % kMaxPending;
        --configCount;
    }
    // the oldest configuration retires once its transition is complete
    while (nConfigs > 0)
    {
        const Old& o = old[0];
        if (q < static_cast<std::int64_t> (o.sample) + o.first + render::kKernelTaps || q < static_cast<std::int64_t> (o.sample))
            break;
        for (int i = 1; i < nConfigs; ++i)
            old[static_cast<std::size_t> (i - 1)] = old[static_cast<std::size_t> (i)];
        --nConfigs;
    }
    // a moving cutoff that has stood still long enough: the stored cutoff again (process)
    if (moving && q - lastCutoffChange >= kCutoffSettle)
    {
        moving = false;
        if (filter != nullptr)
        {
            filter->setCutoff (cutoffValue);
            for (int i = 0; i < nConfigs; ++i)
                old[static_cast<std::size_t> (i)].filter.setCutoff (cutoffValue);
        }
    }
}

double ChipVoice::runFilter (Filter& f, double in, double& lowPass) const noexcept
{
    return moving ? f.processWithCutoffLinearDouble (in, cutoffValue, lowPass) : f.processLinearDouble (in, lowPass);
}

void ChipVoice::prepareBlock (int n) noexcept
{
    if (cfg.model != Model::A)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto j = static_cast<std::size_t> (i);
            cF[j] = fv[j];
            cD[j] = dv[j];
        }
        return;
    }
    // Model A: both buses through the pre-filter (the filter bus's audio and envelopes, its voices' DC, and the direct
    // bus's whole sum with the counts of its own sample); the counts the stage uses wait for the delayed signal
    const double vdc = OutputStage::voiceDc (Model::A);
    for (int i = 0; i < n; ++i)
    {
        const auto j = static_cast<std::size_t> (i);
        const double ac = st.filterInputAc (fa[j], fe[j]);
        const double dc = vdc * fv[j];
        const double di = st.directSumWith (da[j], de[j], dv[j]);
        if (freshPre)
        {
            preAc.prime (ac);
            preDc.prime (dc);
            preDirect.prime (di);
            for (auto& r : countRing)
                r = { fv[j], dv[j] };
            freshPre = false;
        }
        pAc[j] = preAc.process (ac);
        pDi[j] = preDirect.process (di);
        // the voices' DC moves only at routing writes: once it has stood still for the filter's length the FIR returns
        // it as it is and skips its taps (HalfBandFir: a constant input), Per Note and Shared alike
        pDc[j] = preDc.process (dc);
        countPos = (countPos + 1) & (kDelayRing - 1);
        countRing[static_cast<std::size_t> (countPos)] = { fv[j], dv[j] };
        const auto& late = countRing[static_cast<std::size_t> ((countPos + kDelayRing - preDelay) & (kDelayRing - 1))];
        cF[j] = late[0];
        cD[j] = late[1];
    }
}

double ChipVoice::chainSample (int i, std::int64_t q, bool shared, const Filter::Shape* sharedShape, const double* sharedLinear, const double* sharedLowPass,
                               const double* sharedStaticOut, int chips) noexcept
{
    const auto j = static_cast<std::size_t> (i);
    const bool modelA = cfg.model == Model::A;
    const std::int64_t t = q - preDelay; // the filter's signal time
    advanceTo (t);
    if (cF[j] < lastFv || cF[j] > lastFv || cD[j] < lastDv || cD[j] > lastDv)
    {
        lastFv = cF[j];
        lastDv = cD[j];
        st.setVoices (cF[j], cD[j]);
        for (int k = 0; k < nConfigs; ++k)
            old[static_cast<std::size_t> (k)].stage.setVoices (cF[j], cD[j]);
        refreshStaticDc();
    }

    // the filter: its linear output and low-pass value per configuration (Per Note) or the owner's (Shared); the share
    double lin = 0.0, lp = 0.0, so = 0.0, pScale = 1.0;
    std::array<double, kMaxConfigs> linOld {};
    Filter::Shape shapeNow {}; // the output function of the filter in effect (Per Note: the voice's; Shared: the owner's)
    if (! shared)
    {
        if (modelA && ! sameBits (pDc[j], preDcNow))
        {
            preDcNow = pDc[j];
            refreshStaticDc();
        }
        const double in = modelA ? pAc[j] + pDc[j] : st.filterInput (fa[j], fe[j]);
        lin = runFilter (*filter, in, lp);
        for (int k = 0; k < nConfigs; ++k)
        {
            double lpk = 0.0;
            linOld[static_cast<std::size_t> (k)] = runFilter (old[static_cast<std::size_t> (k)].filter, in, lpk);
        }
        lastFilterIn = in;
        lastFilterOut = lin + filter->outputShape (lp);
        shapeNow = filter->outputShapeCoefficients();
    }
    else
    {
        if (sharedShape != nullptr)
            shapeNow = sharedShape[j];
        so = sharedStaticOut[j];
        st.setFilterStaticDc (so);
        for (int k = 0; k < nConfigs; ++k)
            old[static_cast<std::size_t> (k)].stage.setFilterStaticDc (so);
        lin = sharedLinear[j];
        lp = sharedLowPass != nullptr ? sharedLowPass[j] : 0.0;
        if (chips > 1)
        {
            // this chip's share (OutputStage "SHARED"): the static output in full, 1/n of the rest (the output function's
            // part through pScale)
            const double n = static_cast<double> (chips);
            lin = (lin - so) / n + so;
            pScale = 1.0 / n;
        }
        for (int k = 0; k < nConfigs; ++k)
            linOld[static_cast<std::size_t> (k)] = lin;
    }
    const double di = modelA ? pDi[j] : st.directSumWith (da[j], de[j], dv[j]);

    // the configurations blended at the mixer's input (gain x sum) and after the curve (its coefficients)
    double x = 0.0, pa = 0.0, pb = 0.0, pp = 0.0;
    std::array<double, 5> shape {};
    if (nConfigs == 0)
    {
        const double g = st.gainFactor();
        x = g * (lin + di);
        pa = st.postScale();
        pb = st.postOffset();
        pp = st.postGain();
        if (modelA)
        {
            const auto p = shapeOf (shapeNow);
            for (std::size_t m = 0; m < 5; ++m)
                shape[m] = g * pScale * p[m];
        }
    }
    else
    {
        // y = sum over the configurations of (h_i - h_i+1) y_i: h_i the unit step into configuration i (h_0 = 1), the
        // current one last
        auto add = [&] (double w, const OutputStage& stage, double l, const Filter::Shape& f) {
            const double g = stage.gainFactor();
            x += w * g * (l + di);
            pa += w * stage.postScale();
            pb += w * stage.postOffset();
            pp += w * stage.postGain();
            if (modelA)
            {
                const auto p = shapeOf (f);
                for (std::size_t m = 0; m < 5; ++m)
                    shape[m] += w * g * pScale * p[m];
            }
        };
        const Old& newest = old[static_cast<std::size_t> (nConfigs - 1)];
        double hNext = weightAt (t, newest.sample, newest.first, newest.taps);
        add (hNext, st, lin, shapeNow);
        for (int k = nConfigs - 1; k >= 0; --k)
        {
            const Old& o = old[static_cast<std::size_t> (k)];
            const Old& before = old[static_cast<std::size_t> (k > 0 ? k - 1 : 0)];
            const double h = k > 0 ? weightAt (t, before.sample, before.first, before.taps) : 1.0;
            add (h - hNext, o.stage, linOld[static_cast<std::size_t> (k)], shared ? shapeNow : o.filter.outputShapeCoefficients());
            hNext = h;
        }
    }

    // the anti-alias stages; the post-curve coefficients delayed with the signal
    if (freshPost)
    {
        mixFir.prime (x);
        upLin.prime (x);
        upLow.prime (lp);
        down.prime (st.mixerCurve (x + shapeWith (shape, lp, lpMin, lpMax)));
        for (auto& r : postRing)
            r = { pa, pb, pp };
        for (auto& r : shapeRing)
            r = shape;
        freshPost = false;
    }
    postPos = (postPos + 1) & (kDelayRing - 1);
    postRing[static_cast<std::size_t> (postPos)] = { pa, pb, pp };
    const auto& post = postRing[static_cast<std::size_t> ((postPos + kDelayRing - postDelay) & (kDelayRing - 1))];
    double m = 0.0;
    if (! modelA)
        m = st.mixerCurve (mixFir.process (x));
    else
    {
        // the coefficients with the low-pass value they act on: the interpolator's second output is the input
        // shapeDelay samples ago, its first lies half a sample earlier, so it takes the mean of the coefficients of
        // shapeDelay and shapeDelay + 1 samples ago (the interpolated coefficients for the interpolated sample)
        shapeRing[static_cast<std::size_t> (postPos)] = shape;
        const auto& sh = shapeRing[static_cast<std::size_t> ((postPos + kDelayRing - shapeDelay) & (kDelayRing - 1))];
        const auto& shBefore = shapeRing[static_cast<std::size_t> ((postPos + kDelayRing - shapeDelay - 1) & (kDelayRing - 1))];
        std::array<double, 5> shMid {};
        for (std::size_t k = 0; k < 5; ++k)
            shMid[k] = 0.5 * (sh[k] + shBefore[k]);
        double l0 = 0.0, l1 = 0.0, p0 = 0.0, p1 = 0.0;
        upLin.process (x, l0, l1);
        upLow.process (lp, p0, p1);
        const double m0 = st.mixerCurve (l0 + shapeWith (shMid, p0, lpMin, lpMax));
        const double m1 = st.mixerCurve (l1 + shapeWith (sh, p1, lpMin, lpMax));
        m = down.process (m0, m1);
    }
    const double mix = post[0] * m + post[1];
    return board ? st.board (mix, post[2]) : mix * post[2];
}

Chip::BusBlock ChipVoice::block() noexcept
{
    return { fa.data(), da.data(), fe.data(), de.data(), fv.data(), dv.data() };
}

int ChipVoice::renderUntil (double* out, int maxSamples, std::uint64_t untilCycle) noexcept
{
    if (filter == nullptr)
        return 0;
    int done = 0;
    while (done < maxSamples)
    {
        const int want = std::min (kBlock, maxSamples - done);
        blockStart = c.renderSampleIndex();
        const int n = c.renderBusesUntil (block(), want, untilCycle);
        prepareBlock (n);
        for (int i = 0; i < n; ++i)
            out[done + i] += chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingLag, false, nullptr, nullptr, nullptr, nullptr, 1);
        done += n;
        if (n < want)
            break;
    }
    return done;
}

void ChipVoice::render (double* out, int numInternalSamples) noexcept
{
    renderUntil (out, numInternalSamples, ~std::uint64_t { 0 });
}

int ChipVoice::renderUntil (float* out, int maxSamples, std::uint64_t untilCycle) noexcept
{
    if (filter == nullptr)
        return 0;
    int done = 0;
    while (done < maxSamples)
    {
        const int want = std::min (kBlock, maxSamples - done);
        blockStart = c.renderSampleIndex();
        const int n = c.renderBusesUntil (block(), want, untilCycle);
        prepareBlock (n);
        for (int i = 0; i < n; ++i)
            out[done + i] += rounding.next (chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingLag, false, nullptr, nullptr, nullptr, nullptr, 1));
        done += n;
        if (n < want)
            break;
    }
    return done;
}

int ChipVoice::renderBusesUntil (int maxSamples, std::uint64_t untilCycle) noexcept
{
    blockStart = c.renderSampleIndex();
    const int n = c.renderBusesUntil (block(), std::min (kBlock, maxSamples), untilCycle);
    prepareBlock (n);
    return n;
}

double ChipVoice::filterInputAc (int i) const noexcept
{
    const auto j = static_cast<std::size_t> (i);
    return cfg.model == Model::A ? pAc[j] : st.filterInputAc (fa[j], fe[j]);
}

double ChipVoice::staticFilterBusDc (int i) const noexcept
{
    const auto j = static_cast<std::size_t> (i);
    return cfg.model == Model::A ? pDc[j] : OutputStage::voiceDc (cfg.model) * fv[j];
}

void ChipVoice::finishShared (const Filter::Shape* sharedShape, const double* sharedLinear, const double* sharedLowPass, const double* filterStaticOut,
                              int chips, double* out, int n) noexcept
{
    for (int i = 0; i < n && i < kBlock; ++i)
        out[i] += chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingLag, true, sharedShape, sharedLinear, sharedLowPass, filterStaticOut, chips);
}

void ChipVoice::processBuses (const Chip::BusBlock& in, int n, double* out) noexcept
{
    if (filter == nullptr)
        return;
    int done = 0;
    while (done < n)
    {
        const int k = std::min (kBlock, n - done);
        for (int i = 0; i < k; ++i)
        {
            const auto j = static_cast<std::size_t> (i);
            const auto s = static_cast<std::size_t> (done + i);
            fa[j] = in.filterAudio[s];
            da[j] = in.directAudio[s];
            fe[j] = in.filterEnvelope[s];
            de[j] = in.directEnvelope[s];
            fv[j] = in.filterVoices[s];
            dv[j] = in.directVoices[s];
        }
        prepareBlock (k);
        for (int i = 0; i < k; ++i)
            out[done + i] += chainSample (i, static_cast<std::int64_t> (injected++) - kRingLag, false, nullptr, nullptr, nullptr, nullptr, 1);
        done += k;
    }
}
} // namespace vs::bside::chip
