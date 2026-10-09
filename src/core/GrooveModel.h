#pragma once

#include "Genre.h"

#include <array>
#include <vector>

namespace kt
{
/** Onset envelopes split by instrument role, all at the same frame rate. Values are
    half-wave rectified onset strengths (local mean removed), oldest first. */
struct BandEnvelopes
{
    std::vector<float> kick;  // < ~110 Hz, linear magnitude flux
    std::vector<float> snare; // ~200 Hz - 4 kHz, compressed flux
    std::vector<float> hats;  // > ~6 kHz, compressed flux
};

/** A 4-beat bar folded into 16 sixteenth-note bins per band, each normalised to mean 1. */
struct FoldedBar
{
    std::array<float, 16> kick {}, snare {}, hats {};
    bool hasKick = false, hasSnare = false, hasHats = false;
};

struct GrooveFit
{
    double fourOnFloor = 0.0; // 0..1 template strengths
    double backbeat = 0.0;
    double halftime = 0.0;
    double offbeatHats = 1.0; // 0..1: are 8th offbeats populated? (low = tempo likely 2x too fast)
    double plausibility = 0.0; // 0..1: best template strength x genre/tempo plausibility
    bool syncopatedKick = false; // a large share of kick hits fall between the beats
    Feel feel = Feel::Unknown;
};

/** Folds band envelopes at a beat period (frames) and phase (frame of a beat). */
FoldedBar foldBar (const BandEnvelopes& bands, double periodFrames, double phaseFrames);

/** Matches a folded bar against groove templates and weighs them by how typical that
    feel is at `bpm` for the given genre (Auto = DJ-music conventions). */
GrooveFit fitGroove (const FoldedBar& bar, double bpm, Genre genre);

/** How plausible a feel is at a tempo for a genre (0..1). */
double feelPlausibility (Feel feel, double bpm, Genre genre);
} // namespace kt
