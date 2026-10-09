#include "GrooveModel.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace kt
{
namespace
{
    /** Piecewise-linear ramp: 0 below a, 1 between b and c, 0 above d. `floor` is the
        minimum value returned outside the plateau. */
    double trapezoid (double x, double a, double b, double c, double d, double floor = 0.0)
    {
        double v = 0.0;
        if (x <= a || x >= d) v = 0.0;
        else if (x < b) v = (x - a) / (b - a);
        else if (x <= c) v = 1.0;
        else v = (d - x) / (d - c);
        return std::max (floor, v);
    }

    float at (const std::array<float, 16>& p, int step, int rotation)
    {
        return p[(size_t) (((step + 4 * rotation) % 16 + 16) % 16)];
    }

    float mean4 (const std::array<float, 16>& p, int a, int b, int c, int d, int r)
    {
        return 0.25f * (at (p, a, r) + at (p, b, r) + at (p, c, r) + at (p, d, r));
    }
} // namespace

FoldedBar foldBar (const BandEnvelopes& bands, double periodFrames, double phaseFrames)
{
    FoldedBar bar;
    const double stepFrames = periodFrames / 4.0;
    const int n = (int) bands.kick.size();

    auto fold = [&] (const std::vector<float>& env, std::array<float, 16>& out) -> bool
    {
        std::array<double, 16> sum {};
        std::array<int, 16> count {};
        for (int i = 0; i < n; ++i)
        {
            // Bins are shifted slightly late ([-0.3, +0.7) of a step) so swung or laid-back
            // notes still land in their own bin.
            const double q = (i - phaseFrames) / stepFrames + 0.3;
            const int bin = (((int) std::floor (q)) % 16 + 16) % 16;
            sum[(size_t) bin] += env[(size_t) i];
            ++count[(size_t) bin];
        }
        double total = 0.0;
        for (size_t b = 0; b < 16; ++b)
        {
            sum[b] = count[b] > 0 ? sum[b] / count[b] : 0.0;
            total += sum[b];
        }
        const double mean = total / 16.0;
        if (mean <= 1.0e-9)
            return false;
        for (size_t b = 0; b < 16; ++b)
            out[b] = (float) (sum[b] / mean);
        return true;
    };

    bar.hasKick = fold (bands.kick, bar.kick);
    bar.hasSnare = fold (bands.snare, bar.snare);
    bar.hasHats = fold (bands.hats, bar.hats);

    // A snare or clap is a noise burst that lights up the mids *and* the top end, while
    // synth stabs, plucks and basses are mostly mid-only and hats are top-only. The
    // geometric mean of the two bands keeps the snare and rejects both impostors.
    if (bar.hasSnare && bar.hasHats)
    {
        double total = 0.0;
        for (size_t b = 0; b < 16; ++b)
        {
            bar.snare[b] = std::sqrt (bar.snare[b] * bar.hats[b]);
            total += bar.snare[b];
        }
        if (total > 1.0e-9)
            for (auto& v : bar.snare)
                v = (float) (v * 16.0 / total);
    }
    return bar;
}

double feelPlausibility (Feel feel, double bpm, Genre genre)
{
    if (genre != Genre::Auto)
    {
        // Presets already restrict the tempo range; inside it, weigh the feels the genre uses.
        switch (genre)
        {
            case Genre::HouseTechno:
            case Genre::Trance:
                return feel == Feel::FourOnTheFloor ? 1.0 : 0.6;
            case Genre::DrumAndBass:
                return feel == Feel::FourOnTheFloor ? 0.4 : 1.0;
            case Genre::DubstepTrap:
                return feel == Feel::Halftime ? 1.0 : 0.7;
            case Genre::HipHop:
                return feel == Feel::Halftime ? 0.5 : 1.0;
            default:
                break; // Pop / Rock / Other: same conventions as Auto
        }
    }

    // Auto: where each feel lives in DJ / producer tempo conventions.
    switch (feel)
    {
        case Feel::FourOnTheFloor:
            // House, techno, trance, hard dance; rarer at hip-hop or DnB tempos.
            return trapezoid (bpm, 95.0, 112.0, 152.0, 172.0, 0.12);
        case Feel::Backbeat:
        case Feel::Breakbeat:
            // Hip-hop, pop, rock, garage, DnB. Below ~80 BPM a backbeat is usually the
            // half-time reading of a 140-160 BPM dubstep/trap track.
            return trapezoid (bpm, 62.0, 82.0, 186.0, 200.0, 0.25);
        case Feel::Halftime:
            // Dubstep / trap (counted at the full tempo). Above ~160 a half-time groove is
            // just as likely a hip-hop backbeat at half the speed, which wins the tie.
            return trapezoid (bpm, 112.0, 128.0, 160.0, 192.0, 0.08);
        case Feel::Unknown:
            break;
    }
    return 0.3;
}

GrooveFit fitGroove (const FoldedBar& bar, double bpm, Genre genre)
{
    GrooveFit fit;

    // Template matching, each at its own best bar rotation (the downbeat is unknown).
    for (int r = 0; r < 4; ++r)
    {
        if (bar.hasKick)
        {
            // Kick on all four beats, nothing much between them on the 8th offbeats.
            const float onBeats = std::min ({ at (bar.kick, 0, r), at (bar.kick, 4, r), at (bar.kick, 8, r), at (bar.kick, 12, r) });
            const float off8 = mean4 (bar.kick, 2, 6, 10, 14, r);
            fit.fourOnFloor = std::max (fit.fourOnFloor, std::clamp ((onBeats - 0.6 * off8) / 2.4, 0.0, 1.0));
        }
        if (bar.hasSnare)
        {
            // Backbeat: snare on beats 2 and 4, quieter on 1 and 3 and on 8th offbeats.
            const float s2 = at (bar.snare, 4, r), s4 = at (bar.snare, 12, r);
            const float s1 = at (bar.snare, 0, r), s3 = at (bar.snare, 8, r);
            const float off8 = mean4 (bar.snare, 2, 6, 10, 14, r);
            const double bb = std::min (s2, s4) - std::max (0.5f * (s1 + s3), off8);
            fit.backbeat = std::max (fit.backbeat, std::clamp (bb / 2.2, 0.0, 1.0));

            // Half-time: one big snare on beat 3.
            const double ht = s3 - std::max ({ s1, s2, s4, off8 });
            fit.halftime = std::max (fit.halftime, std::clamp (ht / 3.0, 0.0, 1.0));
        }
    }

    // Breakbeat vs. straight backbeat: does a good share of the kick land off the beats?
    if (bar.hasKick)
    {
        double onBeats = 0.0, off = 0.0;
        for (int st = 0; st < 16; ++st)
            (st % 4 == 0 ? onBeats : off) += bar.kick[(size_t) st];
        fit.syncopatedKick = off / 12.0 > 0.55 * (onBeats / 4.0) * 0.5 && off > 0.3 * (onBeats + off);
    }

    // If hats exist but nothing hits the 8th offbeats, every "8th" of the real groove is
    // landing on a beat: this tempo is probably twice too fast.
    if (bar.hasHats)
    {
        double best = 0.0;
        for (int r = 0; r < 4; ++r)
        {
            const double off = mean4 (bar.hats, 2, 6, 10, 14, r);
            const double on = mean4 (bar.hats, 0, 4, 8, 12, r);
            best = std::max (best, off / (on + 0.4));
        }
        fit.offbeatHats = std::clamp (best / 0.45, 0.0, 1.0);
    }

    const double fof = fit.fourOnFloor * feelPlausibility (Feel::FourOnTheFloor, bpm, genre);
    const double bb = fit.backbeat * feelPlausibility (Feel::Backbeat, bpm, genre) * (0.4 + 0.6 * fit.offbeatHats);
    const double ht = fit.halftime * feelPlausibility (Feel::Halftime, bpm, genre) * fit.offbeatHats;

    fit.plausibility = std::max ({ fof, bb, ht });
    if (fit.plausibility < 0.2)
        fit.feel = Feel::Unknown;
    else if (fof >= bb && fof >= ht)
        fit.feel = Feel::FourOnTheFloor;
    else if (ht > bb)
        fit.feel = Feel::Halftime;
    else
        // "Breakbeat" here means the DnB / jungle family: a backbeat at 150+ BPM with a
        // kick that dances around the beats.
        fit.feel = (fit.syncopatedKick && bpm >= 150.0) ? Feel::Breakbeat : Feel::Backbeat;
    return fit;
}
} // namespace kt
