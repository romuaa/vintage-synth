#pragma once

// Helpers of the part's tests (PartTests.cpp, PartAliasTests.cpp; docs/architecture.md 8.21.1): a part driven
// like the shell drives it (host blocks, events at offsets), table compilation, the rate configurations of 4.4 and
// the exact alias measurement of 8.20 (a rectangular DFT over whole periods at a rational frequency).

#include "bside/engine/Part.h"
#include "bside/program/Program.h"
#include "bside/program/TableReader.h"
#include "../unit/RenderTestSupport.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace bs_part_test
{
inline vs::NoteEvent on (int offset, int note, float velocity = 1.0f)
{
    return { offset, vs::EventType::NoteOn, static_cast<std::uint8_t> (note), velocity };
}
inline vs::NoteEvent off (int offset, int note) { return { offset, vs::EventType::NoteOff, static_cast<std::uint8_t> (note), 0.0f }; }
inline vs::NoteEvent control (int offset, vs::EventType type, float value = 0.0f) { return { offset, type, 0, value }; }

/** A host configuration of 4.4: host rate and the voice path's factor (real time or offline). */
struct HostConfig
{
    double rate;
    int factor;
    double band() const { return std::min (20000.0, 20.0 / 48.0 * rate); }
};
/** Every factor of 4.4: real time 4x below 44.1 kHz, 2x to 88.2 kHz, 1x above; offline 4x below 88.2 kHz, 2x above. */
inline std::vector<HostConfig> hostConfigs()
{
    return { { 32000.0, 4 }, { 44100.0, 2 }, { 44100.0, 4 }, { 48000.0, 2 }, { 48000.0, 4 }, { 88200.0, 1 }, { 88200.0, 2 },
             { 96000.0, 1 }, { 96000.0, 2 }, { 192000.0, 1 }, { 192000.0, 2 } };
}

/** A compiled table from its text (the reader's own checks; an error fails the test that asked for it). */
inline vs::bside::program::Program compile (const std::string& text, std::string* error = nullptr)
{
    const auto r = vs::bside::program::readTable (text);
    if (error != nullptr)
        *error = r.ok ? std::string {} : r.message();
    return r.ok ? r.program : vs::bside::program::emptyProgram();
}

/** A part as the shell drives it: host blocks of `block` samples, events at absolute host-sample offsets. */
struct Rig
{
    std::unique_ptr<bs::Part> part = std::make_unique<bs::Part>();
    HostConfig host { 48000.0, 2 };
    int maxBlock = 512;
    bs::PartParams params {};
    int clock = 0;
    std::int64_t now = 0; // host samples rendered

    Rig() = default;
    Rig (HostConfig h, const bs::PartParams& p, int maxBlockSize = 512, int chipClock = 0) { prepare (h, p, maxBlockSize, chipClock); }

    void prepare (HostConfig h, const bs::PartParams& p, int maxBlockSize = 512, int chipClock = 0)
    {
        host = h;
        params = p;
        maxBlock = maxBlockSize;
        clock = chipClock;
        part->setParams (p);
        part->setChipClock (chipClock);
        part->prepare (h.rate, maxBlockSize, h.factor, p, chipClock);
        now = 0;
    }
    void set (const bs::PartParams& p)
    {
        params = p;
        part->setParams (p);
    }

    /** Renders `total` host samples in blocks of `block` (at most the prepared size); `events` carry offsets
        relative to the start of this call, ascending; an event at `total` is applied after the last sample. */
    std::vector<float> run (int total, std::vector<vs::NoteEvent> events = {}, int block = 0)
    {
        if (block <= 0)
            block = maxBlock;
        std::vector<float> out (static_cast<std::size_t> (total));
        std::vector<vs::NoteEvent> chunk;
        std::size_t next = 0;
        for (int start = 0; start < total || (total == 0 && start == 0); start += block)
        {
            const int len = std::min (block, total - start);
            chunk.clear();
            while (next < events.size() && (events[next].offset < start + len || (start + len >= total && events[next].offset >= total)))
            {
                auto e = events[next++];
                e.offset -= start;
                chunk.push_back (e);
            }
            part->render (out.data() + start, len, chunk);
            if (total == 0)
                break;
        }
        now += total;
        return out;
    }
};

inline double peakOf (const std::vector<float>& v, std::size_t from = 0, std::size_t to = static_cast<std::size_t> (-1))
{
    double p = 0.0;
    for (std::size_t i = from; i < std::min (to, v.size()); ++i)
        p = std::max (p, std::abs (static_cast<double> (v[i])));
    return p;
}

inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-300)); }

/** The exact DFT of x (any length n, the mean removed) at bins 0 .. maxBin (Bluestein over the power-of-two FFT):
    the plain rectangular sum over the n samples, up to rounding (as ChipRenderExactTests.cpp measures). */
inline std::vector<double> exactMagnitudes (const std::vector<float>& x, int maxBin)
{
    constexpr double kPi = 3.14159265358979323846;
    const std::size_t n = x.size();
    std::size_t m = 1;
    while (m < 2 * n)
        m <<= 1;
    std::vector<std::complex<double>> w (n);
    for (std::size_t k = 0; k < n; ++k)
    {
        const std::uint64_t kk = (static_cast<std::uint64_t> (k) * static_cast<std::uint64_t> (k)) % (2u * static_cast<std::uint64_t> (n));
        w[k] = std::polar (1.0, -kPi * static_cast<double> (kk) / static_cast<double> (n));
    }
    double mean = 0.0;
    for (const float v : x)
        mean += static_cast<double> (v);
    mean /= static_cast<double> (n);
    std::vector<std::complex<double>> a (m), b (m);
    for (std::size_t k = 0; k < n; ++k)
        a[k] = (static_cast<double> (x[k]) - mean) * w[k];
    b[0] = std::conj (w[0]);
    for (std::size_t k = 1; k < n; ++k)
        b[k] = b[m - k] = std::conj (w[k]);
    bs_render_test::fft (a);
    bs_render_test::fft (b);
    for (std::size_t i = 0; i < m; ++i)
        a[i] = std::conj (a[i] * b[i]);
    bs_render_test::fft (a);
    std::vector<double> mag (static_cast<std::size_t> (maxBin) + 1);
    for (std::size_t k = 0; k <= static_cast<std::size_t> (maxBin); ++k)
        mag[k] = std::abs (std::conj (a[k]) * w[k]) / static_cast<double> (m);
    return mag;
}

/** p / q (lowest terms, minQ <= q <= maxQ, p >= 2) with host rate x p / q nearest to `hz`. p >= 2 matters: everything
    the rendering folds lands on multiples of rate / q = f / p, which are harmonics of f when p = 1 (the measurement
    would not see aliasing at all). */
inline std::pair<int, int> rationalNear (double rate, double hz, int maxQ, int minQ = 40)
{
    std::pair<int, int> best { 2, 3 };
    double err = 1e300;
    for (int q = minQ; q <= maxQ; ++q)
    {
        const int p = static_cast<int> (std::lround (hz * q / rate));
        if (p < 2 || p >= q || std::gcd (p, q) != 1)
            continue;
        const double e = std::abs (rate * p / q - hz);
        if (e < err)
        {
            err = e;
            best = { p, q };
        }
    }
    return best;
}

/** The semitone offset that puts `note` exactly at `hz` (A4 = 440 Hz, equal temperament: the engine's mapping). */
inline double detuneFor (int note, double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0) - static_cast<double> (note); }
inline int noteNear (double hz) { return static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (hz / 440.0))); }

struct AliasRow
{
    double worstDb = -300.0, worstHz = 0.0, fundamentalDb = -300.0, f = 0.0;
};

/** One note (or a unison group) at host rate x p / q, its part output measured exactly: every bin up to the band that
    is not a harmonic, relative to the fundamental (8.20, "the exact measurement"). */
inline AliasRow exactAlias (HostConfig h, bs::PartParams p, int pp, int q, int voices = 1)
{
    const double f = h.rate * pp / q;
    const int note = noteNear (f);
    Rig rig (h, p, 512);
    rig.part->setTestDetune (detuneFor (note, f));
    rig.run (static_cast<int> (3.0 * h.rate), { on (0, note, 1.0f) }); // the board high-pass (1.2 Hz) settles on the note's DC
    const int periods = std::max (1, 24000 / q);
    const int n = periods * q;
    const auto y = rig.run (n);
    const int maxBin = static_cast<int> (h.band() * n / h.rate);
    const auto mag = exactMagnitudes (y, maxBin);
    const int fundamental = periods * pp;
    AliasRow r;
    r.f = f;
    r.fundamentalDb = db (mag[static_cast<std::size_t> (fundamental)]);
    for (int b = 1; b <= maxBin; ++b)
        if (b % fundamental != 0)
        {
            const double d = db (mag[static_cast<std::size_t> (b)] / mag[static_cast<std::size_t> (fundamental)]);
            if (d > r.worstDb)
            {
                r.worstDb = d;
                r.worstHz = b * h.rate / n;
            }
        }
    (void) voices;
    return r;
}
} // namespace bs_part_test
