/*
 *  VSFilterMod cross-platform port -- umbrella header.
 *
 *  Replaces the old per-project stdafx.h, which pulled in MFC (afxwin.h) and
 *  the DirectShow base classes (streams.h). Nothing here needs either.
 */

#pragma once

#include "platform.h"
#include "com_compat.h"
#include "cstring_compat.h"
#include "tchar_compat.h"
#include "atltypes_compat.h"
#include "atlcoll_compat.h"
#include "sync_compat.h"
#include "text_compat.h"
#include "subpic_desc.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
