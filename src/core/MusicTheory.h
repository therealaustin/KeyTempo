#pragma once

#include <string>
#include <vector>

namespace kt
{
/** Keys are indexed 0..23: 0-11 are major keys (tonic pitch class, C = 0),
    12-23 are minor keys (tonic pitch class + 12). All strings are UTF-8. */
namespace theory
{
    constexpr int numKeys = 24;

    inline bool isMinor (int key) noexcept { return key >= 12; }
    inline int tonic (int key) noexcept { return key % 12; }
    inline int makeKey (int tonicPc, bool minor) noexcept { return ((tonicPc % 12 + 12) % 12) + (minor ? 12 : 0); }

    /** Relative major of a minor key, or relative minor of a major key. */
    int relativeKey (int key);

    /** Signed accidental count: +n sharps, -n flats. 6 is reported as sharps (F♯ / D♯m). */
    int signatureAccidentals (int key);

    struct KeySignature
    {
        int count = 0;                         // number of sharps or flats
        bool sharps = true;                    // false = flats
        std::string summary;                   // "3♯", "2♭", "No ♯/♭"
        std::string alternative;               // enharmonic equivalent ("6♭"), may be empty
        std::vector<std::string> accidentals;  // e.g. {"F♯", "C♯", "G♯"}
    };

    KeySignature keySignature (int key);

    /** Name spelled to match its key signature: "E♭ major", "F♯ minor". */
    std::string keyName (int key);

    /** Compact form: "E♭", "F♯m". */
    std::string shortKeyName (int key);

    /** Enharmonic alternative for keys with 6-7 accidentals ("G♭ major"), else empty. */
    std::string enharmonicKeyName (int key);

    /** DJ-friendly Camelot wheel code, e.g. "8A" for A minor. */
    std::string camelot (int key);

    struct TimeSignature
    {
        int numerator = 4, denominator = 4;
        std::string text() const { return std::to_string (numerator) + "/" + std::to_string (denominator); }
        bool operator== (const TimeSignature&) const = default;
    };

    /** Maps detected beats-per-bar (2, 3 or 4) and beat subdivision to a conventional
        time signature: simple 2/4 beats -> 4/4, 3 -> 3/4; compound 2/4 -> 6/8 (or 12/8), 3 -> 9/8.
        Beats are the felt pulse, so in compound time a beat is a dotted quarter. */
    TimeSignature timeSignatureFor (int beatsPerBar, bool compound);

    /** Short description, e.g. "Simple quadruple", "Compound duple". */
    std::string meterDescription (int beatsPerBar, bool compound);

    /** Pitch-class label for chroma displays: C, C♯, D, E♭ ... */
    std::string pitchClassName (int pc);
} // namespace theory
} // namespace kt
