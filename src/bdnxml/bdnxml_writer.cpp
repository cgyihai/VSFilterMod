/*
 *  VSFilterMod cross-platform port -- BDN+XML output.
 */

#include "bdnxml_writer.h"

#include "../subtitles/RTS.h"

#include <png.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <map>
#include <memory>

namespace vsfm {

namespace {

// ---------------------------------------------------------------------------
// Surface handling
//
// The rasterizer's native surface is *not* straight RGBA: colour channels are
// premultiplied by coverage, and the alpha channel is inverted, so 255 means
// fully transparent and 0 means opaque. (See Rasterizer's pixmix: the alpha
// byte is repeatedly multiplied by (256 - coverage), decaying from its initial
// 0xff.) Everything below converts on the way out.
// ---------------------------------------------------------------------------

const DWORD kTransparent = 0xFF000000;   // A=255 inverted, RGB=0

struct Surface
{
    int width = 0;
    int height = 0;
    std::vector<DWORD> pixels;

    void Reset(int w, int h)
    {
        width = w;
        height = h;
        pixels.assign((size_t)w * h, kTransparent);
    }

    void Clear() { std::fill(pixels.begin(), pixels.end(), kTransparent); }
};

/* Converts one native pixel to RGBA exactly as upstream's MaskSubMod does,
 * so a PNG written here is byte-identical to one avs2bdnxml writes from the
 * same rasterisation (it only swaps R and B before writing).
 *
 * This is CMemSubPic::AlphaBlt's MSP_RGBA branch, verbatim in effect:
 *
 *     bd = 0x100 - invA
 *     B  = (B << 8) / bd;  V = ((G << 8) / bd) << 8;  R = ((R << 8) / bd) << 16
 *     d  = B | V | R | (0xff000000 - (invA << 24))
 *
 * Note what it is not: it divides by A+1 rather than A, it truncates rather
 * than rounds, and it does not clamp -- a channel that comes out at 256 bleeds
 * into its neighbour through the OR. A "correct" un-premultiply differs by
 * one on most semi-transparent edge pixels, so fidelity wins over tidiness.
 * Pixels with invA == 0xff are never written upstream, which leaves the
 * Blackness clip's 0x00000000 behind. */
inline void ToMaskSubRgba(DWORD src, BYTE out[4])
{
    const DWORD inv = src >> 24;
    if (inv >= 0xff) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }

    const DWORD bd = 0x100 - inv;
    const DWORD B = ((src & 0x000000ff) << 8) / bd;
    const DWORD V = ((src & 0x0000ff00) / bd) << 8;
    const DWORD R = (((src & 0x00ff0000) >> 8) / bd) << 16;
    const DWORD d = B | V | R | ((0xff000000 - (src & 0xff000000)) & 0xff000000);

    out[0] = (BYTE)(d >> 16);   // R
    out[1] = (BYTE)(d >> 8);    // G
    out[2] = (BYTE)(d);         // B
    out[3] = (BYTE)(d >> 24);   // A
}

/* FNV-1a over the visible pixels, used to collapse runs of identical frames
 * into a single BDN event. */
uint64_t HashRegion(const Surface& s, const RECT& box)
{
    uint64_t h = 14695981039346656037ULL;
    for (int y = box.top; y < box.bottom; ++y) {
        const DWORD* row = &s.pixels[(size_t)y * s.width];
        for (int x = box.left; x < box.right; ++x) {
            h ^= row[x];
            h *= 1099511628211ULL;
        }
    }
    h ^= (uint64_t)(box.right - box.left) << 32;
    h *= 1099511628211ULL;
    h ^= (uint64_t)(box.bottom - box.top);
    return h * 1099511628211ULL;
}

/* Tightens `box` to the pixels that are actually not transparent. RTS's own
 * bbox is a layout bound and is routinely larger than the ink. */
bool TightenToInk(const Surface& s, RECT* box)
{
    int minX = box->right, minY = box->bottom, maxX = box->left - 1, maxY = box->top - 1;

    for (int y = box->top; y < box->bottom; ++y) {
        const DWORD* row = &s.pixels[(size_t)y * s.width];
        for (int x = box->left; x < box->right; ++x) {
            if (((row[x] >> 24) & 0xff) == 0xff) continue;   // fully transparent
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }

    if (maxX < minX || maxY < minY) return false;

    box->left = minX;
    box->top = minY;
    box->right = maxX + 1;
    box->bottom = maxY + 1;
    return true;
}

bool ClampBox(RECT* box, int w, int h)
{
    if (box->left < 0) box->left = 0;
    if (box->top < 0) box->top = 0;
    if (box->right > w) box->right = w;
    if (box->bottom > h) box->bottom = h;
    return box->right > box->left && box->bottom > box->top;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

void PngWrite(png_structp png, png_bytep data, png_size_t length)
{
    FILE* fp = (FILE*)png_get_io_ptr(png);
    if (!fp || std::fwrite(data, 1, length, fp) != length)
        png_error(png, "short write");
}

void PngFlush(png_structp png)
{
    if (FILE* fp = (FILE*)png_get_io_ptr(png))
        std::fflush(fp);
}

bool WritePng(const std::string& path, const Surface& s, const RECT& box, std::string* error)
{
    const int w = box.right - box.left;
    const int h = box.bottom - box.top;

#if defined(_WIN32)
    FILE* fp = _wfopen(U82W(path.c_str(), path.size()).c_str(), L"wb");
#else
    FILE* fp = std::fopen(path.c_str(), "wb");
#endif
    if (!fp) {
        if (error) *error = "cannot open " + path + " for writing";
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); if (error) *error = "png_create_write_struct failed"; return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(fp);
        if (error) *error = "png_create_info_struct failed";
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        if (error) *error = "libpng error while writing " + path;
        return false;
    }

    /* png_init_io() would hand our FILE* to libpng, but libpng normally lives
     * in a separate shared library with its own C runtime, and a FILE* is not
     * valid across that boundary -- on Windows it faults inside fwrite. Drive
     * the writes from here instead. */
    png_set_write_fn(png, (png_voidp)fp, PngWrite, PngFlush);

    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<BYTE> row((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        const DWORD* src = &s.pixels[(size_t)(box.top + y) * s.width + box.left];
        for (int x = 0; x < w; ++x)
            ToMaskSubRgba(src[x], &row[(size_t)x * 4]);
        png_write_row(png, row.data());
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// Timecodes
// ---------------------------------------------------------------------------

/* BDN timecodes are HH:MM:SS:FF on a non-drop grid, so a 23.976 timeline is
 * counted at 24 frames per second of timecode. DropFrame is reported as False
 * below to match. */
std::string FrameToTimecode(int frame, double fps)
{
    int grid = (int)(fps + 0.5);
    if (grid < 1) grid = 1;

    if (frame < 0) frame = 0;

    const int ff = frame % grid;
    const int totalSeconds = frame / grid;
    const int ss = totalSeconds % 60;
    const int mm = (totalSeconds / 60) % 60;
    const int hh = totalSeconds / 3600;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
    return buf;
}

std::string VideoFormatName(int width, int height)
{
    if (width == 1920 && height == 1080) return "1080p";
    if (width == 1280 && height == 720)  return "720p";
    if (width == 720 && height == 480)   return "480p";
    if (width == 720 && height == 576)   return "576p";
    if (width == 3840 && height == 2160) return "2160p";

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%dx%d", width, height);
    return buf;
}

std::string FrameRateName(double fps)
{
    /* BDN expects one of the broadcast rates spelled the conventional way. */
    struct { double value; const char* name; } known[] = {
        { 23.976, "23.976" }, { 24.0, "24" }, { 25.0, "25" },
        { 29.97,  "29.97"  }, { 30.0, "30" }, { 50.0, "50" },
        { 59.94,  "59.94"  }, { 60.0, "60" },
    };
    for (const auto& k : known)
        if (std::fabs(fps - k.value) < 0.01) return k.name;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", fps);
    return buf;
}

std::string XmlEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string StemOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// ---------------------------------------------------------------------------

struct Event
{
    int inFrame = 0;
    int outFrame = 0;
    RECT box = {0, 0, 0, 0};
    std::string png;
};

} // namespace

bool RenderBdnXml(const std::string& subtitlePath,
                  const std::string& xmlPath,
                  const BdnOptions& options,
                  BdnProgress* progress,
                  std::string* error)
{
    if (options.width <= 0 || options.height <= 0) {
        if (error) *error = "invalid frame size";
        return false;
    }
    if (options.fps <= 0.0 || options.renderFps <= 0.0f) {
        if (error) *error = "invalid frame rate";
        return false;
    }

    CCritSec lock;
    CRenderedTextSubtitle rts(&lock);

    if (!rts.Open(CString(U82W(subtitlePath.c_str(), subtitlePath.size()).c_str()), DEFAULT_CHARSET)) {
        if (error) *error = "cannot parse " + subtitlePath;
        return false;
    }

    const CSize size(options.width, options.height);
    const CRect videoRect(0, 0, options.width, options.height);
    rts.Init(size, videoRect);

    Surface surface;
    surface.Reset(options.width, options.height);

    SubPicDesc spd;
    spd.type = MSP_RGBA;
    spd.w = options.width;
    spd.h = options.height;
    spd.bpp = 32;
    spd.pitch = options.width * 4;
    spd.bits = surface.pixels.data();
    spd.vidrect = videoRect;

    const std::string outDir = DirectoryOf(xmlPath);
    const std::string stem = StemOf(xmlPath);

    std::vector<Event> events;
    int imageIndex = 0;
    bool aborted = false;

    /* MaskSubMod's clock, reproduced bit for bit. Upstream renders frame n at
     *
     *     (REFERENCE_TIME)(10000000i64 * n / fps)        // fps is a float
     *
     * and by C++'s usual arithmetic conversions that division happens in
     * single precision. The error is small -- about 0.1 ms an hour in, 0.4 ms
     * at worst over a feature -- but it decides boundary frames: at 24000/1001
     * every 240th frame starts exactly on a centisecond, the resolution of ASS
     * times, and at 293 of the first 399 such frames the float timestamp falls
     * just short of it. RTS truncates to milliseconds, so a line starting on
     * that centisecond appears one frame later than exact arithmetic says.
     * avs2bdnxml sees exactly these frames, so compute them the same way;
     * volatile pins the intermediates to real floats on targets that would
     * otherwise carry excess precision. */
    const float renderFps = options.renderFps;

    /* What RTS itself is told the frame rate is. Upstream's AviSynth filters
     * never set their subpicture queue's rate, so RTS always saw its default,
     * 25.0; the clip's rate only ever reached the timestamp above. It matters
     * only for frame-timed formats such as MicroDVD. */
    const double kQueueFps = 25.0;
    auto frameTime = [renderFps](int n) -> REFERENCE_TIME {
        volatile float num = (float)(10000000LL * (long long)n);
        volatile float q = num / renderFps;
        return (REFERENCE_TIME)q;
    };

    /* Frame range: up to the first frame at or past the last segment's end. */
    REFERENCE_TIME lastStop = 0;
    for (POSITION pos = rts.GetStartPosition(0, kQueueFps); pos; pos = rts.GetNext(pos))
        lastStop = std::max(lastStop, rts.GetStop(pos, kQueueFps));

    int endFrame = (int)((double)lastStop * renderFps / 10000000.0) + 2;
    while (endFrame > 0 && frameTime(endFrame - 1) >= lastStop) --endFrame;
    ++endFrame;
    if (options.lastFrame >= 0 && endFrame > options.lastFrame + 1)
        endFrame = options.lastFrame + 1;

    /* The open event, if any. Its PNG is written when it opens -- the image
     * is known then -- and only its OutTC is filled in when it closes. */
    bool runOpen = false;
    int runSegment = -1;
    uint64_t runHash = 0;
    RECT runBox = {0, 0, 0, 0};

    auto closeRun = [&](int frame) {
        if (!runOpen) return;
        events.back().outFrame = frame;
        runOpen = false;
        runSegment = -1;
    };

    const RECT fullFrame = {0, 0, options.width, options.height};

    for (int n = std::max(0, options.firstFrame); n < endFrame; ++n) {
        if (progress && !progress->OnFrame(n, endFrame)) { aborted = true; break; }

        const REFERENCE_TIME rt = frameTime(n);

        /* Same lookup RTS::Render does internally, so a frame counts as
         * carrying a subtitle exactly when upstream would draw one on it. */
        int segment = -1;
        const STSSegment* stss = rts.SearchSubs((int)(rt / 10000), kQueueFps, &segment);
        if (!stss || stss->subs.GetCount() == 0) {
            closeRun(n);
            continue;
        }

        surface.Clear();
        RECT bbox = {0, 0, 0, 0};
        rts.Render(spd, rt, kQueueFps, bbox);

        RECT ink = bbox;
        const bool visible = ClampBox(&ink, options.width, options.height) &&
                             TightenToInk(surface, &ink);
        if (!visible) {                       // avs2bdnxml's is_empty()
            closeRun(n);
            continue;
        }

        const RECT box = options.crop ? ink : fullFrame;
        const uint64_t hash = HashRegion(surface, box);

        if (runOpen && hash == runHash &&        // avs2bdnxml's is_identical()
            box.left == runBox.left && box.top == runBox.top &&
            box.right == runBox.right && box.bottom == runBox.bottom) {
            runSegment = segment;
            continue;
        }

        closeRun(n);

        Event ev;
        ev.inFrame = n;
        ev.outFrame = n + 1;
        ev.box = box;

        char name[64];
        std::snprintf(name, sizeof(name), "%s_%04d.png", stem.c_str(), imageIndex++);
        ev.png = name;

        if (!WritePng(outDir + ev.png, surface, ev.box, error)) return false;

        events.push_back(ev);
        runOpen = true;
        runSegment = segment;
        runHash = hash;
        runBox = box;
    }

    if (!aborted) closeRun(endFrame);

    if (aborted) {
        if (error) *error = "cancelled";
        return false;
    }

    // -----------------------------------------------------------------------
    // Index
    // -----------------------------------------------------------------------

#if defined(_WIN32)
    FILE* fp = _wfopen(U82W(xmlPath.c_str(), xmlPath.size()).c_str(), L"wb");
#else
    FILE* fp = std::fopen(xmlPath.c_str(), "wb");
#endif
    if (!fp) {
        if (error) *error = "cannot open " + xmlPath + " for writing";
        return false;
    }

    const std::string title = options.title.empty() ? stem : options.title;
    const std::string firstTc = events.empty() ? "00:00:00:00"
                                               : FrameToTimecode(events.front().inFrame, options.fps);
    const std::string lastTc = events.empty() ? "00:00:00:00"
                                              : FrameToTimecode(events.back().outFrame, options.fps);

    std::fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    std::fprintf(fp, "<BDN Version=\"0.93\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
                     " xsi:noNamespaceSchemaLocation=\"BD-03-006-0093b BDN File Format.xsd\">\n");
    std::fprintf(fp, "  <Description>\n");
    std::fprintf(fp, "    <Name Title=\"%s\" Content=\"\"/>\n", XmlEscape(title).c_str());
    std::fprintf(fp, "    <Language Code=\"%s\"/>\n", XmlEscape(options.languageCode).c_str());
    std::fprintf(fp, "    <Format VideoFormat=\"%s\" FrameRate=\"%s\" DropFrame=\"False\"/>\n",
                 VideoFormatName(options.width, options.height).c_str(),
                 FrameRateName(options.fps).c_str());
    std::fprintf(fp, "    <Events Type=\"Graphic\" FirstEventInTC=\"%s\" LastEventOutTC=\"%s\""
                     " NumberofEvents=\"%d\"/>\n",
                 firstTc.c_str(), lastTc.c_str(), (int)events.size());
    std::fprintf(fp, "  </Description>\n");
    std::fprintf(fp, "  <Events>\n");

    for (const Event& ev : events) {
        std::fprintf(fp, "    <Event Forced=\"False\" InTC=\"%s\" OutTC=\"%s\">\n",
                     FrameToTimecode(ev.inFrame, options.fps).c_str(),
                     FrameToTimecode(ev.outFrame, options.fps).c_str());
        std::fprintf(fp, "      <Graphic Width=\"%ld\" Height=\"%ld\" X=\"%ld\" Y=\"%ld\">%s</Graphic>\n",
                     (long)(ev.box.right - ev.box.left), (long)(ev.box.bottom - ev.box.top),
                     (long)ev.box.left, (long)ev.box.top,
                     XmlEscape(ev.png).c_str());
        std::fprintf(fp, "    </Event>\n");
    }

    std::fprintf(fp, "  </Events>\n");
    std::fprintf(fp, "</BDN>\n");
    std::fclose(fp);

    if (progress) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "wrote %d events", (int)events.size());
        progress->OnMessage(msg);
    }

    return true;
}

} // namespace vsfm
