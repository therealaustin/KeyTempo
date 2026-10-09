// Developer tool for KeyTempo.
//
//   kt_tool analyze [--genre NAME] <audio file> [more files...]
//       Runs the analysis engine over whole files (WAV/AIFF/FLAC/MP3/Ogg) and prints
//       tempo, key and time signature — handy for checking accuracy on real music.
//
//   kt_tool snapshot <out.png> [audio file]
//       Feeds audio through the actual plug-in processor, opens its editor and saves
//       a screenshot. Without a file, a synthetic groove + chord progression is used.

#include "../src/plugin/PluginEditor.h"
#include "../src/plugin/PluginProcessor.h"

#include <AnalysisEngine.h>
#include <MusicTheory.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../tests/bench/GrooveLab.h"

namespace
{
juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

std::unique_ptr<juce::AudioFormatReader> openReader (const juce::File& file)
{
    static juce::AudioFormatManager manager;
    if (manager.getNumKnownFormats() == 0)
        manager.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader> (manager.createReaderFor (file));
}

/** Mono audio at its native sample rate. */
bool loadMono (const juce::File& file, std::vector<float>& mono, double& sampleRate)
{
    auto reader = openReader (file);
    if (reader == nullptr)
        return false;
    sampleRate = reader->sampleRate;
    const auto length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
    reader->read (&buffer, 0, length, 0, true, true);
    mono.assign ((size_t) length, 0.0f);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), buffer.getReadPointer (ch), 1.0f / buffer.getNumChannels(), length);
    return true;
}

/** A demo clip from the benchmark generator: hard-mode DnB two-step at 174 BPM. */
std::vector<float> syntheticDemo (double sr, double seconds)
{
    auto clip = groove::make (groove::Style::DnbTwoStep, 174.0, 3, sr, true);
    clip.audio.resize (std::min (clip.audio.size(), (size_t) (seconds * sr)));
    return clip.audio;
}

kt::Genre parseGenre (const juce::String& name)
{
    for (int g = 0; g < (int) kt::Genre::numGenres; ++g)
        if (juce::String (kt::genreName ((kt::Genre) g)).containsIgnoreCase (name))
            return (kt::Genre) g;
    return kt::Genre::Auto;
}

int analyse (const juce::StringArray& files, kt::Genre genre)
{
    for (const auto& path : files)
    {
        std::vector<float> mono;
        double sr = 0;
        if (! loadMono (juce::File::getCurrentWorkingDirectory().getChildFile (path), mono, sr))
        {
            std::printf ("%s: could not read\n", path.toRawUTF8());
            continue;
        }

        kt::AnalysisEngine engine;
        engine.prepare (sr);
        engine.setGenre (genre);
        const auto updateEvery = (size_t) (0.4 * sr);
        size_t since = 0;
        for (size_t i = 0; i < mono.size(); i += 512)
        {
            const int n = (int) std::min<size_t> (512, mono.size() - i);
            engine.push (mono.data() + i, n);
            if ((since += (size_t) n) >= updateEvery)
            {
                engine.update();
                since = 0;
            }
        }
        engine.update();

        const auto& t = engine.getTempo();
        const auto& k = engine.getKey();
        const auto& m = engine.getMeter();
        juce::String tempoText ("-");
        if (t.valid)
        {
            tempoText = juce::String::formatted ("%.2f BPM  (conf %.2f, feel: %s", t.bpm, t.confidence, kt::feelName (t.feel));
            if (t.alternateBpm > 0)
                tempoText << juce::String::formatted (", also fits %.2f", t.alternateBpm);
            tempoText << ")";
        }
        std::printf ("%s  [%s]\n  tempo  %s\n  key    %s\n  meter  %s\n", path.toRawUTF8(), kt::genreName (genre), tempoText.toRawUTF8(),
                     k.valid ? (utf8 (kt::theory::keyName (k.key)) + " / rel. " + utf8 (kt::theory::keyName (kt::theory::relativeKey (k.key)))
                                + " / " + utf8 (kt::theory::camelot (k.key))).toRawUTF8() : "-",
                     m.valid ? juce::String::formatted ("%d/%d", m.numerator, m.denominator).toRawUTF8() : "-");
    }
    return 0;
}

int snapshot (const juce::String& outPath, const juce::String& audioPath)
{
    juce::ScopedJuceInitialiser_GUI gui;

    std::vector<float> mono;
    double sr = 44100.0;
    if (audioPath.isNotEmpty())
    {
        if (! loadMono (juce::File::getCurrentWorkingDirectory().getChildFile (audioPath), mono, sr))
            return 1;
        mono.resize (std::min (mono.size(), (size_t) (sr * 40)));
    }
    else
    {
        mono = syntheticDemo (sr, 45.0);
    }

    KeyTempoProcessor processor;
    processor.setRateAndBufferSizeDetails (sr, 512);
    processor.prepareToPlay (sr, 512);

    // Feed at roughly 10x real time so the analysis thread's FIFO never overflows.
    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;
    for (size_t i = 0; i + 512 <= mono.size(); i += 512)
    {
        block.copyFrom (0, 0, mono.data() + i, 512);
        block.copyFrom (1, 0, mono.data() + i, 512);
        processor.processBlock (block, midi);
        juce::Thread::sleep (1);
    }
    juce::Thread::sleep (1500);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setVisible (true);
    // Run the message loop briefly so the editor's 30 Hz timer settles its smoothed values.
    juce::Timer::callAfterDelay (1200, [] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    juce::MessageManager::getInstance()->runDispatchLoop();

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (outPath));
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat png;
    const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
    editor.reset();
    processor.releaseResources();
    std::printf ("%s %s\n", ok ? "wrote" : "failed to write", out.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.size() >= 2 && args[0] == "analyze")
    {
        auto genre = kt::Genre::Auto;
        juce::StringArray files;
        for (int i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--genre" && i + 1 < args.size())
                genre = parseGenre (args[++i]);
            else
                files.add (args[i]);
        }
        return analyse (files, genre);
    }
    if (args.size() >= 2 && args[0] == "demo")
    {
        // Writes the synthetic demo used by `snapshot` to a WAV file.
        const auto audio = syntheticDemo (44100.0, 30.0);
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        out.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (out));
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24));
        if (writer == nullptr)
            return 1;
        const float* data[] = { audio.data() };
        writer->writeFromFloatArrays (data, 1, (int) audio.size());
        return 0;
    }
    if (args.size() >= 2 && args[0] == "snapshot")
        return snapshot (args[1], args.size() > 2 ? args[2] : juce::String());

    std::printf ("usage:\n  kt_tool analyze [--genre auto|house|trance|drum|dubstep|hip|pop] <audio files...>\n  kt_tool demo <out.wav>\n  kt_tool snapshot <out.png> [audio file]\n");
    return 1;
}
