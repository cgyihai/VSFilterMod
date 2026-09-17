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
    double fps = 23.976;

    /* Frame range to export; end < 0 means "to the last subtitle". */
    int  firstFrame = 0;
    int  lastFrame = -1;

    std::string languageCode = "und";   // ISO 639-2
    std::string title;                  // defaults to the output file's stem

    /* Emit one PNG per frame for animated lines. With this off, an animated
     * line is sampled once at its midpoint, which is much faster but freezes
     * \t, \move and karaoke. */
    bool animated = true;

    /* Crop each image to its ink bounding box. Blu-ray decoders prefer small
     * windows, and it is what every other BDN exporter does. */
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
