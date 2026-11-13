// SPDX-License-Identifier: GPL-3.0-or-later
// GDI+ for the graph strips' drawing code (graphstrip.cpp and the strips): the include with what it needs before
// it, and the two conversions the drawing uses everywhere. Only for .cpp files.
#pragma once

#include <windows.h>

#include <objidl.h>

#include <algorithm>

namespace Gdiplus { // gdiplus.h uses min / max unqualified
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace ap::graph {

inline Gdiplus::Color col(COLORREF c, int alpha = 255) {
    return Gdiplus::Color(static_cast<BYTE>(alpha), static_cast<BYTE>(c & 0xFF), static_cast<BYTE>((c >> 8) & 0xFF),
                          static_cast<BYTE>((c >> 16) & 0xFF));
}

inline Gdiplus::PointF pt(double x, double y) {
    return Gdiplus::PointF(static_cast<float>(x), static_cast<float>(y));
}

} // namespace ap::graph
