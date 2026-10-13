// Tempo benchmark over synthesised genre tracks (see bench/GrooveLab.h).
//
//   kt_tempo_bench                 Auto genre
//   kt_tempo_bench --genre         Also run with the matching genre preset
//   kt_tempo_bench --only DnB      Only styles whose name contains the text
//   kt_tempo_bench --wav DIR       Also write each clip as a WAV (for listening)
//   kt_tempo_bench --files LIST    Real tracks instead: each line "path<TAB>sampleRate<TAB>trueBpm<TAB>name",
//                                  path = raw mono float32 audio, e.g. from
//                                  ffmpeg -i song.mp3 -ac 1 -ar 44100 -f f32le song.f32
//                                  An optional 5th column gives the time (s) of any true downbeat;
//                                  the whole-track grid is then scored against it. An optional 6th
//                                  column gives the key ("Am", "F#m", "8A", "1m"), scored too.
//   kt_tempo_bench --min N         Exit with an error if fewer than N clips are correct (CI)
//   kt_tempo_bench --seed N        Different random variations of every clip
//   kt_tempo_bench --hard          Add delays, reverb, triplet percussion, rolls, dropped kicks
//
// For each clip the engine runs like the plug-in does (0.4 s updates). Reported:
//   final   – tempo shown at the end of the clip
//   stable  – share of updates after the first 12 s that showed the correct tempo
// A result is correct within ±0.25 BPM. Halftime DnB accepts 87 or 174 in Auto.

#include "AnalysisEngine.h"
#include "MusicTheory.h"
#include "TrackAnalyzer.h"
#include "bench/GrooveLab.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        case Style::DnbTwoStep: case Style::DnbAmen: case Style::Neurofunk: case Style::Liquid: case Style::DnbHalftime: case Style::DnbRolling:
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
unsigned seedOffset = 0;

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

int runAll (Genre forcedGenre, bool usePreset, const char* filter, const char* wavDir)
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
                results[i] = run (cases[i], g, (unsigned) (cases[i].bpm * 10) + (unsigned) cases[i].style * 1000 + seedOffset * 7919u, wavDir);
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
    return ok;
}
} // namespace

struct RealTrack
{
    std::string path, name;
    double sampleRate = 44100.0, bpm = 0.0;
    double downbeat = -1.0; // seconds of any true downbeat (beat 1), < 0 if unknown
    int key = -1;           // 0..23 (MusicTheory.h), -1 if unknown
};

/** Parses "Am", "F#m", "E♭ minor", "Gb", Camelot "8A" or Open Key "1m". -1 if not a key. */
int parseKey (std::string text)
{
    while (! text.empty() && std::isspace ((unsigned char) text.back())) text.pop_back();
    while (! text.empty() && std::isspace ((unsigned char) text.front())) text.erase (0, 1);
    if (text.empty())
        return -1;

    if (std::isdigit ((unsigned char) text[0]))
    {
        const int number = std::atoi (text.c_str());
        const char mode = (char) std::toupper ((unsigned char) text.back());
        if (number < 1 || number > 12)
            return -1;
        std::string camelot;
        if (mode == 'A' || mode == 'B')
            camelot = std::to_string (number) + mode;
        else if (mode == 'M' || mode == 'D') // Open Key: 1d = C major = 8B
            camelot = std::to_string ((number + 6) % 12 + 1) + (mode == 'M' ? 'A' : 'B');
        for (int k = 0; k < theory::numKeys; ++k)
            if (theory::camelot (k) == camelot)
                return k;
        return -1;
    }

    static const int letters[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A..G
    const char letter = (char) std::toupper ((unsigned char) text[0]);
    if (letter < 'A' || letter > 'G')
        return -1;
    int pc = letters[letter - 'A'];
    std::string rest = text.substr (1);
    if (rest.rfind ("#", 0) == 0) { ++pc; rest.erase (0, 1); }
    else if (rest.rfind ("\xe2\x99\xaf", 0) == 0) { ++pc; rest.erase (0, 3); }   // ♯
    else if (rest.rfind ("\xe2\x99\xad", 0) == 0) { --pc; rest.erase (0, 3); }   // ♭
    else if (rest.rfind ("b", 0) == 0) { --pc; rest.erase (0, 1); }
    while (! rest.empty() && rest[0] == ' ') rest.erase (0, 1);
    const bool minor = ! rest.empty() && rest[0] == 'm' && rest.rfind ("maj", 0) != 0;
    return theory::makeKey (pc, minor);
}

const char* classifyKey (int got, int want)
{
    if (got < 0) return "none";
    if (got == want) return "ok";
    if (got == theory::relativeKey (want)) return "relative";
    if (theory::tonic (got) == theory::tonic (want)) return "parallel";
    const int interval = (theory::tonic (got) - theory::tonic (want) + 12) % 12;
    if (theory::isMinor (got) == theory::isMinor (want) && (interval == 5 || interval == 7)) return "fifth";
    return "other";
}

/** Whole-track (TrackAnalyzer) scores against the truth. Drift and grid offsets are only
    meaningful when the BPM is in the right family, so they are NaN otherwise. */
struct GridScore
{
    double bpm = 0, bpmError = 0;
    double driftMs = std::nan ("");    // grid position error at the end of the track
    double beatOffsetMs = std::nan (""); // grid vs. the true downbeat, to the nearest beat
    int downbeatShift = -1;              // beats between our beat 1 and the true one (0 = right)
    int key = -1;
};

GridScore scoreGrid (const TrackAnalysis& a, const RealTrack& t)
{
    GridScore g;
    if (! a.valid)
        return g;
    g.bpm = a.bpm;
    g.key = a.key;
    g.bpmError = a.bpm - t.bpm;
    if (std::abs (a.bpm / t.bpm - 1.0) > 0.012)
        return g;

    // Beat k of the true grid is at downbeat + k * 60 / bpm; ours drifts by (true / got - 1)
    // of the elapsed time.
    g.driftMs = 1000.0 * a.durationSeconds * (t.bpm / a.bpm - 1.0);
    if (t.downbeat < 0)
        return g;

    const double period = 60.0 / a.bpm;
    const double beats = (t.downbeat - a.firstBeatSeconds) / period;
    const double k = std::round (beats);
    g.beatOffsetMs = (beats - k) * period * 1000.0;
    const int n = std::max (1, a.numerator);
    g.downbeatShift = (int) (((long long) k % n + n) % n);
    return g;
}

/** Real-track mode: the plug-in engine (same scoring as the synthetic benchmark) and the
    whole-track analyser TrueGrid uses. */
int runFiles (const char* listPath, Genre genre)
{
    std::vector<RealTrack> tracks;
    std::ifstream list (listPath);
    for (std::string line; std::getline (list, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> cols;
        for (size_t from = 0;;)
        {
            const size_t tab = line.find ('\t', from);
            cols.push_back (line.substr (from, tab - from));
            if (tab == std::string::npos)
                break;
            from = tab + 1;
        }
        if (cols.size() < 3)
            continue;
        RealTrack t;
        t.path = cols[0];
        t.sampleRate = std::atof (cols[1].c_str());
        t.bpm = std::atof (cols[2].c_str());
        t.name = cols.size() > 3 ? cols[3] : t.path;
        if (cols.size() > 4 && ! cols[4].empty())
            t.downbeat = std::atof (cols[4].c_str());
        if (cols.size() > 5)
            t.key = parseKey (cols[5]);
        tracks.push_back (t);
    }

    struct Out { double got = 0, stable = 0, firstCorrect = -1; float conf = 0; std::string feel; GridScore grid; };
    std::vector<Out> outs (tracks.size());
    std::atomic<size_t> next { 0 };
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < std::max (1u, std::thread::hardware_concurrency()); ++w)
        pool.emplace_back ([&]
        {
            for (size_t i; (i = next++) < tracks.size();)
            {
                const auto& t = tracks[i];
                std::ifstream f (t.path, std::ios::binary | std::ios::ate);
                std::vector<float> audio ((size_t) f.tellg() / sizeof (float));
                f.seekg (0);
                f.read ((char*) audio.data(), (std::streamsize) (audio.size() * sizeof (float)));

                AnalysisEngine engine;
                engine.prepare (t.sampleRate);
                engine.setGenre (genre);
                const auto interval = (size_t) (0.4 * t.sampleRate);
                size_t since = 0, updates = 0, good = 0;
                for (size_t p = 0; p < audio.size(); p += 512)
                {
                    const int n = (int) std::min<size_t> (512, audio.size() - p);
                    engine.push (audio.data() + p, n);
                    if ((since += (size_t) n) >= interval)
                    {
                        engine.update();
                        since = 0;
                        const auto& r = engine.getTempo();
                        const bool ok = r.valid && std::abs (r.bpm - t.bpm) <= 0.25;
                        if (ok && outs[i].firstCorrect < 0)
                            outs[i].firstCorrect = (double) p / t.sampleRate;
                        if (p > (size_t) (30.0 * t.sampleRate)) // after the first 30 s
                        {
                            ++updates;
                            good += ok;
                        }
                    }
                }
                const auto& r = engine.getTempo();
                TrackAnalysisOptions options;
                options.genre = genre;
                outs[i] = { r.valid ? r.bpm : 0.0, updates ? (double) good / updates : 0.0, outs[i].firstCorrect, r.confidence,
                            r.valid ? feelName (r.feel) : "-",
                            scoreGrid (analyseTrack (audio.data(), audio.size(), t.sampleRate, options), t) };
            }
        });
    for (auto& th : pool)
        th.join();

    std::printf ("\n=== Real tracks, %s: plug-in engine ===\n%-32s %7s %8s %6s %8s %8s  %-6s %s\n", genreName (genre), "track", "true",
                 "got", "conf", "stable", "locks@", "class", "feel");
    int ok = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto& o = outs[i];
        const bool good = std::abs (o.got - tracks[i].bpm) <= 0.25;
        ok += good;
        std::printf ("%-32.32s %7.2f %8.2f %6.2f %7.0f%% %7.0fs  %-6s %s\n", tracks[i].name.c_str(), tracks[i].bpm, o.got, o.conf,
                     o.stable * 100, o.firstCorrect, good ? "ok" : classify (o.got, tracks[i].bpm), o.feel.c_str());
    }
    std::printf ("TOTAL: %d/%zu correct at end\n", ok, tracks.size());

    // Whole-track analyser. "drift" = how far the constant grid is off by the end of the
    // track; "offset" = grid vs. the listed downbeat; "db" = our beat 1 vs. the true one.
    std::printf ("\n=== Real tracks, %s: whole-track analyser (TrueGrid) ===\n%-32s %7s %8s %8s %8s %8s %4s  %s\n", genreName (genre),
                 "track", "true", "got", "error", "drift", "offset", "db", "class");
    int gridOk = 0, haveDownbeat = 0, offsetOk = 0, downbeatOk = 0, inFamily = 0;
    double absErrSum = 0, worstErr = 0, absDriftSum = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto& g = outs[i].grid;
        const bool good = std::abs (g.bpmError) <= 0.25 && g.bpm > 0;
        gridOk += good;
        char drift[16] = "-", offset[16] = "-", db[8] = "-";
        if (! std::isnan (g.driftMs))
        {
            ++inFamily;
            absErrSum += std::abs (g.bpmError);
            worstErr = std::max (worstErr, std::abs (g.bpmError));
            absDriftSum += std::abs (g.driftMs);
            std::snprintf (drift, sizeof (drift), "%+.0fms", g.driftMs);
        }
        if (tracks[i].downbeat >= 0)
        {
            ++haveDownbeat;
            if (! std::isnan (g.beatOffsetMs))
            {
                std::snprintf (offset, sizeof (offset), "%+.0fms", g.beatOffsetMs);
                std::snprintf (db, sizeof (db), g.downbeatShift == 0 ? "ok" : "+%d", g.downbeatShift);
                offsetOk += std::abs (g.beatOffsetMs) <= 20.0;
                downbeatOk += g.downbeatShift == 0 && std::abs (g.beatOffsetMs) <= 20.0;
            }
        }
        std::printf ("%-32.32s %7.2f %8.3f %+8.3f %8s %8s %4s  %s\n", tracks[i].name.c_str(), tracks[i].bpm, g.bpm, g.bpmError, drift,
                     offset, db, good ? "ok" : classify (g.bpm, tracks[i].bpm));
    }
    std::printf ("TOTAL: %d/%zu correct", gridOk, tracks.size());
    if (inFamily > 0)
        std::printf (", mean |error| %.3f BPM (worst %.3f), mean |drift| %.0f ms", absErrSum / inFamily, worstErr, absDriftSum / inFamily);
    if (haveDownbeat > 0)
        std::printf (", grid within 20 ms %d/%d, downbeat %d/%d", offsetOk, haveDownbeat, downbeatOk, haveDownbeat);
    std::printf ("\n");

    // Key, for tracks that list one (6th column).
    int haveKey = 0, keyOk = 0, keyRelated = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (tracks[i].key < 0)
            continue;
        if (haveKey++ == 0)
            std::printf ("\n=== Key (whole-track analyser) ===\n%-32s %-10s %-10s %s\n", "track", "true", "got", "class");
        const int got = outs[i].grid.key;
        const char* c = classifyKey (got, tracks[i].key);
        keyOk += got == tracks[i].key;
        keyRelated += std::strcmp (c, "relative") == 0 || std::strcmp (c, "fifth") == 0 || std::strcmp (c, "parallel") == 0;
        std::printf ("%-32.32s %-10s %-10s %s\n", tracks[i].name.c_str(), theory::camelot (tracks[i].key).c_str(),
                     got >= 0 ? theory::camelot (got).c_str() : "-", c);
    }
    if (haveKey > 0)
        std::printf ("TOTAL: %d/%d keys correct, %d more off by a relative, fifth or parallel key\n", keyOk, haveKey, keyRelated);
    return ok;
}

int main (int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp (argv[i], "--files") == 0)
        {
            Genre g = Genre::Auto;
            for (int j = 1; j + 1 < argc; ++j)
                if (std::strcmp (argv[j], "--genre-index") == 0)
                    g = (Genre) std::atoi (argv[j + 1]);
            runFiles (argv[i + 1], g);
            return 0;
        }

    bool preset = false;
    const char* filter = nullptr;
    const char* wavDir = nullptr;
    int minCorrect = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--genre") == 0) preset = true;
        else if (std::strcmp (argv[i], "--only") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp (argv[i], "--wav") == 0 && i + 1 < argc) wavDir = argv[++i];
        else if (std::strcmp (argv[i], "--hard") == 0) hardMode = true;
        else if (std::strcmp (argv[i], "--min") == 0 && i + 1 < argc) minCorrect = std::atoi (argv[++i]);
        else if (std::strcmp (argv[i], "--seed") == 0 && i + 1 < argc) seedOffset = (unsigned) std::atoi (argv[++i]);
    }
    int worst = runAll (Genre::Auto, false, filter, wavDir);
    if (preset)
        worst = std::min (worst, runAll (Genre::Auto, true, filter, nullptr));
    if (minCorrect > 0 && worst < minCorrect)
    {
        std::printf ("FAILED: fewer than %d correct\n", minCorrect);
        return 1;
    }
    return 0;
}
