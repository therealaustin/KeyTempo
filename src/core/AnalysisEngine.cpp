#include "AnalysisEngine.h"

#include "MusicTheory.h"

#include <algorithm>
#include <cmath>

namespace kt
{
void AnalysisEngine::prepare (double sr)
{
    sampleRate = sr;
    tempoDetector.prepare (sr);
    keyDetector.prepare (sr);
    reset();
}

void AnalysisEngine::reset()
{
    tempoDetector.reset();
    keyDetector.reset();
    tempo = {};
    key = {};
    meter = {};
    dupleAcc = tripleAcc = compoundAcc = meterWeight = 0.0;
    pendingBpm = 0.0;
    pendingCount = 0;
    pendingKey = -1;
    pendingKeyCount = 0;
    signalSeconds = 0.0;
}

void AnalysisEngine::setTempoRange (double lo, double hi)
{
    if (lo != minBpm || hi != maxBpm)
    {
        minBpm = lo;
        maxBpm = hi;
        tempo = {}; // re-acquire inside the new range
        pendingCount = 0;
        meter = {};
        dupleAcc = tripleAcc = compoundAcc = meterWeight = 0.0;
    }
}

void AnalysisEngine::setKeyMemorySeconds (double seconds)
{
    keyDetector.setMemorySeconds (seconds);
}

void AnalysisEngine::push (const float* mono, int numSamples)
{
    double energy = 0.0;
    for (int i = 0; i < numSamples; ++i)
        energy += (double) mono[i] * mono[i];
    if (numSamples > 0 && std::sqrt (energy / numSamples) > 1.0e-4)
        signalSeconds += numSamples / sampleRate;

    tempoDetector.push (mono, numSamples);
    keyDetector.push (mono, numSamples);
}

void AnalysisEngine::update()
{
    // ---- Tempo: follow small drifts smoothly, but only jump after a change persists.
    const auto raw = tempoDetector.analyse (minBpm, maxBpm);
    if (raw.valid)
    {
        if (! tempo.valid)
        {
            tempo = raw;
        }
        else if (std::abs (raw.bpm - tempo.bpm) / tempo.bpm < 0.02)
        {
            tempo.bpm = 0.8 * tempo.bpm + 0.2 * raw.bpm;
            tempo.confidence = 0.7f * tempo.confidence + 0.3f * raw.confidence;
            pendingCount = 0;
        }
        else
        {
            if (pendingCount > 0 && std::abs (raw.bpm - pendingBpm) / pendingBpm < 0.02)
                ++pendingCount;
            else
            {
                pendingBpm = raw.bpm;
                pendingCount = 1;
            }

            tempo.confidence *= 0.85f;
            if (pendingCount >= 4 || raw.confidence > tempo.confidence + 0.3f)
            {
                tempo = raw;
                pendingCount = 0;
                // The beat changed, so the bar grouping must be re-learned.
                dupleAcc = tripleAcc = compoundAcc = meterWeight = 0.0;
                meter.valid = false;
            }
        }
    }
    else if (tempo.valid)
    {
        tempo.confidence *= 0.97f; // hold the last value through breaks, fading confidence
    }

    if (raw.valid && tempo.valid && std::abs (raw.bpm - tempo.bpm) / tempo.bpm < 0.02)
        updateMeter (raw);

    // ---- Key: the chroma is already time-averaged; just debounce flicker between close keys.
    auto k = keyDetector.analyse();
    if (! k.valid || signalSeconds < 3.0)
    {
        key.chroma = k.chroma;
        return;
    }

    if (! key.valid || k.key == key.key)
    {
        key = k;
        pendingKeyCount = 0;
    }
    else
    {
        if (k.key == pendingKey)
            ++pendingKeyCount;
        else
        {
            pendingKey = k.key;
            pendingKeyCount = 1;
        }

        const int previous = key.key;
        const float previousConfidence = key.confidence;
        key = k; // keep chroma and tuning fresh
        if (pendingKeyCount < 3 && k.confidence < previousConfidence + 0.25f)
        {
            key.key = previous;
            key.confidence = previousConfidence * 0.9f;
        }
        else
        {
            pendingKeyCount = 0;
        }
    }
}

void AnalysisEngine::updateMeter (const TempoResult& raw)
{
    // Slow exponential average (~10 updates) so the meter doesn't flicker bar to bar.
    const double a = 0.1;
    dupleAcc = (1.0 - a) * dupleAcc + a * raw.dupleScore;
    tripleAcc = (1.0 - a) * tripleAcc + a * raw.tripleScore;
    compoundAcc = (1.0 - a) * compoundAcc + a * raw.compoundScore;
    meterWeight = (1.0 - a) * meterWeight + a;

    if (meterWeight < 0.4) // roughly 5 consistent updates before committing
        return;

    const double duple = dupleAcc / meterWeight, triple = tripleAcc / meterWeight;
    const double subdivision = compoundAcc / meterWeight;

    // Most music is duple/quadruple, so triple has to win clearly.
    const bool isTriple = triple > duple + 0.08;
    const bool isCompound = subdivision > 0.08;

    meter.compound = isCompound;
    meter.beatsPerBar = isTriple ? 3 : (isCompound ? 2 : 4);
    const auto sig = theory::timeSignatureFor (meter.beatsPerBar, meter.compound);
    meter.numerator = sig.numerator;
    meter.denominator = sig.denominator;

    const double groupingMargin = std::abs (triple - (duple + 0.08));
    const double subdivisionMargin = std::abs (subdivision - 0.08);
    meter.confidence = (float) (std::clamp (groupingMargin / 0.25, 0.0, 1.0)
                                * std::clamp (0.5 + subdivisionMargin / 0.2, 0.0, 1.0)
                                * std::min (1.0, meterWeight / 0.6))
                       * tempo.confidence;
    meter.valid = true;
}
} // namespace kt
