/*
 * Fuzzes the script parser and the renderer through the CSRI entry points,
 * which is how Aegisub hands untrusted scripts to the plugin.
 *
 * Two ways to run it:
 *
 *  - as a libFuzzer target: build with -DVSFM_LIBFUZZER and
 *    -fsanitize=fuzzer,address,undefined, then run the binary on a corpus;
 *
 *  - stand-alone, with no special toolchain: fuzz_render ITERATIONS SEED
 *    file.ass... mutates the given scripts with a fixed-seed generator and
 *    renders each mutant. Built with -Db_sanitize=address,undefined it finds
 *    memory errors and undefined behaviour; CI runs it that way. A failing
 *    iteration is written to fuzz-crash.ass first, so it can be replayed
 *    with "fuzz_render 0 0 fuzz-crash.ass". An input that takes 5 s or more
 *    is reported with its iteration number; VSFM_FUZZ_DUMP=N saves
 *    iteration N's input as fuzz-input.ass.
 */

#include "csri.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(VSFM_LIBFUZZER)
#include <csignal>
#include <unistd.h>
#define VSFM_FUZZ_WATCHDOG 1
#endif

namespace {

void RenderOnce(const uint8_t* data, size_t size)
{
    csri_rend* rend = csri_renderer_default();
    csri_inst* inst = csri_open_mem(rend, data, size, nullptr);
    if (!inst) return;

    const unsigned W = 320, H = 180;
    csri_fmt fmt;
    fmt.pixfmt = CSRI_F_BGR_;
    fmt.width = W;
    fmt.height = H;
    if (csri_request_fmt(inst, &fmt) == 0) {
        std::vector<unsigned char> buf((size_t)W * H * 4, 0);
        csri_frame frame;
        std::memset(&frame, 0, sizeof(frame));
        frame.pixfmt = CSRI_F_BGR_;
        frame.planes[0] = buf.data();
        frame.strides[0] = (ptrdiff_t)W * 4;
        for (double t : {0.0, 0.37, 1.0, 2.5, 7.0, 61.0})
            csri_render(inst, &frame, t);
    }
    csri_close(inst);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > (1u << 16)) return 0;
    RenderOnce(data, size);
    return 0;
}

#ifndef VSFM_LIBFUZZER

namespace {

struct Rng
{
    uint64_t s;
    uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    size_t Below(size_t n) { return n ? (size_t)(Next() % n) : 0; }
};

/* Fragments that steer mutants into the interesting parts of the parser. */
const char* const kTokens[] = {
    "{", "}", "\\", "(", ")", ",", "&H", "&", "\\N", "\\h", "\\n", "{\\p1}",
    "{\\p0}", "m 0 0 l 100 0 100 100", " b 1 2 3 4 5 6", " s 1 2 3 4 5 6 c",
    "\\t(", "\\move(", "\\pos(", "\\org(", "\\clip(", "\\iclip(", "\\fad(",
    "\\fade(", "\\distort(", "\\rnd", "\\rnds", "\\z", "\\jitter(", "\\mover(",
    "\\moves3(", "\\moves4(", "\\movevc(", "\\1vc(", "\\1va(", "\\1img(",
    "\\frs", "\\fsvp", "\\blur", "\\be", "\\bord", "\\shad", "\\fsc", "\\fsp",
    "\\fax", "\\fay", "\\frx", "\\fry", "\\frz", "\\fn", "\\fn@", "\\b", "\\i",
    "\\k", "\\kf", "\\ko", "\\kt", "\\r", "\\an", "\\a", "\\q", "\\c", "\\3c",
    "\\alpha", "\\fe", "\\pbo", "\\xbord", "\\ybord", "0", "-1", "999999999",
    "-2147483648", "4294967295", "1e308", "-1e308", "nan", "inf", "0.5",
    "1e-300", "&HFFFFFFFF&", "\xef\xbf\xbf", "\xf3\xb0\x81\x81", "\xe4\xb8\xad",
    "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,",
    "Style: Default,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,10,10,10,1\n",
    "\n[Events]\n", "\n[V4+ Styles]\n", "\n[Fonts]\nfontname: x.ttf\n", "\n",
};

std::string Mutate(std::string s, Rng& rng)
{
    const int edits = 1 + (int)rng.Below(8);
    for (int e = 0; e < edits; ++e) {
        const size_t pos = rng.Below(s.size() + 1);
        switch (rng.Below(6)) {
        case 0:                                     // flip a byte
            if (!s.empty()) s[rng.Below(s.size())] ^= (char)(1u << rng.Below(8));
            break;
        case 1:                                     // insert a token
            s.insert(pos, kTokens[rng.Below(sizeof(kTokens) / sizeof(kTokens[0]))]);
            break;
        case 2: {                                   // delete a span
            const size_t n = rng.Below(32);
            if (pos < s.size()) s.erase(pos, n);
            break;
        }
        case 3: {                                   // duplicate a span
            if (s.empty()) break;
            const size_t from = rng.Below(s.size());
            const size_t n = 1 + rng.Below(64);
            s.insert(pos, s.substr(from, n));
            break;
        }
        case 4:                                     // random byte
            s.insert(pos, 1, (char)rng.Below(256));
            break;
        default: {                                  // replace a number
            size_t d = s.find_first_of("0123456789", pos);
            if (d == std::string::npos) break;
            size_t end = s.find_first_not_of("0123456789.", d);
            if (end == std::string::npos) end = s.size();
            static const char* const kNumbers[] = {
                "0", "1", "-1", "255", "65535", "99999", "2147483647",
                "-2147483648", "4294967296", "1e9", "0.001", "360000"};
            s.replace(d, end - d, kNumbers[rng.Below(sizeof(kNumbers) / sizeof(kNumbers[0]))]);
            break;
        }
        }
    }
    if (s.size() > (1u << 16)) s.resize(1u << 16);
    return s;
}

#ifdef VSFM_FUZZ_WATCHDOG
/* An input that takes this long is a hang -- an infinite loop in a parser,
 * say -- and is reported like a crash, its script left in fuzz-crash.ass.
 * Generous, because legitimate inputs can be slow under the sanitizers: a
 * mutated PlayResY of 1 scales a 48 pt font to 8000 pixels, which takes
 * 10 s to render in a release build and ten times that under ASan. */
const unsigned kHangSeconds = 180;

void OnHang(int)
{
    static const char msg[] = "fuzz_render: input took too long (hang); see fuzz-crash.ass\n";
    ssize_t r = write(2, msg, sizeof(msg) - 1);
    (void)r;
    std::abort();
}
#endif

/* A PlayResX or PlayResY near zero scales everything up by the frame size
 * over it -- 180x for "PlayResY: 1" on this 320x180 frame -- and widening the
 * resulting giant shapes by a correspondingly giant border legitimately takes
 * minutes. Such mutants test nothing but patience, so they are skipped. */
bool HasTinyPlayRes(const std::string& s)
{
    for (const char* key : {"PlayResX:", "PlayResY:"}) {
        size_t at = 0;
        while ((at = s.find(key, at)) != std::string::npos) {
            at += std::strlen(key);
            const long v = std::strtol(s.c_str() + at, nullptr, 10);
            if (v > 0 && v < 16) return true;
        }
    }
    return false;
}

bool ReadFile(const char* path, std::string* out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s ITERATIONS SEED file.ass...\n", argv[0]);
        return 2;
    }
    const long iterations = std::strtol(argv[1], nullptr, 10);
    Rng rng{std::strtoull(argv[2], nullptr, 10) * 0x9E3779B97F4A7C15ull + 1};

    /* VSFM_FUZZ_DUMP=N saves iteration N's input as fuzz-input.ass. */
    const char* dumpEnv = std::getenv("VSFM_FUZZ_DUMP");
    const long dumpAt = dumpEnv ? std::strtol(dumpEnv, nullptr, 10) : -1;

    std::vector<std::string> seeds;
    for (int i = 3; i < argc; ++i) {
        std::string s;
        if (!ReadFile(argv[i], &s)) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        seeds.push_back(s);
    }

#ifdef VSFM_FUZZ_WATCHDOG
    std::signal(SIGALRM, OnHang);
#endif

    // The seeds as given, then mutants.
    for (const std::string& s : seeds) RenderOnce((const uint8_t*)s.data(), s.size());

    for (long it = 0; it < iterations; ++it) {
        const std::string m = Mutate(seeds[rng.Below(seeds.size())], rng);
        if (HasTinyPlayRes(m)) continue;
        if (FILE* f = std::fopen("fuzz-crash.ass", "wb")) {
            std::fwrite(m.data(), 1, m.size(), f);
            std::fclose(f);
        }
#ifdef VSFM_FUZZ_WATCHDOG
        alarm(kHangSeconds);
#endif
        const auto t0 = std::chrono::steady_clock::now();
        RenderOnce((const uint8_t*)m.data(), m.size());
#ifdef VSFM_FUZZ_WATCHDOG
        alarm(0);
#endif
        /* Slow inputs are reported, so that one which only becomes a hang on
         * another machine can be found again by its iteration number. */
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (secs >= 5.0)
            std::fprintf(stderr, "iteration %ld took %.1f s\n", it, secs);
        if (dumpAt == it) {
            if (FILE* f = std::fopen("fuzz-input.ass", "wb")) {
                std::fwrite(m.data(), 1, m.size(), f);
                std::fclose(f);
            }
        }
        if ((it + 1) % 500 == 0) std::fprintf(stderr, "%ld iterations\n", it + 1);
    }
    std::remove("fuzz-crash.ass");
    std::printf("ok: %zu seeds, %ld mutants rendered\n", seeds.size(), iterations);
    return 0;
}

#endif
