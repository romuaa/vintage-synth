// The chip's output stage (OutputStage.h; docs/architecture.md 8.19.1; every constant: docs/decisions.md
// "B-Side S3a"). All constants are our own fits to data generated with the reference adapter on traces written
// for fitting (never the committed gate traces): static levels of voices held by the test bit at every envelope
// level, routing and mode; held tones and two tones through the low-pass at 2047 and direct (Model A).

#include "bside/chip/OutputStage.h"

#include <algorithm>
#include <array>

namespace vs::bside::chip
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Constants
{
    double mixerDc, voiceDc, envDc, scale, n2, n3, n4, n5, gainVoice, gainLow, gainBand, gainHigh, y0, curveMin, curveMax;
    std::array<double, 16> volume;
};

// Model A: fitted jointly with the voice bend (OutputStage.h) and the filter's static functions (Filter.cpp) to
// 310 static levels (rms 0.0073 reference units, levels up to 0.65; largest 0.039) and 28 held-tone and
// two-tone spectra (harmonics 2..5 and the products, rms 0.83 dB, largest 3.4 dB) (decisions "B-Side S3a").
constexpr Constants kA {
    0.387973, 0.386539, 0.403086, 0.179676, 0.074531, 0.049466, 0.001478, -0.003231, 0.131771, 0.049526, 0.072176, 0.078753,
    -0.3877, -3.297366, 4.081098,
    { 0.0, 0.0783, 0.1545, 0.2292, 0.3018, 0.3729, 0.4423, 0.5101, 0.5765, 0.6414, 0.7048, 0.7668, 0.8272, 0.8862, 0.9438, 1.0 }
};
// Model B: rms error 0.00039, largest 0.0021.
constexpr Constants kB {
    -0.064730, -0.067657, 0.0, 0.230904, 0.044339, 0.002267, 0.0, 0.0, 0.004324, 0.003923, 0.003732, 0.003463,
    -0.1918, -8.0, 8.0,
    { 0.0, 1.0 / 15, 2.0 / 15, 3.0 / 15, 4.0 / 15, 5.0 / 15, 6.0 / 15, 7.0 / 15, 8.0 / 15, 9.0 / 15, 10.0 / 15, 11.0 / 15, 12.0 / 15,
      13.0 / 15, 14.0 / 15, 1.0 }
};

// the board filters (8.15.7), fixed in Hz (our filters are clock-independent)
constexpr double kBoardHighPassHz = 1.196;
constexpr double kBoardLowPassHz = 15400.0;

const Constants& constantsFor (Model m) noexcept { return m == Model::A ? kA : kB; }
} // namespace

double OutputStage::voiceDc (Model m) noexcept { return constantsFor (m).voiceDc; }
double OutputStage::envelopeDc (Model m) noexcept { return constantsFor (m).envDc; }

OutputStage::OutputStage() noexcept
{
    prepare (Model::B, 96000.0, false);
}

void OutputStage::prepare (Model model, double internalRate, bool volumeDc) noexcept
{
    mdl = model;
    rate = internalRate > 1000.0 && std::isfinite (internalRate) ? internalRate : 96000.0;
    volumeDcOn = volumeDc;
    hpA = std::exp (-2.0 * kPi * kBoardHighPassHz / rate);
    const double gl = std::tan (kPi * std::min (kBoardLowPassHz, 0.45 * rate) / rate);
    lpG = gl / (1.0 + gl);
    lpKeep = 1.0 - lpG;
    lpIn = 2.0 * lpG;
    lpNext = 1.0 - 2.0 * lpG;
    update();
}

void OutputStage::reset() noexcept
{
    hpX = 0.0;
    hpY = 0.0;
    lpS = 0.0;
}

void OutputStage::setVolumeDc (bool on) noexcept
{
    if (on != volumeDcOn)
    {
        volumeDcOn = on;
        update();
    }
}

void OutputStage::setRouting (std::uint8_t resonanceRouting) noexcept
{
    const auto r = static_cast<std::uint8_t> (resonanceRouting & 0x07);
    if (r != routing)
    {
        routing = r;
        update();
    }
}

void OutputStage::setModeVolume (std::uint8_t value) noexcept
{
    if (value != modeVolume)
    {
        modeVolume = value;
        update();
    }
}

void OutputStage::setFilterStaticDc (double filterOutputDc) noexcept
{
    const double v = std::isfinite (filterOutputDc) ? filterOutputDc : 0.0;
    if (v < filterStaticDc || v > filterStaticDc)
    {
        filterStaticDc = v;
        update();
    }
}

void OutputStage::update() noexcept
{
    const Constants& c = constantsFor (mdl);
    nFilter = 0;
    nDirect = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (((routing >> i) & 1) != 0)
            ++nFilter;
        else if (! (i == 2 && (modeVolume & 0x80) != 0))
            ++nDirect;
    }
    if (! externalCounts)
    {
        cntF = nFilter;
        cntD = nDirect;
    }
    envDc = c.envDc;
    mixerDcConst = c.mixerDc;
    voiceDcConst = c.voiceDc;
    filterBusDc = c.voiceDc * cntF;
    staticDirect = c.mixerDc + c.voiceDc * cntD;
    gain = 1.0
           / (1.0 + c.gainVoice * (cntD - 3.0) + ((modeVolume & 0x10) != 0 ? c.gainLow : 0.0)
              + ((modeVolume & 0x20) != 0 ? c.gainBand : 0.0) + ((modeVolume & 0x40) != 0 ? c.gainHigh : 0.0));
    n2 = c.n2;
    n3 = c.n3;
    n4 = c.n4;
    n5 = c.n5;
    quintic = mdl == Model::A;
    curveMin = c.curveMin;
    curveMax = c.curveMax;
    level = c.volume[static_cast<std::size_t> (modeVolume & 0x0f)] * c.scale;
    if (volumeDcOn)
    {
        offset = c.y0;
        curveDc = 0.0;
    }
    else
    {
        offset = 0.0;
        curveDc = curve (gain * (staticDirect + filterStaticDc));
    }
}
} // namespace vs::bside::chip
