#include "MusicTheory.h"

#include <cstdlib>

namespace kt::theory
{
namespace
{
    // Signed accidentals for the major key on each tonic pitch class.
    constexpr int majorSignature[12] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };

    const char* const sharpNames[12] = { "C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B" };
    const char* const flatNames[12]  = { "C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B" };

    const char* const sharpOrder[7] = { "F♯", "C♯", "G♯", "D♯", "A♯", "E♯", "B♯" };
    const char* const flatOrder[7]  = { "B♭", "E♭", "A♭", "D♭", "G♭", "C♭", "F♭" };

    int relativeMajorTonic (int key) { return isMinor (key) ? (tonic (key) + 3) % 12 : tonic (key); }

    std::string spelledTonic (int key, int signedCount)
    {
        return signedCount < 0 ? flatNames[tonic (key)] : sharpNames[tonic (key)];
    }
} // namespace

int relativeKey (int key)
{
    return isMinor (key) ? makeKey (tonic (key) + 3, false) : makeKey (tonic (key) + 9, true);
}

int signatureAccidentals (int key)
{
    return majorSignature[relativeMajorTonic (key)];
}

KeySignature keySignature (int key)
{
    KeySignature sig;
    const int s = signatureAccidentals (key);
    sig.count = std::abs (s);
    sig.sharps = s >= 0;

    if (s == 0)
        sig.summary = "No ♯/♭";
    else
        sig.summary = std::to_string (sig.count) + (sig.sharps ? "♯" : "♭");

    if (sig.count >= 5)
        sig.alternative = std::to_string (12 - sig.count) + (sig.sharps ? "♭" : "♯");

    for (int i = 0; i < sig.count; ++i)
        sig.accidentals.emplace_back (sig.sharps ? sharpOrder[i] : flatOrder[i]);

    return sig;
}

std::string keyName (int key)
{
    return spelledTonic (key, signatureAccidentals (key)) + (isMinor (key) ? " minor" : " major");
}

std::string shortKeyName (int key)
{
    return spelledTonic (key, signatureAccidentals (key)) + (isMinor (key) ? "m" : "");
}

std::string enharmonicKeyName (int key)
{
    const int s = signatureAccidentals (key);
    if (std::abs (s) < 6)
        return {};
    // Re-spell with the opposite accidental family.
    return std::string (s > 0 ? flatNames[tonic (key)] : sharpNames[tonic (key)]) + (isMinor (key) ? " minor" : " major");
}

std::string camelot (int key)
{
    const int number = ((relativeMajorTonic (key) * 7) % 12 + 7) % 12 + 1;
    return std::to_string (number) + (isMinor (key) ? "A" : "B");
}

TimeSignature timeSignatureFor (int beatsPerBar, bool compound)
{
    if (compound)
        return beatsPerBar == 3 ? TimeSignature { 9, 8 } : (beatsPerBar == 4 ? TimeSignature { 12, 8 } : TimeSignature { 6, 8 });
    return beatsPerBar == 3 ? TimeSignature { 3, 4 } : (beatsPerBar == 2 ? TimeSignature { 2, 4 } : TimeSignature { 4, 4 });
}

std::string meterDescription (int beatsPerBar, bool compound)
{
    const char* grouping = beatsPerBar == 3 ? "triple" : (beatsPerBar == 2 ? "duple" : "quadruple");
    return std::string (compound ? "Compound " : "Simple ") + grouping;
}

std::string pitchClassName (int pc)
{
    static const char* const names[12] = { "C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B" };
    return names[((pc % 12) + 12) % 12];
}
} // namespace kt::theory
