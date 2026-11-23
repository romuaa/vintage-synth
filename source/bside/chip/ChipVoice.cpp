// One chip's whole analog chain (ChipVoice.h; docs/architecture.md 8.19.4). Reachable from the audio thread: no
// allocation, no locks, no exceptions; every loop is bounded (kBlock samples per inner block, kMaxPending writes,
// kMaxConfigs configurations).

#include "bside/chip/ChipVoice.h"

#include <algorithm>
#include <cmath>

namespace vs::bside::chip
{
namespace
{
constexpr std::int64_t kRingDelay = Chip::kLatencySamples + 1; // a ring sample is output this many samples after it completes
}

ChipVoice::ChipVoice() noexcept
{
    prepare (Config {});
}

void ChipVoice::prepare (const Config& config) noexcept
{
    cfg = config;
    c.prepare (cfg);
    latency = Chip::kLatencySamples; // the renderer's; a delaying stage before the filter adds its own here
    st = OutputStage {};
    st.prepare (cfg.model, cfg.internalRate, cfg.volumeDc);
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
    const bool config = reg == reg::modeVolume;
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
    if ((p.value & 0x7f) == (modeVolumeNow & 0x7f))
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
    modeVolumeNow = p.value;
    st.setModeVolume (p.value);
    if (filter != nullptr)
        filter->setMode (p.value);
    refreshStaticDc();
}

void ChipVoice::refreshStaticDc() noexcept
{
    // with volume DC off the stage subtracts what the filter makes of its bus's static DC (OutputStage "volumeDc off");
    // Shared: the owner's figure, per sample (chainSample)
    if (filter == nullptr)
        return;
    st.setFilterStaticDc (filter->staticOutput (st.staticFilterBusDc()));
    for (int i = 0; i < nConfigs; ++i)
    {
        auto& o = old[static_cast<std::size_t> (i)];
        o.stage.setFilterStaticDc (o.filter.staticOutput (o.stage.staticFilterBusDc()));
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

double ChipVoice::runFilter (Filter& f, double in) const noexcept
{
    return moving ? f.processWithCutoffDouble (in, cutoffValue) : f.processDouble (in);
}

double ChipVoice::chainSample (int i, std::int64_t q, const double* sharedOut, const double* sharedStaticOut, int chips) noexcept
{
    const auto j = static_cast<std::size_t> (i);
    advanceTo (q);
    if (fv[j] < lastFv || fv[j] > lastFv || dv[j] < lastDv || dv[j] > lastDv)
    {
        lastFv = fv[j];
        lastDv = dv[j];
        st.setVoices (fv[j], dv[j]);
        for (int k = 0; k < nConfigs; ++k)
            old[static_cast<std::size_t> (k)].stage.setVoices (fv[j], dv[j]);
        refreshStaticDc();
    }
    double fo = 0.0;
    if (sharedOut != nullptr)
    {
        // Shared: the static output of this sample (the owner's, for the static DC it fed in), set where it changes
        const double so = sharedStaticOut[j];
        st.setFilterStaticDc (so);
        for (int k = 0; k < nConfigs; ++k)
            old[static_cast<std::size_t> (k)].stage.setFilterStaticDc (so);
        fo = OutputStage::sharedShare (sharedOut[j], so, chips);
    }
    else
        fo = runFilter (*filter, st.filterInput (fa[j], fe[j]));
    double mix = st.mixerOutput (fo, da[j], de[j]);
    double post = st.postGain();
    if (nConfigs > 0)
    {
        // y = sum over the configurations of (h_i - h_i+1) y_i: h_i the unit step into configuration i (h_0 = 1), the
        // current one last
        double hNext = weightAt (q, old[static_cast<std::size_t> (nConfigs - 1)].sample, old[static_cast<std::size_t> (nConfigs - 1)].first,
                                 old[static_cast<std::size_t> (nConfigs - 1)].taps);
        double m = hNext * mix, p = hNext * post;
        for (int k = nConfigs - 1; k >= 0; --k)
        {
            Old& o = old[static_cast<std::size_t> (k)];
            const double h = k > 0 ? weightAt (q, old[static_cast<std::size_t> (k - 1)].sample, old[static_cast<std::size_t> (k - 1)].first,
                                               old[static_cast<std::size_t> (k - 1)].taps)
                                   : 1.0;
            const double w = h - hNext;
            const double fok = sharedOut != nullptr ? fo : runFilter (o.filter, o.stage.filterInput (fa[j], fe[j]));
            m += w * o.stage.mixerOutput (fok, da[j], de[j]);
            p += w * o.stage.postGain();
            hNext = h;
        }
        mix = m;
        post = p;
    }
    return board ? st.board (mix, post) : mix * post;
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
        for (int i = 0; i < n; ++i)
            out[done + i] += chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingDelay, nullptr, nullptr, 1);
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
        for (int i = 0; i < n; ++i)
            out[done + i] += rounding.next (chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingDelay, nullptr, nullptr, 1));
        done += n;
        if (n < want)
            break;
    }
    return done;
}

int ChipVoice::renderBusesUntil (int maxSamples, std::uint64_t untilCycle) noexcept
{
    blockStart = c.renderSampleIndex();
    return c.renderBusesUntil (block(), std::min (kBlock, maxSamples), untilCycle);
}

double ChipVoice::filterInputAc (int i) const noexcept
{
    const auto j = static_cast<std::size_t> (i);
    return st.filterInputAc (fa[j], fe[j]);
}

double ChipVoice::staticFilterBusDc (int i) const noexcept
{
    return OutputStage::voiceDc (cfg.model) * fv[static_cast<std::size_t> (i)];
}

void ChipVoice::finishShared (const double* sharedFilterOut, const double* filterStaticOut, int chips, double* out, int n) noexcept
{
    for (int i = 0; i < n && i < kBlock; ++i)
        out[i] += chainSample (i, static_cast<std::int64_t> (blockStart) + i - kRingDelay, sharedFilterOut, filterStaticOut, chips);
}
} // namespace vs::bside::chip
