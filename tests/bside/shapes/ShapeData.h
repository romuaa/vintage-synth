#pragma once

// Loading the committed shape measurements (tests/bside/shapes/: <name>.bstrace and <name>.bsref, the
// plans of ShapePlan.h) and measuring a set of shape tables against them. JUCE-free; used by the fit tool
// and by bside_trace.

#include "ShapePlan.h"

#include "bside/chip/CombinedWaveforms.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vs::bside::shapes
{
inline std::string slurp (const std::filesystem::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

struct LoadedPlan
{
    PlanSpec spec;
    std::vector<Sample> samples;
    std::vector<LatchEvent> latch; // the latch experiment only
    std::string error; // empty when loaded
};

/** Loads every plan of allPlans() from `dir`: the trace must equal the plan's text byte for byte and the
    reference must belong to it (SHA-256 and schedule). */
inline std::vector<LoadedPlan> loadAll (const std::filesystem::path& dir)
{
    std::vector<LoadedPlan> v;
    for (const auto& spec : allPlans())
    {
        LoadedPlan lp;
        lp.spec = spec;
        const Plan plan = buildPlan (spec);
        const auto traceText = slurp (dir / (spec.name + ".bstrace"));
        const auto refText = slurp (dir / (spec.name + ".bsref"));
        const auto tr = trace::readTrace (traceText);
        const auto rr = trace::readReference (refText);
        std::string why;
        const std::string_view tb (traceText);
        if (traceText != plan.text)
            lp.error = "trace differs from its plan";
        else if (! tr.ok)
            lp.error = "trace: " + tr.message();
        else if (! rr.ok)
            lp.error = "reference: " + rr.message();
        else if (! trace::referenceMatchesTrace (rr.reference, tr.trace, &tb, &why))
            lp.error = "reference does not belong to the trace: " + why;
        else if (! extract (spec, plan, tr.trace, rr.reference, lp.samples, why))
            lp.error = "extraction: " + why;
        else if (spec.kind == Kind::latch && ! extractLatch (plan, tr.trace, rr.reference, lp.latch, why))
            lp.error = "latch extraction: " + why;
        v.push_back (std::move (lp));
    }
    return v;
}

/** Agreement of tables with samples: over the observed bits (11..4) of every sample. */
struct FitError
{
    std::size_t samples = 0;      // samples with at least one observed bit
    std::size_t exact = 0;        // ... whose observed bits all agree
    std::size_t bitsObserved = 0, bitsWrong = 0;
    int maxError = 0;             // largest |ours - reference| of the observed value, in rd1B steps (16 LSB)
    double sumSquares = 0;
    double exactRate() const noexcept { return samples == 0 ? 1.0 : static_cast<double> (exact) / static_cast<double> (samples); }
    double rms() const noexcept { return samples == 0 ? 0.0 : std::sqrt (sumSquares / static_cast<double> (samples)); }
    void add (std::uint8_t ours, std::uint8_t reference, std::uint8_t observed) noexcept
    {
        if (observed == 0)
            return;
        ++samples;
        const auto o = static_cast<std::uint8_t> (ours & observed), r = static_cast<std::uint8_t> (reference & observed);
        const auto diff = static_cast<unsigned> (o ^ r);
        exact += diff == 0 ? 1u : 0u;
        bitsObserved += static_cast<std::size_t> (std::popcount (static_cast<unsigned> (observed)));
        bitsWrong += static_cast<std::size_t> (std::popcount (diff));
        const int e = std::abs (static_cast<int> (o) - static_cast<int> (r));
        maxError = std::max (maxError, e);
        sumSquares += static_cast<double> (e) * e;
    }
};

/** What our tables give for a sample's observed value. */
inline std::uint8_t predict (const chip::combined::ShapeTables& t, const Sample& s) noexcept
{
    if (! s.pulseHigh)
        return 0;
    const auto v = static_cast<unsigned> (chip::wave::shape (t, s.wave, s.index, static_cast<std::uint16_t> (s.noiseMask << 4)) >> 4);
    return static_cast<std::uint8_t> (v & s.noiseMask);
}

/** Fit errors per plan of the given tables (one per model). */
inline std::map<std::string, FitError> errorsByPlan (const std::vector<LoadedPlan>& plans, const chip::combined::ShapeTables& a,
                                                     const chip::combined::ShapeTables& b)
{
    std::map<std::string, FitError> m;
    for (const auto& lp : plans)
    {
        auto& e = m[lp.spec.name];
        for (const auto& s : lp.samples)
            e.add (predict (s.model == chip::Model::A ? a : b, s), s.value, s.observed());
    }
    return m;
}
} // namespace vs::bside::shapes
