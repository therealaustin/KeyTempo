#pragma once

#include "KeyDetector.h"
#include "TempoDetector.h"

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
    void setKeyMemorySeconds (double seconds);

    void push (const float* mono, int numSamples);

    /** Re-runs both estimators and updates the smoothed results. */
    void update();

    const TempoResult& getTempo() const noexcept { return tempo; }
    const KeyResult& getKey() const noexcept { return key; }
    const MeterResult& getMeter() const noexcept { return meter; }

private:
    TempoDetector tempoDetector;
    KeyDetector keyDetector;

    double sampleRate = 44100.0;
    double minBpm = 70.0, maxBpm = 180.0;
    double signalSeconds = 0.0;

    TempoResult tempo;
    double pendingBpm = 0.0;
    int pendingCount = 0;

    MeterResult meter;
    double dupleAcc = 0.0, tripleAcc = 0.0, compoundAcc = 0.0, meterWeight = 0.0;
    void updateMeter (const TempoResult& raw);

    KeyResult key;
    int pendingKey = -1, pendingKeyCount = 0;
};
} // namespace kt
