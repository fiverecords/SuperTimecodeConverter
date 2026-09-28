// Super Timecode Converter
// Copyright (c) 2026 Fiverecords -- MIT License
// https://github.com/fiverecords/SuperTimecodeConverter
//
// OnAirFollow -- which deck an engine in ON AIR mode follows (DESIGN D34).
//
// The rule is Joaky's, for issue #23: "el actual hasta que calla" -- the
// current deck until it goes quiet.  Keep the deck being followed while it
// plays and is on air.  When it goes quiet or stops, move to the loudest
// deck that plays and is on air.  When there is none, stay where it is.
//
// The same rule serves Pro DJ Link and StageLinQ; each input says what "on
// air" and "loudest" mean for its hardware (getOnAirLevel()).
#pragma once

namespace OnAirFollow
{
    /// One deck as the rule sees it.  `level` < 0: not on air.  Otherwise
    /// larger is louder; levels are only compared between decks of the same
    /// input.
    struct Deck
    {
        bool   playing = false;
        double level   = -1.0;

        bool candidate() const { return playing && level >= 0.0; }
    };

    /// `decks[i]` is deck i + 1 of `count`.  Returns the deck to follow,
    /// 1-based: `current` while it qualifies, else the loudest deck that
    /// qualifies (the lowest number on a tie), else `current` -- or 0 when
    /// `current` is not a deck of this input either.
    inline int pick(int current, const Deck* decks, int count)
    {
        const bool currentValid = current >= 1 && current <= count;
        if (currentValid && decks[current - 1].candidate())
            return current;

        int best = 0;
        double bestLevel = -1.0;
        for (int i = 0; i < count; ++i)
        {
            if (decks[i].candidate() && decks[i].level > bestLevel)
            {
                best = i + 1;
                bestLevel = decks[i].level;
            }
        }
        if (best != 0)
            return best;
        return currentValid ? current : 0;
    }
}
