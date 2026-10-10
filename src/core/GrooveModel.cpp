#include "GrooveModel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#ifdef KT_TEMPO_DEBUG
#include <cstdio>
#endif

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

    /** Step `step` of a pattern rotated by `rotation` 8th notes (2 steps each): the
        downbeat is unknown, and the beat phase may itself be off by an 8th when off-beat
        hats or bass are louder than the kick. */
    float at (const std::array<float, 16>& p, int step, int rotation)
    {
        return p[(size_t) (((step + 2 * rotation) % 16 + 16) % 16)];
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
#ifdef KT_TEMPO_DEBUG
    std::fprintf (stderr, "                M:");
    for (auto v : bar.snare) std::fprintf (stderr, "%4.1f", v);
    std::fprintf (stderr, "\n");
#endif
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

    // The beat phase may be off by an 8th when off-beat hats or bass outweigh the kick.
    // Kicks sit on beats far more than on offbeats in every style here, so let the kick
    // decide the 8th shift, then match templates at whole-beat rotations of that grid.
    int shift = 0;
    if (bar.hasKick)
    {
        double onBeat = 0.0, onOff = 0.0;
        for (size_t st = 0; st < 16; st += 4)
        {
            onBeat += bar.kick[st];
            onOff += bar.kick[st + 2];
        }
        shift = onOff > 2.2 * onBeat ? 1 : 0;
    }

    // Template matching, each at its own best bar rotation (the downbeat is unknown).
    for (int r = shift; r < 8; r += 2)
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

    // Too-fast check: at the true tempo the hats (or whatever plays between the kicks)
    // use the 8th offbeats; at twice the tempo those 8ths land on beats and the offbeats
    // go quiet. Measured on the kick-corrected grid. Without hats, compare all bands
    // symmetrically (beats vs. offbeats).
    if (bar.hasHats)
    {
        double best = 0.0;
        for (int r = shift; r < 8; r += 2)
        {
            const double off = mean4 (bar.hats, 2, 6, 10, 14, r);
            const double on = mean4 (bar.hats, 0, 4, 8, 12, r);
            best = std::max (best, off / (on + 0.4));
        }
        fit.offbeatHats = std::clamp (best / 0.45, 0.0, 1.0);
    }
    else if (bar.hasKick || bar.hasSnare)
    {
        double on = 0.0, off = 0.0;
        for (size_t st = 0; st < 16; st += 2)
        {
            const double v = (bar.hasKick ? bar.kick[st] : 0.0f) + (bar.hasSnare ? bar.snare[st] : 0.0f);
            ((st / 2) % 2 == 0 ? on : off) += v;
        }
        fit.offbeatHats = std::clamp (std::min (on, off) / std::max (1.0e-9, std::max (on, off)) / 0.3, 0.0, 1.0);
    }

    // Grid alignment: share of each band's onset energy on even 16th steps (the 8th-note
    // grid) minus the share on odd steps. Independent of bar rotation.
    {
        auto alignment = [] (const std::array<float, 16>& p)
        {
            double even = 0.0, odd = 0.0;
            for (size_t st = 0; st < 16; ++st)
                (st % 2 == 0 ? even : odd) += p[st];
            return (even - odd) / std::max (1.0e-9, even + odd);
        };
        double sum = 0.0, weights = 0.0;
        if (bar.hasKick)  { sum += 0.4 * alignment (bar.kick);  weights += 0.4; }
        if (bar.hasSnare) { sum += 0.3 * alignment (bar.snare); weights += 0.3; }
        if (bar.hasHats)  { sum += 0.3 * alignment (bar.hats);  weights += 0.3; }
        fit.gridAlignment = weights > 0.0 ? std::clamp (sum / weights, 0.0, 1.0) : 0.0;
    }

    const double fof = fit.fourOnFloor * feelPlausibility (Feel::FourOnTheFloor, bpm, genre);
    const double bb = fit.backbeat * feelPlausibility (Feel::Backbeat, bpm, genre) * (0.4 + 0.6 * fit.offbeatHats);
    const double ht = fit.halftime * feelPlausibility (Feel::Halftime, bpm, genre) * fit.offbeatHats;

    // Generic evidence for rhythms the templates don't capture (busy breaks, rolling
    // DnB with rides on beat 3, syncopated house): everything sits on the 8th grid and
    // the 8th offbeats are used (so it isn't twice too fast).
    const double gridFit = std::clamp ((fit.gridAlignment - 0.12) / 0.35, 0.0, 1.0);
    // Requires a structured kick: at the right tempo the kick pattern repeats bar after bar
    // and folds into sharp peaks; at a 3:2 reading it drifts across the bar and smears.
    double kickStructure = 0.5;
    if (bar.hasKick)
        kickStructure = std::clamp ((*std::max_element (bar.kick.begin(), bar.kick.end()) - 1.8) / 1.2, 0.0, 1.0);
    const double generic = 0.6 * gridFit * fit.offbeatHats * kickStructure * feelPlausibility (Feel::Backbeat, bpm, genre);

    fit.plausibility = std::max ({ fof, bb, ht, generic });
    if (std::max ({ fof, bb, ht }) < 0.2)
        fit.feel = (generic >= 0.2 && fit.syncopatedKick && bpm >= 150.0) ? Feel::Breakbeat : Feel::Unknown;
    else if (fof >= 0.6 * std::max (bb, ht)) // claps on 2 & 4 over a four-on-the-floor kick is still four-on-the-floor
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
