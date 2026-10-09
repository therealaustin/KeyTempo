// Tempo benchmark over synthesised genre tracks (see bench/GrooveLab.h).
//
//   kt_tempo_bench                 Auto genre
//   kt_tempo_bench --genre         Also run with the matching genre preset
//   kt_tempo_bench --only DnB      Only styles whose name contains the text
//   kt_tempo_bench --wav DIR       Also write each clip as a WAV (for listening)
//   kt_tempo_bench --hard          Add delays, reverb, triplet percussion, rolls, dropped kicks
//
// For each clip the engine runs like the plug-in does (0.4 s updates). Reported:
//   final   – tempo shown at the end of the clip
//   stable  – share of updates after the first 12 s that showed the correct tempo
// A result is correct within ±0.25 BPM. Halftime DnB accepts 87 or 174 in Auto.

#include "AnalysisEngine.h"
#include "bench/GrooveLab.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

using namespace kt;
using groove::Style;

namespace
{
Genre presetFor (Style s)
{
    switch (s)
    {
        case Style::House: case Style::TechHouse: case Style::ProgDotted: case Style::Techno: case Style::UKGarage:
            return Genre::HouseTechno;
        case Style::Trance:
            return Genre::Trance;
        case Style::DnbTwoStep: case Style::DnbAmen: case Style::Neurofunk: case Style::Liquid: case Style::DnbHalftime:
            return Genre::DrumAndBass;
        case Style::Dubstep: case Style::Trap:
            return Genre::DubstepTrap;
        case Style::BoomBap:
            return Genre::HipHop;
        case Style::PopRock:
            return Genre::PopRock;
    }
    return Genre::Auto;
}

const char* classify (double got, double want)
{
    if (got <= 0) return "none";
    const double r = got / want;
    auto near = [&] (double x) { return std::abs (r - x) < 0.012; };
    if (std::abs (got - want) <= 0.25) return "ok";
    if (near (1.0)) return "drift";
    if (near (0.5)) return "half";
    if (near (2.0)) return "double";
    if (near (2.0 / 3.0)) return "2:3";
    if (near (1.5)) return "3:2";
    if (near (0.75)) return "3:4";
    if (near (4.0 / 3.0)) return "4:3";
    return "other";
}

bool acceptable (Style s, Genre g, double got, double want)
{
    if (std::abs (got - want) <= 0.25)
        return true;
    return s == Style::DnbHalftime && g == Genre::Auto && std::abs (got - want / 2) <= 0.25;
}

struct Result
{
    Style style;
    double want, got, stable;
    float confidence;
    std::string feel;
};

void writeWav (const std::string& path, const std::vector<float>& a, double sr)
{
    std::ofstream f (path, std::ios::binary);
    auto u32 = [&] (uint32_t v) { f.write ((const char*) &v, 4); };
    auto u16 = [&] (uint16_t v) { f.write ((const char*) &v, 2); };
    f.write ("RIFF", 4); u32 (36 + (uint32_t) a.size() * 2); f.write ("WAVEfmt ", 8);
    u32 (16); u16 (1); u16 (1); u32 ((uint32_t) sr); u32 ((uint32_t) sr * 2); u16 (2); u16 (16);
    f.write ("data", 4); u32 ((uint32_t) a.size() * 2);
    for (float v : a) { const auto s = (int16_t) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767); f.write ((const char*) &s, 2); }
}

bool hardMode = false;

Result run (const groove::Case& c, Genre genre, unsigned seed, const char* wavDir)
{
    const auto clip = groove::make (c.style, c.bpm, seed, 44100.0, hardMode);
    if (wavDir != nullptr)
        writeWav (std::string (wavDir) + "/" + groove::styleName (c.style) + "_" + std::to_string ((int) c.bpm) + ".wav", clip.audio, clip.sampleRate);

    AnalysisEngine engine;
    engine.prepare (clip.sampleRate);
    engine.setGenre (genre);

    const auto interval = (size_t) (0.4 * clip.sampleRate);
    size_t since = 0, updates = 0, good = 0;
    for (size_t i = 0; i < clip.audio.size(); i += 512)
    {
        const int n = (int) std::min<size_t> (512, clip.audio.size() - i);
        engine.push (clip.audio.data() + i, n);
        if ((since += (size_t) n) >= interval)
        {
            engine.update();
            since = 0;
            if (i > (size_t) (12.0 * clip.sampleRate))
            {
                ++updates;
                const auto& t = engine.getTempo();
                if (t.valid && acceptable (c.style, genre, t.bpm, c.bpm))
                    ++good;
            }
        }
    }
    const auto& t = engine.getTempo();
    return { c.style, c.bpm, t.valid ? t.bpm : 0.0, updates ? (double) good / updates : 0.0, t.confidence,
             t.valid ? std::string (feelName (t.feel)) : std::string ("-") };
}

void runAll (Genre forcedGenre, bool usePreset, const char* filter, const char* wavDir)
{
    auto cases = groove::benchmarkCases();
    if (filter != nullptr)
        cases.erase (std::remove_if (cases.begin(), cases.end(), [&] (auto& c) { return std::strstr (groove::styleName (c.style), filter) == nullptr; }), cases.end());

    std::vector<Result> results (cases.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    const unsigned threads = std::max (1u, std::thread::hardware_concurrency());
    for (unsigned w = 0; w < threads; ++w)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < cases.size();)
            {
                const Genre g = usePreset ? presetFor (cases[i].style) : forcedGenre;
                results[i] = run (cases[i], g, (unsigned) (cases[i].bpm * 10) + (unsigned) cases[i].style * 1000, wavDir);
            }
        });
    for (auto& t : pool)
        t.join();

    std::printf ("\n=== %s%s ===\n", usePreset ? "Genre preset" : genreName (forcedGenre), hardMode ? " (hard)" : "");
    std::printf ("%-20s %7s %8s %6s %7s  %-6s %s\n", "style", "true", "got", "conf", "stable", "class", "feel");
    std::map<std::string, std::pair<int, int>> byStyle;
    int ok = 0;
    double stableSum = 0;
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        const Genre g = usePreset ? presetFor (r.style) : forcedGenre;
        const bool good = acceptable (r.style, g, r.got, r.want);
        ok += good;
        stableSum += r.stable;
        auto& s = byStyle[groove::styleName (r.style)];
        s.first += good;
        s.second += 1;
        std::printf ("%-20s %7.2f %8.2f %6.2f %6.0f%%  %-6s %s\n", groove::styleName (r.style), r.want, r.got, r.confidence,
                     r.stable * 100, good ? "ok" : classify (r.got, r.want), r.feel.c_str());
    }
    std::printf ("\nBy style:");
    for (auto& [name, s] : byStyle)
        std::printf ("  %s %d/%d", name.c_str(), s.first, s.second);
    std::printf ("\nTOTAL: %d/%zu correct at end, mean stability %.0f%%\n", ok, results.size(), 100.0 * stableSum / (double) results.size());
}
} // namespace

int main (int argc, char** argv)
{
    bool preset = false;
    const char* filter = nullptr;
    const char* wavDir = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--genre") == 0) preset = true;
        else if (std::strcmp (argv[i], "--only") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp (argv[i], "--wav") == 0 && i + 1 < argc) wavDir = argv[++i];
        else if (std::strcmp (argv[i], "--hard") == 0) hardMode = true;
    }
    runAll (Genre::Auto, false, filter, wavDir);
    if (preset)
        runAll (Genre::Auto, true, filter, nullptr);
    return 0;
}
