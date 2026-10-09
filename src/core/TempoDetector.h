#pragma once

#include "Fft.h"
#include "Genre.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace kt
{
struct TempoResult
{
    double bpm = 0.0;
    float confidence = 0.0f; // 0..1
    bool valid = false;
    Feel feel = Feel::Unknown;

    // Raw meter evidence for this window (see TempoDetector::analyse). The engine
    // accumulates these over time before deciding on a time signature.
    float dupleScore = 0.0f;    // bar repeats every 2 or 4 beats
    float tripleScore = 0.0f;   // bar repeats every 3 beats
    float compoundScore = 0.0f; // > 0: beats divide in three (6/8 feel); < 0: in two
};

/** Tempo and meter estimation.

    Tempo:
      1. Spectral-flux onset envelope (log-compressed STFT, ~6 ms hop).
      2. Autocorrelation of the last N seconds of that envelope.
      3. Each candidate BPM is scored by the autocorrelation at 1-4x its beat period
         (sub-frame interpolated, ~0.1 BPM precision) plus its half-beat subdivision,
         then weighted by a log-normal tempo prior centred inside the chosen range.

    Meter (given the winning beat period P):
      - Bar grouping compares periodicity at 3P against 2P/4P, mostly from a
        low-frequency (kick/bass) envelope, since downbeats are usually carried there.
      - Subdivision compares periodicity at P/3 against P/2 (triplet vs straight feel). */
class TempoDetector
{
public:
    void prepare (double sampleRate, double windowSeconds = 12.0);
    void reset();

    void push (const float* mono, int numSamples);

    /** Raw estimate over the current window. Cheap enough to call a few times a second. */
    TempoResult analyse (double minBpm, double maxBpm) const;

    double getEnvelopeRate() const noexcept { return envRate; }

private:
    void processFrame();

    double sampleRate = 44100.0, envRate = 0.0;
    int frameSize = 1024, hop = 256, maxBin = 0, lowBin = 0;
    std::unique_ptr<Fft> fft;

    std::vector<float> inputRing, frame, mags, prevLog, prevLow, prevAccent;
    int ringWrite = 0, samplesUntilFrame = 0;
    bool havePrev = false;

    std::vector<float> env, lowEnv, accentEnv; // onset strength rings: full band, below ~110 Hz, lightly compressed
    std::vector<uint8_t> active;    // whether each envelope frame had signal
    int envWrite = 0, envCount = 0;
};
} // namespace kt
