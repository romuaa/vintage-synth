#pragma once

// Deterministic generators for the reader tests: a seeded random source, a generator of valid
// tables, and a byte/token level mutator. No dependency on the library under test.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bst
{
/** Number of operator new calls so far (FuzzTests.cpp replaces the global operator new). Under AddressSanitizer
    it is not replaced: allocationsAreCounted() is then false and the count stays 0. */
std::size_t totalAllocations();
bool allocationsAreCounted();

/** xoshiro-style small generator: deterministic across platforms and standard libraries. */
class Rng
{
public:
    explicit Rng (std::uint64_t seed) : s (seed * 0x9e3779b97f4a7c15ull + 0x1234567ull) { next(); next(); }
    std::uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545f4914f6cdd1dull;
    }
    /** Uniform in [0, n). */
    std::uint32_t below (std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t> ((next() >> 33) % n); }
    bool chance (std::uint32_t percent) { return below (100) < percent; }
    int range (int lo, int hi) { return lo + static_cast<int> (below (static_cast<std::uint32_t> (hi - lo + 1))); }

private:
    std::uint64_t s;
};

/** A random table text that is valid in structure (it may still be rejected, e.g. for a zero-delay loop). */
inline std::string generateTable (Rng& r)
{
    static const char* const waveCmds[] = { "wave tri+gate +7", "wave saw ~", "wave pulse+noise =c#4", "wave none -0.5", "pitch +12", "pitch =60", "pitch ~",
                                            "gate on", "gate off", "adsr 0 9 a f", "wave noise+gate", "wave sync+ring+test+tri +0.01" };
    static const char* const pulseCmds[] = { "set 2048", "set 25%", "set 0", "set 4095", "sweep +5 x3", "sweep -100 x255", "pingpong 100 3000 7" };
    static const char* const filterCmds[] = { "cutoff 1000", "res 12", "volume 15", "mode lp", "mode lp+bp+hp", "route 13", "route none", "sweep -96 x16", "sweep +1 x1" };
    static const char* const laneNames[] = { "wave1", "wave2", "wave3", "pulse1", "pulse2", "pulse3", "filter" };

    std::string t = "bside-table 1\n";
    if (r.chance (50))
    {
        t += "[restart]\n";
        if (r.chance (60)) t += std::string ("mode ") + (r.chance (50) ? "classic" : "phase") + "\n";
        if (r.chance (40)) t += "lead " + std::to_string (r.range (1, 8)) + "\n";
        if (r.chance (30)) t += "ad 0f\nsr a0\n";
        if (r.chance (30)) t += "gateoff " + std::to_string (r.range (1, 8)) + "\n";
        if (r.chance (30)) t += "firstwave pulse+gate\n";
    }
    for (int lane = 0; lane < 7; ++lane)
    {
        if (r.chance (45))
            continue;
        t += std::string ("[") + laneNames[lane] + "]\n";
        const int rows = r.range (0, 12);
        std::vector<std::string> labels;
        for (int row = 0; row < rows; ++row)
        {
            if (r.chance (25) && labels.size() < 5)
            {
                labels.push_back (std::string ("l") + std::to_string (labels.size()));
                t += labels.back() + ":\n";
            }
            if (r.chance (8))
            {
                t += "release:\n";
                labels.push_back ("release");
            }
            std::string line = "    ";
            const int cmds = r.range (1, 3);
            for (int c = 0; c < cmds; ++c)
            {
                if (c > 0) line += ", ";
                if (lane < 3) line += waveCmds[r.below (12)];
                else if (lane < 6) line += pulseCmds[r.below (4)]; // no flow-ending commands in the middle
                else line += filterCmds[r.below (6)];
            }
            if (r.chance (15) && ! labels.empty())
                line += std::string (", ") + (r.chance (50) ? "jump " : "repeat ") + labels[r.below (static_cast<std::uint32_t> (labels.size()))]
                        + "";
            if (line.find ("repeat") != std::string::npos)
                line += " " + std::to_string (r.range (1, 255));
            else if (line.find ("jump") == std::string::npos && r.chance (25))
                line += " ticks " + std::to_string (r.chance (30) ? 0 : r.range (1, 9));
            t += line + (r.chance (20) ? "   # comment\n" : "\n");
        }
        if (lane >= 3 && r.chance (40))
            t += lane < 6 ? "    sweep +3 x4\n" : "    sweep -50 x10\n";
        if (r.chance (30))
            t += "    end\n";
        // A label must name a row: drop trailing labels by adding a closing row.
        t += lane < 3 ? "    gate on\n" : lane < 6 ? "    set 1\n" : "    res 1\n";
    }
    return t;
}

/** Building blocks for mutations: tokens of the formats. */
inline const std::vector<std::string>& vocabulary()
{
    static const std::vector<std::string> v = {
        "[wave1]", "[pulse2]", "[filter]", "[restart]", "wave", "pitch", "gate", "adsr", "set", "sweep", "pingpong", "cutoff", "res", "volume", "mode",
        "route", "jump", "repeat", "end", "ticks", "x16", "x0", "x256", "+7", "-12", "+0.15", "=C#6", "=61", "=128", "~", "key1", "tri+saw", "noise+gate",
        "none", "lp+bp", "lp+lp", "25%", "100%", "4095", "4096", "2047", "2048", "255", "256", "-1", "0", "1", ",", "#", ":", "release:", "loop:", "loop",
        "bside-table 1", "bside-table 2", "\n", "\r\n", "\t", " ", "  ", "99999999999999999999", "\xff", "\xc0\x80", "\xed\xa0\x80", "\x01", std::string ("\0", 1),
        "bside-trace 1", "clock", "model A", "model B", "check", "end 100", "bside-ref 1", "trace-sha256", "adapter", "emulator", "state", "bside-spec 1",
        "frames 2", "frame", "segment", "rate 48000", "bands 23", "-1200", "300", "301", "ffffff", "7fffff", "18", "19", "0f"
    };
    return v;
}

inline std::string mutate (std::string s, Rng& r, const std::vector<std::string>& corpus)
{
    const int rounds = 1 + static_cast<int> (r.below (4));
    for (int k = 0; k < rounds; ++k)
    {
        switch (r.below (10))
        {
            case 0: // flip a byte
                if (! s.empty()) s[r.below (static_cast<std::uint32_t> (s.size()))] = static_cast<char> (r.below (256));
                break;
            case 1: // insert a random byte
                s.insert (r.below (static_cast<std::uint32_t> (s.size() + 1)), 1, static_cast<char> (r.below (256)));
                break;
            case 2: // delete a range
                if (! s.empty())
                {
                    const auto at = r.below (static_cast<std::uint32_t> (s.size()));
                    s.erase (at, 1 + r.below (12));
                }
                break;
            case 3: // duplicate a range
                if (! s.empty())
                {
                    const auto at = r.below (static_cast<std::uint32_t> (s.size()));
                    const auto len = 1 + r.below (40);
                    s.insert (at, s.substr (at, len));
                }
                break;
            case 4: // truncate
                if (! s.empty()) s.resize (r.below (static_cast<std::uint32_t> (s.size())));
                break;
            case 5: // splice from another input
            {
                const auto& o = corpus[r.below (static_cast<std::uint32_t> (corpus.size()))];
                if (! o.empty())
                {
                    const auto from = r.below (static_cast<std::uint32_t> (o.size()));
                    s.insert (r.below (static_cast<std::uint32_t> (s.size() + 1)), o.substr (from, 1 + r.below (60)));
                }
                break;
            }
            case 6: // insert a vocabulary token
            case 7:
            {
                const auto& v = vocabulary();
                s.insert (r.below (static_cast<std::uint32_t> (s.size() + 1)), v[r.below (static_cast<std::uint32_t> (v.size()))]);
                break;
            }
            case 8: // replace one whitespace-delimited word by a token
            {
                const auto& v = vocabulary();
                if (! s.empty())
                {
                    auto at = r.below (static_cast<std::uint32_t> (s.size()));
                    auto b = at, e = at;
                    while (b > 0 && s[b - 1] != ' ' && s[b - 1] != '\n') --b;
                    while (e < s.size() && s[e] != ' ' && s[e] != '\n') ++e;
                    s.replace (b, e - b, v[r.below (static_cast<std::uint32_t> (v.size()))]);
                }
                break;
            }
            default: // repeat a line many times
                if (! s.empty())
                {
                    const auto at = r.below (static_cast<std::uint32_t> (s.size()));
                    auto b = at, e = at;
                    while (b > 0 && s[b - 1] != '\n') --b;
                    while (e < s.size() && s[e] != '\n') ++e;
                    const auto line = s.substr (b, e - b) + "\n";
                    std::string many;
                    const auto n = 2 + r.below (r.chance (10) ? 1200 : 80);
                    for (std::uint32_t i = 0; i < n; ++i) many += line;
                    s.insert (b, many);
                }
                break;
        }
        if (s.size() > 80000)
            s.resize (80000);
    }
    return s;
}
} // namespace bst
