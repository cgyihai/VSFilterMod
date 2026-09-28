/*
 *  VFR timecodes for the AviSynth entry points.
 *
 *  Ported from upstream VSFilterMod's vfr.cpp (Niels Martin Hansen). The two
 *  parsers are unchanged; what changed is everything around them:
 *    - the file is opened portably, and a missing file is an error rather than
 *      a crash (upstream called fgets on a null FILE*);
 *    - a v2 file with fewer than two timestamps no longer indexes before the
 *      start of the vector when extrapolating past its end.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#include "vfr.h"
#include "../compat/vsfm_compat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Work in seconds per frame (spf) rather than fps: it is the natural unit for
// the translation being done.
class TimecodesV1 : public VFRTranslator
{
    double default_spf;
    double first_non_section_timestamp;
    int first_non_section_frame;

    struct FrameRateSection
    {
        double start_time;
        double spf;
        int start_frame;
        int end_frame;
    };
    std::vector<FrameRateSection> sections;

public:
    double TimeStampFromFrameNumber(int n) override
    {
        for (size_t i = 0; i < sections.size(); i++) {
            const FrameRateSection& sect = sections[i];
            if (n >= sect.start_frame && n <= sect.end_frame)
                return sect.start_time + (n - sect.start_frame) * sect.spf;
        }
        if (n < 0) return 0.0;
        return first_non_section_timestamp + (n - first_non_section_frame) * default_spf;
    }

    explicit TimecodesV1(FILE* vfrfile)
    {
        char buf[100];

        default_spf = -1;
        double cur_time = 0.0;

        FrameRateSection temp_section;
        temp_section.start_time = 0.0;
        temp_section.spf = -1;
        temp_section.start_frame = 0;
        temp_section.end_frame = 0;

        while (std::fgets(buf, sizeof(buf), vfrfile)) {
            if (buf[0] == '#') continue;

            if (_strnicmp(buf, "Assume ", 7) == 0 && default_spf < 0) {
                default_spf = std::atof(buf + 7);
                default_spf = default_spf > 0 ? 1 / default_spf : -1;
                temp_section.spf = default_spf;
                continue;
            }

            int start_frame, end_frame;
            float fps;
            if (std::sscanf(buf, "%d,%d,%f", &start_frame, &end_frame, &fps) == 3) {
                temp_section.end_frame = start_frame - 1;
                if (temp_section.end_frame >= temp_section.start_frame) {
                    cur_time += (temp_section.end_frame - temp_section.start_frame + 1) * temp_section.spf;
                    sections.push_back(temp_section);
                }
                temp_section.spf = 1 / fps;
                temp_section.start_frame = start_frame;
                temp_section.end_frame = end_frame;
                temp_section.start_time = cur_time;
                cur_time += (end_frame - start_frame + 1) / fps;
                sections.push_back(temp_section);

                temp_section.spf = default_spf;
                temp_section.start_frame = end_frame + 1;
                temp_section.end_frame = end_frame;    // yes, negative duration
                temp_section.start_time = cur_time;
            }
        }

        first_non_section_timestamp = cur_time;
        first_non_section_frame = temp_section.start_frame;
    }
};

class TimecodesV2 : public VFRTranslator
{
    std::vector<double> timestamps;
    int last_known_frame = -1;
    double last_known_timestamp = 0.0;
    double assumed_spf = 0.0;

public:
    double TimeStampFromFrameNumber(int n) override
    {
        if (n >= 0 && n < (int)timestamps.size()) return timestamps[n];
        if (n < 0) return 0.0;
        return last_known_timestamp + (n - last_known_frame) * assumed_spf;
    }

    explicit TimecodesV2(FILE* vfrfile)
    {
        char buf[50];
        timestamps.reserve(8192);

        while (std::fgets(buf, sizeof(buf), vfrfile)) {
            if (buf[0] == '#') continue;
            timestamps.push_back(std::atof(buf) / 1000);
        }

        if (!timestamps.empty()) {
            last_known_frame = (int)timestamps.size() - 1;
            last_known_timestamp = timestamps[last_known_frame];
        }
        if (timestamps.size() >= 2)
            assumed_spf = last_known_timestamp - timestamps[last_known_frame - 1];
    }
};

} // namespace

std::unique_ptr<VFRTranslator> GetVFRTranslator(const std::wstring& path)
{
#if defined(_WIN32)
    FILE* f = _wfopen(path.c_str(), L"r");
#else
    const std::string narrow = vsfm::W2U8(path.c_str(), path.size());
    FILE* f = std::fopen(narrow.c_str(), "r");
#endif
    if (!f) return nullptr;

    // "# timecode format v1": the version digit is character 19.
    char buf[32] = {0};
    std::unique_ptr<VFRTranslator> res;
    if (std::fgets(buf, sizeof(buf), f) && buf[0] == '#' && std::strlen(buf) > 19) {
        if (buf[19] == '1')      res.reset(new TimecodesV1(f));
        else if (buf[19] == '2') res.reset(new TimecodesV2(f));
    }
    std::fclose(f);
    return res;
}
