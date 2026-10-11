#pragma once

#include "Genre.h"

#include <cstddef>

namespace kt
{
/** Bump whenever analysis results can change, so libraries know what to re-scan. */
inline constexpr int analysisVersion = 3;

/** Whole-track analysis result, for libraries and batch tools. */
struct TrackAnalysis
{
    bool valid = false;
    double durationSeconds = 0.0;

    // Tempo
    double bpm = 0.0;
    float bpmConfidence = 0.0f;  // 0..1
    double alternateBpm = 0.0;   // strongest rival reading (often 2x or 1/2), 0 if none
    Feel feel = Feel::Unknown;

    // Beat grid (constant tempo): beat k is at firstBeatSeconds + k * 60 / bpm.
    // firstBeatSeconds is the first *downbeat* (beat 1 of a bar) at or after 0.
    double firstBeatSeconds = 0.0;
    float downbeatConfidence = 0.0f; // 0..1: how clearly beat 1 stands out

    // Key (0..11 major, 12..23 minor; see MusicTheory.h)
    int key = -1;
    float keyConfidence = 0.0f;
    float tuningCents = 0.0f;

    // Time signature
    bool meterValid = false;
    int numerator = 4, denominator = 4;
    float meterConfidence = 0.0f;

    int engineVersion = analysisVersion;
};

struct TrackAnalysisOptions
{
    Genre genre = Genre::Auto;
    /** How often the estimators re-run while streaming through the file. The plug-in
        uses 0.4 s for responsiveness; 1.2 s gives the same answers ~3x cheaper. */
    double updateIntervalSeconds = 1.2;
};

/** Analyses a whole mono track. Thread-safe (no shared state); call from any thread. */
TrackAnalysis analyseTrack (const float* mono, size_t numSamples, double sampleRate, const TrackAnalysisOptions& options = {});
} // namespace kt
