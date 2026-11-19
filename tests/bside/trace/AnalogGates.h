#pragma once

// The analog gates of docs/architecture.md 8.9.4 / 8.19.1 against the committed reference data (S3a): our
// chain (AnalogChain.h) rendered at 192 kHz, reduced with the shared SpecAnalysis functions and measured with
// AnalogMeasure.h only, exactly as the reference's numbers are measured from its committed .bsspec.
// Shared by bside_trace (the gates) and nothing else; offline test code (allocates).

#include "AnalogChain.h"

#include "bside/trace/AnalogMeasure.h"
#include "bside/trace/Compare.h"
#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"
#include "bside/trace/SpecAnalysis.h"
#include "bside/trace/TraceFormat.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vs::bside::test
{
struct Rendered
{
    bool ok = false;
    std::string why;
    trace::Trace trace;
    trace::Spec reference; // committed
    trace::Spec ours;
    ChainRender chain;
    /** Segments (index into reference.segments) with an ambiguous sample (voices sounding on both paths). */
    std::vector<bool> segmentAmbiguous;
};

inline std::string slurpFile (const std::filesystem::path& p)
{
    std::ifstream f (p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/** Renders a committed trace through the chain and analyses it like the reference (cached per name). */
inline const Rendered& renderedTrace (const std::filesystem::path& dir, const std::string& name)
{
    static std::map<std::string, Rendered> cache;
    const auto it = cache.find (name);
    if (it != cache.end())
        return it->second;
    Rendered& r = cache[name];
    const std::string bytes = slurpFile (dir / (name + ".bstrace"));
    const std::string specText = slurpFile (dir / (name + ".bsspec"));
    if (bytes.empty() || specText.empty())
    {
        r.why = "no reference data committed for " + name;
        return r;
    }
    const auto t = trace::readTrace (bytes);
    const auto s = trace::readSpec (specText);
    if (! t.ok || ! s.ok)
    {
        r.why = "unreadable reference data for " + name;
        return r;
    }
    r.trace = t.trace;
    r.reference = s.spec;
    r.chain = renderChain (t.trace);
    if (! trace::analyseRender (t.trace, trace::sha256Hex (bytes), r.chain.out, r.ours, &r.why))
        return r;
    r.segmentAmbiguous.assign (r.reference.segments.size(), false);
    for (std::size_t i = 0; i < r.reference.segments.size(); ++i)
    {
        const auto& seg = r.reference.segments[i];
        // a segment's spectrum uses its frames' samples; one ambiguous sample there disqualifies it
        const std::size_t a = static_cast<std::size_t> (seg.firstFrame) * trace::kSpecHop;
        const std::size_t b = std::min (r.chain.ambiguous.size(), static_cast<std::size_t> (seg.endFrame) * trace::kSpecHop);
        for (std::size_t k = a; k < b; ++k)
            if (r.chain.ambiguous[k] != 0)
            {
                r.segmentAmbiguous[i] = true;
                break;
            }
    }
    r.ok = true;
    return r;
}

/** The frames of a spec where no 48 kHz sample is ambiguous (a frame covers its own 1200 samples and the
    4096-point analysis window around its centre). */
inline std::vector<bool> clearFrames (const Rendered& r)
{
    std::vector<bool> ok (static_cast<std::size_t> (r.reference.frames), true);
    for (int f = 0; f < r.reference.frames; ++f)
    {
        const long centre = static_cast<long> (f) * trace::kSpecHop + trace::kSpecHop / 2;
        const long a = std::max (0L, centre - trace::kFeatureFftSize / 2), b = centre + trace::kFeatureFftSize / 2;
        for (long k = a; k < b && k < static_cast<long> (r.chain.ambiguous.size()); ++k)
            if (r.chain.ambiguous[static_cast<std::size_t> (k)] != 0)
            {
                ok[static_cast<std::size_t> (f)] = false;
                break;
            }
    }
    return ok;
}

/** A copy of a spec with only the frames in `keep` (no segments): for compareScripted over the clear frames. */
inline trace::Spec keepFrames (const trace::Spec& s, const std::vector<bool>& keep)
{
    trace::Spec o;
    o.traceSha256 = s.traceSha256;
    for (int f = 0; f < s.frames; ++f)
    {
        if (! keep[static_cast<std::size_t> (f)])
            continue;
        o.levelTenthDb.push_back (s.levelTenthDb[static_cast<std::size_t> (f)]);
        for (int b = 0; b < trace::kSpecBands; ++b)
            o.bandTenthDb.push_back (static_cast<std::int16_t> (s.band (f, b)));
        ++o.frames;
    }
    return o;
}

/** Harmonics 2..5 of a segment relative to its fundamental (dB, toneLevelDb). */
inline std::array<double, 4> harmonics (const trace::SpecSegment& s, double f0)
{
    std::array<double, 4> h {};
    const double l1 = trace::toneLevelDb (s, f0);
    for (int k = 2; k <= 5; ++k)
        h[static_cast<std::size_t> (k - 2)] = trace::toneLevelDb (s, k * f0) - l1;
    return h;
}
} // namespace vs::bside::test
