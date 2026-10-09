#pragma once

#include <AnalysisEngine.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

/** Latest analysis results, written by the analysis thread and read by the UI.
    Each field is atomic; occasional cross-field tearing is harmless for display. */
struct Readout
{
    std::atomic<bool> tempoValid { false };
    std::atomic<float> bpm { 0.0f }, tempoConfidence { 0.0f };

    std::atomic<bool> keyValid { false };
    std::atomic<int> key { -1 }, runnerUp { -1 };
    std::atomic<float> keyConfidence { 0.0f }, tuningCents { 0.0f };
    std::array<std::atomic<float>, 12> chroma {};

    std::atomic<bool> meterValid { false };
    std::atomic<int> numerator { 4 }, denominator { 4 }, beatsPerBar { 4 };
    std::atomic<bool> compound { false };
    std::atomic<float> meterConfidence { 0.0f };

    std::atomic<float> inputLevel { 0.0f };    // peak, linear
    std::atomic<float> hostBpm { 0.0f };       // 0 if the host doesn't report one
    std::atomic<int> hostNumerator { 0 }, hostDenominator { 0 };
};

class KeyTempoProcessor final : public juce::AudioProcessor,
                                private juce::Thread
{
public:
    KeyTempoProcessor();
    ~KeyTempoProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Clears all accumulated evidence and starts listening afresh. */
    void requestReset() noexcept { resetRequested = true; }

    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    const Readout& getReadout() const noexcept { return readout; }

    static juce::StringArray tempoRangeNames();
    static juce::StringArray keyMemoryNames();
    static juce::StringArray tempoScaleNames();

private:
    template <typename Sample>
    void processSamples (juce::AudioBuffer<Sample>& buffer);

    void run() override; // analysis thread
    void publish();
    void readHostInfo();
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState state;
    std::atomic<float>* rangeParam = nullptr;
    std::atomic<float>* memoryParam = nullptr;
    std::atomic<float>* holdParam = nullptr;

    // Audio thread -> analysis thread, single producer / single consumer.
    juce::AbstractFifo fifo { 1 << 18 };
    std::vector<float> fifoBuffer;
    std::vector<float> monoScratch;

    juce::CriticalSection engineLock;
    kt::AnalysisEngine engine;
    double preparedRate = 0.0;
    int samplesSinceUpdate = 0; // analysis thread only
    std::atomic<bool> resetRequested { false }, engineReady { false };
    std::atomic<float> blockPeak { 0.0f };

    Readout readout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyTempoProcessor)
};
