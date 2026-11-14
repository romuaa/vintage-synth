#include "bside/trace/SpecAnalysis.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace vs::bside::trace
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kFloorDb = -120.0;

/** In-place iterative radix-2 FFT (size a power of two), double precision. */
class Fft
{
public:
    explicit Fft (int size) : n (size), twiddle (static_cast<std::size_t> (size / 2))
    {
        for (int k = 0; k < n / 2; ++k)
            twiddle[static_cast<std::size_t> (k)] = std::polar (1.0, -2.0 * kPi * k / n);
    }

    void forward (std::vector<std::complex<double>>& x) const
    {
        const auto N = static_cast<std::size_t> (n);
        for (std::size_t i = 1, j = 0; i < N; ++i)
        {
            std::size_t bit = N >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (x[i], x[j]);
        }
        for (std::size_t len = 2; len <= N; len <<= 1)
        {
            const std::size_t step = N / len;
            for (std::size_t i = 0; i < N; i += len)
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const auto w = twiddle[k * step] * x[i + k + len / 2];
                    x[i + k + len / 2] = x[i + k] - w;
                    x[i + k] += w;
                }
        }
    }

private:
    int n;
    std::vector<std::complex<double>> twiddle;
};

std::int16_t tenthDb (double meanSquare)
{
    double db = meanSquare <= 1.0e-12 ? kFloorDb : std::max (kFloorDb, 10.0 * std::log10 (meanSquare));
    db = std::min (db, static_cast<double> (kSpecMaxTenthDb) / 10.0);
    return static_cast<std::int16_t> (std::lround (db * 10.0));
}

/** The harness's band edges: band 0 from 0 Hz, band b >= 1 from 125 Hz * 2^((b - 1) / 3 - 1/6). */
double bandLowerEdge (int b)
{
    return b == 0 ? 0.0 : 125.0 * std::pow (2.0, (b - 1) / 3.0 - 1.0 / 6.0);
}

std::vector<float> hannFloat (int n, double& power)
{
    // Same window as the harness: float, periodic Hann.
    std::vector<float> w (static_cast<std::size_t> (n));
    power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        w[static_cast<std::size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * static_cast<float> (kPi) * static_cast<float> (i) / static_cast<float> (n));
        power += static_cast<double> (w[static_cast<std::size_t> (i)]) * w[static_cast<std::size_t> (i)];
    }
    return w;
}
} // namespace

std::uint64_t specSampleCount (const Trace& t)
{
    if (t.clockHz == 0)
        return 0;
    return (t.endCycle * static_cast<std::uint64_t> (kSpecRate) + t.clockHz - 1) / t.clockHz;
}

std::uint64_t specFrameCount (const Trace& t)
{
    const auto n = specSampleCount (t);
    return std::max<std::uint64_t> (1, (n + kSpecHop - 1) / kSpecHop);
}

std::uint64_t frameOfCycle (const Trace& t, std::uint64_t cycle)
{
    return t.clockHz == 0 ? 0 : cycle * static_cast<std::uint64_t> (kSpecRate / kSpecHop) / t.clockHz;
}

std::vector<PlannedSegment> segmentPlan (const Trace& t)
{
    std::vector<PlannedSegment> out;
    if (! validateTrace (t))
        return out;
    const std::uint64_t wholeFrames = specSampleCount (t) / kSpecHop;

    std::vector<std::uint64_t> events { 0 };
    for (const auto& w : t.writes)
        if (w.cycle != events.back())
            events.push_back (w.cycle);

    for (std::size_t i = 0; i < events.size() && out.size() < static_cast<std::size_t> (kSpecMaxSegments); ++i)
    {
        const std::uint64_t first = frameOfCycle (t, events[i]) + 1 + kSegmentSettleFrames;
        const std::uint64_t end = i + 1 < events.size() ? std::min (frameOfCycle (t, events[i + 1]), wholeFrames) : wholeFrames;
        if (end < first + kSegmentMinFrames)
            continue;
        PlannedSegment s;
        const auto index = out.size();
        s.name = std::string ("s") + static_cast<char> ('0' + index / 10) + static_cast<char> ('0' + index % 10);
        s.firstFrame = static_cast<std::uint32_t> (first);
        s.endFrame = static_cast<std::uint32_t> (end);
        out.push_back (s);
    }
    return out;
}

void featureFrames (const float* mono, std::size_t n, int frames, std::vector<std::int16_t>& level, std::vector<std::int16_t>& bands)
{
    level.assign (static_cast<std::size_t> (frames), 0);
    bands.assign (static_cast<std::size_t> (frames) * kSpecBands, 0);

    std::vector<int> bandOfBin (kFeatureFftSize / 2, -1);
    for (int k = 1; k < kFeatureFftSize / 2; ++k)
    {
        const double hz = k * static_cast<double> (kSpecRate) / kFeatureFftSize;
        int b = 0;
        while (b + 1 < kSpecBands && hz >= bandLowerEdge (b + 1))
            ++b;
        bandOfBin[static_cast<std::size_t> (k)] = b;
    }
    double windowPower = 0.0;
    const auto window = hannFloat (kFeatureFftSize, windowPower);
    const Fft fft (kFeatureFftSize);
    std::vector<std::complex<double>> buf (kFeatureFftSize);
    const auto sn = static_cast<long long> (n);

    for (int fr = 0; fr < frames; ++fr)
    {
        const long long centre = static_cast<long long> (fr) * kSpecHop + kSpecHop / 2;
        const long long start = centre - kFeatureFftSize / 2;
        for (int i = 0; i < kFeatureFftSize; ++i)
        {
            const long long idx = start + i;
            const float x = (idx >= 0 && idx < sn) ? mono[idx] * window[static_cast<std::size_t> (i)] : 0.0f;
            buf[static_cast<std::size_t> (i)] = { static_cast<double> (x), 0.0 };
        }
        fft.forward (buf);
        double power[kSpecBands] = {};
        for (int k = 1; k < kFeatureFftSize / 2; ++k)
            power[bandOfBin[static_cast<std::size_t> (k)]] += std::norm (buf[static_cast<std::size_t> (k)]);
        for (int b = 0; b < kSpecBands; ++b)
            bands[static_cast<std::size_t> (fr * kSpecBands + b)] = tenthDb (2.0 * power[b] / (kFeatureFftSize * windowPower));

        double sum = 0.0;
        for (long long i = static_cast<long long> (fr) * kSpecHop; i < std::min (static_cast<long long> (fr + 1) * kSpecHop, sn); ++i)
            sum += static_cast<double> (mono[i]) * mono[i];
        level[static_cast<std::size_t> (fr)] = tenthDb (sum / kSpecHop);
    }
}

std::array<std::int16_t, kSpecBins> twelfthOctaveSpectrum (const float* mono, std::size_t first, std::size_t end)
{
    std::array<std::int16_t, kSpecBins> out {};
    out.fill (static_cast<std::int16_t> (kFloorDb * 10));
    constexpr int N = kSegmentFftSize;
    if (mono == nullptr || end < first + N)
        return out;

    std::vector<double> window (N);
    double windowPower = 0.0;
    for (int i = 0; i < N; ++i)
    {
        window[static_cast<std::size_t> (i)] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / N);
        windowPower += window[static_cast<std::size_t> (i)] * window[static_cast<std::size_t> (i)];
    }
    const Fft fft (N);
    std::vector<std::complex<double>> buf (N);
    std::vector<double> bin (N / 2, 0.0); // mean-square power per FFT bin
    int count = 0;
    for (std::size_t start = first; start + N <= end; start += N / 2, ++count)
    {
        for (int i = 0; i < N; ++i)
            buf[static_cast<std::size_t> (i)] = { static_cast<double> (mono[start + static_cast<std::size_t> (i)]) * window[static_cast<std::size_t> (i)], 0.0 };
        fft.forward (buf);
        for (int k = 1; k < N / 2; ++k)
            bin[static_cast<std::size_t> (k)] += 2.0 * std::norm (buf[static_cast<std::size_t> (k)]) / (N * windowPower);
    }
    for (auto& b : bin)
        b /= count;

    const double df = static_cast<double> (kSpecRate) / N;
    for (int k = 0; k < kSpecBins; ++k)
    {
        const double lo = 20.0 * std::pow (2.0, (k - 0.5) / 12.0), hi = 20.0 * std::pow (2.0, (k + 0.5) / 12.0);
        const int jLo = std::max (1, static_cast<int> (std::floor (lo / df + 0.5)));
        const int jHi = std::min (N / 2 - 1, static_cast<int> (std::floor (hi / df + 0.5)));
        double p = 0.0;
        for (int j = jLo; j <= jHi; ++j)
        {
            const double a = std::max (lo, (j - 0.5) * df), b = std::min (hi, (j + 0.5) * df);
            if (b > a)
                p += bin[static_cast<std::size_t> (j)] * (b - a) / df;
        }
        out[static_cast<std::size_t> (k)] = tenthDb (p);
    }
    return out;
}

bool analyseRender (const Trace& t, const std::string& traceSha256, const std::vector<float>& mono, Spec& out, std::string* why)
{
    const auto fail = [&] (const char* m) {
        if (why != nullptr)
            *why = m;
        return false;
    };
    if (! validateTrace (t))
        return fail ("the trace does not validate");
    if (mono.size() != specSampleCount (t))
        return fail ("the render does not hold specSampleCount() samples");
    const auto frames = specFrameCount (t);
    if (frames > static_cast<std::uint64_t> (kSpecMaxFrames))
        return fail ("the trace is too long for a spec (more than 100000 frames)");

    out = Spec {};
    out.traceSha256 = traceSha256;
    out.frames = static_cast<int> (frames);
    featureFrames (mono.data(), mono.size(), out.frames, out.levelTenthDb, out.bandTenthDb);
    for (const auto& p : segmentPlan (t))
    {
        SpecSegment s;
        s.name = p.name;
        s.firstFrame = p.firstFrame;
        s.endFrame = p.endFrame;
        s.spectrum = twelfthOctaveSpectrum (mono.data(), static_cast<std::size_t> (p.firstFrame) * kSpecHop, static_cast<std::size_t> (p.endFrame) * kSpecHop);
        out.segments.push_back (s);
    }
    std::string v;
    if (! validateSpec (out, &v))
    {
        if (why != nullptr)
            *why = "the spec does not validate: " + v;
        return false;
    }
    return true;
}
} // namespace vs::bside::trace
