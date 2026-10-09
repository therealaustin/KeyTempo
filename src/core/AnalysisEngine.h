#pragma once

#include "KeyDetector.h"
#include "TempoDetector.h"

#include <vector>

namespace kt
{
struct MeterResult
{
    int numerator = 4, denominator = 4;
    int beatsPerBar = 4;     // felt beats per bar: 2 (compound duple), 3 or 4
    bool compound = false;   // beats divide into three (6/8, 9/8)
    float confidence = 0.0f; // 0..1
    bool valid = false;
};

/** Owns both detectors and smooths their output into stable, display-ready values.
    Not thread-safe: feed and query it from one (non-realtime) thread. */
class AnalysisEngine
{
public:
    void prepare (double sampleRate);
    void reset();

    void setTempoRange (double minBpm, double maxBpm);
    void setGenre (Genre g) { genre = g; const auto r = tempoRangeFor (g); setTempoRange (r.lo, r.hi); }
    void setKeyMemorySeconds (double seconds);

    void push (const float* mono, int numSamples);

    /** Re-runs both estimators and updates the smoothed results. */
    void update();

    const TempoResult& getTempo() const noexcept { return tempo; }
    /** The latest single-window reading (what the detector hears right now). */
    const TempoResult& getRawTempo() const noexcept { return lastRaw; }
    const KeyResult& getKey() const noexcept { return key; }
    const MeterResult& getMeter() const noexcept { return meter; }

private:
    TempoDetector tempoDetector;
    KeyDetector keyDetector;

    double sampleRate = 44100.0;
    double minBpm = 60.0, maxBpm = 200.0;
    Genre genre = Genre::Auto;
    double signalSeconds = 0.0;

    TempoResult tempo, lastRaw;

    /** One tempo reading the track might have, with its accumulated evidence. */
    struct Hypothesis
    {
        double bpm, weight, bpmWeight;
        Feel feel;
        float grooveFit;
    };
    std::vector<Hypothesis> hypotheses;
    double tempoMemorySeconds = 90.0;
    static constexpr double updateIntervalSeconds = 0.4; // how often the host calls update()
    void updateTempoMemory (const TempoResult& raw);

    MeterResult meter;
    double dupleAcc = 0.0, tripleAcc = 0.0, compoundAcc = 0.0, meterWeight = 0.0;
    void updateMeter (const TempoResult& raw);

    KeyResult key;
    int pendingKey = -1, pendingKeyCount = 0;
};
} // namespace kt
