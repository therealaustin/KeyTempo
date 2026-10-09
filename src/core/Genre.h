#pragma once

namespace kt
{
/** Genre setting: constrains the tempo range and which groove templates may explain
    the rhythm, following DJ / Beatport tempo conventions (e.g. DnB is 174, not 87). */
enum class Genre
{
    Auto = 0,
    HouseTechno,
    Trance,
    DrumAndBass,
    DubstepTrap,
    HipHop,
    PopRock,
    numGenres
};

/** The rhythmic "feel" that best explained the beat. */
enum class Feel
{
    Unknown = 0,
    FourOnTheFloor, // kick on every beat: house, techno, trance
    Backbeat,       // snare/clap on 2 & 4: pop, hip-hop, garage
    Breakbeat,      // backbeat with a syncopated kick: DnB two-step, jungle, breaks
    Halftime,       // snare on 3: dubstep, trap, halftime DnB
};

inline const char* genreName (Genre g)
{
    switch (g)
    {
        case Genre::Auto:        return "Auto";
        case Genre::HouseTechno: return "House / Techno";
        case Genre::Trance:      return "Trance";
        case Genre::DrumAndBass: return "Drum & Bass / Jungle";
        case Genre::DubstepTrap: return "Dubstep / Trap";
        case Genre::HipHop:      return "Hip-Hop / R&B";
        case Genre::PopRock:     return "Pop / Rock / Other";
        case Genre::numGenres:   break;
    }
    return "?";
}

inline const char* feelName (Feel f)
{
    switch (f)
    {
        case Feel::FourOnTheFloor: return "Four-on-the-floor";
        case Feel::Backbeat:       return "Backbeat";
        case Feel::Breakbeat:      return "Breakbeat";
        case Feel::Halftime:       return "Half-time";
        case Feel::Unknown:        break;
    }
    return "-";
}

struct TempoRange
{
    double lo, hi;
};

/** Hard tempo limits per genre. */
inline TempoRange tempoRangeFor (Genre g)
{
    switch (g)
    {
        case Genre::HouseTechno: return { 105.0, 152.0 };
        case Genre::Trance:      return { 120.0, 155.0 };
        case Genre::DrumAndBass: return { 150.0, 190.0 };
        case Genre::DubstepTrap: return { 125.0, 160.0 };
        case Genre::HipHop:      return { 60.0, 115.0 };
        case Genre::PopRock:     return { 60.0, 200.0 };
        case Genre::Auto:
        case Genre::numGenres:   break;
    }
    return { 60.0, 200.0 };
}
} // namespace kt
