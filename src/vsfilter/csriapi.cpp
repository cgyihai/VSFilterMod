/*
 *	Copyright (C) 2007 Niels Martin Hansen
 *	http://aegisub.net/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "../compat/vsfm_compat.h"
#include "vsfm_version.h"
#include "../subtitles/RTS.h"

#include <limits>

/* Visibility of the csri_* entry points. On ELF and Mach-O the build hides
 * everything else, so the CSRI loader's dlsym() finds exactly the ABI
 * and none of VSFilter's internals leak into the host's symbol namespace --
 * which matters when the host is Aegisub, itself linked against libass. */
#if defined(_WIN32)
#define CSRIAPI extern "C" __declspec(dllexport)
#else
#define CSRIAPI extern "C" __attribute__((visibility("default")))
#endif
#define CSRI_OWN_HANDLES
typedef const char *csri_rend;
extern "C" struct csri_vsfilter_inst
{
    CRenderedTextSubtitle *rts;
    CCritSec *cs;
    CSize script_res;
    CSize screen_res;
    CRect video_rect;
    /* Holds a csri_pixfmt. Stored as int because this struct has to be
     * complete before csri.h is included (CSRI_OWN_HANDLES), and an
     * elaborated `enum csri_pixfmt` forward reference is an MSVC extension
     * that ISO C++ rejects. */
    int pixfmt;
    size_t readorder;
};
typedef struct csri_vsfilter_inst csri_inst;
#include "csri.h"
#include "csri/stream.h"
#ifdef _VSMOD
static csri_rend csri_vsfilter = "vsfiltermod";
#else
static csri_rend csri_vsfilter = "vsfilter";
#endif

/* No C++ exception may cross into the host: Aegisub and the frame servers
 * call these through a C ABI, and an exception unwinding out of them ends
 * the process. The parsers report malformed fields by throwing, and an
 * allocation can fail; either way the call fails as a bad script does --
 * no instance, or no subtitle drawn. */
template <typename F>
static void NoThrow(F f)
{
    try { f(); } catch(...) {}
}

static void DeleteInst(csri_inst *inst)
{
    if(!inst) return;
    delete inst->rts;
    delete inst->cs;
    delete inst;
}

static csri_inst *OpenFile(csri_rend *renderer, const char *filename, struct csri_openflag *flags)
{
    (void)renderer; (void)flags;
    if(!filename) return 0;

    /* CSRI specifies UTF-8 paths. */
    const CStringW path = UTF8To16(filename);
    if(path.IsEmpty()) return 0;

    csri_inst *inst = new csri_inst();
    inst->rts = 0;
    inst->cs = 0;
    bool ok = false;
    NoThrow([&] {
        inst->cs = new CCritSec();
        inst->rts = new CRenderedTextSubtitle(inst->cs);
        ok = inst->rts->Open(CString(path), DEFAULT_CHARSET);
    });
    if(ok)
    {
        inst->readorder = 0;
        return inst;
    }

    DeleteInst(inst);
    return 0;
}

CSRIAPI csri_inst *csri_open_file(csri_rend *renderer, const char *filename, struct csri_openflag *flags)
{
    csri_inst *inst = 0;
    NoThrow([&] { inst = OpenFile(renderer, filename, flags); });
    return inst;
}


static csri_inst *OpenMem(csri_rend *renderer, const void *data, size_t length, struct csri_openflag *flags)
{
    (void)renderer; (void)flags;
    /* Parses straight out of `data`; CSimpleTextSubtitle::Open no longer
     * spools it to a temp file first. This is the path Aegisub drives on
     * every edit. */
    csri_inst *inst = new csri_inst();
    inst->rts = 0;
    inst->cs = 0;
    bool ok = false;
    NoThrow([&] {
        inst->cs = new CCritSec();
        inst->rts = new CRenderedTextSubtitle(inst->cs);
        ok = inst->rts->Open((BYTE*)data, (int)length, DEFAULT_CHARSET, _T("CSRI memory subtitles"));
    });
    if(ok)
    {
        inst->readorder = 0;
        return inst;
    }

    DeleteInst(inst);
    return 0;
}

CSRIAPI csri_inst *csri_open_mem(csri_rend *renderer, const void *data, size_t length, struct csri_openflag *flags)
{
    csri_inst *inst = 0;
    NoThrow([&] { inst = OpenMem(renderer, data, length, flags); });
    return inst;
}


CSRIAPI void csri_close(csri_inst *inst)
{
    NoThrow([&] { DeleteInst(inst); });
}


CSRIAPI int csri_request_fmt(csri_inst *inst, const struct csri_fmt *fmt)
{
    if(!inst || !fmt) return -1;

    if(fmt->width == 0 || fmt->height == 0
       || fmt->width > (unsigned)(std::numeric_limits<int>::max)() / 4
       || fmt->height > (unsigned)(std::numeric_limits<int>::max)())
        return -1;

    // Check if pixel format is supported
    switch(fmt->pixfmt)
    {
    case CSRI_F_BGRA:
    case CSRI_F_BGR_:
    case CSRI_F_BGR:
    case CSRI_F_YUY2:
    case CSRI_F_YV12:
        inst->pixfmt = fmt->pixfmt;
        break;

    default:
        return -1;
    }
    inst->screen_res = CSize(fmt->width, fmt->height);
    inst->video_rect = CRect(0, 0, fmt->width, fmt->height);
    return 0;
}


static void Render(csri_inst *inst, struct csri_frame *frame, double time)
{
    if(!inst || !frame)
        return;

    CAutoLock renderLock(inst->cs);

    const double arbitrary_framerate = 25.0;
    SubPicDesc spd;
    spd.w = inst->screen_res.cx;
    spd.h = inst->screen_res.cy;
    switch(inst->pixfmt)
    {
    case CSRI_F_BGRA:
        if(!frame->planes[0])
            return;

        /* VSFilter's native surface is premultiplied BGR with inverted alpha
         * (255 is transparent). Render directly so CSRI composes in place
         * without converting through straight alpha. */
        spd.type = MSP_RGBA;
        spd.bpp = 32;
        spd.bits = frame->planes[0];
        spd.pitch = frame->strides[0];
        break;

    case CSRI_F_BGR_:
        spd.type = MSP_RGB32;
        spd.bpp = 32;
        spd.bits = frame->planes[0];
        spd.pitch = frame->strides[0];
        break;

    case CSRI_F_BGR:
        spd.type = MSP_RGB24;
        spd.bpp = 24;
        spd.bits = frame->planes[0];
        spd.pitch = frame->strides[0];
        break;

    case CSRI_F_YUY2:
        spd.type = MSP_YUY2;
        spd.bpp = 16;
        spd.bits = frame->planes[0];
        spd.pitch = frame->strides[0];
        break;

    case CSRI_F_YV12:
        spd.type = MSP_YV12;
        spd.bpp = 12;
        spd.bits = frame->planes[0];
        spd.bitsU = frame->planes[1];
        spd.bitsV = frame->planes[2];
        spd.pitch = frame->strides[0];
        spd.pitchUV = frame->strides[1];
        break;

    default:
        // eh?
        return;
    }
    spd.vidrect = inst->video_rect;

    CRect rendered_bbox(0, 0, 0, 0);
    inst->rts->Render(spd, (REFERENCE_TIME)(time * 10000000), arbitrary_framerate, rendered_bbox);
}

CSRIAPI void csri_render(csri_inst *inst, struct csri_frame *frame, double time)
{
    NoThrow([&] { Render(inst, frame, time); });
}


// Stream extension implementation
static csri_inst *vsfilter_init_stream(csri_rend *renderer, const void *header, size_t headerlen, struct csri_openflag *flags)
{
    return csri_open_mem(renderer, header, headerlen, flags);
}

static void PushPacket(csri_inst *inst, const void *packet, size_t packetlen, double pts_start, double pts_end)
{
    if (!inst || !inst->rts || !packet || packetlen == 0) return;

    CStringA strA((LPCSTR)packet, packetlen);
    CStringW str = UTF8To16(strA).Trim();
    
    if (str.IsEmpty()) return;

    STSEntry stse;
    int fields = (inst->rts->m_sver >= 6) ? 10 : 9;

    CAtlList<CStringW> sl;
    Explode(str, sl, ',', fields);
    if(sl.GetCount() == fields)
    {
        stse.readorder = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        stse.layer = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        stse.style = sl.RemoveHead();
        stse.actor = sl.RemoveHead();
        stse.marginRect.left = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        stse.marginRect.right = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        stse.marginRect.top = stse.marginRect.bottom = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        if(fields == 10) stse.marginRect.bottom = vsfm::WcsToL(sl.RemoveHead(), NULL, 10);
        stse.effect = sl.RemoveHead();
        stse.str = sl.RemoveHead();
    }

    if(!stse.str.IsEmpty())
    {
        inst->rts->Add(stse.str, true, (int)(pts_start * 1000), (int)(pts_end * 1000), 
            stse.style, stse.actor, stse.effect, stse.marginRect, stse.layer, stse.readorder);
    }
}

static void vsfilter_push_packet(csri_inst *inst, const void *packet, size_t packetlen, double pts_start, double pts_end)
{
    NoThrow([&] { PushPacket(inst, packet, packetlen, pts_start, pts_end); });
}

static void vsfilter_discard(csri_inst *inst, int all)
{
    NoThrow([&] { if (inst && inst->rts) {
        // VSFilter's RTS doesn't have an exact "discard" per-se for streaming,
        // but we can remove all entries if `all` is requested (similar to NewSegment).
        if (all) {
            inst->rts->RemoveAll();
            inst->rts->CreateSegments();
        }
    } });
}

static struct csri_stream_ext vsfilter_stream_ext = {
    vsfilter_init_stream,
    vsfilter_push_packet,
    vsfilter_discard
};

CSRIAPI void *csri_query_ext(csri_rend *rend, csri_ext_id extname)
{
    if (strcmp(extname, CSRI_EXT_STREAM_ASS) == 0) {
        return &vsfilter_stream_ext;
    }
    return 0;
}

// Get info for renderer
static struct csri_info csri_vsfilter_info =
{
#ifdef _DEBUG
#ifdef _VSMOD
    "vsfiltermod_textsub_debug", // name
#else
    "vsfilter_textsub_debug", // name
#endif
    "2.39", // version (assumed version number, svn revision, patchlevel)
#else
#ifdef _VSMOD
    "vsfiltermod_textsub", // name
#else
    "vsfilter_textsub", // name
#endif
    "2.39", // version (assumed version number, svn revision, patchlevel)
#endif
    // 2.38-0611 is base svn 611
    // 2.38-0611-1 is with clipfix and fax/fay patch
    // 2.38-0611-2 adds CSRI
    // 2.38-0611-3 fixes a bug in CSRI and adds fontcrash-fix and float-pos
    // 2.38-0611-4 fixes be1-dots and ugly-fade bugs and adds xbord/ybord/xshad/yshad/blur tags and extends be
    // 2.39 merges with guliverkli2 fork
#ifdef _VSMOD
    "VSFilterMod/TextSub " VSFM_VERSION " (cross-platform)", // longname
#else
    "VSFilter/TextSub (guliverkli2)", // longname
#endif
    "Gabest", // author
    "Copyright (c) 2003-2008 by Gabest and others" // copyright
};
CSRIAPI struct csri_info *csri_renderer_info(csri_rend *rend)
{
    return &csri_vsfilter_info;
}
// Only one supported, obviously
CSRIAPI csri_rend *csri_renderer_byname(const char *name, const char *specific)
{
    if(strcmp(name, csri_vsfilter_info.name))
        return 0;
    if(specific && strcmp(specific, csri_vsfilter_info.specific))
        return 0;
    return &csri_vsfilter;
}
// Still just one
CSRIAPI csri_rend *csri_renderer_default()
{
    return &csri_vsfilter;
}
// And no further
CSRIAPI csri_rend *csri_renderer_next(csri_rend *prev)
{
    return 0;
}

/* -----------------------------------------------------------------------
 * TypeSub editor integration (interface version 1).
 *
 * Self-description of the extension tags this renderer understands beyond
 * the core ASS override set. Editors (TypeSub) look this symbol up with
 * GetProcAddress/dlsym after loading the plugin and merge the table into
 * their autocompletion -- so adding a new tag here never requires an
 * editor release. The symbol is OPTIONAL: hosts must fall back to their
 * built-in table when it is absent, and renderers without extensions may
 * simply not export it.
 *
 * Returns a pointer to a static UTF-8 JSON document owned by the plugin,
 * valid for the plugin's lifetime:
 *   { "version": 1, "renderer": "<name>",
 *     "tags": [ { "name": "1img", "signature": "\1img(file[,x,y[,angle]])",
 *                 "help": "one-line description" }, ... ] }
 * --------------------------------------------------------------------- */
CSRIAPI const char *csri_typesub_tags_json(void)
{
    return R"json({
  "version": 1,
  "renderer": "vsfiltermod-cgyihai",
  "tags": [
    {
      "name": "1img",
      "signature": "\\1img(file[,x,y[,angle]])",
      "help": "主体填充位图（路径相对字幕文件）"
    },
    {
      "name": "2img",
      "signature": "\\2img(file[,x,y[,angle]])",
      "help": "边框填充位图"
    },
    {
      "name": "3img",
      "signature": "\\3img(file[,x,y[,angle]])",
      "help": "阴影填充位图"
    },
    {
      "name": "4img",
      "signature": "\\4img(file[,x,y[,angle]])",
      "help": "次要填充位图"
    },
    {
      "name": "1vc",
      "signature": "\\1vc(&Hc1&,&Hc2&,&Hc3&,&Hc4&)",
      "help": "主体四角渐变色"
    },
    {
      "name": "2vc",
      "signature": "\\2vc(&Hc1&,&Hc2&,&Hc3&,&Hc4&)",
      "help": "边框四角渐变色"
    },
    {
      "name": "3vc",
      "signature": "\\3vc(&Hc1&,&Hc2&,&Hc3&,&Hc4&)",
      "help": "阴影四角渐变色"
    },
    {
      "name": "4vc",
      "signature": "\\4vc(&Hc1&,&Hc2&,&Hc3&,&Hc4&)",
      "help": "次要四角渐变色"
    },
    {
      "name": "1va",
      "signature": "\\1va(&Ha1&,&Ha2&,&Ha3&,&Ha4&)",
      "help": "主体四角渐变透明度"
    },
    {
      "name": "2va",
      "signature": "\\2va(&Ha1&,&Ha2&,&Ha3&,&Ha4&)",
      "help": "边框四角渐变透明度"
    },
    {
      "name": "3va",
      "signature": "\\3va(&Ha1&,&Ha2&,&Ha3&,&Ha4&)",
      "help": "阴影四角渐变透明度"
    },
    {
      "name": "4va",
      "signature": "\\4va(&Ha1&,&Ha2&,&Ha3&,&Ha4&)",
      "help": "次要四角渐变透明度"
    },
    {
      "name": "glow",
      "signature": "\\glow(radius[,strength])",
      "help": "文字发光；\\glow20 是半径 20 的简写"
    },
    {
      "name": "rgb",
      "signature": "\\rgb(distance)",
      "help": "RGB 色差分离；\\rgb4 是偏移 4 像素的简写"
    },

    {
      "name": "glowc",
      "signature": "\\glowc&HBBGGRR&",
      "help": "发光颜色覆盖（默认跟随主要颜色）"
    },
    {
      "name": "glowa",
      "signature": "\\glowa&HAA&",
      "help": "发光透明度覆盖（00 不透明 FF 透明）"
    },
    {
      "name": "iglow",
      "signature": "\\iglow(radius[,strength])",
      "help": "内发光；\\iglow8 是半径 8 的简写；配套 \\iglowc/\\iglowa"
    },
    {
      "name": "mblur",
      "signature": "\\mblur(angle,length)",
      "help": "方向动态模糊；\\mblur45,10 沿 45 度拖尾 10 像素"
    },
    {
      "name": "bevel",
      "signature": "\\bevel(size[,strength])",
      "help": "斜面浮雕；\\bevel2 是尺寸 2 的简写"
    },
    {
      "name": "clipf",
      "signature": "\\clipf(radius)",
      "help": "羽化裁剪；\\clipf15 柔化矩形/矢量裁剪边缘"
    },
    {
      "name": "jitter",
      "signature": "\\jitter(left,right,up,down,period[,seed])",
      "help": "逐帧随机抖动"
    },
    {
      "name": "distort",
      "signature": "\\distort(x1,y1,x2,y2,x3,y3)",
      "help": "三点双线性扭曲变形"
    },
    {
      "name": "mover",
      "signature": "\\mover(x1,y1,x2,y2,alp1,alp2,r1,r2[,t1,t2])",
      "help": "径向位移（半径+透明度渐变）"
    },
    {
      "name": "moves3",
      "signature": "\\moves3(x1,y1,x2,y2,x3,y3[,t1,t2])",
      "help": "二次贝塞尔曲线移动"
    },
    {
      "name": "moves4",
      "signature": "\\moves4(x1,y1,x2,y2,x3,y3,x4,y4[,t1,t2])",
      "help": "三次贝塞尔曲线移动"
    },
    {
      "name": "movevc",
      "signature": "\\movevc(x1,y1[,x2,y2[,t1,t2]])",
      "help": "移动矢量裁剪"
    },
    {
      "name": "rnd",
      "signature": "\\rnd(val)",
      "help": "逐字随机偏移（xyz 同时）"
    },
    {
      "name": "rndx",
      "signature": "\\rndx(val)",
      "help": "逐字随机 X 偏移"
    },
    {
      "name": "rndy",
      "signature": "\\rndy(val)",
      "help": "逐字随机 Y 偏移"
    },
    {
      "name": "rndz",
      "signature": "\\rndz(val)",
      "help": "逐字随机 Z 偏移"
    },
    {
      "name": "z",
      "signature": "\\z(val)",
      "help": "Z 轴深度"
    },
    {
      "name": "frs",
      "signature": "\\frs(deg)",
      "help": "字符自身朝向旋转"
    },
    {
      "name": "fsc",
      "signature": "\\fsc(pct)",
      "help": "等比缩放（x/y 同时）"
    }
  ]
})json";
}

