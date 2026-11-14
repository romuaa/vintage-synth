// SHA-256, the .bsref reference format and the .bsspec spec format (docs/architecture.md 8.9.3).

#include "FuzzSupport.h"

#include "bside/trace/ReferenceFormat.h"
#include "bside/trace/Sha256.h"

#include <catch2/catch_test_macros.hpp>

using namespace vs::bside::trace;

namespace
{
const std::string kSha (64, 'a');
const std::string kCommit (40, 'b');

std::string refHeader()
{
    return "bside-ref 1\ntrace-sha256 " + kSha + "\nadapter " + kCommit + "\nemulator " + kCommit + "\n";
}

const char* kState = "state 4096 ffffff 000001 7fffff 7fffff 0 1 ff 0 80 0 1 2 7fff 1 0 ff 3 0 10 2f";

void expectRefError (const std::string& text, int line, int col, const std::string& what)
{
    const auto r = readReference (text);
    INFO ("text:\n" << text << "message: " << r.message());
    REQUIRE_FALSE (r.ok);
    const auto& e = r.errors.stored().front();
    CHECK (e.line == line);
    CHECK (e.column == col);
    CHECK (e.what.find (what) != std::string::npos);
}

std::string frameLine (int level = -500, int band = -600)
{
    std::string s = "frame " + std::to_string (level);
    for (int b = 0; b < 23; ++b)
        s += " " + std::to_string (band);
    return s + "\n";
}

std::string segLine (const std::string& name, int a, int b, int v = -700)
{
    std::string s = "segment " + name + " " + std::to_string (a) + " " + std::to_string (b);
    for (int k = 0; k < 120; ++k)
        s += " " + std::to_string (v);
    return s + "\n";
}

std::string specHeader (int frames)
{
    return "bside-spec 1\ntrace-sha256 " + kSha + "\nrate 48000\nbands 23\nframes " + std::to_string (frames) + "\n";
}

void expectSpecError (const std::string& text, int line, int col, const std::string& what)
{
    const auto r = readSpec (text);
    INFO ("text (head):\n" << text.substr (0, 300) << "\nmessage: " << r.message());
    REQUIRE_FALSE (r.ok);
    const auto& e = r.errors.stored().front();
    CHECK (e.line == line);
    CHECK (e.column == col);
    CHECK (e.what.find (what) != std::string::npos);
}
} // namespace

TEST_CASE ("sha256: known vectors", "[bside][sha]")
{
    CHECK (sha256Hex ("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK (sha256Hex ("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK (sha256Hex ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
           == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK (sha256Hex (std::string (1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // The padding boundaries.
    CHECK (sha256Hex (std::string (55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK (sha256Hex (std::string (56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK (sha256Hex (std::string (57, 'a')) == "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6");
    CHECK (sha256Hex (std::string (63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK (sha256Hex (std::string (64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK (sha256Hex (std::string (65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    CHECK (sha256Hex (std::string (119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    CHECK (sha256Hex (std::string (120, 'a')) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
    CHECK (sha256Hex (std::string (128, 'a')) == "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e");
    CHECK (sha256Hex ("bside") == "d40116f6e994a0366ed9734cb6054adda7a6808c712c99a911d2e08db63fc31c");
}

TEST_CASE ("reference: reads, writes canonical text, round trips", "[bside][ref]")
{
    const auto r = readReference (refHeader() + "# a checkpoint\n" + kState + "\nstate 4096 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    INFO (r.message());
    REQUIRE (r.ok);
    const auto& ref = r.reference;
    CHECK (ref.traceSha256 == kSha);
    CHECK (ref.adapter == kCommit);
    REQUIRE (ref.points.size() == 2);
    CHECK (ref.points[0].cycle == 4096);
    CHECK (ref.points[0].state.v[0] == 0xffffff);
    CHECK (ref.points[0].state.v[1] == 1);
    CHECK (ref.points[0].state.v[2] == 0x7fffff);
    CHECK (ref.points[0].state.v[6] == 0xff);
    CHECK (ref.points[0].state.v[12] == 0x7fff);
    CHECK (ref.points[0].state.v[18] == 0x10);
    CHECK (ref.points[0].state.v[19] == 0x2f);
    std::string text;
    REQUIRE (writeReference (ref, text));
    CHECK (text == refHeader() + "state 4096 ffffff 000001 7fffff 7fffff 000000 000001 ff 00 80 0 1 2 7fff 0001 0000 ff 03 00 10 2f\n"
                                 "state 4096 000000 000000 000000 000000 000000 000000 00 00 00 0 0 0 0000 0000 0000 00 00 00 00 00\n");
    const auto again = readReference (text);
    REQUIRE (again.ok);
    CHECK (again.reference.points[0].state == ref.points[0].state);
    std::string text2;
    REQUIRE (writeReference (again.reference, text2));
    CHECK (text2 == text);
    // Upper-case hashes are normalised to lower case.
    CHECK (readReference ("bside-ref 1\ntrace-sha256 " + std::string (64, 'A') + "\nadapter " + std::string (40, 'F') + "\nemulator " + std::string (64, '0') + "\n").ok);
}

TEST_CASE ("reference errors", "[bside][ref][errors]")
{
    expectRefError ("", 1, 1, "missing header");
    expectRefError ("bside-ref 2\n", 1, 11, "unsupported reference version");
    expectRefError ("bside-spec 1\n", 1, 1, "missing header");
    expectRefError ("bside-ref 1\n", 2, 1, "missing header words");
    expectRefError ("bside-ref 1\nadapter " + kCommit + "\n", 2, 1, "out of order");
    expectRefError ("bside-ref 1\ntrace-sha256 abc\n", 2, 14, "bad hash");
    expectRefError ("bside-ref 1\ntrace-sha256 " + std::string (63, 'a') + "\n", 2, 14, "bad hash");
    expectRefError ("bside-ref 1\ntrace-sha256 " + std::string (64, 'g') + "\n", 2, 14, "bad hash");
    expectRefError ("bside-ref 1\ntrace-sha256 " + kSha + "\nadapter " + std::string (41, 'a') + "\n", 3, 9, "bad hash");
    expectRefError ("bside-ref 1\ntrace-sha256 " + kSha + "\ntrace-sha256 " + kSha + "\n", 3, 1, "out of order");
    expectRefError ("bside-ref 1\nstate 0\n", 2, 1, "before the header words");
    expectRefError (refHeader() + "bogus\n", 5, 1, "unknown statement");
    expectRefError (refHeader() + "state\n", 5, 6, "missing value");
    expectRefError (refHeader() + "state x 0\n", 5, 7, "not a number");
    expectRefError (refHeader() + "state 1099511627777 0\n", 5, 7, "out of range");
    expectRefError (refHeader() + "state 5 0 0 0\n", 5, 14, "missing value");
    expectRefError (refHeader() + "state 5 1000000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 5, 9, "acc1: value");
    expectRefError (refHeader() + "state 5 0 0 0 800000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 5, 15, "noise1");
    expectRefError (refHeader() + "state 5 0 0 0 0 0 0 100 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 5, 21, "env1");
    expectRefError (refHeader() + "state 5 0 0 0 0 0 0 0 0 0 3 0 0 0 0 0 0 0 0 0 0\n", 5, 27, "phase1");
    expectRefError (refHeader() + "state 5 0 0 0 0 0 0 0 0 0 0 0 0 8000 0 0 0 0 0 0 0\n", 5, 33, "rate1");
    expectRefError (refHeader() + "state 5 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 zz\n", 5, 47, "rd1C");
    expectRefError (refHeader() + "state 5 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 5, 49, "unexpected");
    expectRefError (refHeader() + "state 9 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\nstate 8 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 6, 7, "before the previous checkpoint");
    expectRefError (refHeader() + std::string ("state 5 \0 0\n", 12), 5, 9, "invalid text");
    expectRefError (refHeader() + "state 5 " + std::string (260, '0') + "\n", 5, 257, "line too long");
}

TEST_CASE ("reference: matching a trace", "[bside][ref]")
{
    Trace t;
    t.clockHz = 985248;
    t.model = Model::A;
    t.check = 40;
    t.endCycle = 100;
    t.writes = { { 0, 0x18, 0x0f }, { 50, 0x04, 0x11 } };
    std::string traceText;
    REQUIRE (writeTrace (t, traceText));
    std::vector<Checkpoint> schedule;
    REQUIRE (checkpointSchedule (t, schedule)); // 0w, 40p, 50w, 80p, 100e
    REQUIRE (schedule.size() == 5);

    Reference ref;
    ref.traceSha256 = sha256Hex (traceText);
    ref.adapter = kCommit;
    ref.emulator = std::string (64, 'c');
    for (const auto& s : schedule)
        ref.points.push_back ({ s.cycle, {} });
    const std::string_view bytes (traceText);
    std::string why;
    CHECK (referenceMatchesTrace (ref, t, &bytes, &why));
    CHECK (referenceMatchesTrace (ref, t, nullptr, &why));

    auto wrongHash = ref;
    wrongHash.traceSha256 = std::string (64, '0');
    CHECK_FALSE (referenceMatchesTrace (wrongHash, t, &bytes, &why));
    CHECK (why.find ("different trace") != std::string::npos);
    CHECK (referenceMatchesTrace (wrongHash, t, nullptr, &why)); // not checked without the bytes

    auto fewer = ref;
    fewer.points.pop_back();
    CHECK_FALSE (referenceMatchesTrace (fewer, t, nullptr, &why));
    CHECK (why.find ("4 checkpoints") != std::string::npos);

    auto moved = ref;
    moved.points[1].cycle = 41;
    CHECK_FALSE (referenceMatchesTrace (moved, t, nullptr, &why));
    CHECK (why.find ("checkpoint 1") != std::string::npos);
}

TEST_CASE ("spec: reads, writes canonical text, round trips", "[bside][spec]")
{
    const auto text = specHeader (3) + "# frames\n" + frameLine (-100, -200) + frameLine (-1200, 300) + frameLine() + segLine ("attack", 0, 2)
                      + segLine ("tail_1", 1, 3, 300);
    const auto r = readSpec (text);
    INFO (r.message());
    REQUIRE (r.ok);
    const auto& s = r.spec;
    CHECK (s.frames == 3);
    CHECK (s.levelTenthDb[0] == -100);
    CHECK (s.levelTenthDb[1] == -1200);
    CHECK (s.band (0, 22) == -200);
    CHECK (s.band (1, 0) == 300);
    REQUIRE (s.segments.size() == 2);
    CHECK (s.segments[0].name == "attack");
    CHECK (s.segments[0].firstFrame == 0);
    CHECK (s.segments[0].endFrame == 2);
    CHECK (s.segments[0].spectrum[119] == -700);
    CHECK (s.segments[1].spectrum[0] == 300);
    std::string out;
    REQUIRE (writeSpec (s, out));
    CHECK (out == specHeader (3) + frameLine (-100, -200) + frameLine (-1200, 300) + frameLine() + segLine ("attack", 0, 2) + segLine ("tail_1", 1, 3, 300));
    const auto again = readSpec (out);
    REQUIRE (again.ok);
    std::string out2;
    REQUIRE (writeSpec (again.spec, out2));
    CHECK (out2 == out);
    // No segments is fine.
    CHECK (readSpec (specHeader (1) + frameLine()).ok);
}

TEST_CASE ("spec errors", "[bside][spec][errors]")
{
    const std::string h2 = specHeader (2);
    expectSpecError ("", 1, 1, "missing header");
    expectSpecError ("bside-spec 3\n", 1, 12, "unsupported spec version");
    expectSpecError ("bside-spec 1\n", 2, 1, "missing header words");
    expectSpecError ("bside-spec 1\nrate 48000\n", 2, 1, "out of order");
    expectSpecError ("bside-spec 1\ntrace-sha256 " + kSha + "\nrate 44100\n", 3, 6, "unsupported sample rate");
    expectSpecError ("bside-spec 1\ntrace-sha256 " + kSha + "\nrate 48000\nbands 24\n", 4, 7, "unsupported band count");
    expectSpecError ("bside-spec 1\ntrace-sha256 " + kSha + "\nrate 48000\nbands 23\nframes 0\n", 5, 8, "out of range");
    expectSpecError ("bside-spec 1\ntrace-sha256 " + kSha + "\nrate 48000\nbands 23\nframes 100001\n", 5, 8, "out of range");
    expectSpecError ("bside-spec 1\ntrace-sha256 " + kSha + "\nrate 48000\nbands 23\nframes x\n", 5, 8, "not a number");
    expectSpecError (h2 + frameLine(), 7, 1, "only 1 frame lines");
    expectSpecError (h2 + frameLine() + frameLine() + frameLine(), 8, 1, "more frame lines");
    expectSpecError (h2 + frameLine() + segLine ("a", 0, 1), 7, 1, "before all 2 frames");
    expectSpecError (h2 + frameLine() + "frame 1\n", 7, 8, "missing value");
    expectSpecError (h2 + frameLine (1201), 6, 7, "out of range");
    expectSpecError (h2 + frameLine (-1201), 6, 7, "out of range");
    expectSpecError (h2 + frameLine (0, 301), 6, 9, "out of range");
    expectSpecError (h2 + "frame x\n", 6, 7, "not a number");
    expectSpecError (h2 + frameLine() + frameLine() + frameLine().substr (0, 6) + " 1\n", 8, 1, "more frame lines");
    expectSpecError (h2 + frameLine() + frameLine() + "bogus\n", 8, 1, "unknown statement");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("Bad", 0, 1), 8, 9, "bad segment name");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("a", 0, 1) + segLine ("a", 0, 1), 9, 9, "duplicate segment");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("a", 1, 1), 8, 13, "empty or reversed");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("a", 0, 3), 8, 13, "out of range");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("a", 0, 1, 9999), 8, 15, "out of range");
    expectSpecError (h2 + frameLine() + frameLine() + "segment a 0 1 1 2\n", 8, 18, "missing value");
    expectSpecError (h2 + frameLine() + frameLine() + segLine ("a", 0, 1) + frameLine(), 9, 1, "'frame' after a 'segment'");
    expectSpecError (h2 + frameLine() + frameLine() + segLine (std::string (33, 'a'), 0, 1), 8, 9, "bad segment name");
    std::string many = h2 + frameLine() + frameLine();
    for (int i = 0; i < 64; ++i)
        many += segLine ("s" + std::to_string (i), 0, 1);
    CHECK (readSpec (many).ok);
    expectSpecError (many + segLine ("extra", 0, 1), 8 + 64, 1, "too many segments");
    expectSpecError (h2 + "# c\n" + std::string (1100, 'x') + "\n", 7, 1025, "line too long");
}

TEST_CASE ("spec: bounds on frames", "[bside][spec][bounds]")
{
    std::string t = specHeader (100000);
    const auto line = frameLine (-1200, -1200);
    for (int i = 0; i < 100000; ++i)
        t += line;
    CHECK (t.size() <= kMaxSpecBytes);
    const auto r = readSpec (t);
    INFO (r.message());
    REQUIRE (r.ok);
    CHECK (r.spec.frames == 100000);
    CHECK (r.spec.bandTenthDb.size() == 2300000);
    std::string out;
    REQUIRE (writeSpec (r.spec, out));
    CHECK (out == t);
}

TEST_CASE ("spec and reference: random ones round trip", "[bside][spec][ref]")
{
    bst::Rng rng (5150);
    for (int i = 0; i < 60; ++i)
    {
        Spec s;
        s.traceSha256 = kSha;
        s.frames = rng.range (1, 40);
        for (int f = 0; f < s.frames; ++f)
        {
            s.levelTenthDb.push_back (static_cast<std::int16_t> (rng.range (-1200, 300)));
            for (int b = 0; b < kSpecBands; ++b)
                s.bandTenthDb.push_back (static_cast<std::int16_t> (rng.range (-1200, 300)));
        }
        const int nseg = rng.range (0, 4);
        for (int k = 0; k < nseg; ++k)
        {
            SpecSegment g;
            g.name = "seg" + std::to_string (k);
            g.firstFrame = static_cast<std::uint32_t> (rng.range (0, s.frames - 1));
            g.endFrame = static_cast<std::uint32_t> (rng.range (static_cast<int> (g.firstFrame) + 1, s.frames));
            for (auto& v : g.spectrum)
                v = static_cast<std::int16_t> (rng.range (-1200, 300));
            s.segments.push_back (g);
        }
        std::string text;
        REQUIRE (writeSpec (s, text));
        const auto r = readSpec (text);
        INFO (r.message());
        REQUIRE (r.ok);
        CHECK (r.spec.levelTenthDb == s.levelTenthDb);
        CHECK (r.spec.bandTenthDb == s.bandTenthDb);
        REQUIRE (r.spec.segments.size() == s.segments.size());
        for (size_t k = 0; k < s.segments.size(); ++k)
            CHECK (r.spec.segments[k].spectrum == s.segments[k].spectrum);
    }
    for (int i = 0; i < 60; ++i)
    {
        Reference ref;
        ref.traceSha256 = kSha;
        ref.adapter = kCommit;
        ref.emulator = kCommit;
        std::uint64_t c = 0;
        for (int k = 0; k < rng.range (0, 30); ++k)
        {
            c += static_cast<std::uint64_t> (rng.range (0, 5000));
            Reference::Point p;
            p.cycle = c;
            for (int f = 0; f < kStateFields; ++f)
                p.state.v[static_cast<size_t> (f)] = static_cast<std::uint32_t> (rng.next() % (static_cast<std::uint64_t> (kStateFieldMax[f]) + 1));
            ref.points.push_back (p);
        }
        std::string text;
        REQUIRE (writeReference (ref, text));
        const auto r = readReference (text);
        INFO (text << r.message());
        REQUIRE (r.ok);
        REQUIRE (r.reference.points.size() == ref.points.size());
        for (size_t k = 0; k < ref.points.size(); ++k)
        {
            CHECK (r.reference.points[k].cycle == ref.points[k].cycle);
            CHECK (r.reference.points[k].state == ref.points[k].state);
        }
    }
}

TEST_CASE ("spec and reference: writers refuse invalid data", "[bside][spec][ref]")
{
    Reference ref;
    std::string out = "x";
    CHECK_FALSE (writeReference (ref, out)); // empty hashes
    CHECK (out.empty());
    ref.traceSha256 = kSha;
    ref.adapter = kCommit;
    ref.emulator = kCommit;
    CHECK (writeReference (ref, out));
    ref.points.push_back ({ 5, {} });
    ref.points[0].state.v[9] = 3; // phase above 2
    CHECK_FALSE (writeReference (ref, out));
    Spec s;
    s.traceSha256 = kSha;
    CHECK_FALSE (writeSpec (s, out)); // zero frames
}
