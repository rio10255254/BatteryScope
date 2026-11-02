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

namespace {
void rectFill(HDC dc, const RECT& r, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    SetDCBrushColor(dc, color); FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
int fitSteps(const RECT& r, int unit, int steps) {
    const int room = int(std::min(r.right - r.left, r.bottom - r.top)) / (2 * std::max(1, unit));
    return std::clamp(steps, 0, std::max(0, room - 1));
}
// 5x7 glyphs; each row string uses '#' for a lit cell.
struct DigitGlyph { wchar_t c; int width; const char* rows[7]; };
const DigitGlyph digitGlyphs[] = {
    {L'0', 5, {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {L'1', 5, {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {L'2', 5, {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {L'3', 5, {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {L'4', 5, {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {L'5', 5, {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {L'6', 5, {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {L'7', 5, {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {L'8', 5, {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {L'9', 5, {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {L'W', 5, {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {L'%', 5, {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
    {L'.', 2, {"..", "..", "..", "..", "..", "##", "##"}},
    {L'-', 4, {"....", "....", "....", "####", "....", "....", "...."}},
    {L'\u2014', 5, {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {L' ', 2, {"..", "..", "..", "..", "..", "..", ".."}},
};
const DigitGlyph* digitGlyph(wchar_t c) {
    for (const auto& glyph : digitGlyphs) if (glyph.c == c) return &glyph;
    return nullptr;
}
HANDLE pixelFontHandle = nullptr;
std::vector<bool> pixelCoverage;
}
void pixelFill(HDC dc, const RECT& r, COLORREF color, int unit, int steps) {
    unit = std::max(1, unit); steps = fitSteps(r, unit, steps);
    for (int i = 0; i < steps; ++i) {
        const int inset = (steps - i) * unit;
        rectFill(dc, {r.left + inset, r.top + i * unit, r.right - inset, r.top + (i + 1) * unit}, color);
        rectFill(dc, {r.left + inset, r.bottom - (i + 1) * unit, r.right - inset, r.bottom - i * unit}, color);
    }
    rectFill(dc, {r.left, r.top + steps * unit, r.right, r.bottom - steps * unit}, color);
}
void pixelFrame(HDC dc, const RECT& r, COLORREF color, int unit, int steps) {
    unit = std::max(1, unit); steps = fitSteps(r, unit, steps);
    const int s = steps * unit;
    rectFill(dc, {r.left + s, r.top, r.right - s, r.top + unit}, color);
    rectFill(dc, {r.left + s, r.bottom - unit, r.right - s, r.bottom}, color);
    rectFill(dc, {r.left, r.top + s, r.left + unit, r.bottom - s}, color);
    rectFill(dc, {r.right - unit, r.top + s, r.right, r.bottom - s}, color);
    for (int i = 1; i < steps; ++i) { // stair pixels between the edges
        const int x = (steps - i) * unit, y = i * unit;
        rectFill(dc, {r.left + x, r.top + y, r.left + x + unit, r.top + y + unit}, color);
        rectFill(dc, {r.right - x - unit, r.top + y, r.right - x, r.top + y + unit}, color);
        rectFill(dc, {r.left + x, r.bottom - y - unit, r.left + x + unit, r.bottom - y}, color);
        rectFill(dc, {r.right - x - unit, r.bottom - y - unit, r.right - x, r.bottom - y}, color);
    }
}
void pixelBox(HDC dc, const RECT& r, COLORREF fill, COLORREF outline, int unit, int steps, COLORREF shadow) {
    RECT body = r;
    if (shadow != CLR_INVALID) {
        body.bottom -= unit;
        RECT drop = body; OffsetRect(&drop, 0, unit);
        pixelFill(dc, drop, shadow, unit, steps);
    }
    pixelFill(dc, body, fill, unit, steps);
    if (outline != CLR_INVALID) pixelFrame(dc, body, outline, unit, steps);
}
void pixelSegments(HDC dc, const RECT& r, double fraction, COLORREF on, COLORREF off, int unit) {
    unit = std::max(1, unit);
    const int block = unit * 2, pitch = block + unit;
    const int count = std::max(1, int(r.right - r.left + unit) / pitch);
    const int lit = fraction > 0 ? std::max(1, int(std::lround(std::clamp(fraction, 0.0, 1.0) * count))) : 0;
    for (int i = 0; i < count; ++i)
        rectFill(dc, {r.left + i * pitch, r.top, r.left + i * pitch + block, r.bottom}, i < lit ? on : off);
}
void pixelBattery(HDC dc, const RECT& r, double fraction, COLORREF outline, COLORREF level, COLORREF empty,
    bool bolt, COLORREF boltColor, COLORREF boltEdge, int unit, int cells) {
    unit = std::max(1, unit);
    RECT body{r.left, r.top, r.right - 2 * unit, r.bottom};
    rectFill(dc, {body.right, r.top + (r.bottom - r.top) / 3, r.right, r.bottom - (r.bottom - r.top) / 3}, outline);
    pixelFrame(dc, body, outline, unit, 1);
    RECT inner{body.left + 2 * unit, body.top + 2 * unit, body.right - 2 * unit, body.bottom - 2 * unit};
    const int width = int(inner.right - inner.left);
    cells = std::max(1, cells);
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const int lit = clamped > 0 ? std::max(1, int(std::ceil(clamped * cells - .001))) : 0;
    for (int i = 0; i < cells; ++i) {
        const int left = inner.left + width * i / cells, right = inner.left + width * (i + 1) / cells - (i + 1 < cells ? unit : 0);
        rectFill(dc, {left, inner.top, right, inner.bottom}, i < lit ? level : empty);
    }
    if (!bolt) return;
    static const char* shape[7] = {"...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..."};
    const int cell = std::max(1, std::min(unit, int(body.bottom - body.top - 2 * unit) / 7));
    const int x0 = (body.left + body.right) / 2 - cell * 5 / 2, y0 = (body.top + body.bottom) / 2 - cell * 7 / 2;
    for (int pass = 0; pass < 2; ++pass)
        for (int y = 0; y < 7; ++y) for (int x = 0; x < 5; ++x) if (shape[y][x] == '#') {
            RECT dot{x0 + x * cell, y0 + y * cell, x0 + (x + 1) * cell, y0 + (y + 1) * cell};
            if (pass == 0) InflateRect(&dot, cell, cell);
            rectFill(dc, dot, pass == 0 ? boltEdge : boltColor);
        }
}
void pixelChevron(HDC dc, const RECT& r, COLORREF c, int unit) {
    static const char* shape[3] = {"#...#", ".#.#.", "..#.."};
    const int x0 = (r.left + r.right) / 2 - unit * 5 / 2, y0 = (r.top + r.bottom) / 2 - unit * 3 / 2;
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) if (shape[y][x] == '#')
        rectFill(dc, {x0 + x * unit, y0 + y * unit, x0 + (x + 1) * unit, y0 + (y + 1) * unit}, c);
}
void pixelDots(HDC dc, const RECT& r, COLORREF c, int unit) {
    const int x0 = (r.left + r.right) / 2 - unit * 7 / 2, y0 = (r.top + r.bottom) / 2 - unit / 2;
    for (int i = 0; i < 3; ++i) rectFill(dc, {x0 + i * 3 * unit, y0, x0 + i * 3 * unit + unit, y0 + unit}, c);
}
void pixelDottedLine(HDC dc, POINT a, POINT b, COLORREF c, int unit) {
    unit = std::max(1, unit);
    if (a.y == b.y) { for (int x = std::min(a.x, b.x); x < std::max(a.x, b.x); x += unit * 3) rectFill(dc, {x, a.y, x + unit, a.y + unit}, c); }
    else { for (int y = std::min(a.y, b.y); y < std::max(a.y, b.y); y += unit * 3) rectFill(dc, {a.x, y, a.x + unit, y + unit}, c); }
}
int pixelDigitsWidth(const std::wstring& text, int unit) {
    int width = 0;
    for (wchar_t c : text) {
        const auto* glyph = digitGlyph(c);
        if (!glyph) return 0;
        width += (glyph->width + 1) * unit;
    }
    return width > 0 ? width - unit : 0;
}
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color) {
    if (text.empty() || !pixelDigitsWidth(text, unit)) return false;
    for (wchar_t c : text) {
        const auto* glyph = digitGlyph(c);
        for (int row = 0; row < 7; ++row) for (int column = 0; column < glyph->width; ++column)
            if (glyph->rows[row][column] == '#')
                rectFill(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit, y + (row + 1) * unit}, color);
        x += (glyph->width + 1) * unit;
    }
    return true;
}
HICON makePercentIcon(int size, int percent, COLORREF badge, COLORREF edge, COLORREF digits) {
    static const char* glyphs[10][5] = {
        {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"}, {"###", "..#", "###", "#..", "###"},
        {"###", "..#", ".##", "..#", "###"}, {"#.#", "#.#", "###", "..#", "..#"}, {"###", "#..", "###", "..#", "###"},
        {"###", "#..", "###", "#.#", "###"}, {"###", "..#", ".#.", ".#.", ".#."}, {"###", "#.#", "###", "#.#", "###"},
        {"###", "#.#", "###", "..#", "###"}};
    if (size < 8) return nullptr;
    const std::wstring text = std::to_wstring(std::clamp(percent, 0, 100));
    const int n = int(text.size());
    // Largest whole pixel size that fits the digits inside a one-pixel margin.
    const int unit = std::max(1, std::min((size - 2) / (4 * n - 1), (size - 4) / 5));
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) { if (color) DeleteObject(color); return nullptr; }
    auto* pixels = static_cast<DWORD*>(bits);
    auto argb = [](COLORREF c) { return 0xFF000000u | (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c); };
    auto paint = [&](int left, int top, int right, int bottom, DWORD value) {
        for (int y = std::max(0, top); y < std::min(size, bottom); ++y)
            for (int x = std::max(0, left); x < std::min(size, right); ++x) pixels[y * size + x] = value;
    };
    std::fill(pixels, pixels + size * size, 0u); // transparent
    const int corner = std::max(1, size / 16);
    paint(corner, 0, size - corner, size, argb(edge)); paint(0, corner, size, size - corner, argb(edge));
    paint(corner, corner, size - corner, size - corner, argb(badge));
    const int width = (4 * n - 1) * unit, x0 = (size - width) / 2, y0 = (size - 5 * unit + 1) / 2;
    for (int i = 0; i < n; ++i) {
        const auto& glyph = glyphs[text[size_t(i)] - L'0'];
        for (int row = 0; row < 5; ++row) for (int column = 0; column < 3; ++column) if (glyph[row][column] == '#') {
            const int x = x0 + (i * 4 + column) * unit, y = y0 + row * unit;
            paint(x, y, x + unit, y + unit, argb(digits));
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    std::vector<BYTE> zeros(size_t((size + 15) / 16 * 2 * size), 0);
    if (mask) SetBitmapBits(mask, DWORD(zeros.size()), zeros.data());
    ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = color; icon.hbmMask = mask;
    HICON result = mask ? CreateIconIndirect(&icon) : nullptr;
    DeleteObject(color); if (mask) DeleteObject(mask);
    return result;
}
void drawMascot(HDC dc, int x, int y, int unit, Mood mood, double level, int frame, const MascotColors& c) {
    // Cells: o outline, b body (filled to the battery level), e face, c cheek,
    // s sweat, a accent (Zz or bolt). Columns 12-15 hold effects beside the body.
    std::string grid[MascotRows] = {
        "....oooo........", "..oooooooo......", ".obbbbbbbbo.....", ".obbbbbbbbo.....", ".obbbbbbbbo.....",
        ".obbbbbbbbo.....", ".obbbbbbbbo.....", ".obbbbbbbbo.....", ".obbbbbbbbo.....", ".obbbbbbbbo.....",
        ".obbbbbbbbo.....", ".obbbbbbbbo.....", "..oooooooo......", "...oo..oo......."};
    auto put = [&](int row, int column, char cell) { if (row >= 0 && row < MascotRows && column >= 0 && column < MascotCells) grid[row][size_t(column)] = cell; };
    const bool blink = frame == 1;
    // Eyes at columns 3-4 and 6-7, rows 5-6; mouth on row 8.
    auto eyes = [&](const char* top, const char* bottom) {
        for (int i = 0; i < 6; ++i) { if (top[i] != '.') put(5, 3 + i, top[i]); if (bottom[i] != '.') put(6, 3 + i, bottom[i]); }
    };
    auto mouth = [&](const char* row8, const char* row9) {
        for (int i = 0; i < 6; ++i) { if (row8[i] != '.') put(8, 3 + i, row8[i]); if (row9 && row9[i] != '.') put(9, 3 + i, row9[i]); }
    };
    switch (mood) {
    case Mood::sleepy:
        for (int column : {3, 4, 6, 7}) put(7, column, 'e'); // closed eyes, one row low
        put(9, 5, 'e');
        put(1, 12, 'a'); put(1, 13, 'a'); put(1, 14, 'a'); put(2, 13, 'a'); put(3, 12, 'a'); put(3, 13, 'a'); put(3, 14, 'a');
        put(4, 11, 'a'); put(4, 12, 'a'); put(5, 11, 'a'); put(6, 11, 'a'); put(6, 12, 'a');
        break;
    case Mood::busy:
        eyes(blink ? "......" : "e..e..", blink ? "ee.ee." : "e..e..");
        mouth(".ee...", ".ee..."); put(8, 2, 'c'); put(8, 7, 'c');
        put(3, 12, 's'); put(4, 11, 's'); put(4, 12, 's'); put(4, 13, 's'); put(5, 11, 's'); put(5, 12, 's'); put(5, 13, 's'); put(6, 12, 's');
        break;
    case Mood::charging:
        eyes(".e..e.", "e.ee.e"); mouth("e....e", ".eeee."); put(8, 2, 'c'); put(8, 9, 'c');
        put(2, 14, 'a'); put(3, 13, 'a'); put(4, 12, 'a'); put(4, 13, 'a'); put(4, 14, 'a'); put(5, 13, 'a'); put(6, 12, 'a');
        break;
    case Mood::tired:
        eyes("ee.ee.", blink ? "ee.ee." : "......"); mouth(".eeee.", nullptr);
        break;
    case Mood::critical: // two 3x3 crosses for eyes
        for (int column : {2, 6}) {
            put(4, column, 'e'); put(4, column + 2, 'e'); put(5, column + 1, 'e'); put(6, column, 'e'); put(6, column + 2, 'e');
        }
        mouth(".eeee.", "e....e"); put(3, 12, 's'); put(4, 12, 's');
        break;
    case Mood::full:
        eyes("eeeeee", "ee.ee."); put(5, 2, 'e'); put(5, 9, 'e'); mouth("e....e", ".eeee.");
        put(1, 12, 'a'); put(0, 13, 'a'); put(1, 14, 'a'); put(2, 13, 'a');
        break;
    case Mood::calm:
    default:
        eyes(blink ? "......" : "e..e..", blink ? "ee.ee." : "e..e.."); mouth("e...e.", ".eee.."); put(8, 2, 'c'); put(8, 8, 'c');
        break;
    }
    // Body rows 2-11 (ten rows) fill from the bottom with the battery level.
    const int filled = std::clamp(int(std::lround(std::clamp(level, 0.0, 1.0) * 10)), level > 0 ? 1 : 0, 10);
    for (int row = 0; row < MascotRows; ++row) for (int column = 0; column < MascotCells; ++column) {
        const char cell = grid[row][size_t(column)];
        COLORREF color;
        switch (cell) {
        case 'o': color = c.outline; break;
        case 'b': color = row >= 12 - filled ? c.level : c.empty; break;
        case 'e': color = row >= 12 - filled ? c.face : c.faceLight; break;
        case 'c': color = c.cheek; break;
        case 's': color = c.sweat; break;
        case 'a': color = c.accent; break;
        default: continue;
        }
        rectFill(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit, y + (row + 1) * unit}, color);
    }
}
bool saveBitmapPng(HBITMAP bitmap, const std::wstring& path) {
    if (!graphicsToken || !bitmap) return false;
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return false;
    std::vector<BYTE> buffer(bytes);
    auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (CompareStringOrdinal(encoders[i].MimeType, -1, L"image/png", -1, TRUE) != CSTR_EQUAL) continue;
        Gdiplus::Bitmap image(bitmap, nullptr);
        return image.Save(path.c_str(), &encoders[i].Clsid, nullptr) == Gdiplus::Ok;
    }
    return false;
}
bool loadPixelFont(HMODULE module, int resource) {
    if (pixelFontHandle) return true;
    HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL data = info ? LoadResource(module, info) : nullptr;
    void* bytes = data ? LockResource(data) : nullptr;
    const DWORD size = info ? SizeofResource(module, info) : 0;
    DWORD installed = 0;
    if (!bytes || !size || !(pixelFontHandle = AddFontMemResourceEx(bytes, size, nullptr, &installed)) || !installed) {
        pixelFontHandle = nullptr; return false;
    }
    // Confirm GDI resolves the family to this font, then record which characters it has.
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, PixelFontFace);
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t face[LF_FACESIZE]{}; GetTextFaceW(dc, LF_FACESIZE, face);
    bool ok = CompareStringOrdinal(face, -1, PixelFontFace, -1, TRUE) == CSTR_EQUAL;
    const DWORD rangeBytes = ok ? GetFontUnicodeRanges(dc, nullptr) : 0;
    if (rangeBytes) {
        std::vector<BYTE> buffer(rangeBytes);
        auto set = reinterpret_cast<GLYPHSET*>(buffer.data());
        if (GetFontUnicodeRanges(dc, set)) {
            pixelCoverage.assign(65536, false);
            for (DWORD i = 0; i < set->cRanges; ++i)
                for (unsigned c = set->ranges[i].wcLow; c < unsigned(set->ranges[i].wcLow) + set->ranges[i].cGlyphs && c < 65536; ++c)
                    pixelCoverage[c] = true;
        } else ok = false;
    } else ok = false;
    SelectObject(dc, old); DeleteObject(font); DeleteDC(dc);
    if (!ok) unloadPixelFont();
    return ok;
}
void unloadPixelFont() {
    if (pixelFontHandle) RemoveFontMemResourceEx(pixelFontHandle);
    pixelFontHandle = nullptr; pixelCoverage.clear();
}
bool pixelFontLoaded() { return pixelFontHandle != nullptr; }
bool pixelCovers(const std::wstring& text) {
    if (pixelCoverage.empty()) return false;
    for (wchar_t c : text) if (c != L'\r' && c != L'\n' && !pixelCoverage[size_t(c)]) return false;
    return true;
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
