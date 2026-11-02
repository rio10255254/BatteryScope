#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
// gdiplus.h expects min/max macros, which NOMINMAX removes.
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>

namespace ui {
namespace {
ULONG_PTR graphicsToken = 0;
Gdiplus::Color color(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}
Gdiplus::RectF rectF(const RECT& r) {
    return Gdiplus::RectF(float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top));
}
void roundPath(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    radius = std::min({radius, r.Width / 2, r.Height / 2});
    if (radius <= .5f) { path.AddRectangle(r); return; }
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}
struct Canvas {
    Gdiplus::Graphics g;
    explicit Canvas(HDC dc) : g(dc) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
};
void roundCaps(Gdiplus::Pen& pen) {
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
}
// GDI fallbacks when GDI+ is unavailable.
void gdiRound(HDC dc, const RECT& r, COLORREF fill, COLORREF* outline, float radius) {
    HBRUSH brush = outline ? static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(fill);
    HPEN pen = outline ? CreatePen(PS_SOLID, 1, *outline) : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    const int d = int(radius * 2);
    RoundRect(dc, r.left, r.top, r.right + (outline ? 0 : 1), r.bottom + (outline ? 0 : 1), d, d);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    if (outline) DeleteObject(pen); else DeleteObject(brush);
}
}

bool systemPrefersLight() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
}
bool highContrastOn() {
    HIGHCONTRASTW contrast{}; contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}
COLORREF mix(COLORREF a, COLORREF b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto channel = [t](int x, int y) { return int(std::lround(x + (y - x) * t)); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}
Palette palette(ThemeChoice choice, bool forceContrast) {
    Palette p;
    if (forceContrast || highContrastOn()) {
        // Contrast themes: all text and strokes in WindowText, emphasis through the
        // Highlight/HighlightText pair, disabled text in GrayText.
        p.background = p.surface = p.accentSoft = GetSysColor(COLOR_WINDOW);
        p.raised = GetSysColor(COLOR_BTNFACE);
        p.border = p.ink = p.muted = p.accent = p.warn = p.danger = GetSysColor(COLOR_WINDOWTEXT);
        p.faint = GetSysColor(COLOR_GRAYTEXT);
        p.accentDim = p.selected = GetSysColor(COLOR_HIGHLIGHT);
        p.accentInk = p.selectedInk = GetSysColor(COLOR_HIGHLIGHTTEXT);
        const COLORREF bg = p.background;
        p.dark = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000 < 128;
        p.highContrast = true;
        return p;
    }
    const bool light = choice == ThemeChoice::light || (choice == ThemeChoice::system && systemPrefersLight());
    if (light) {
        p.background = RGB(242, 245, 248); p.surface = RGB(255, 255, 255); p.raised = RGB(235, 240, 244); p.border = RGB(214, 222, 230);
        p.ink = RGB(16, 24, 32); p.muted = RGB(84, 99, 114); p.faint = RGB(124, 136, 148);
        p.accent = RGB(4, 128, 92); p.accentDim = RGB(146, 210, 188); p.accentSoft = RGB(221, 242, 234); p.accentInk = RGB(255, 255, 255);
        p.warn = RGB(168, 104, 0); p.danger = RGB(190, 52, 40);
        p.selected = RGB(221, 242, 234); p.selectedInk = p.ink;
        p.dark = false;
    } else {
        p.background = RGB(14, 19, 26); p.surface = RGB(23, 31, 41); p.raised = RGB(31, 41, 53); p.border = RGB(43, 55, 68);
        p.ink = RGB(236, 242, 247); p.muted = RGB(150, 164, 178); p.faint = RGB(98, 112, 126);
        p.accent = RGB(104, 225, 188); p.accentDim = RGB(52, 116, 100); p.accentSoft = RGB(27, 56, 53); p.accentInk = RGB(8, 30, 24);
        p.warn = RGB(242, 196, 107); p.danger = RGB(245, 146, 146);
        p.selected = RGB(27, 56, 53); p.selectedInk = p.ink;
        p.dark = true;
    }
    return p;
}

bool startGraphics() {
    if (graphicsToken) return true;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr) != Gdiplus::Ok) graphicsToken = 0;
    return graphicsToken != 0;
}
void stopGraphics() {
    if (graphicsToken) Gdiplus::GdiplusShutdown(graphicsToken);
    graphicsToken = 0;
}
bool graphicsReady() { return graphicsToken != 0; }

void fillRound(HDC dc, const RECT& r, COLORREF c, float radius, BYTE alpha) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (!graphicsToken) { gdiRound(dc, r, c, nullptr, radius); return; }
    Canvas canvas(dc);
    Gdiplus::GraphicsPath path; roundPath(path, rectF(r), radius);
    Gdiplus::SolidBrush brush(color(c, alpha));
    canvas.g.FillPath(&brush, &path);
}
void strokeRound(HDC dc, const RECT& r, COLORREF c, float radius, float width) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (!graphicsToken) { gdiRound(dc, r, 0, &c, radius); return; }
    Canvas canvas(dc);
    auto box = rectF(r); box.Inflate(-width / 2, -width / 2);
    Gdiplus::GraphicsPath path; roundPath(path, box, std::max(0.0f, radius - width / 2));
    Gdiplus::Pen pen(color(c), width);
    canvas.g.DrawPath(&pen, &path);
}
void drawSeries(HDC dc, const RECT& plot, const std::vector<std::vector<POINT>>& segments, COLORREF line, float width, COLORREF fill) {
    if (!graphicsToken) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, int(width)), line); HGDIOBJ old = SelectObject(dc, pen);
        for (const auto& segment : segments) if (segment.size() > 1) Polyline(dc, segment.data(), int(segment.size()));
        SelectObject(dc, old); DeleteObject(pen); return;
    }
    Canvas canvas(dc);
    for (const auto& segment : segments) {
        if (segment.empty()) continue;
        std::vector<Gdiplus::PointF> points;
        points.reserve(segment.size());
        for (const auto& p : segment) points.emplace_back(float(p.x), float(p.y));
        if (points.size() == 1) {
            Gdiplus::SolidBrush dot(color(line));
            canvas.g.FillEllipse(&dot, points[0].X - width, points[0].Y - width, width * 2, width * 2);
            continue;
        }
        Gdiplus::GraphicsPath area;
        area.AddLines(points.data(), INT(points.size()));
        area.AddLine(points.back().X, points.back().Y, points.back().X, float(plot.bottom));
        area.AddLine(points.back().X, float(plot.bottom), points.front().X, float(plot.bottom));
        area.CloseFigure();
        Gdiplus::LinearGradientBrush gradient(Gdiplus::PointF(0, float(plot.top) - 1), Gdiplus::PointF(0, float(plot.bottom) + 1),
            color(fill, 96), color(fill, 0));
        canvas.g.FillPath(&gradient, &area);
        Gdiplus::Pen pen(color(line), width); roundCaps(pen);
        canvas.g.DrawLines(&pen, points.data(), INT(points.size()));
    }
}
void drawDashedLine(HDC dc, POINT a, POINT b, COLORREF c, float width) {
    if (!graphicsToken) {
        HPEN pen = CreatePen(PS_DOT, 1, c); HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, a.x, a.y, nullptr); LineTo(dc, b.x, b.y); SelectObject(dc, old); DeleteObject(pen); return;
    }
    Canvas canvas(dc);
    Gdiplus::Pen pen(color(c), width);
    pen.SetDashStyle(Gdiplus::DashStyleDash);
    canvas.g.DrawLine(&pen, float(a.x), float(a.y), float(b.x), float(b.y));
}
void drawBattery(HDC dc, const RECT& r, double fraction, COLORREF outline, COLORREF level, bool bolt, COLORREF boltColor, COLORREF boltEdge) {
    const int height = r.bottom - r.top, nub = std::max(2, height / 6);
    const float stroke = std::max(1.5f, height * .09f), radius = height * .26f;
    RECT body{r.left, r.top, r.right - nub - 1, r.bottom};
    RECT tip{body.right + 1, r.top + height * 3 / 10, r.right, r.bottom - height * 3 / 10};
    fillRound(dc, tip, outline, float(nub) / 2);
    strokeRound(dc, body, outline, radius, stroke);
    const int gap = int(std::ceil(stroke + height * .1f));
    RECT inner{body.left + gap, body.top + gap, body.right - gap, body.bottom - gap};
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    if (clamped > 0 && inner.right > inner.left) {
        RECT filled = inner;
        filled.right = inner.left + std::max(int(std::lround((inner.right - inner.left) * clamped)), int(radius / 2) + 1);
        fillRound(dc, filled, level, std::max(1.0f, radius - gap));
    }
    if (!bolt || !graphicsToken) return;
    Canvas canvas(dc);
    const float cx = (body.left + body.right) / 2.0f, cy = (body.top + body.bottom) / 2.0f, s = height * .42f;
    const Gdiplus::PointF shape[] = {
        {cx + s * .15f, cy - s}, {cx - s * .55f, cy + s * .12f}, {cx - s * .02f, cy + s * .12f},
        {cx - s * .15f, cy + s}, {cx + s * .55f, cy - s * .12f}, {cx + s * .02f, cy - s * .12f}};
    Gdiplus::GraphicsPath path; path.AddPolygon(shape, 6);
    Gdiplus::Pen edge(color(boltEdge), std::max(1.5f, height * .1f)); edge.SetLineJoin(Gdiplus::LineJoinRound);
    canvas.g.DrawPath(&edge, &path);
    Gdiplus::SolidBrush brush(color(boltColor));
    canvas.g.FillPath(&brush, &path);
}
void drawChevron(HDC dc, const RECT& r, COLORREF c, float width) {
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    const float s = std::min(r.right - r.left, r.bottom - r.top) * .16f;
    if (!graphicsToken) {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, int(width)), c); HGDIOBJ old = SelectObject(dc, pen);
        POINT points[] = {{int(cx - s * 1.6f), int(cy - s * .6f)}, {int(cx), int(cy + s * .8f)}, {int(cx + s * 1.6f), int(cy - s * .6f)}};
        Polyline(dc, points, 3); SelectObject(dc, old); DeleteObject(pen); return;
    }
    Canvas canvas(dc);
    Gdiplus::Pen pen(color(c), width); roundCaps(pen);
    const Gdiplus::PointF points[] = {{cx - s * 1.6f, cy - s * .6f}, {cx, cy + s * .8f}, {cx + s * 1.6f, cy - s * .6f}};
    canvas.g.DrawLines(&pen, points, 3);
}
void drawDots(HDC dc, const RECT& r, COLORREF c, float radius) {
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f, step = radius * 3.4f;
    for (int i = -1; i <= 1; ++i) {
        RECT dot{int(std::lround(cx + i * step - radius)), int(std::lround(cy - radius)),
            int(std::lround(cx + i * step + radius)), int(std::lround(cy + radius))};
        fillRound(dc, dot, c, radius);
    }
}

HICON IconCache::get(const std::wstring& path, int size) {
    if (path.empty() || size <= 0) return nullptr;
    if (size != size_) { clear(); size_ = size; }
    std::wstring key = path;
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    auto found = icons_.find(key);
    if (found != icons_.end()) return found->second;
    if (icons_.size() >= 512) clear();
    HICON icon = nullptr;
    if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size)) != S_OK) icon = nullptr;
    icons_[key] = icon;
    return icon;
}
void IconCache::clear() {
    for (auto& item : icons_) if (item.second) DestroyIcon(item.second);
    icons_.clear();
}
}
