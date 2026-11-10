#pragma once

#include "Genre.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace kt
{
/** Bump whenever analysis results can change, so libraries know what to re-scan. */
inline constexpr int analysisVersion = 5;

/** A section of constant tempo, starting on a beat. The segments together form the
    dynamic beat grid: each start is a grid marker (beat position + BPM), the shape
    Serato, rekordbox and Engine DJ use. Beat k of a segment is at startSeconds + k * 60 / bpm,
    up to the next segment's start. */
struct TempoSegment
{
    double startSeconds = 0.0; // a beat; the first segment starts on the first downbeat
    double bpm = 0.0;
    float confidence = 0.0f;   // 0..1
    int beatInBar = 1;         // which beat of the bar startSeconds is (1 = downbeat)
    bool ramp = false;         // part of a gradual tempo change (markers every few bars)
};

/** A section in one key. */
struct KeySegment
{
    double startSeconds = 0.0; // 0 for the first segment
    int key = -1;              // 0..11 major, 12..23 minor
    float confidence = 0.0f;
};

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
    // When the tempo changes, bpm and this grid are the dominant (longest-held) section's,
    // and tempoSegments has the full grid.
    double firstBeatSeconds = 0.0;
    float downbeatConfidence = 0.0f; // 0..1: how clearly beat 1 stands out

    // Tempo changes. A steady track (nearly all) has one segment equal to bpm /
    // firstBeatSeconds, and bpmMin == bpmMax == bpm. Empty when the tempo is not valid.
    std::vector<TempoSegment> tempoSegments;
    double bpmMin = 0.0, bpmMax = 0.0;
    bool variableTempo = false;

    // Key (0..11 major, 12..23 minor; see MusicTheory.h). When the key changes, the
    // dominant (longest-held) one; keySegments has them all (one for a single-key track,
    // empty when the key is not valid).
    int key = -1;
    float keyConfidence = 0.0f;
    float tuningCents = 0.0f;
    std::vector<KeySegment> keySegments;

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
    /** Look for tempo and key changes within the track. Off = one segment each. */
    bool detectChanges = true;
    /** Shortest section, in bars, that counts as a tempo or key change. Shorter
        differences (fills, breakdowns, a half-time bridge) never split a track. */
    int minSegmentBars = 16;
    /** Grid marker spacing, in bars, while the tempo glides between two sections. */
    int rampMarkerBars = 4;
};

/** Streaming whole-track analysis: push the track's mono samples in any block size,
    then call finish() once. Memory is ~1 MB per minute of audio (onset envelopes only,
    no samples kept), so a decoder can stream straight into it. One instance per track;
    instances are independent, so many can run on different threads. */
class TrackAnalyzer
{
public:
    explicit TrackAnalyzer (double sampleRate, const TrackAnalysisOptions& options = {});
    ~TrackAnalyzer();

    void push (const float* mono, int numSamples);
    TrackAnalysis finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Convenience: analyses a whole mono track held in memory. */
TrackAnalysis analyseTrack (const float* mono, size_t numSamples, double sampleRate, const TrackAnalysisOptions& options = {});
} // namespace kt
