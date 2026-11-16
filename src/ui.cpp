#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <tuple>
#include <utility>
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
        p.keyTop = p.keyBottom = p.raised;
        p.litTop = p.litMid = p.litBottom = p.glow = p.halo = p.selected;
        p.rim = p.border; p.well = p.background; p.ledOff = p.faint; p.shadow = p.border;
        const COLORREF bg = p.background;
        p.dark = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000 < 128;
        p.highContrast = true;
        return p;
    }
    const bool light = choice == ThemeChoice::light || (choice == ThemeChoice::system && systemPrefersLight());
    if (light) {
        // Day: a cool, neutral panel (so the amber light reads as light, not as the panel's own
        // colour), keys nearly white, wells a shade darker, text dark enough to read at a glance.
        p.background = p.surface = RGB(236, 238, 242); p.raised = RGB(246, 247, 249); p.border = RGB(204, 207, 214);
        p.ink = RGB(24, 26, 31); p.muted = RGB(66, 70, 79); p.faint = RGB(124, 128, 138);
        p.accent = RGB(150, 82, 0); p.accentDim = RGB(222, 184, 120); p.accentSoft = RGB(250, 228, 186); p.accentInk = RGB(30, 26, 20);
        p.warn = RGB(166, 90, 0); p.danger = RGB(190, 48, 40);
        p.selected = RGB(250, 228, 186); p.selectedInk = p.ink;
        p.keyTop = RGB(253, 253, 254); p.keyBottom = RGB(232, 234, 239);
        p.litTop = RGB(250, 212, 142); p.litMid = RGB(251, 236, 210); p.litBottom = RGB(249, 244, 234);
        p.rim = RGB(238, 162, 52); p.well = RGB(219, 222, 228); p.glow = RGB(244, 186, 86); p.halo = RGB(246, 170, 50);
        p.ledOff = RGB(198, 202, 210); p.shadow = RGB(52, 60, 78);
        p.dark = false;
    } else {
        // Night: a charcoal panel, smoked keys, deep wells, an ice-blue neon light.
        p.background = p.surface = RGB(27, 27, 29); p.raised = RGB(46, 47, 53); p.border = RGB(52, 53, 59);
        p.ink = RGB(240, 240, 243); p.muted = RGB(182, 183, 192); p.faint = RGB(120, 121, 130);
        p.accent = RGB(180, 195, 255); p.accentDim = RGB(74, 85, 144); p.accentSoft = RGB(37, 43, 69); p.accentInk = RGB(242, 246, 255);
        p.warn = RGB(242, 169, 59); p.danger = RGB(255, 107, 99);
        p.selected = RGB(37, 43, 69); p.selectedInk = p.ink;
        p.keyTop = RGB(49, 50, 56); p.keyBottom = RGB(37, 38, 43);
        p.litTop = RGB(92, 96, 122); p.litMid = RGB(60, 62, 78); p.litBottom = RGB(46, 47, 58); // frosted glass over the dark: lighter and a little blue
        p.rim = RGB(220, 246, 255); p.well = RGB(14, 14, 16); p.glow = RGB(221, 246, 255); p.halo = RGB(125, 145, 255);
        p.ledOff = RGB(36, 37, 43); p.shadow = RGB(0, 0, 0);
        p.dark = true;
    }
    return p;
}

Palette mixPalette(const Palette& a, const Palette& b, double t) {
    Palette p = t < .5 ? a : b;
    auto m = [t](COLORREF x, COLORREF y) { return mix(x, y, t); };
    p.background = m(a.background, b.background); p.surface = m(a.surface, b.surface); p.raised = m(a.raised, b.raised); p.border = m(a.border, b.border);
    p.ink = m(a.ink, b.ink); p.muted = m(a.muted, b.muted); p.faint = m(a.faint, b.faint);
    p.accent = m(a.accent, b.accent); p.accentDim = m(a.accentDim, b.accentDim); p.accentSoft = m(a.accentSoft, b.accentSoft); p.accentInk = m(a.accentInk, b.accentInk);
    p.warn = m(a.warn, b.warn); p.danger = m(a.danger, b.danger); p.selected = m(a.selected, b.selected); p.selectedInk = m(a.selectedInk, b.selectedInk);
    p.keyTop = m(a.keyTop, b.keyTop); p.keyBottom = m(a.keyBottom, b.keyBottom); p.litTop = m(a.litTop, b.litTop); p.litMid = m(a.litMid, b.litMid);
    p.litBottom = m(a.litBottom, b.litBottom); p.rim = m(a.rim, b.rim); p.well = m(a.well, b.well); p.glow = m(a.glow, b.glow);
    p.halo = m(a.halo, b.halo); p.ledOff = m(a.ledOff, b.ledOff); p.shadow = m(a.shadow, b.shadow);
    return p;
}
void drawRimLight(HDC dc, const RECT& r, float radius, COLORREF c, double angle, double strength) {
    if (!graphicsToken || strength <= .01 || r.right <= r.left) return;
    Canvas canvas(dc);
    auto& g = canvas.g;
    const auto box = rectF(r);
    const float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2, reach = std::max(box.Width, box.Height) / 2;
    const float dx = float(std::cos(angle)) * reach, dy = float(std::sin(angle)) * reach;
    const BYTE alpha = BYTE(std::clamp(strength, 0.0, 1.0) * 255);
    Gdiplus::LinearGradientBrush rim(Gdiplus::PointF(cx - dx, cy - dy), Gdiplus::PointF(cx + dx + .01f, cy + dy + .01f), color(c, 0), color(c, alpha));
    Gdiplus::GraphicsPath path; roundPath(path, box, radius);
    // A wide soft band inside the edge, then a thin bright line on it.
    Gdiplus::GraphicsState state = g.Save();
    g.SetClip(&path);
    Gdiplus::Pen bloom(&rim, std::max(3.0f, box.Height * .18f)); bloom.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&bloom, &path);
    g.Restore(state);
    Gdiplus::Pen line(&rim, std::max(1.4f, box.Height * .035f)); line.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&line, &path);
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

namespace {
void rectFill(HDC dc, const RECT& r, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    SetDCBrushColor(dc, color); FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
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
int pixelDigitsWidth(const std::wstring& text, int unit) {
    int width = 0;
    for (wchar_t c : text) {
        const auto* glyph = digitGlyph(c);
        if (!glyph) return 0;
        width += (glyph->width + 1) * unit;
    }
    return width > 0 ? width - unit : 0;
}
bool pixelDigitRows(const std::wstring& text, std::string rows[7]) {
    if (text.empty() || !pixelDigitsWidth(text, 1)) return false;
    for (int row = 0; row < 7; ++row) rows[row].clear();
    for (size_t i = 0; i < text.size(); ++i) {
        const auto* glyph = digitGlyph(text[i]);
        for (int row = 0; row < 7; ++row) { rows[row].append(glyph->rows[row], size_t(glyph->width)); if (i + 1 < text.size()) rows[row] += ' '; } // ' ': between glyphs
    }
    return true;
}
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color, int gap, COLORREF ghost) {
    if (text.empty() || !pixelDigitsWidth(text, unit)) return false;
    gap = std::clamp(gap, 0, unit - 1);
    for (wchar_t c : text) {
        const auto* glyph = digitGlyph(c);
        for (int row = 0; row < 7; ++row) for (int column = 0; column < glyph->width; ++column) {
            const bool on = glyph->rows[row][column] == '#';
            if (on || ghost != CLR_INVALID)
                rectFill(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit - gap, y + (row + 1) * unit - gap}, on ? color : ghost);
        }
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
HICON makeLevelIcon(int size, double fraction, COLORREF badge, COLORREF edge, COLORREF fill, int steps) {
    if (size < 8) return nullptr;
    fraction = std::clamp(fraction, 0.0, 1.0);
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
    const int corner = std::max(1, size / 16), pad = std::max(2, size / 6);
    paint(corner, 0, size - corner, size, argb(edge)); paint(0, corner, size, size - corner, argb(edge));
    paint(corner, corner, size - corner, size - corner, argb(badge));
    const int inner = size - pad * 2;
    const COLORREF dim = mix(badge, fill, .22);
    if (steps > 0) {
        const int gap = std::max(1, size / 16), bar = std::max(1, (inner - gap * (steps - 1)) / steps);
        const int lit = int(std::lround(fraction * steps));
        for (int k = 0; k < steps; ++k) {
            const int height = std::max(2, inner * (k + 1) / steps), x = pad + k * (bar + gap);
            paint(x, size - pad - height, x + bar, size - pad, argb(k < lit ? fill : dim));
        }
    } else {
        paint(pad, pad, size - pad, size - pad, argb(dim));
        paint(pad, size - pad - int(std::lround(inner * fraction)), size - pad, size - pad, argb(fill));
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    std::vector<BYTE> zeros(size_t((size + 15) / 16 * 2 * size), 0);
    if (mask) SetBitmapBits(mask, DWORD(zeros.size()), zeros.data());
    ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = color; icon.hbmMask = mask;
    HICON result = mask ? CreateIconIndirect(&icon) : nullptr;
    DeleteObject(color); if (mask) DeleteObject(mask);
    return result;
}
void drawMascot(HDC dc, int x, int y, int unit, Mood mood, double level, int frame, const MascotColors& c, int look) {
    look = std::clamp(look, -1, 1);
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
        for (int i = 0; i < 6; ++i) { if (top[i] != '.') put(5, 3 + i + look, top[i]); if (bottom[i] != '.') put(6, 3 + i + look, bottom[i]); }
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

// ----- Keycap style -----
namespace ui {
namespace {
// Everything soft in this style is worked out once and then only copied: a key with its
// shadow and light, a well with its shading, the halo of an LED. Each is a small bitmap with
// per-pixel alpha, made in plain arithmetic from the distance to a rounded rect and kept by
// size, state and colours; drawing one is a single blend.
double roundDistance(double px, double py, double cx, double cy, double hw, double hh, double r) {
    const double qx = std::abs(px - cx) - (hw - r), qy = std::abs(py - cy) - (hh - r);
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}
double smoothStep(double a) { a = std::clamp(a, 0.0, 1.0); return a * a * (3 - 2 * a); }
// A sprite's pixel while it is worked out: premultiplied, in floating point, so that soft light
// is not rounded layer by layer. It is rounded once, with a little ordered noise: a glow that
// falls off over dozens of pixels would otherwise show its 8-bit steps as bands.
struct Px {
    double a = 0, r = 0, g = 0, b = 0;
    void over(COLORREF c, double alpha) {
        alpha = std::clamp(alpha, 0.0, 1.0);
        r = GetRValue(c) * alpha + r * (1 - alpha); g = GetGValue(c) * alpha + g * (1 - alpha); b = GetBValue(c) * alpha + b * (1 - alpha);
        a = alpha + a * (1 - alpha);
    }
};
uint32_t pack(const Px& p, int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    const double n1 = (h & 0xFFFF) / 65536.0, n2 = ((h >> 16) & 0xFFFF) / 65536.0, noise = (n1 + n2) / 2 - .5 + (n1 - .5) * .5; // roughly triangular, about +-0.75
    const auto level = [&](double v, double top) { return uint32_t(std::clamp(std::floor(v + .5 + noise), 0.0, top)); };
    const uint32_t alpha = p.a <= 0 ? 0 : level(p.a * 255, 255);
    if (!alpha) return 0;
    return (alpha << 24) | (level(p.r, alpha) << 16) | (level(p.g, alpha) << 8) | level(p.b, alpha);
}
// How light thins with distance: bright by its source and a long, faint tail.
double glowFalloff(double d, double spread) { const double s = std::clamp(1 - d / spread, 0.0, 1.0); return s * s * (3 - 2 * s) * (.35 + .65 * s); }
struct Baked { HBITMAP bitmap = nullptr; int w = 0, h = 0, margin = 0; };
HDC bakeDC = nullptr;
size_t bakedBytes = 0;
Baked bake(int w, int h, uint32_t** bits) {
    Baked baked;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return baked;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr);
    if (!bakeDC) bakeDC = CreateCompatibleDC(screen);
    void* memory = nullptr;
    baked.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!baked.bitmap || !memory) { if (baked.bitmap) DeleteObject(baked.bitmap); return Baked{}; }
    baked.w = w; baked.h = h; *bits = static_cast<uint32_t*>(memory);
    bakedBytes += size_t(w) * size_t(h) * 4;
    return baked;
}
void stamp(HDC dc, const Baked& baked, int x, int y, BYTE alpha = 255) {
    if (!baked.bitmap || !bakeDC) return;
    HGDIOBJ old = SelectObject(bakeDC, baked.bitmap);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, x, y, baked.w, baked.h, bakeDC, 0, 0, baked.w, baked.h, blend);
    SelectObject(bakeDC, old);
}
// The colours a key or a well is made of, as one number: a changed theme makes new bitmaps.
uint32_t paletteTag(const Palette& p) {
    uint32_t tag = 2166136261u;
    for (COLORREF c : {p.keyTop, p.keyBottom, p.litTop, p.litMid, p.litBottom, p.rim, p.well, p.halo, p.shadow}) tag = (tag ^ c) * 16777619u;
    return tag ^ (p.dark ? 0x80000000u : 0);
}
// The room light and shadow have on each side before they must be gone (see LightBounds).
struct Room {
    short left = 32767, top = 32767, right = 32767, bottom = 32767;
    bool operator<(const Room& o) const { return std::tie(left, top, right, bottom) < std::tie(o.left, o.top, o.right, o.bottom); }
    Room within(int margin) const {
        const auto cap = [&](short v) { return short(std::clamp<int>(v, 0, margin)); };
        return {cap(left), cap(top), cap(right), cap(bottom)};
    }
    bool open(int margin) const { return left >= margin && top >= margin && right >= margin && bottom >= margin; }
};
// How much farther away a point counts as being, so that light reaching `spread` px in the open
// is gone at the edge of the room on that side. (ox, oy): the point's offset from the shape's
// centre; (ax, ay): how far it lies beyond the shape's straight sides. `down` shifts the room
// for something drawn that much lower (a shadow).
double squeeze(const Room& room, double spread, double ox, double oy, double ax, double ay, int down = 0) {
    const double rx = ox < 0 ? room.left : room.right, ry = oy < 0 ? room.top + down : room.bottom - down;
    const double fx = rx >= spread ? 1 : spread / std::max(rx, .5), fy = ry >= spread ? 1 : spread / std::max(ry, .5);
    if (fx == 1 && fy == 1) return 1;
    if (ax <= 0 && ay <= 0) return std::max(fx, fy);
    if (ay <= 0) return fx;
    if (ax <= 0) return fy;
    return std::sqrt((ax * ax * fx * fx + ay * ay * fy * fy) / (ax * ax + ay * ay));
}
struct HaloId {
    int w, h, radius, spread; COLORREF color; Room room;
    bool operator<(const HaloId& o) const { return std::tie(w, h, radius, spread, color, room) < std::tie(o.w, o.h, o.radius, o.spread, o.color, o.room); }
};
struct KeyId {
    int w, h, radius; unsigned char lit, pressed, hover; uint32_t tag; Room room, shade; // room for its light, and for its shadow
    bool operator<(const KeyId& o) const { return std::tie(w, h, radius, lit, pressed, hover, tag, room, shade) < std::tie(o.w, o.h, o.radius, o.lit, o.pressed, o.hover, o.tag, o.room, o.shade); }
};
struct WellId {
    int w, h, radius; uint32_t tag;
    bool operator<(const WellId& o) const { return std::tie(w, h, radius, tag) < std::tie(o.w, o.h, o.radius, o.tag); }
};
std::map<HaloId, Baked> halos;
std::map<KeyId, Baked> keys;
std::map<WellId, Baked> wells;
// A soft halo round a rounded rect at full strength; its strength is chosen when it is drawn.
const Baked& haloSprite(const HaloId& id) {
    if (auto found = halos.find(id); found != halos.end()) return found->second;
    const int w = id.w + id.spread * 2, h = id.h + id.spread * 2;
    uint32_t* out = nullptr;
    Baked baked = bake(w, h, &out);
    if (baked.bitmap) {
        const double hw = id.w / 2.0, hh = id.h / 2.0, r = std::min({double(id.radius), hw, hh}), spread = std::max(1, id.spread);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const double ox = x + .5 - w / 2.0, oy = y + .5 - h / 2.0;
            double d = roundDistance(x + .5, y + .5, w / 2.0, h / 2.0, hw, hh, r);
            if (d > 0) d *= squeeze(id.room, spread, ox, oy, std::abs(ox) - (hw - r), std::abs(oy) - (hh - r));
            Px pixel;
            pixel.over(id.color, d <= 0 ? 1 : glowFalloff(d, spread));
            out[size_t(y) * size_t(w) + size_t(x)] = pack(pixel, x, y);
        }
        baked.margin = id.spread;
    }
    return halos.emplace(id, baked).first->second;
}
// The most a key's light and shadow reach past its face, in any state.
int keyMargin(int h, bool lit) {
    const int unit = std::max(1, h / 22);
    return std::max(lit ? std::max(10, h * 3 / 5) : 0, (unit * 6 + 2) + (unit * 4 + 1)) + 1;
}
// A key: its light on the panel (when lit), its shadow, its face from top to bottom, the catch
// light along its top edge and, lit, the light showing along its rim.
const Baked& keySprite(const KeyId& id, const Palette& p) {
    if (auto found = keys.find(id); found != keys.end()) return found->second;
    const double lit = id.lit / 16.0, pressed = id.pressed / 4.0, hover = id.hover / 4.0;
    const int w = id.w, h = id.h, unit = std::max(1, h / 22);
    const double radius = std::min({double(id.radius), w / 2.0, h / 2.0});
    const int glowSpread = std::max(10, h * 3 / 5); // the light spreads wide and thin over the panel
    // Under the pointer the shadow lengthens, as if the key stood a little taller; pressed, it shortens.
    const int drop = int(std::lround((1 - pressed) * unit * (2.5 + 1.5 * hover))) + 1;
    const int shadowSpread = unit * 4 + 2 + int(std::lround(hover * unit * 2));
    const int margin = keyMargin(h, lit > 0);
    const int W = w + margin * 2, H = h + margin * 2;
    uint32_t* out = nullptr;
    Baked baked = bake(W, H, &out);
    if (baked.bitmap) {
        baked.margin = margin;
        const double cx = W / 2.0, cy = H / 2.0, hw = w / 2.0, hh = h / 2.0;
        const int press = int(std::lround(pressed * unit));
        const COLORREF lift = RGB(255, 255, 255);
        const double brighten = hover * (p.dark ? .05 : .25);
        const COLORREF top = mix(mix(p.keyTop, p.litTop, lit), lift, brighten);
        const COLORREF middle = mix(mix(mix(p.keyTop, p.keyBottom, .62), p.litMid, lit), lift, brighten);
        const COLORREF bottom = mix(mix(p.keyBottom, p.litBottom, lit), lift, brighten);
        const double band = std::max(1.0, double(unit) * (lit > .5 ? 2.0 : 1.0));
        const COLORREF catchLight = mix(p.dark ? RGB(80, 82, 92) : RGB(255, 255, 255), p.rim, lit);
        const double catchAlpha = (p.dark ? 40 + 150 * lit : 230) / 255.0;
        const double glowAlpha = (p.dark ? 150 : 140) / 255.0 * lit, shadowAlpha = (p.dark ? 150 : 52) / 255.0, contactAlpha = (p.dark ? 110 : 34) / 255.0;
        const double rimWidth = p.dark ? 1.6 : 1.1, rimAlpha = (p.dark ? 235 : 150) / 255.0 * lit;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const double px = x + .5, py = y + .5;
            Px pixel;
            const double body = roundDistance(px, py - press, cx, cy, hw, hh, radius);
            if (body < -1) { // well inside the face: nothing under it shows
                const double t = std::clamp((py - press - margin) / h, 0.0, 1.0);
                COLORREF face = t < .62 ? mix(top, middle, t / .62) : mix(middle, bottom, (t - .62) / .38);
                if (py - press - margin < band) face = mix(face, catchLight, catchAlpha);
                if (lit > 0) face = mix(face, p.rim, std::clamp(rimWidth + body + .5, 0.0, 1.0) * rimAlpha);
                out[size_t(y) * size_t(W) + size_t(x)] = 0xFF000000u | (uint32_t(GetRValue(face)) << 16) | (uint32_t(GetGValue(face)) << 8) | GetBValue(face);
                continue;
            }
            const double ox = px - cx, ax = std::abs(ox) - (hw - radius);
            if (lit > 0) {
                double d = roundDistance(px, py, cx, cy, hw, hh, radius);
                if (d > 0) d *= squeeze(id.room, glowSpread, ox, py - cy, ax, std::abs(py - cy) - (hh - radius));
                pixel.over(p.halo, (d <= 0 ? 1 : glowFalloff(d, glowSpread)) * glowAlpha);
            }
            {
                double d = roundDistance(px, py - drop, cx, cy, hw, hh, radius);
                if (d > 0) d *= squeeze(id.shade, shadowSpread, ox, py - drop - cy, ax, std::abs(py - drop - cy) - (hh - radius), drop);
                pixel.over(p.shadow, (d <= 0 ? 1 : smoothStep(1 - d / shadowSpread)) * shadowAlpha);
            }
            if (id.shade.bottom >= 3) { const double d = roundDistance(px, py - 1, cx, cy, hw, hh, radius); pixel.over(p.shadow, (d <= 0 ? 1 : smoothStep(1 - d / 2)) * contactAlpha); }
            const double cover = std::clamp(.5 - body, 0.0, 1.0);
            if (cover > 0) {
                const double t = std::clamp((py - press - margin) / h, 0.0, 1.0);
                COLORREF face = t < .62 ? mix(top, middle, t / .62) : mix(middle, bottom, (t - .62) / .38);
                if (py - press - margin < band) face = mix(face, catchLight, catchAlpha);
                if (lit > 0) face = mix(face, p.rim, std::clamp(rimWidth + body + .5, 0.0, 1.0) * rimAlpha);
                pixel.over(face, cover);
            }
            out[size_t(y) * size_t(W) + size_t(x)] = pack(pixel, x, y);
        }
    }
    return keys.emplace(id, baked).first->second;
}
// A well: the panel sunk in, shaded under its top edge, a faint light along its lower edge.
const Baked& wellSprite(const WellId& id, const Palette& p) {
    if (auto found = wells.find(id); found != wells.end()) return found->second;
    uint32_t* out = nullptr;
    Baked baked = bake(id.w, id.h, &out);
    if (baked.bitmap) {
        const double hw = id.w / 2.0, hh = id.h / 2.0, radius = std::min({double(id.radius), hw, hh});
        const double shade = std::clamp(id.h * .22, 3.0, 9.0), shadeAlpha = (p.dark ? 200 : 60) / 255.0, lineAlpha = (p.dark ? 14 : 150) / 255.0;
        for (int y = 0; y < id.h; ++y) {
            COLORREF row = p.well;
            if (y + .5 < shade) row = mix(row, p.shadow, shadeAlpha * (1 - (y + .5) / shade));
            if (y == id.h - 1) row = mix(row, RGB(255, 255, 255), lineAlpha);
            for (int x = 0; x < id.w; ++x) {
                Px pixel;
                pixel.over(row, std::clamp(.5 - roundDistance(x + .5, y + .5, hw, hh, hw, hh, radius), 0.0, 1.0));
                out[size_t(y) * size_t(id.w) + size_t(x)] = pack(pixel, x, y);
            }
        }
    }
    return wells.emplace(id, baked).first->second;
}
void solid(HDC dc, const RECT& r, COLORREF c) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    SetDCBrushColor(dc, c); FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
// Sizes and colours change with the window, the scale and the theme: past this much, start over.
void trimSprites() { if (bakedBytes > 40u * 1024 * 1024 || halos.size() + keys.size() + wells.size() > 1500) clearSprites(); }
}
void clearSprites() {
    for (auto* cache : {&halos}) for (auto& [id, baked] : *cache) if (baked.bitmap) DeleteObject(baked.bitmap);
    for (auto& [id, baked] : keys) if (baked.bitmap) DeleteObject(baked.bitmap);
    for (auto& [id, baked] : wells) if (baked.bitmap) DeleteObject(baked.bitmap);
    halos.clear(); keys.clear(); wells.clear(); bakedBytes = 0;
}
namespace {
// What light (0) and shadows (1) must stay inside, in device coordinates.
struct Bounds { bool set = false; RECT area{}; } boundsOf[2];
// The room round `r` (logical) inside the current bounds, at most `margin` on a side.
Room roomFor(HDC dc, const RECT& r, int margin, int kind) {
    Room room;
    if (boundsOf[kind].set) {
        const RECT& bounds = boundsOf[kind].area;
        POINT corners[2] = {{r.left, r.top}, {r.right, r.bottom}};
        LPtoDP(dc, corners, 2);
        const auto side = [](long v) { return short(std::clamp<long>(v, 0, 32767)); };
        room = {side(corners[0].x - bounds.left), side(corners[0].y - bounds.top), side(bounds.right - corners[1].x), side(bounds.bottom - corners[1].y)};
    }
    return room.open(margin) ? Room{} : room.within(margin);
}
}
LightBounds::LightBounds(HDC dc, const RECT& area, bool shadowsOnly) {
    POINT corners[2] = {{area.left, area.top}, {area.right, area.bottom}};
    LPtoDP(dc, corners, 2);
    for (int kind = 0; kind < 2; ++kind) {
        had_[kind] = boundsOf[kind].set; previous_[kind] = boundsOf[kind].area;
        if (kind == 0 && shadowsOnly) continue;
        boundsOf[kind].area = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
        boundsOf[kind].set = true;
    }
}
LightBounds::~LightBounds() { for (int kind = 0; kind < 2; ++kind) { boundsOf[kind].set = had_[kind]; boundsOf[kind].area = previous_[kind]; } }
void softHalo(HDC dc, const RECT& r, float radius, COLORREF c, BYTE alpha, int spread, int dx, int dy) {
    if (alpha == 0 || r.right <= r.left || r.bottom <= r.top) return;
    trimSprites();
    spread = std::max(1, spread);
    const RECT placed{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
    const HaloId id{int(r.right - r.left), int(r.bottom - r.top), int(std::lround(radius)), spread, c, roomFor(dc, placed, spread, 0)};
    const Baked& baked = haloSprite(id);
    stamp(dc, baked, r.left - id.spread + dx, r.top - id.spread + dy, alpha);
}
void keycap(HDC dc, RECT r, float radius, const Palette& p, double lit, double pressed, double hover, double opacity) {
    if (r.right <= r.left || r.bottom <= r.top || opacity <= .004) return;
    opacity = std::min(opacity, 1.0);
    lit = std::clamp(lit, 0.0, 1.0); pressed = std::clamp(pressed, 0.0, 1.0); hover = std::clamp(hover, 0.0, 1.0);
    if (p.highContrast) {
        if (opacity < .5) return;
        fillRound(dc, r, lit > .5 ? p.selected : p.raised, radius);
        strokeRound(dc, r, lit > .5 ? p.selectedInk : p.border, radius, lit > .5 ? 2.0f : 1.0f);
        return;
    }
    trimSprites();
    // A key in a given state is one bitmap: light, shadow, face and edges together.
    const int height = int(r.bottom - r.top);
    const KeyId id{int(r.right - r.left), height, int(std::lround(radius)),
        static_cast<unsigned char>(std::lround(lit * 16)), static_cast<unsigned char>(std::lround(pressed * 4)), static_cast<unsigned char>(std::lround(hover * 4)), paletteTag(p),
        roomFor(dc, r, keyMargin(height, lit > 0), 0), roomFor(dc, r, keyMargin(height, false), 1)};
    const Baked& baked = keySprite(id, p);
    stamp(dc, baked, r.left - baked.margin, r.top - baked.margin, BYTE(std::lround(opacity * 255)));
}
void keycapStretched(HDC dc, RECT r, int baseW, int baseH, float radius, const Palette& p, double lit) {
    if (r.right <= r.left || r.bottom <= r.top || baseW <= 0 || baseH <= 0) return;
    if (p.highContrast) { keycap(dc, r, radius, p, lit); return; }
    trimSprites();
    lit = std::clamp(lit, 0.0, 1.0);
    // Room above and below as where it is; at its sides it is on the move, so none is taken off.
    const int litMargin = keyMargin(baseH, lit > 0), shadeMargin = keyMargin(baseH, false);
    Room light = roomFor(dc, r, litMargin, 0), shade = roomFor(dc, r, shadeMargin, 1);
    if (light.left != 32767) light.left = light.right = short(litMargin);
    if (shade.left != 32767) shade.left = shade.right = short(shadeMargin);
    const KeyId id{baseW, baseH, int(std::lround(radius)), static_cast<unsigned char>(std::lround(lit * 16)), 4, 0, paletteTag(p), light, shade};
    const Baked& baked = keySprite(id, p);
    if (!baked.bitmap || !bakeDC) return;
    const int m = baked.margin, cap = m + std::min(baseW / 2, int(radius) + 3);
    const double sy = double(r.bottom - r.top) / baseH;
    const int top = int(r.top) - int(std::lround(m * sy)), height = int(r.bottom - r.top) + int(std::lround(m * sy)) * 2;
    const int left = int(r.left) - m, right = int(r.right) + m;
    if (right - left < cap * 2) { stamp(dc, baked, left, int(r.top) - m); return; }
    HGDIOBJ old = SelectObject(bakeDC, baked.bitmap);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, left, top, cap, height, bakeDC, 0, 0, cap, baked.h, blend);
    GdiAlphaBlend(dc, left + cap, top, right - left - cap * 2, height, bakeDC, cap, 0, baked.w - cap * 2, baked.h, blend);
    GdiAlphaBlend(dc, right - cap, top, cap, height, bakeDC, baked.w - cap, 0, cap, baked.h, blend);
    SelectObject(bakeDC, old);
}
void spotlight(HDC dc, const RECT& key, float radius, POINT at, COLORREF c, double strength, int reach) {
    if (!graphicsToken || strength <= .01 || reach <= 0 || key.right <= key.left) return;
    Canvas canvas(dc);
    auto& g = canvas.g;
    Gdiplus::GraphicsPath face; roundPath(face, rectF(key), radius);
    Gdiplus::GraphicsState state = g.Save();
    g.SetClip(&face);
    Gdiplus::GraphicsPath pool; pool.AddEllipse(float(at.x - reach), float(at.y - reach), float(reach * 2), float(reach * 2));
    Gdiplus::PathGradientBrush brush(&pool);
    brush.SetCenterPoint(Gdiplus::PointF(float(at.x), float(at.y)));
    brush.SetCenterColor(color(c, BYTE(std::clamp(strength, 0.0, 1.0) * 255)));
    Gdiplus::Color edge = color(c, 0); int count = 1;
    brush.SetSurroundColors(&edge, &count);
    g.FillPath(&brush, &pool);
    g.Restore(state);
}
void ripple(HDC dc, const RECT& key, float radius, POINT at, COLORREF c, double progress) {
    // A soft wave, not a drawn ring: light that rises toward the wave's front and falls away at
    // once beyond it, with a faint wash left behind; all of it thins as it spreads. Worked out
    // per pixel into one scratch bitmap (a key is small) and laid on with a single blend.
    const int w = int(key.right - key.left), h = int(key.bottom - key.top);
    if (progress <= 0 || progress >= 1 || w <= 0 || h <= 0 || w > 4096 || h > 1024) return;
    static HDC scratchDC = nullptr; static HBITMAP scratch = nullptr; static HGDIOBJ scratchOld = nullptr; static uint32_t* pixels = nullptr; static int scratchW = 0, scratchH = 0;
    if (!scratch || scratchW < w || scratchH < h) {
        if (!scratchDC) scratchDC = CreateCompatibleDC(nullptr);
        if (scratch) { SelectObject(scratchDC, scratchOld); DeleteObject(scratch); scratch = nullptr; }
        scratchW = std::max(scratchW, w); scratchH = std::max(scratchH, h);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = scratchW; info.bmiHeader.biHeight = -scratchH; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* memory = nullptr;
        scratch = scratchDC ? CreateDIBSection(scratchDC, &info, DIB_RGB_COLORS, &memory, nullptr, 0) : nullptr;
        if (!scratch || !memory) { if (scratch) DeleteObject(scratch); scratch = nullptr; scratchW = scratchH = 0; return; }
        pixels = static_cast<uint32_t*>(memory); scratchOld = SelectObject(scratchDC, scratch);
    }
    const double ax = at.x - key.left, ay = at.y - key.top;
    double reach = 0; // far enough to pass the key's farthest corner
    for (const double cx : {0.0, double(w)}) for (const double cy : {0.0, double(h)}) reach = std::max(reach, std::hypot(cx - ax, cy - ay));
    const double eased = 1 - (1 - progress) * (1 - progress), front = reach * eased, fade = (1 - progress) * (1 - progress);
    const double depth = std::max(10.0, h * .9), rise = depth * .28, peak = .34 * fade, wash = .05 * fade;
    const double hw = w / 2.0, hh = h / 2.0, corner = std::min({double(radius), hw, hh});
    const uint32_t cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = pixels + size_t(y) * size_t(scratchW);
        for (int x = 0; x < w; ++x) {
            const double d = std::hypot(x + .5 - ax, y + .5 - ay);
            double a = 0;
            if (d < front) {
                const double behind = front - d; // how far behind the wave's front
                a = behind < rise ? peak * smoothStep(behind / rise) : behind < depth ? wash + (peak - wash) * (1 - smoothStep((behind - rise) / (depth - rise))) : wash;
                a *= std::clamp(.5 - roundDistance(x + .5, y + .5, hw, hh, hw, hh, corner), 0.0, 1.0); // inside the key's rounded face
            }
            const uint32_t alpha = uint32_t(a * 255 + .5);
            row[x] = alpha ? (alpha << 24) | ((cr * alpha / 255) << 16) | ((cg * alpha / 255) << 8) | (cb * alpha / 255) : 0;
        }
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, key.left, key.top, w, h, scratchDC, 0, 0, w, h, blend);
}
void well(HDC dc, const RECT& r, float radius, const Palette& p) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (p.highContrast) { strokeRound(dc, r, p.border, radius, 1.0f); return; }
    trimSprites();
    const WellId id{int(r.right - r.left), int(r.bottom - r.top), int(std::lround(radius)), paletteTag(p)};
    stamp(dc, wellSprite(id, p), r.left, r.top);
}
void led(HDC dc, const RECT& r, COLORREF on, COLORREF halo, double lit, COLORREF off) {
    lit = std::clamp(lit, 0.0, 1.0);
    if (lit > 0) softHalo(dc, r, 1.0f, halo, BYTE(150 * lit), std::max(3, int(std::min(r.right - r.left, r.bottom - r.top)) / 2 + 2));
    solid(dc, r, mix(off, on, lit));
}
void pixelIcon(HDC dc, const char* const* rows, int count, int x, int y, int unit, COLORREF c, int gap) {
    if (gap < 0) gap = unit >= 3 ? std::max(1, unit / 4) : 0;
    for (int row = 0; row < count; ++row)
        for (int column = 0; rows[row][column]; ++column)
            if (rows[row][column] == '#') solid(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit - gap, y + (row + 1) * unit - gap}, c);
}
}
