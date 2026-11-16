#ifndef NACRE_WINDOWS_BRANDING_H
#define NACRE_WINDOWS_BRANDING_H

/* Original shell mark, shared by the icon generator and the running Windows app.
 * Geometry matches src/macos/branding.h; GDI+ is y-down, so the Mac icon's
 * y-up coordinates are flipped here. */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
using std::max;
using std::min;
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace nacre {

inline Gdiplus::Color color(unsigned rgb, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, (BYTE)((rgb >> 16) & 255), (BYTE)((rgb >> 8) & 255),
                          (BYTE)(rgb & 255));
}

inline void round_rect_path(Gdiplus::GraphicsPath &path, float x, float y, float w, float h,
                            float radius) {
    float d = min(radius * 2.0f, min(w, h));
    if (d <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

inline void fill_round(Gdiplus::Graphics &g, float x, float y, float w, float h, float radius,
                       unsigned rgb) {
    Gdiplus::SolidBrush brush(color(rgb));
    if (radius <= 0.0f) {
        g.FillRectangle(&brush, Gdiplus::RectF(x, y, w, h));
        return;
    }
    Gdiplus::GraphicsPath path;
    round_rect_path(path, x, y, w, h, radius);
    g.FillPath(&brush, &path);
}

/* The app icon on a 512-unit canvas, scaled to `size` device pixels. The Mac artwork
 * leaves a 28-unit margin for its Dock conventions; Windows taskbar and Explorer
 * icons run nearly edge to edge, so the same art is enlarged about its center to
 * span 8..504 and look as big as its neighbors. */
inline void draw_icon(Gdiplus::Graphics &g, float size) {
    Gdiplus::GraphicsState state = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.ScaleTransform(size / 512.0f, size / 512.0f);
    const float fill = 496.0f / 456.0f;
    g.TranslateTransform(256.0f, 256.0f);
    g.ScaleTransform(fill, fill);
    g.TranslateTransform(-256.0f, -256.0f);
    fill_round(g, 28, 28, 456, 456, 100, 0x243E32);
    Gdiplus::Pen pen(color(0xCDDDC7), 10.0f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    for (int i = 0; i < 5; i++) {
        g.DrawBezier(&pen, 142.0f + i * 31, 366.0f, 90.0f + i * 43, 132.0f, 377.0f,
                     105.0f + i * 26, 358.0f, 182.0f + i * 29);
    }
    g.Restore(state);
}

/* The small sidebar mark, in sidebar coordinates. */
inline void draw_shell_mark(Gdiplus::Graphics &g) {
    Gdiplus::Pen pen(color(0x587668), 1.5f);
    for (int i = 0; i < 4; i++) {
        g.DrawBezier(&pen, 26.0f + i * 4, 29.0f, 58.0f, 22.0f + i * 4, 65.0f - i * 3, 48.0f,
                     51.0f, 54.0f - i * 4);
    }
}

} // namespace nacre

#endif
