/*
 *  VSFilterMod cross-platform port -- the only part of ISubPic.h the renderer
 *  ever touched.
 *
 *  Note that neither Rasterizer nor RTS reads SubPicDesc::type: the whole
 *  drawing path writes 32-bit BGRA. The enum is kept because the CSRI entry
 *  point and the BDN+XML writer still label their surfaces with it.
 */

#pragma once

#include "platform.h"
#include "atltypes_compat.h"

enum
{
    MSP_RGB32, MSP_RGB24, MSP_RGB16, MSP_RGB15,
    MSP_YUY2, MSP_YV12, MSP_IYUV, MSP_AYUV,
    MSP_RGBA, MSP_NV12, MSP_P010, MSP_P016
};

#pragma pack(push, 1)
struct SubPicDesc
{
    int type;
    int w, h, bpp, pitch, pitchUV;
    void* bits;
    BYTE* bitsU;
    BYTE* bitsV;
    RECT vidrect;   // video rectangle

    SubPicDesc()
        : type(0), w(0), h(0), bpp(0), pitch(0), pitchUV(0)
        , bits(nullptr), bitsU(nullptr), bitsV(nullptr)
    {
        vidrect.left = vidrect.top = vidrect.right = vidrect.bottom = 0;
    }
};
#pragma pack(pop)
