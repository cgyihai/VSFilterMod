/*
 *  VSFilterMod cross-platform port -- BDN+XML output.
 *
 *  Renders an .ass/.ssa script through the VSFilterMod rasterizer and writes a
 *  BDN (Blu-ray Disc Network) index plus one PNG per event -- the interchange
 *  format BDSup2Sub, Scenarist and easyBD read.
 *
 *  This is a straight consumer of CRenderedTextSubtitle: every mod tag renders
 *  exactly as it does in Aegisub's preview, because it is the same code path.
 */

#pragma once

#include "../compat/vsfm_compat.h"

#include <string>
#include <vector>

namespace vsfm {

struct BdnOptions
{
    int  width = 1920;
    int  height = 1080;

    /* The nominal rate, used for the timecode grid and the FrameRate label
     * (23.976 counts on a 24 frame grid, as avs2bdnxml does). */
    double fps = 23.976;

    /* The rate the renderer is clocked at. Upstream's AviSynth filter takes
     * this as (float)fps_numerator / fps_denominator of the clip, so for
     * 23.976 material it is (float)24000/1001, not (float)23.976. Keep it in
     * single precision: see frameTime() in the writer. */
    float renderFps = 24000.0f / 1001.0f;

    /* Frame range to export; end < 0 means "to the last subtitle". */
    int  firstFrame = 0;
    int  lastFrame = -1;

    std::string languageCode = "und";   // ISO 639-2
    std::string title;                  // defaults to the output file's stem

    /* Crop each image to its ink bounding box (avs2bdnxml -a1). Off writes
     * the full frame for every event, like avs2bdnxml -a0. */
    bool crop = true;
};

struct BdnProgress
{
    virtual ~BdnProgress() {}
    /* Return false to abort. */
    virtual bool OnFrame(int frame, int totalFrames) { (void)frame; (void)totalFrames; return true; }
    virtual void OnMessage(const std::string& text) { (void)text; }
};

/* Writes `xmlPath` and the PNGs beside it. Returns false and fills `error` on
 * failure. `subtitlePath` is UTF-8. */
bool RenderBdnXml(const std::string& subtitlePath,
                  const std::string& xmlPath,
                  const BdnOptions& options,
                  BdnProgress* progress,
                  std::string* error);

} // namespace vsfm
