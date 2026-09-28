/*
 *  VSFilterMod cross-platform port -- AviSynth+ entry points.
 *
 *  Restores upstream's TextSubMod / MaskSubMod (and the TextSub / MaskSub
 *  aliases, and TextSubModSwapUV), so that AviSynth scripts -- and therefore
 *  avs2bdnxml, which renders through MaskSubMod -- can load this build the way
 *  they loaded upstream's.
 *
 *  Written against AviSynth+'s C interface rather than the C++ one: the C ABI
 *  is identical across compilers and across Windows, Linux and macOS, which the
 *  C++ interface is not.
 *
 *  The render path mirrors upstream's CFilter::Render step for step, because
 *  that is what decides the pixels avs2bdnxml sees:
 *
 *    - timestamp: (REFERENCE_TIME)(10000000i64 * n / fps), fps a float, so the
 *      division is single precision; or the VFR timecode when vfr= is given;
 *    - the subpicture queue: IsAnimated() is always true in RTS, so every
 *      frame renders at exactly that timestamp, into an in-memory subpicture
 *      cleared over its previous dirty rectangle;
 *    - RTS is always handed fps = 25.0 -- the queue's default, which upstream
 *      never changed from its AviSynth filters;
 *    - only the rendered bounding box is composited;
 *    - RGB32 and RGB24 are blitted upside down (AviSynth stores RGB bottom-up)
 *      but MaskSubMod's RGBA is not, exactly as upstream.
 *
 *  Not carried over: VobSub(), the SSF renderer that upstream tried before
 *  RTS, and the background thread that reloaded the subtitle when the file
 *  changed on disk.
 *
 *  AviSynth is not linked. The plugin is loaded *by* AviSynth, so its C API is
 *  already in the process; the symbols are resolved from that module at init.
 *  A link-time dependency would stop this same library loading as a CSRI
 *  plugin in Aegisub on a machine without AviSynth.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#include "../compat/vsfm_compat.h"
#include "../subtitles/RTS.h"
#include "../subpic/mem_subpic.h"
#include "vfr.h"

#define AVSC_NO_DECLSPEC
#include "avisynth/avisynth_c.h"

#include <memory>
#include <mutex>
#include <string>

#if defined(_WIN32)
#define VSFM_AVS_EXPORT extern "C" __declspec(dllexport)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define VSFM_AVS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// ---------------------------------------------------------------------------
// The C API, resolved from the AviSynth module that loaded us
// ---------------------------------------------------------------------------

struct AvsApi
{
    avs_add_function_func        add_function = nullptr;
    avs_new_c_filter_func        new_c_filter = nullptr;
    avs_get_frame_func           get_frame = nullptr;
    avs_release_video_frame_func release_video_frame = nullptr;
    avs_make_writable_func       make_writable = nullptr;
    avs_get_write_ptr_p_func     get_write_ptr_p = nullptr;
    avs_get_pitch_p_func         get_pitch_p = nullptr;
    avs_invoke_func              invoke = nullptr;
    avs_release_value_func       release_value = nullptr;
    avs_take_clip_func           take_clip = nullptr;
    avs_set_to_clip_func         set_to_clip = nullptr;
    avs_release_clip_func        release_clip = nullptr;
    avs_save_string_func         save_string = nullptr;
    avs_set_var_func             set_var = nullptr;
    avs_get_var_func             get_var = nullptr;

    bool ok = false;
} g_avs;

void* FindAvisynthSymbol(const char* name)
{
#if defined(_WIN32)
    static HMODULE module = nullptr;
    if (!module) module = GetModuleHandleW(L"avisynth.dll");
    if (!module) module = LoadLibraryW(L"avisynth.dll");
    return module ? (void*)GetProcAddress(module, name) : nullptr;
#else
    /* Hosts commonly dlopen() libavisynth with RTLD_LOCAL, which hides its
     * symbols from RTLD_DEFAULT; RTLD_NOLOAD returns a handle to the copy that
     * is already mapped instead of loading a second one. */
    static void* handle = nullptr;
    if (!handle) {
        const char* names[] = {
#if defined(__APPLE__)
            "libavisynth.dylib", "libavisynth.11.dylib", "libavisynth.10.dylib",
#endif
            "libavisynth.so", "libavisynth.so.11", "libavisynth.so.10",
        };
        for (const char* n : names) {
            handle = dlopen(n, RTLD_NOW | RTLD_NOLOAD);
            if (handle) break;
        }
    }
    void* sym = handle ? dlsym(handle, name) : nullptr;
    return sym ? sym : dlsym(RTLD_DEFAULT, name);
#endif
}

template <typename T>
bool Resolve(T& slot, const char* name)
{
    slot = reinterpret_cast<T>(FindAvisynthSymbol(name));
    return slot != nullptr;
}

bool LoadAvsApi()
{
    if (g_avs.ok) return true;
    bool ok = true;
    ok &= Resolve(g_avs.add_function, "avs_add_function");
    ok &= Resolve(g_avs.new_c_filter, "avs_new_c_filter");
    ok &= Resolve(g_avs.get_frame, "avs_get_frame");
    ok &= Resolve(g_avs.release_video_frame, "avs_release_video_frame");
    ok &= Resolve(g_avs.make_writable, "avs_make_writable");
    ok &= Resolve(g_avs.get_write_ptr_p, "avs_get_write_ptr_p");
    ok &= Resolve(g_avs.get_pitch_p, "avs_get_pitch_p");
    ok &= Resolve(g_avs.invoke, "avs_invoke");
    ok &= Resolve(g_avs.release_value, "avs_release_value");
    ok &= Resolve(g_avs.take_clip, "avs_take_clip");
    ok &= Resolve(g_avs.set_to_clip, "avs_set_to_clip");
    ok &= Resolve(g_avs.release_clip, "avs_release_clip");
    ok &= Resolve(g_avs.save_string, "avs_save_string");
    ok &= Resolve(g_avs.set_var, "avs_set_var");
    ok &= Resolve(g_avs.get_var, "avs_get_var");
    g_avs.ok = ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool FileExists(const std::wstring& path)
{
#if defined(_WIN32)
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    const std::string n = vsfm::W2U8(path.c_str(), path.size());
    return stat(n.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* A path from an AviSynth script. On Windows it is in the ANSI code page --
 * that is how upstream read it (MFC's CString(const char*)) -- unless
 * AviSynth+ is running the script as UTF-8. Take the ANSI reading when it
 * names a file, else the UTF-8 one. Elsewhere scripts are UTF-8. */
std::wstring ScriptPath(const char* s)
{
    if (!s) return std::wstring();
#if defined(_WIN32)
    auto convert = [s](UINT cp) {
        const int n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(cp, 0, s, -1, &w[0], n);
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    };
    const std::wstring ansi = convert(CP_ACP);
    if (FileExists(ansi)) return ansi;
    const std::wstring utf8 = convert(CP_UTF8);
    return FileExists(utf8) ? utf8 : ansi;
#else
    return vsfm::U82W(s, std::strlen(s));
#endif
}

// ---------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------

/* Upstream's MaskSubMod set a script-global variable RGBA=true, and every
 * RGB32 TextSubMod rendered in RGBA mode from then on -- including ones
 * elsewhere in the same script. Odd, but it is how scripts behave, so keep it:
 * read the variable at render time exactly as upstream's GetFrame did. */
const char* const kRgbaVar = "RGBA";

/* The subpicture queue's frame rate, which upstream's AviSynth path never
 * set. RTS only uses it for frame-timed formats (MicroDVD). */
const double kQueueFps = 25.0;

bool s_swapUV = false;

struct TextSubFilter
{
    std::mutex lock;
    CCritSec rtsLock;
    std::unique_ptr<CRenderedTextSubtitle> rts;
    std::unique_ptr<vsfm::MemSubPic> subpic;
    std::unique_ptr<VFRTranslator> vfr;
    float fps = -1;              // upstream m_fps; <= 0 means "use the clip's"
    int matrix = vsfm::YCC_MATRIX_AUTO;
    int range = vsfm::YCC_RANGE_AUTO;

    /* The queue's cached subpicture interval. */
    REFERENCE_TIME cachedStart = 0, cachedStop = 0;
};

int SubPicTypeFor(AVS_FilterInfo* fi)
{
    const AVS_VideoInfo& vi = fi->vi;
    if (avs_is_rgb32(&vi)) {
        AVS_Value v = g_avs.get_var(fi->env, kRgbaVar);
        const bool rgba = avs_defined(v) && v.type == 'b' && avs_as_bool(v);
        if (avs_defined(v)) g_avs.release_value(v);
        return rgba ? MSP_RGBA : MSP_RGB32;
    }
    if (avs_is_rgb24(&vi)) return MSP_RGB24;
    if (avs_is_yuy2(&vi)) return MSP_YUY2;
    if (vi.pixel_type == AVS_CS_YV12) return s_swapUV ? MSP_IYUV : MSP_YV12;
    if (vi.pixel_type == AVS_CS_I420) return s_swapUV ? MSP_YV12 : MSP_IYUV;
    return -1;
}

AVS_VideoFrame* AVSC_CC GetFrame(AVS_FilterInfo* fi, int n)
{
    TextSubFilter* f = (TextSubFilter*)fi->user_data;

    AVS_VideoFrame* frame = g_avs.get_frame(fi->child, n);
    if (!frame) return nullptr;
    g_avs.make_writable(fi->env, &frame);

    const int type = SubPicTypeFor(fi);
    if (type < 0 || !vsfm::IsSupportedSubPicType(type)) {
        fi->error = "TextSubMod: unsupported colorspace; use RGB32, RGB24, YUY2, YV12 or I420";
        return frame;
    }

    SubPicDesc dst;
    dst.type = type;
    dst.w = fi->vi.width;
    dst.h = fi->vi.height;
    dst.pitch = g_avs.get_pitch_p(frame, AVS_DEFAULT_PLANE);
    dst.bits = g_avs.get_write_ptr_p(frame, AVS_DEFAULT_PLANE);
    if (type == MSP_YV12 || type == MSP_IYUV) {
        dst.pitchUV = g_avs.get_pitch_p(frame, AVS_PLANAR_U);
        dst.bitsU = g_avs.get_write_ptr_p(frame, AVS_PLANAR_U);
        dst.bitsV = g_avs.get_write_ptr_p(frame, AVS_PLANAR_V);
    }
    dst.bpp = dst.pitch / dst.w * 8;             // upstream's own derivation
    dst.vidrect.left = dst.vidrect.top = 0;
    dst.vidrect.right = dst.w;
    dst.vidrect.bottom = dst.h;

    std::lock_guard<std::mutex> guard(f->lock);

    const float fps = f->fps > 0 ? f->fps
                                 : (float)fi->vi.fps_numerator / (float)fi->vi.fps_denominator;

    REFERENCE_TIME rt;
    if (!f->vfr) {
        volatile float num = (float)(10000000LL * (long long)n);
        volatile float q = num / fps;
        rt = (REFERENCE_TIME)q;
    } else {
        rt = (REFERENCE_TIME)(10000000 * f->vfr->TimeStampFromFrameNumber(n));
    }

    // CMemSubPicAllocator: one subpicture, typed after the first frame's clip.
    if (!f->subpic)
        f->subpic.reset(new vsfm::MemSubPic(dst.w, dst.h, type, f->matrix, f->range));

    // CSubPicQueueNoThread::LookupSubPic
    bool have = f->cachedStart <= rt && rt < f->cachedStop;
    if (!have) {
        CAutoLock rtsGuard(&f->rtsLock);
        POSITION pos = f->rts->GetStartPosition(rt, kQueueFps);
        if (pos) {
            // IsAnimated() is always true, so the interval is [rt, rt + 1).
            const REFERENCE_TIME start = rt, stop = rt + 1;
            SubPicDesc& spd = f->subpic->Lock();
            CRect r(0, 0, 0, 0);
            f->rts->Render(spd, (start + stop) / 2, kQueueFps, r);
            f->subpic->Unlock(r);
            f->cachedStart = start;
            f->cachedStop = stop;
            have = true;
        }
    }

    if (have) {
        if (type == MSP_RGB32 || type == MSP_RGB24) dst.h = -dst.h;
        f->subpic->AlphaBlt(dst);
    }

    return frame;
}

void AVSC_CC FreeFilter(AVS_FilterInfo* fi)
{
    delete (TextSubFilter*)fi->user_data;
    fi->user_data = nullptr;
}

AVS_Value Error(AVS_ScriptEnvironment* env, const std::string& msg)
{
    return avs_new_value_error(g_avs.save_string(env, msg.c_str(), (int)msg.size()));
}

/* Builds the filter on `child`. Consumes nothing; the caller releases `child`. */
AVS_Value MakeTextSub(AVS_ScriptEnvironment* env, AVS_Value child, const char* fn,
                      int charset, float fps, const char* vfrPath, const char* name)
{
    std::unique_ptr<TextSubFilter> f(new TextSubFilter());
    f->fps = fps;

    if (vfrPath) {
        f->vfr = GetVFRTranslator(ScriptPath(vfrPath));
        if (!f->vfr)
            return Error(env, std::string(name) + ": cannot read VFR timecodes \"" + vfrPath + "\"");
    }

    f->rts.reset(new CRenderedTextSubtitle(&f->rtsLock));
    if (!f->rts->Open(CString(ScriptPath(fn).c_str()), charset))
        return Error(env, std::string(name) + ": Can't open \"" + fn + "\"");

    f->matrix = f->rts->m_eYCbCrMatrix;
    f->range = f->rts->m_eYCbCrRange;

    AVS_FilterInfo* fi = nullptr;
    AVS_Clip* clip = g_avs.new_c_filter(env, &fi, child, 1);
    if (!clip || !fi) return Error(env, std::string(name) + ": cannot create filter");

    fi->user_data = f.release();
    fi->get_frame = GetFrame;
    fi->free_filter = FreeFilter;

    AVS_Value ret;
    g_avs.set_to_clip(&ret, clip);
    g_avs.release_clip(clip);
    return ret;
}

// TextSubMod(clip, [file]s, [charset]i, [fps]f, [vfr]s)
AVS_Value AVSC_CC TextSubCreate(AVS_ScriptEnvironment* env, AVS_Value args, void* user_data)
{
    const char* name = (const char*)user_data;
    AVS_Value clip = avs_array_elt(args, 0);
    AVS_Value file = avs_array_elt(args, 1);
    AVS_Value cs   = avs_array_elt(args, 2);
    AVS_Value fps  = avs_array_elt(args, 3);
    AVS_Value vfr  = avs_array_elt(args, 4);

    if (!avs_defined(file))
        return Error(env, std::string(name) + ": You must specify a subtitle file to use");

    return MakeTextSub(env, clip, avs_as_string(file),
                       avs_defined(cs) ? avs_as_int(cs) : DEFAULT_CHARSET,
                       avs_defined(fps) ? (float)avs_as_float(fps) : -1.0f,
                       avs_defined(vfr) ? avs_as_string(vfr) : nullptr,
                       name);
}

// MaskSubMod([file]s, [width]i, [height]i, [fps]f, [length]i, [charset]i, [vfr]s)
AVS_Value AVSC_CC MaskSubCreate(AVS_ScriptEnvironment* env, AVS_Value args, void* user_data)
{
    const char* name = (const char*)user_data;
    AVS_Value file   = avs_array_elt(args, 0);
    AVS_Value width  = avs_array_elt(args, 1);
    AVS_Value height = avs_array_elt(args, 2);
    AVS_Value fps    = avs_array_elt(args, 3);
    AVS_Value length = avs_array_elt(args, 4);
    AVS_Value cs     = avs_array_elt(args, 5);
    AVS_Value vfr    = avs_array_elt(args, 6);

    if (!avs_defined(file))
        return Error(env, std::string(name) + ": You must specify a subtitle file to use");
    if (!avs_defined(fps) && !avs_defined(vfr))
        return Error(env, std::string(name) + ": You must specify either FPS or a VFR timecodes file");

    /* Blackness(width, height, fps, length, pixel_type="RGB32"), passing the
     * caller's values through untouched -- undefined ones stay undefined and
     * take Blackness's own defaults, as upstream's did. */
    AVS_Value bargs[5] = { width, height, fps, length, avs_new_value_string("RGB32") };
    const char* bnames[5] = { "width", "height", "fps", "length", "pixel_type" };
    AVS_Value black = g_avs.invoke(env, "Blackness", avs_new_value_array(bargs, 5), bnames);
    if (avs_is_error(black)) return black;
    if (!avs_is_clip(black)) {
        g_avs.release_value(black);
        return Error(env, std::string(name) + ": Blackness did not return a clip");
    }

    g_avs.set_var(env, g_avs.save_string(env, kRgbaVar, -1), avs_new_value_bool(1));

    AVS_Value ret = MakeTextSub(env, black, avs_as_string(file),
                                avs_defined(cs) ? avs_as_int(cs) : DEFAULT_CHARSET,
                                avs_defined(fps) ? (float)avs_as_float(fps) : -1.0f,
                                avs_defined(vfr) ? avs_as_string(vfr) : nullptr,
                                name);
    g_avs.release_value(black);
    return ret;
}

// TextSubModSwapUV(bool)
AVS_Value AVSC_CC SwapUV(AVS_ScriptEnvironment*, AVS_Value args, void*)
{
    /* Upstream's effect: it only changes which planar type the clip is mapped
     * to. With AviSynth handing over the U and V planes explicitly, that does
     * not move a single sample -- kept for script compatibility. */
    AVS_Value v = avs_array_elt(args, 0);
    s_swapUV = avs_defined(v) ? !!avs_as_bool(v) : false;
    AVS_Value vv;
    vv.type = 'v';
    vv.array_size = 0;
    vv.d.integer = 0;
    return vv;
}

const char* PluginInit(AVS_ScriptEnvironment* env)
{
    if (!LoadAvsApi()) return "VSFilterMod: cannot resolve the AviSynth C API";

    static const char kTextSubMod[] = "TextSubMod";
    static const char kTextSub[] = "TextSub";
    static const char kMaskSubMod[] = "MaskSubMod";
    static const char kMaskSub[] = "MaskSub";

    const char* textArgs = "c[file]s[charset]i[fps]f[vfr]s";
    const char* maskArgs = "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s";

    g_avs.add_function(env, "TextSubMod", textArgs, TextSubCreate, (void*)kTextSubMod);
    g_avs.add_function(env, "TextSubModSwapUV", "b", SwapUV, nullptr);
    g_avs.add_function(env, "MaskSubMod", maskArgs, MaskSubCreate, (void*)kMaskSubMod);

    g_avs.add_function(env, "TextSub", textArgs, TextSubCreate, (void*)kTextSub);
    g_avs.add_function(env, "TextSubSwapUV", "b", SwapUV, nullptr);
    g_avs.add_function(env, "MaskSub", maskArgs, MaskSubCreate, (void*)kMaskSub);

    return "VSFilterMod (cross-platform) -- TextSubMod, MaskSubMod";
}

} // namespace

/* Only the classic entry point. avisynth_c_plugin_init2 would declare this
 * plugin ready for the V11 interface's 64-bit values, which it has not been
 * checked against. */
VSFM_AVS_EXPORT const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env)
{
    return PluginInit(env);
}
