// The alias gate after the decimator (docs/architecture.md 8.21.1, 8.17, 8.20.1): at the part's output, every rate
// configuration of 4.4, the band of 8.4.5; single notes at about 1, 2 and 3.85 kHz, sawtooth and pulse, direct and
// through the low-pass at resonance 0, within the budgets of 8.4.5 on both models; narrow Model B pulses (widths 1 and
// 4095 included) through the whole path with 1 chip and with 16 chips in unison, within -100 dB. Every row is measured
// exactly (8.20): the note at host rate x p / q (p >= 2: what folds lands off the harmonics), a rectangular DFT over
// whole periods. A reduced set in the default run, the full scan hidden ("[.part-alias-full]").
#include <catch2/catch_test_macros.hpp>

#include "PartTestSupport.h"

#include <cstdio>

using namespace bs_part_test;

namespace
{
/** The budget of 8.4.5 for a note at hz: Model B -100 dB to 3.85 kHz; Model A -96 dB to 2 kHz, -80 dB above. */
double budget (int model, double hz) { return model == 1 ? -100.0 : (hz <= 2000.0 * 1.01 ? -96.0 : -80.0); }

struct Row
{
    HostConfig host;
    int model, wave; // wave: 2 saw, 3 pulse
    bool lowPass;
    double hz;
};

struct Worst
{
    double margin = -300.0;
    std::string where;
    int rows = 0;
};

void runRow (const Row& r, Worst& w)
{
    bs::PartParams p;
    p.chipModel = r.model;
    p.osc1Wave = r.wave;
    p.osc1Filter = r.lowPass ? 1 : 0;
    p.filterMode = 1;
    p.filterResonance = 0;
    p.filterCutoff = 2047.0f;
    const auto pq = rationalNear (r.host.rate, r.hz, 1200);
    const auto a = exactAlias (r.host, p, pq.first, pq.second);
    const double limit = budget (r.model, a.f);
    char text[256];
    std::snprintf (text, sizeof (text), "%.0f Hz x%d, Model %s, %s, %s, %.1f Hz: worst %.1f dB at %.0f Hz (budget %.0f)", r.host.rate, r.host.factor,
                   r.model == 0 ? "A" : "B", r.wave == 2 ? "saw" : "pulse", r.lowPass ? "low-pass" : "direct", a.f, a.worstDb, a.worstHz, limit);
    INFO (text);
    CHECK (a.worstDb <= limit);
    ++w.rows;
    if (a.worstDb - limit > w.margin)
    {
        w.margin = a.worstDb - limit;
        w.where = text;
    }
}

void runNarrow (HostConfig h, float width, int chips, Worst& w, double hz)
{
    bs::PartParams p;
    p.chipModel = 1;
    p.osc1Wave = 3;
    p.osc1Pw = width * 100.0f / 4095.0f;
    p.osc1Filter = 0;
    p.velVolume = 0.0f;
    if (chips > 1)
    {
        p.assignMode = 1;
        p.unisonVoices = chips == 16 ? 3 : 1;
        p.unisonDetune = 0.0f; // equal frequencies: the sum stays periodic, its pattern of phases included
    }
    const auto pq = rationalNear (h.rate, hz, 1200);
    const auto a = exactAlias (h, p, pq.first, pq.second);
    char text[256];
    std::snprintf (text, sizeof (text), "%.0f Hz x%d, Model B pulse width %.0f, %d chip(s), %.1f Hz: worst %.1f dB at %.0f Hz", h.rate, h.factor,
                   static_cast<double> (width), chips, a.f, a.worstDb, a.worstHz);
    INFO (text);
    CHECK (a.worstDb <= -100.0);
    ++w.rows;
    if (a.worstDb + 100.0 > w.margin)
    {
        w.margin = a.worstDb + 100.0;
        w.where = text;
    }
}

std::vector<Row> rows (const std::vector<HostConfig>& hosts)
{
    std::vector<Row> v;
    for (const auto& h : hosts)
        for (const int model : { 1, 0 })
            for (const int wave : { 2, 3 })
                for (const bool lp : { false, true })
                    for (const double hz : { 1000.0, 2000.0, 3850.0 })
                        v.push_back ({ h, model, wave, lp, hz });
    return v;
}
} // namespace

TEST_CASE ("Part alias: single notes after the decimator meet the budgets of 8.4.5 (reduced set: 48 kHz x2, 96 kHz x1)", "[bside][part][alias][slow]")
{
    Worst w;
    for (const auto& r : rows ({ { 48000.0, 2 }, { 96000.0, 1 } }))
        runRow (r, w);
    UNSCOPED_INFO ("worst against the budget: " << w.margin << " dB (" << w.where << ")");
    CHECK (w.rows == 2 * 2 * 2 * 2 * 3);
}

TEST_CASE ("Part alias: narrow Model B pulses after the decimator meet -100 dB exactly, 1 chip and 16 in unison (reduced set)", "[bside][part][alias][slow]")
{
    Worst w;
    for (const float width : { 1.0f, 4095.0f })
        for (const int chips : { 1, 16 })
            runNarrow ({ 48000.0, 2 }, width, chips, w, 3800.0);
    UNSCOPED_INFO ("worst against -100 dB: " << w.margin << " dB (" << w.where << ")");
    CHECK (w.rows == 4);
}

TEST_CASE ("Part alias: the full scan, every rate configuration of 4.4", "[.part-alias-full]")
{
    Worst w;
    for (const auto& r : rows (hostConfigs()))
        runRow (r, w);
    std::printf ("single notes: %d rows, worst against the budget %.1f dB (%s)\n", w.rows, w.margin, w.where.c_str());
    Worst n;
    for (const auto& h : hostConfigs())
        for (const float width : { 1.0f, 4.0f, 4091.0f, 4095.0f })
            for (const int chips : { 1, 16 })
                for (const double hz : { 1000.0, 2000.0, 3800.0 })
                    runNarrow (h, width, chips, n, hz);
    std::printf ("narrow pulses: %d rows, worst against -100 dB %.1f dB (%s)\n", n.rows, n.margin, n.where.c_str());
}
