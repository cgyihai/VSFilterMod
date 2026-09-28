/*
 *  vsfmrender -- renders an .ass/.ssa script to BDN+XML.
 */

#include "bdnxml_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "vsfmrender -- VSFilterMod BDN+XML renderer\n"
        "\n"
        "usage: %s [options] <subtitle.ass> <output.xml>\n"
        "\n"
        "options:\n"
        "  -s, --size WxH       frame size (default 1920x1080)\n"
        "  -f, --fps RATE       23.976, 24, 25, 29.97, 50, 59.94, or an exact\n"
        "                       num/den such as 24000/1001 (default 23.976,\n"
        "                       which means 24000/1001 as in avs2bdnxml)\n"
        "  -l, --lang CODE      ISO 639-2 language code (default und)\n"
        "  -t, --title TEXT     title recorded in the index\n"
        "      --first N        first frame to export\n"
        "      --last N         last frame to export\n"
        "      --no-crop        write full-frame images (avs2bdnxml -a0)\n"
        "  -q, --quiet          suppress progress output\n"
        "  -h, --help           this message\n",
        argv0);
}

bool ParseSize(const char* s, int* w, int* h)
{
    const char* x = std::strpbrk(s, "xX*");
    if (!x) return false;
    *w = std::atoi(s);
    *h = std::atoi(x + 1);
    return *w > 0 && *h > 0;
}

/* Accepts "num/den" or a decimal. The NTSC decimals are taken to mean their
 * exact rationals -- avs2bdnxml's own table maps 23.976 to 24000/1001, and so
 * does AviSynth when a script passes video.framerate to MaskSub -- because the
 * renderer is clocked at (float)num/den, and (float)23.976 is a different
 * number that drifts by several frames over a feature. */
bool ParseFps(const char* s, double* nominal, float* render)
{
    const char* slash = std::strchr(s, '/');
    if (slash) {
        const double num = std::atof(s), den = std::atof(slash + 1);
        if (num <= 0 || den <= 0) return false;
        *nominal = num / den;
        *render = (float)num / (float)den;
        return true;
    }

    const double v = std::atof(s);
    if (v <= 0) return false;

    static const struct { double dec; double num; double den; } ntsc[] = {
        { 23.976, 24000, 1001 }, { 29.97, 30000, 1001 }, { 47.952, 48000, 1001 },
        { 59.94, 60000, 1001 }, { 119.88, 120000, 1001 },
    };
    for (const auto& r : ntsc) {
        if (v > r.dec - 0.001 && v < r.dec + 0.001) {
            *nominal = r.num / r.den;
            *render = (float)r.num / (float)r.den;
            return true;
        }
    }
    *nominal = v;
    *render = (float)v;
    return true;
}

class ConsoleProgress final : public vsfm::BdnProgress
{
public:
    explicit ConsoleProgress(bool quiet) : m_quiet(quiet) {}

    bool OnFrame(int frame, int totalFrames) override
    {
        if (m_quiet) return true;
        if (frame - m_lastReported < 50 && frame != 0) return true;
        m_lastReported = frame;
        std::fprintf(stderr, "\rframe %d / %d", frame, totalFrames);
        std::fflush(stderr);
        return true;
    }

    void OnMessage(const std::string& text) override
    {
        if (m_quiet) return;
        std::fprintf(stderr, "\r%-40s\n", text.c_str());
    }

private:
    bool m_quiet;
    int m_lastReported = -1000;
};

} // namespace

int main(int argc, char** argv)
{
    vsfm::BdnOptions opt;
    std::string input, output;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "vsfmrender: %s requires a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") { Usage(argv[0]); return 0; }
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "-s" || a == "--size") {
            if (!ParseSize(next("--size"), &opt.width, &opt.height)) {
                std::fprintf(stderr, "vsfmrender: bad size, expected WxH\n");
                return 2;
            }
        }
        else if (a == "-f" || a == "--fps") {
            if (!ParseFps(next("--fps"), &opt.fps, &opt.renderFps)) {
                std::fprintf(stderr, "vsfmrender: bad frame rate\n");
                return 2;
            }
        }
        else if (a == "-l" || a == "--lang") opt.languageCode = next("--lang");
        else if (a == "-t" || a == "--title") opt.title = next("--title");
        else if (a == "--first") opt.firstFrame = std::atoi(next("--first"));
        else if (a == "--last") opt.lastFrame = std::atoi(next("--last"));
        else if (a == "--no-crop") opt.crop = false;
        else if (!a.empty() && a[0] == '-' && a != "-") {
            std::fprintf(stderr, "vsfmrender: unknown option %s\n", a.c_str());
            return 2;
        }
        else if (input.empty()) input = a;
        else if (output.empty()) output = a;
        else {
            std::fprintf(stderr, "vsfmrender: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }

    if (input.empty() || output.empty()) {
        Usage(argv[0]);
        return 2;
    }

    ConsoleProgress progress(quiet);
    std::string error;

    if (!vsfm::RenderBdnXml(input, output, opt, &progress, &error)) {
        std::fprintf(stderr, "\nvsfmrender: %s\n", error.c_str());
        return 1;
    }

    return 0;
}
