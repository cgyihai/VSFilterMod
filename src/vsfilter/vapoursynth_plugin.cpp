/*
 *  VSFilterMod cross-platform port -- VapourSynth entry point.
 *
 *  vsfm.TextSubMod(clip, file, charset, fps, vfr, accurate), with the
 *  identifier and namespace of upstream's VapourSynth filter
 *  (com.holywu.vsfiltermod / vsfm), so scripts written for it run unchanged.
 *  Written against API 4 (VapourSynth R55 and later); upstream's targeted the
 *  retired API 3.
 *
 *  Pixels:
 *
 *    - YUV420P8 and RGB24 render exactly as upstream's filter did: the same
 *      subpicture, the same YV12 conversion and blend, and for RGB the same
 *      packed RGB32 blend it ran on a converted copy of the frame;
 *    - other bit depths are blended at the clip's own precision. Upstream
 *      split 10- and 16-bit YUV420 samples into two 8-bit halves, drew the
 *      subtitle on the high half only and recombined them -- or, with
 *      accurate=1, drew it on the low half too, which corrupts the colours.
 *      Here the same arithmetic runs on whole samples, for every depth from 9
 *      to 16 bits; `accurate` is accepted and has nothing left to select;
 *    - also accepted, unlike upstream: YUV 4:2:2 and Gray, and RGB above 8
 *      bits. YUV 4:4:4 is not: the subtitle's chroma is computed per pixel
 *      pair, as upstream's YUV conversion does -- convert to RGB for it.
 *
 *  The timing and the subpicture follow upstream's shared filter path; see
 *  sub_source.h. The subtitle file and the VFR timecodes file are paths in
 *  UTF-8 -- on Windows, upstream's ANSI fallback is kept.
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

#include "../compat/vsfm_compat.h"
#include "vsfm_version.h"
#include "sub_source.h"

#include "vapoursynth/VapourSynth4.h"

#include <memory>
#include <mutex>
#include <string>

namespace {

enum class Layout { Yuv420P8, Yuv, Gray, Rgb };

struct TextSubData
{
    VSNode* node = nullptr;
    VSVideoInfo vi = {};
    Layout layout = Layout::Yuv420P8;
    int subpicType = MSP_YV12;
    float fps = -1;

    std::mutex lock;
    vsfm::SubtitleSource source;
};

void Composite(const TextSubData* d, const vsfm::MemSubPic* subpic, VSFrame* frame, const VSAPI* vsapi)
{
    const int planes = d->vi.format.numPlanes;
    if (d->layout == Layout::Yuv420P8) {
        /* Upstream's path: AlphaBlt onto a YV12 picture of the frame. */
        SubPicDesc dst;
        dst.type = MSP_YV12;
        dst.w = d->vi.width;
        dst.h = d->vi.height;
        dst.bpp = 8;
        dst.pitch = (int)vsapi->getStride(frame, 0);
        dst.pitchUV = (int)vsapi->getStride(frame, 1);
        dst.bits = vsapi->getWritePtr(frame, 0);
        dst.bitsU = vsapi->getWritePtr(frame, 1);
        dst.bitsV = vsapi->getWritePtr(frame, 2);
        dst.vidrect.left = dst.vidrect.top = 0;
        dst.vidrect.right = dst.w;
        dst.vidrect.bottom = dst.h;
        subpic->AlphaBlt(dst);
        return;
    }

    vsfm::MemSubPic::Planes t;
    t.family = d->layout == Layout::Rgb  ? vsfm::MemSubPic::PLANES_RGB
             : d->layout == Layout::Gray ? vsfm::MemSubPic::PLANES_GRAY
                                         : vsfm::MemSubPic::PLANES_YUV;
    t.bits = d->vi.format.bitsPerSample;
    t.subW = d->vi.format.subSamplingW;
    t.subH = d->vi.format.subSamplingH;
    for (int i = 0; i < planes && i < 3; i++) {
        t.data[i] = vsapi->getWritePtr(frame, i);
        t.stride[i] = vsapi->getStride(frame, i);
    }
    subpic->AlphaBltPlanes(t);
}

const VSFrame* VS_CC TextSubGetFrame(int n, int activationReason, void* instanceData, void** frameData,
                                     VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi)
{
    (void)frameData;
    TextSubData* d = static_cast<TextSubData*>(instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    const VSFrame* src = vsapi->getFrameFilter(n, d->node, frameCtx);

    /* One subpicture per instance, reused frame to frame as upstream's queue
     * reused it: render and composite under one lock. */
    std::lock_guard<std::mutex> guard(d->lock);

    const REFERENCE_TIME rt = d->source.Timestamp(n, d->fps);
    const vsfm::MemSubPic* subpic = d->source.Render(rt, d->subpicType, d->vi.width, d->vi.height);
    if (!subpic) return src;
    const RECT& dirty = subpic->Dirty();
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return src;

    /* SubtitleSource::Render does not throw, and the blends cannot. */
    VSFrame* dst = vsapi->copyFrame(src, core);
    vsapi->freeFrame(src);
    Composite(d, subpic, dst, vsapi);
    return dst;
}

void VS_CC TextSubFree(void* instanceData, VSCore* core, const VSAPI* vsapi)
{
    (void)core;
    TextSubData* d = static_cast<TextSubData*>(instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

/* Which layout a clip is drawn in, or an empty string naming why none. */
std::string ChooseLayout(const VSVideoInfo& vi, Layout* layout, int* subpicType)
{
    const VSVideoFormat& f = vi.format;
    if (f.colorFamily == cfUndefined || vi.width <= 0 || vi.height <= 0)
        return "only clips of constant format and dimensions are supported";
    if (f.sampleType != stInteger || f.bitsPerSample < 8 || f.bitsPerSample > 16)
        return "only 8- to 16-bit integer clips are supported";

    switch (f.colorFamily) {
    case cfYUV:
        if (f.subSamplingW == 1 && f.subSamplingH == 1) {
            *layout = f.bitsPerSample == 8 ? Layout::Yuv420P8 : Layout::Yuv;
            *subpicType = MSP_YV12;
            return std::string();
        }
        if (f.subSamplingW == 1 && f.subSamplingH == 0) {
            *layout = Layout::Yuv;
            *subpicType = MSP_YUY2;
            return std::string();
        }
        return "YUV must be 4:2:0 or 4:2:2; convert other YUV to RGB";
    case cfGray:
        *layout = Layout::Gray;
        *subpicType = MSP_YUY2;
        return std::string();
    case cfRGB:
        *layout = Layout::Rgb;
        *subpicType = MSP_RGB32;
        return std::string();
    default:
        return "unsupported colour family";
    }
}

void TextSubCreateUnguarded(const VSMap* in, VSMap* out, void* userData, VSCore* core, const VSAPI* vsapi)
{
    const std::string name = static_cast<const char*>(userData);
    std::unique_ptr<TextSubData> d(new TextSubData());
    int err = 0;

    auto fail = [&](const std::string& msg) {
        vsapi->mapSetError(out, (name + ": " + msg).c_str());
        if (d->node) vsapi->freeNode(d->node);
        d->node = nullptr;
    };

    d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d->vi = *vsapi->getVideoInfo(d->node);

    const std::string why = ChooseLayout(d->vi, &d->layout, &d->subpicType);
    if (!why.empty()) return fail(why);

    const char* file = vsapi->mapGetData(in, "file", 0, nullptr);

    int64_t charset = vsapi->mapGetInt(in, "charset", 0, &err);
    if (err) charset = DEFAULT_CHARSET;

    float fps = (float)vsapi->mapGetFloat(in, "fps", 0, &err);
    if (err) fps = -1.0f;
    /* Upstream: the given fps, else the clip's, as a float. */
    d->fps = (fps > 0.0f || !d->vi.fpsNum) ? fps : (float)d->vi.fpsNum / (float)d->vi.fpsDen;

    const char* vfr = vsapi->mapGetData(in, "vfr", 0, &err);
    if (!err && vfr && !d->source.OpenVfr(vsfm::ScriptPath(vfr, true)))
        return fail(std::string("cannot read VFR timecodes \"") + vfr + "\"");

    if (!d->vi.fpsNum && fps <= 0.0f && !d->source.HasVfr())
        return fail("variable framerate clip must have fps or vfr specified");

    /* accurate= chose upstream's two-pass 16-bit hack; samples are always
     * blended whole here. Read it so that it is not reported as unknown. */
    (void)vsapi->mapGetInt(in, "accurate", 0, &err);

    if (!d->source.Open(vsfm::ScriptPath(file, true), (int)charset))
        return fail(std::string("can't open \"") + file + "\"");

    VSFilterDependency deps[] = { { d->node, rpStrictSpatial } };
    TextSubData* data = d.release();
    vsapi->createVideoFilter(out, name.c_str(), &data->vi, TextSubGetFrame, TextSubFree,
                             fmParallelRequests, deps, 1, data, core);
}

/* No C++ exception may unwind into VapourSynth (an allocation failure, say). */
void VS_CC TextSubCreate(const VSMap* in, VSMap* out, void* userData, VSCore* core, const VSAPI* vsapi)
{
    try {
        TextSubCreateUnguarded(in, out, userData, core, vsapi);
    } catch (...) {
        vsapi->mapSetError(out, (std::string(static_cast<const char*>(userData)) + ": out of memory").c_str());
    }
}

} // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi)
{
    vspapi->configPlugin("com.holywu.vsfiltermod", "vsfm",
                         "VSFilterMod " VSFM_VERSION " (cross-platform)",
                         VS_MAKE_VERSION(VSFM_VERSION_MAJOR, VSFM_VERSION_MINOR),
                         VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("TextSubMod",
                             "clip:vnode;"
                             "file:data;"
                             "charset:int:opt;"
                             "fps:float:opt;"
                             "vfr:data:opt;"
                             "accurate:int:opt;",
                             "clip:vnode;",
                             TextSubCreate, const_cast<char*>("TextSubMod"), plugin);
}
