/*
 *  VFR timecodes for the AviSynth entry points (TextSubMod/MaskSubMod's vfr=
 *  argument). Ported from upstream's vfr.cpp; the parsing is unchanged.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#pragma once

#include <memory>
#include <string>

class VFRTranslator
{
public:
    virtual ~VFRTranslator() {}
    /* Seconds. */
    virtual double TimeStampFromFrameNumber(int n) = 0;
};

/* Reads a Matroska v1 or v2 timecodes file. Returns nullptr if the file cannot
 * be opened or is neither format -- upstream dereferenced a null FILE* on a
 * missing file. `path` is a native path (see OpenScriptPath). */
std::unique_ptr<VFRTranslator> GetVFRTranslator(const std::wstring& path);
