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
    lastRaw = {};
    hypotheses.clear();
    key = {};
    meter = {};
    dupleAcc = tripleAcc = compoundAcc = meterWeight = 0.0;
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
        lastRaw = {};
        hypotheses.clear();
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
    // ---- Tempo: accumulate evidence for each distinct tempo hypothesis over the track.
    const auto raw = tempoDetector.analyse (minBpm, maxBpm, genre);
    const double previousBpm = tempo.valid ? tempo.bpm : 0.0;
    updateTempoMemory (raw);
    if (tempo.valid && previousBpm > 0.0 && std::abs (tempo.bpm - previousBpm) / previousBpm > 0.03)
    {
        // The beat changed, so the bar grouping must be re-learned.
        dupleAcc = tripleAcc = compoundAcc = meterWeight = 0.0;
        meter.valid = false;
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

void AnalysisEngine::updateTempoMemory (const TempoResult& raw)
{
    // Evidence decays slowly (the "memory"), so a long track builds a stable answer and
    // breakdowns, intros and fills can't overturn it, yet a real tempo change still wins.
    const double decay = std::exp (-updateIntervalSeconds / tempoMemorySeconds);
    for (auto& h : hypotheses)
        h.weight *= decay;

    if (raw.valid)
    {
        // Readings backed by a recognisable groove (drums playing) count far more than
        // readings from pads, vocals or a breakdown.
        // Windows without a kick (breakdowns, build-up snare rolls) count very little.
        const double w = (0.1 + raw.confidence) * (0.08 + raw.grooveFit) * (0.1 + 0.9 * raw.drumPresence);

        Hypothesis* match = nullptr;
        for (auto& h : hypotheses)
            if (std::abs (h.bpm - raw.bpm) / h.bpm < 0.015 && (match == nullptr || h.weight > match->weight))
                match = &h;
        if (match == nullptr)
        {
            hypotheses.push_back ({ raw.bpm, 0.0, 0.0, Feel::Unknown, 0.0f });
            match = &hypotheses.back();
        }

        // Precise tempo: confidence-weighted average of readings that agree (~20 s of
        // readings), which averages away per-window jitter.
        const double avgDecay = std::exp (-updateIntervalSeconds / 20.0);
        match->bpmWeight = match->bpmWeight * avgDecay + w;
        match->bpm += (raw.bpm - match->bpm) * (w / match->bpmWeight);
        match->weight += w;
        if (raw.feel != Feel::Unknown)
        {
            match->feel = raw.feel;
            match->grooveFit = raw.grooveFit;
        }
        lastRaw = raw;
    }

    // Forget hypotheses that no longer matter.
    hypotheses.erase (std::remove_if (hypotheses.begin(), hypotheses.end(), [] (const Hypothesis& h) { return h.weight < 1.0e-4; }),
                      hypotheses.end());
    if (hypotheses.empty())
    {
        tempo.valid = false;
        return;
    }

    double total = 0.0;
    const Hypothesis* best = nullptr;
    const Hypothesis* current = nullptr;
    for (auto& h : hypotheses)
    {
        total += h.weight;
        if (best == nullptr || h.weight > best->weight)
            best = &h;
        if (tempo.valid && std::abs (h.bpm - tempo.bpm) / tempo.bpm < 0.015 && (current == nullptr || h.weight > current->weight))
            current = &h;
    }

    // Hysteresis: a challenger must clearly out-weigh what is on screen.
    const Hypothesis* shown = (current != nullptr && best->weight < 1.3 * current->weight) ? current : best;

    // Strongest other reading that isn't just the same tempo.
    const Hypothesis* alternate = nullptr;
    for (auto& h : hypotheses)
        if (&h != shown && std::abs (h.bpm - shown->bpm) / shown->bpm > 0.03 && (alternate == nullptr || h.weight > alternate->weight))
            alternate = &h;

    tempo.valid = true;
    tempo.bpm = shown->bpm;
    tempo.feel = shown->feel;
    tempo.grooveFit = shown->grooveFit;
    tempo.alternateBpm = alternate != nullptr ? alternate->bpm : 0.0;

    // Confidence: share of all evidence behind this tempo, scaled by how much evidence
    // there is yet (a couple of seconds of drums shouldn't read as 100%).
    const double share = shown->weight / total;
    const double amount = 1.0 - std::exp (-shown->weight / 1.5);
    const float target = (float) std::clamp (share * amount * 1.15, 0.0, 1.0);
    tempo.confidence += (target - tempo.confidence) * 0.3f;
}
} // namespace kt
