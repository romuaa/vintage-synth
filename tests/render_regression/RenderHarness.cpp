#include "RenderHarness.h"

#include "vintage/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace vs::rr
{
// ------------------------------------------------------------------ script parsing

Script parseScript (const std::string& text)
{
    Script script;
    bool haveLength = false;
    std::istringstream in (text);
    std::string line;
    int lineNo = 0;

    const auto fail = [&] (const std::string& msg) {
        throw std::runtime_error ("script line " + std::to_string (lineNo) + ": " + msg);
    };

    while (std::getline (in, line))
    {
        ++lineNo;
        if (const auto hash = line.find ('#'); hash != std::string::npos)
            line.erase (hash);

        std::istringstream ls (line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;)
            tok.push_back (t);
        if (tok.empty())
            continue;

        const auto number = [&] (const std::string& s) {
            char* end = nullptr;
            const double v = std::strtod (s.c_str(), &end);
            if (end == s.c_str() || *end != '\0')
                fail ("not a number: " + s);
            return v;
        };

        if (tok[0] == "channels")
        {
            if (tok.size() != 2)
                fail ("channels takes one value");
            const double c = number (tok[1]);
            if (c < 0.5 || c > 2.5)
                fail ("channels must be 1 or 2");
            script.channels = c < 1.5 ? 1 : 2;
            continue;
        }

        if (tok[0] == "rate" || tok[0] == "length")
        {
            if (tok.size() != 2)
                fail (tok[0] + " takes one value");
            (tok[0] == "rate" ? script.sampleRate : script.length) = number (tok[1]);
            haveLength = haveLength || tok[0] == "length";
            continue;
        }

        Event e;
        size_t i = 0;
        if (const char c = tok[0][0]; (c >= '0' && c <= '9') || c == '.')
            e.time = number (tok[i++]);
        if (i >= tok.size())
            fail ("missing command");

        const std::string& cmd = tok[i++];
        const auto args = tok.size() - i;
        const auto arg = [&] (size_t k) { return number (tok[i + k]); };
        const auto channelAt = [&] (size_t k) { return args > k ? static_cast<int> (arg (k)) : 1; };

        if (cmd == "param")
        {
            if (args != 2)
                fail ("param <id> <value>");
            e.kind = Event::Kind::param;
            e.id = tok[i];
            e.value = arg (1);
        }
        else if (cmd == "on")
        {
            if (args < 2 || args > 3)
                fail ("on <note> <velocity> [channel]");
            e.kind = Event::Kind::noteOn;
            e.value = arg (0);
            e.data = static_cast<int> (arg (1));
            e.channel = channelAt (2);
        }
        else if (cmd == "off")
        {
            if (args < 1 || args > 2)
                fail ("off <note> [channel]");
            e.kind = Event::Kind::noteOff;
            e.value = arg (0);
            e.channel = channelAt (1);
        }
        else if (cmd == "cc")
        {
            if (args < 2 || args > 3)
                fail ("cc <controller> <value> [channel]");
            e.kind = Event::Kind::cc;
            e.value = arg (0);
            e.data = static_cast<int> (arg (1));
            e.channel = channelAt (2);
        }
        else if (cmd == "bend")
        {
            if (args < 1 || args > 2)
                fail ("bend <0..16383> [channel]");
            e.kind = Event::Kind::bend;
            e.value = arg (0);
            e.channel = channelAt (1);
        }
        else if (cmd == "alloff")
        {
            e.kind = Event::Kind::allOff;
            e.channel = channelAt (0);
        }
        else if (cmd == "play")
        {
            if (args < 1 || args > 2)
                fail ("play <bpm> [ppq]");
            e.kind = Event::Kind::play;
            e.value = arg (0);
            e.ppq = args > 1 ? arg (1) : 0.0;
            if (e.value <= 0.0)
                fail ("play: tempo must be positive");
        }
        else if (cmd == "tempo")
        {
            if (args != 1 || arg (0) <= 0.0)
                fail ("tempo <bpm>");
            e.kind = Event::Kind::tempo;
            e.value = arg (0);
        }
        else if (cmd == "locate")
        {
            if (args != 1)
                fail ("locate <ppq>");
            e.kind = Event::Kind::locate;
            e.value = arg (0);
        }
        else if (cmd == "stop")
        {
            if (args != 0)
                fail ("stop takes no arguments");
            e.kind = Event::Kind::stop;
        }
        else
            fail ("unknown command: " + cmd);

        if (e.time < 0.0)
            fail ("negative time");
        script.events.push_back (e);
    }

    if (! haveLength || script.length <= 0.0)
        throw std::runtime_error ("script: 'length' is required");
    if (script.sampleRate < 8000.0)
        throw std::runtime_error ("script: unreasonable sample rate");

    std::stable_sort (script.events.begin(), script.events.end(),
                      [] (const Event& a, const Event& b) { return a.time < b.time; });
    return script;
}

// ------------------------------------------------------------------ rendering

std::vector<float> renderScript (const Script& script, int blockSize)
{
    return renderScriptStereo (script, blockSize)[0];
}

std::vector<std::vector<float>> renderScriptChannels (const Script& script, int blockSize)
{
    auto both = renderScriptStereo (script, blockSize);
    if (script.channels == 1)
        both.resize (1);
    return both;
}

namespace
{
/** The host transport of a script: stopped until `play`; the position of a block's first sample
    is computed from the last play/tempo/locate anchor (never accumulated per block). */
struct ScriptPlayHead final : public juce::AudioPlayHead
{
    bool playing = false;
    double bpm = 120.0, anchorPpq = 0.0, sampleRate = 48000.0;
    long long anchor = 0, now = 0;

    double ppqAt (long long sample) const { return anchorPpq + static_cast<double> (sample - anchor) * bpm / (60.0 * sampleRate); }

    void apply (const Event& e, long long at)
    {
        switch (e.kind)
        {
            case Event::Kind::play:   playing = true; bpm = e.value; anchorPpq = e.ppq; anchor = at; break;
            case Event::Kind::tempo:  anchorPpq = ppqAt (at); anchor = at; bpm = e.value; break;
            case Event::Kind::locate: anchorPpq = e.value; anchor = at; break;
            case Event::Kind::stop:   playing = false; break;
            case Event::Kind::param: case Event::Kind::noteOn: case Event::Kind::noteOff:
            case Event::Kind::cc: case Event::Kind::bend: case Event::Kind::allOff: break;
        }
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setPpqPosition (playing ? ppqAt (now) : 0.0);
        return info;
    }
};

/** Parameter and transport commands: applied between blocks, they split the block. */
bool isBlockEvent (Event::Kind k)
{
    return k == Event::Kind::param || k == Event::Kind::play || k == Event::Kind::tempo || k == Event::Kind::locate
           || k == Event::Kind::stop;
}
} // namespace

std::vector<std::vector<float>> renderScriptStereo (const Script& script, int blockSize)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    vs::VintageSynthProcessor proc;
    ScriptPlayHead playHead;
    playHead.sampleRate = script.sampleRate;
    proc.setPlayHead (&playHead);
    proc.setPlayConfigDetails (0, 2, script.sampleRate, blockSize);
    proc.prepareToPlay (script.sampleRate, blockSize);

    const auto total = static_cast<int> (std::llround (script.length * script.sampleRate));
    const auto sampleOf = [&] (const Event& e) { return static_cast<int> (std::llround (e.time * script.sampleRate)); };

    std::vector<bool> applied (script.events.size(), false);
    std::vector<float> out, outR;
    out.reserve (static_cast<size_t> (total));
    outR.reserve (static_cast<size_t> (total));
    juce::AudioBuffer<float> buffer (2, blockSize);

    const auto applyParam = [&] (const Event& e) {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.getState().getParameter (e.id));
        if (p == nullptr)
            throw std::runtime_error ("script: unknown parameter id: " + e.id);
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (e.value)));
    };

    int pos = 0;
    while (pos < total)
    {
        int end = std::min (pos + blockSize, total);

        // Parameter events due now are applied before the chunk; the next one shortens it.
        for (size_t k = 0; k < script.events.size(); ++k)
        {
            const auto& e = script.events[k];
            if (! isBlockEvent (e.kind) || applied[k])
                continue;
            const int s = sampleOf (e);
            if (s <= pos)
            {
                if (e.kind == Event::Kind::param)
                    applyParam (e);
                else
                    playHead.apply (e, pos);
                applied[k] = true;
            }
            else if (s < end)
                end = s;
        }

        const int len = end - pos;
        juce::MidiBuffer midi;
        for (const auto& e : script.events)
        {
            const int s = sampleOf (e);
            if (isBlockEvent (e.kind) || s < pos || s >= end)
                continue;
            const auto note = static_cast<int> (e.value);
            switch (e.kind)
            {
                case Event::Kind::noteOn:  midi.addEvent (juce::MidiMessage::noteOn (e.channel, note, static_cast<juce::uint8> (e.data)), s - pos); break;
                case Event::Kind::noteOff: midi.addEvent (juce::MidiMessage::noteOff (e.channel, note), s - pos); break;
                case Event::Kind::cc:      midi.addEvent (juce::MidiMessage::controllerEvent (e.channel, note, e.data), s - pos); break;
                case Event::Kind::bend:    midi.addEvent (juce::MidiMessage::pitchWheel (e.channel, note), s - pos); break;
                case Event::Kind::allOff:  midi.addEvent (juce::MidiMessage::allNotesOff (e.channel), s - pos); break;
                case Event::Kind::param: case Event::Kind::play: case Event::Kind::tempo:
                case Event::Kind::locate: case Event::Kind::stop: break;
            }
        }

        buffer.setSize (2, len, false, false, true);
        buffer.clear();
        playHead.now = pos;
        proc.processBlock (buffer, midi);
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        out.insert (out.end(), l, l + len);
        outR.insert (outR.end(), r, r + len);
        pos = end;
    }

    proc.releaseResources();
    return { out, outR };
}

// ------------------------------------------------------------------ features

namespace
{
float toDb (double meanSquare)
{
    return meanSquare <= 1.0e-12 ? kFloorDb : std::max (kFloorDb, static_cast<float> (10.0 * std::log10 (meanSquare)));
}

float quantise (float db) { return std::round (db * 10.0f) / 10.0f; }

/** Lower edge in Hz of band b (b >= 1); band 0 starts at 0 Hz. Upper edge of the last band: Nyquist. */
double bandLowerEdge (int b)
{
    if (b == 0)
        return 0.0;
    return 125.0 * std::pow (2.0, (b - 1) / 3.0 - 1.0 / 6.0);
}
} // namespace

Features extractFeatures (const std::vector<float>& mono, double sampleRate)
{
    Features f;
    f.sampleRate = sampleRate;
    const int hop = static_cast<int> (std::lround (sampleRate * 0.025));
    const int n = static_cast<int> (mono.size());
    f.frames = (n + hop - 1) / hop;
    f.bandDb.assign (static_cast<size_t> (f.frames * kNumBands), kFloorDb);
    f.levelDb.assign (static_cast<size_t> (f.frames), kFloorDb);

    // Bin -> band map (bin centre frequency decides).
    std::vector<int> bandOfBin (kFftSize / 2, -1);
    for (int k = 1; k < kFftSize / 2; ++k)
    {
        const double hz = k * sampleRate / kFftSize;
        int b = 0;
        while (b + 1 < kNumBands && hz >= bandLowerEdge (b + 1))
            ++b;
        bandOfBin[static_cast<size_t> (k)] = b;
    }

    std::vector<float> window (kFftSize);
    double windowPower = 0.0;
    for (int i = 0; i < kFftSize; ++i)
    {
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / static_cast<float> (kFftSize));
        windowPower += static_cast<double> (window[static_cast<size_t> (i)]) * window[static_cast<size_t> (i)];
    }

    juce::dsp::FFT fft (12);
    static_assert (kFftSize == 4096);
    std::vector<float> buf (2 * kFftSize);

    for (int fr = 0; fr < f.frames; ++fr)
    {
        // Band energies: Hann-windowed FFT centred on the frame centre.
        const int centre = fr * hop + hop / 2;
        const int start = centre - kFftSize / 2;
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i)
        {
            const int idx = start + i;
            if (idx >= 0 && idx < n)
                buf[static_cast<size_t> (i)] = mono[static_cast<size_t> (idx)] * window[static_cast<size_t> (i)];
        }
        fft.performFrequencyOnlyForwardTransform (buf.data()); // magnitudes in buf[0 .. N/2]

        std::vector<double> power (kNumBands, 0.0);
        for (int k = 1; k < kFftSize / 2; ++k)
            power[static_cast<size_t> (bandOfBin[static_cast<size_t> (k)])] += static_cast<double> (buf[static_cast<size_t> (k)]) * buf[static_cast<size_t> (k)];
        for (int b = 0; b < kNumBands; ++b)
            f.bandDb[static_cast<size_t> (fr * kNumBands + b)] = quantise (toDb (2.0 * power[static_cast<size_t> (b)] / (kFftSize * windowPower)));

        // Level envelope: RMS of the hop-long block around the frame centre.
        double sum = 0.0;
        for (int i = fr * hop; i < std::min ((fr + 1) * hop, n); ++i)
            sum += static_cast<double> (mono[static_cast<size_t> (i)]) * mono[static_cast<size_t> (i)];
        f.levelDb[static_cast<size_t> (fr)] = quantise (toDb (sum / hop));
    }
    return f;
}

// ------------------------------------------------------------------ comparison

Comparison compare (const Features& ref, const Features& cand, const Tolerances& tol)
{
    Comparison c;
    std::ostringstream rep;

    if (ref.frames != cand.frames || std::abs (ref.sampleRate - cand.sampleRate) > 0.5)
    {
        c.pass = false;
        rep << "frame count / sample rate differ: reference " << ref.frames << " @ " << ref.sampleRate
            << ", candidate " << cand.frames << " @ " << cand.sampleRate << "\n";
        c.report = rep.str();
        return c;
    }

    long active = 0, over = 0;
    double sumAbs = 0.0;
    int worstFrame = 0, worstBand = 0;
    for (int fr = 0; fr < ref.frames; ++fr)
        for (int b = 0; b < kNumBands; ++b)
        {
            const float a = ref.band (fr, b), d = cand.band (fr, b);
            if (std::max (a, d) <= tol.bandGateDb)
                continue;
            const float delta = std::abs (std::max (a, tol.bandGateDb) - std::max (d, tol.bandGateDb));
            ++active;
            sumAbs += delta;
            over += delta > tol.cellDb ? 1 : 0;
            if (delta > c.maxCell)
            {
                c.maxCell = delta;
                worstFrame = fr;
                worstBand = b;
            }
        }
    c.cellFraction = active > 0 ? static_cast<double> (over) / static_cast<double> (active) : 0.0;
    c.meanCell = active > 0 ? sumAbs / static_cast<double> (active) : 0.0;

    int lastRef = -1, lastCand = -1, worstLevelFrame = 0;
    for (int fr = 0; fr < ref.frames; ++fr)
    {
        const float a = ref.levelDb[static_cast<size_t> (fr)], d = cand.levelDb[static_cast<size_t> (fr)];
        const float delta = std::abs (std::max (a, tol.levelGateDb) - std::max (d, tol.levelGateDb));
        if (delta > c.maxLevel)
        {
            c.maxLevel = delta;
            worstLevelFrame = fr;
        }
        if (a > tol.levelGateDb)
            lastRef = fr;
        if (d > tol.levelGateDb)
            lastCand = fr;
    }
    c.tailShift = std::abs (lastRef - lastCand);

    const auto check = [&] (bool ok, const std::string& what) {
        if (! ok)
        {
            c.pass = false;
            rep << "FAIL " << what << "\n";
        }
    };
    check (c.cellFraction <= tol.maxCellFraction,
           "band cells off by more than " + std::to_string (tol.cellDb) + " dB: " + std::to_string (100.0 * c.cellFraction)
               + " % (allowed " + std::to_string (100.0 * tol.maxCellFraction) + " %); worst " + std::to_string (c.maxCell)
               + " dB at frame " + std::to_string (worstFrame) + ", band " + std::to_string (worstBand));
    check (c.meanCell <= tol.meanCellDb, "mean band delta " + std::to_string (c.meanCell) + " dB (allowed " + std::to_string (tol.meanCellDb) + ")");
    check (c.maxLevel <= tol.levelDb, "level envelope delta " + std::to_string (c.maxLevel) + " dB at frame " + std::to_string (worstLevelFrame)
                                          + " (allowed " + std::to_string (tol.levelDb) + ")");
    check (c.tailShift <= tol.tailFrames, "tail end shifted by " + std::to_string (c.tailShift) + " frames (reference ends at frame "
                                              + std::to_string (lastRef) + ", candidate at " + std::to_string (lastCand) + ")");
    c.report = rep.str();
    return c;
}

// ------------------------------------------------------------------ golden files

std::string featuresToText (const Features& f)
{
    std::ostringstream o;
    o << "vsgolden 1\nrate " << f.sampleRate << "\nbands " << kNumBands << "\nframes " << f.frames << "\nlevel";
    for (const float v : f.levelDb)
        o << ' ' << std::lround (v * 10.0f);
    o << '\n';
    for (int fr = 0; fr < f.frames; ++fr)
    {
        o << "b";
        for (int b = 0; b < kNumBands; ++b)
            o << ' ' << std::lround (f.band (fr, b) * 10.0f);
        o << '\n';
    }
    return o.str();
}

Features featuresFromText (const std::string& text)
{
    std::istringstream in (text);
    std::string word;
    const auto expect = [&] (const char* w) {
        if (! (in >> word) || word != w)
            throw std::runtime_error (std::string ("golden file: expected '") + w + "'");
    };

    Features f;
    int version = 0, bands = 0;
    expect ("vsgolden");
    in >> version;
    if (version != 1)
        throw std::runtime_error ("golden file: unsupported version");
    expect ("rate");
    in >> f.sampleRate;
    expect ("bands");
    in >> bands;
    if (bands != kNumBands)
        throw std::runtime_error ("golden file: band layout changed, regenerate");
    expect ("frames");
    in >> f.frames;
    if (! in || f.frames <= 0 || f.frames > 100000)
        throw std::runtime_error ("golden file: bad header");

    expect ("level");
    f.levelDb.resize (static_cast<size_t> (f.frames));
    for (auto& v : f.levelDb)
    {
        int t = 0;
        in >> t;
        v = static_cast<float> (t) / 10.0f;
    }
    f.bandDb.resize (static_cast<size_t> (f.frames * kNumBands));
    for (int fr = 0; fr < f.frames; ++fr)
    {
        expect ("b");
        for (int b = 0; b < kNumBands; ++b)
        {
            int t = 0;
            in >> t;
            f.bandDb[static_cast<size_t> (fr * kNumBands + b)] = static_cast<float> (t) / 10.0f;
        }
    }
    if (! in)
        throw std::runtime_error ("golden file: truncated");
    return f;
}

std::string channelFeaturesToText (const std::vector<Features>& channels)
{
    if (channels.size() == 1)
        return featuresToText (channels[0]);
    std::string out = "vsgolden 2\nchannels " + std::to_string (channels.size()) + "\n";
    for (const auto& f : channels)
        out += featuresToText (f);
    return out;
}

std::vector<Features> channelFeaturesFromText (const std::string& text)
{
    std::istringstream in (text);
    std::string word;
    int version = 0;
    if (! (in >> word) || word != "vsgolden" || ! (in >> version))
        throw std::runtime_error ("golden file: expected 'vsgolden <version>'");
    if (version == 1)
        return { featuresFromText (text) };
    if (version != 2)
        throw std::runtime_error ("golden file: unsupported version");
    int channels = 0;
    if (! (in >> word) || word != "channels" || ! (in >> channels) || channels < 1 || channels > 2)
        throw std::runtime_error ("golden file: expected 'channels 1|2'");
    // The rest is N version-1 bodies, each starting with "vsgolden 1".
    std::string rest ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::vector<Features> out;
    size_t from = rest.find ("vsgolden 1");
    while (from != std::string::npos)
    {
        const size_t next = rest.find ("vsgolden 1", from + 1);
        out.push_back (featuresFromText (rest.substr (from, next == std::string::npos ? std::string::npos : next - from)));
        from = next;
    }
    if (static_cast<int> (out.size()) != channels)
        throw std::runtime_error ("golden file: channel count mismatch");
    return out;
}

void maybeWriteWav (const std::vector<std::vector<float>>& channels, double sampleRate, const std::string& name)
{
    if (channels.size() == 1)
    {
        maybeWriteWav (channels[0], sampleRate, name);
        return;
    }
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    if (dir.isEmpty() || channels.empty())
        return;
    juce::File d (dir);
    d.createDirectory();
    auto file = d.getChildFile (juce::String (name) + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (static_cast<int> (channels.size()))
                                                   .withBitsPerSample (24));
    if (writer == nullptr)
        return;
    std::vector<const float*> ptrs;
    for (const auto& c : channels)
        ptrs.push_back (c.data());
    writer->writeFromFloatArrays (ptrs.data(), static_cast<int> (ptrs.size()), static_cast<int> (channels[0].size()));
}

void maybeWriteWav (const std::vector<float>& mono, double sampleRate, const std::string& name)
{
    const auto dir = juce::SystemStats::getEnvironmentVariable ("VS_RENDER_DIR", {});
    if (dir.isEmpty())
        return;
    juce::File d (dir);
    d.createDirectory();
    auto file = d.getChildFile (juce::String (name) + ".wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (1)
                                                   .withBitsPerSample (24));
    if (writer == nullptr)
        return;
    const float* ch = mono.data();
    writer->writeFromFloatArrays (&ch, 1, static_cast<int> (mono.size()));
}
} // namespace vs::rr
