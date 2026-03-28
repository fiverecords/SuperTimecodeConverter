// btt_build.cpp -- BTT library build unit for JUCE/MSVC
// Copyright (c) 2021 Michael Krzyzaniak -- MIT License
//
// This is the ONLY file to add to your Projucer project for BTT.
// Same pattern as how JUCE internally includes sqlite3.

#ifdef _MSC_VER
  #pragma warning(push, 0)    // Suppress all warnings for third-party C code
#endif

// Undo any macros from JUCE/Windows PCH that clash with BTT identifiers
#ifdef real
  #undef real
#endif
#ifdef imag
  #undef imag
#endif
#ifdef small
  #undef small
#endif
#ifdef near
  #undef near
#endif
#ifdef far
  #undef far
#endif

#include "btt_amalgamation.inc"

#ifdef _MSC_VER
  #pragma warning(pop)
#endif
