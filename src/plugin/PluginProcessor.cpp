#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr const char* rangeId = "tempoRange";
    constexpr const char* memoryId = "keyMemory";
    constexpr const char* holdId = "hold";
    constexpr const char* scaleId = "tempoScale";

    struct Range { double lo, hi; };
    constexpr Range tempoRanges[] = { { 70.0, 180.0 }, { 50.0, 100.0 }, { 80.0, 160.0 }, { 100.0, 200.0 } };
    constexpr double keyMemorySeconds[] = { 15.0, 45.0, 120.0, 0.0 };

    constexpr double analysisIntervalSeconds = 0.4;
} // namespace

juce::StringArray KeyTempoProcessor::tempoRangeNames()  { return { "70-180 BPM", "50-100 BPM", "80-160 BPM", "100-200 BPM" }; }
juce::StringArray KeyTempoProcessor::keyMemoryNames()   { return { "15 s", "45 s", "2 min", "Whole track" }; }
// Parameter value names stay ASCII: some hosts and wrappers don't round-trip other text.
juce::StringArray KeyTempoProcessor::tempoScaleNames()  { return { "Half", "Normal", "Double" }; }

juce::AudioProcessorValueTreeState::ParameterLayout KeyTempoProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { rangeId, 1 }, "Tempo Range", tempoRangeNames(), 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { memoryId, 1 }, "Key Memory", keyMemoryNames(), 1));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { scaleId, 1 }, "Tempo Display", tempoScaleNames(), 1));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { holdId, 1 }, "Hold", false));
    return layout;
}

KeyTempoProcessor::KeyTempoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("KeyTempo analysis"),
      state (*this, nullptr, "KeyTempo", createLayout())
{
    rangeParam = state.getRawParameterValue (rangeId);
    memoryParam = state.getRawParameterValue (memoryId);
    holdParam = state.getRawParameterValue (holdId);

    fifoBuffer.resize ((size_t) fifo.getTotalSize());
    startThread (juce::Thread::Priority::low);
}

KeyTempoProcessor::~KeyTempoProcessor()
{
    stopThread (2000);
}

bool KeyTempoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void KeyTempoProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    monoScratch.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);

    const juce::ScopedLock sl (engineLock);
    // Only re-initialise when the rate changes, so a transport restart or buffer-size
    // change in the host doesn't throw away what has been learned.
    if (! engineReady || ! juce::approximatelyEqual (preparedRate, sampleRate))
    {
        fifo.reset(); // safe: the host never calls processBlock during prepareToPlay
        engine.prepare (sampleRate);
        preparedRate = sampleRate;
        engineReady = true;
    }
}

void KeyTempoProcessor::releaseResources() {}

void KeyTempoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)  { processSamples (buffer); }
void KeyTempoProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) { processSamples (buffer); }

template <typename Sample>
void KeyTempoProcessor::processSamples (juce::AudioBuffer<Sample>& buffer)
{
    juce::ScopedNoDenormals noDenormals;

    // Audio passes through untouched; extra output channels (if any) are cleared.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    readHostInfo();

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numSamples == 0 || numChannels == 0)
        return;

    if ((int) monoScratch.size() < numSamples) // host exceeded its announced block size
        return;

    // Downmix to mono.
    const float gain = 1.0f / (float) numChannels;
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += (float) buffer.getReadPointer (ch)[i];
        const float v = sum * gain;
        monoScratch[(size_t) i] = v;
        peak = juce::jmax (peak, std::abs (v));
    }
    blockPeak = juce::jmax (blockPeak.load (std::memory_order_relaxed), peak);

    if (holdParam->load() > 0.5f)
        return;

    // Push into the FIFO; if the analysis thread falls behind, drop rather than block.
    const auto scope = fifo.write (numSamples);
    if (scope.blockSize1 > 0)
        std::copy_n (monoScratch.data(), scope.blockSize1, fifoBuffer.data() + scope.startIndex1);
    if (scope.blockSize2 > 0)
        std::copy_n (monoScratch.data() + scope.blockSize1, scope.blockSize2, fifoBuffer.data() + scope.startIndex2);
}

void KeyTempoProcessor::readHostInfo()
{
    if (auto* head = getPlayHead())
    {
        if (const auto pos = head->getPosition())
        {
            readout.hostBpm = pos->getBpm() ? (float) *pos->getBpm() : 0.0f;
            if (const auto sig = pos->getTimeSignature())
            {
                readout.hostNumerator = sig->numerator;
                readout.hostDenominator = sig->denominator;
            }
        }
    }
}

void KeyTempoProcessor::run()
{
    int lastRange = -1;
    double lastMemory = -2.0;

    while (! threadShouldExit())
    {
        wait (25);

        {
            const juce::ScopedLock sl (engineLock);
            if (! engineReady)
                continue;

            if (resetRequested.exchange (false))
            {
                fifo.read (fifo.getNumReady()); // discard queued audio (consumer side, so thread-safe)
                engine.reset();
                samplesSinceUpdate = 0;
                publish();
            }

            const int range = juce::jlimit (0, 3, (int) rangeParam->load());
            if (range != lastRange)
            {
                engine.setTempoRange (tempoRanges[range].lo, tempoRanges[range].hi);
                lastRange = range;
            }
            const double memory = keyMemorySeconds[juce::jlimit (0, 3, (int) memoryParam->load())];
            if (! juce::exactlyEqual (memory, lastMemory))
            {
                engine.setKeyMemorySeconds (memory);
                lastMemory = memory;
            }

            // Drain everything the audio thread has written, re-estimating every
            // `analysisInterval` seconds of *audio* (not wall-clock), so results are the
            // same whether the host plays in real time or renders faster.
            const bool holding = holdParam->load() > 0.5f;
            const auto interval = (int) (preparedRate * analysisIntervalSeconds);
            const auto scope = fifo.read (fifo.getNumReady());
            for (auto [start, size] : { std::pair { scope.startIndex1, scope.blockSize1 }, std::pair { scope.startIndex2, scope.blockSize2 } })
            {
                while (size > 0)
                {
                    const int chunk = juce::jmin (size, interval - samplesSinceUpdate);
                    engine.push (fifoBuffer.data() + start, chunk);
                    start += chunk;
                    size -= chunk;
                    samplesSinceUpdate += chunk;
                    if (samplesSinceUpdate >= interval)
                    {
                        if (! holding)
                        {
                            engine.update();
                            publish();
                        }
                        samplesSinceUpdate = 0;
                    }
                }
            }
        }

        // Input meter with a gentle release.
        const float peak = blockPeak.exchange (0.0f);
        readout.inputLevel = juce::jmax (peak, readout.inputLevel.load() * 0.85f);
    }
}

void KeyTempoProcessor::publish()
{
    const auto& t = engine.getTempo();
    readout.tempoValid = t.valid;
    readout.bpm = (float) t.bpm;
    readout.tempoConfidence = t.confidence;

    const auto& k = engine.getKey();
    readout.keyValid = k.valid;
    readout.key = k.key;
    readout.runnerUp = k.runnerUp;
    readout.keyConfidence = k.confidence;
    readout.tuningCents = k.tuningCents;
    for (size_t i = 0; i < 12; ++i)
        readout.chroma[i] = k.chroma[i];

    const auto& m = engine.getMeter();
    readout.meterValid = m.valid;
    readout.numerator = m.numerator;
    readout.denominator = m.denominator;
    readout.beatsPerBar = m.beatsPerBar;
    readout.compound = m.compound;
    readout.meterConfidence = m.confidence;
}

juce::AudioProcessorEditor* KeyTempoProcessor::createEditor()
{
    return new KeyTempoEditor (*this);
}

void KeyTempoProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void KeyTempoProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KeyTempoProcessor();
}
