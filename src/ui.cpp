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
        // Day: an ivory panel, keys a shade lighter, wells a shade darker, amber light.
        p.background = p.surface = RGB(230, 227, 222); p.raised = RGB(241, 238, 233); p.border = RGB(205, 201, 195);
        p.ink = RGB(42, 40, 37); p.muted = RGB(87, 83, 77); p.faint = RGB(140, 135, 127);
        p.accent = RGB(135, 84, 10); p.accentDim = RGB(217, 181, 122); p.accentSoft = RGB(243, 223, 184); p.accentInk = RGB(42, 40, 37);
        p.warn = RGB(161, 92, 0); p.danger = RGB(184, 53, 44);
        p.selected = RGB(243, 223, 184); p.selectedInk = p.ink;
        p.keyTop = RGB(243, 240, 235); p.keyBottom = RGB(228, 224, 218);
        p.litTop = RGB(248, 213, 146); p.litMid = RGB(244, 232, 210); p.litBottom = RGB(242, 237, 230);
        p.rim = RGB(240, 170, 66); p.well = RGB(211, 207, 201); p.glow = RGB(245, 195, 103); p.halo = RGB(244, 175, 61);
        p.ledOff = RGB(195, 190, 183); p.shadow = RGB(90, 78, 60);
        p.dark = false;
    } else {
        // Night: a charcoal panel, smoked keys, deep wells, an ice-blue neon light.
        p.background = p.surface = RGB(27, 27, 29); p.raised = RGB(46, 47, 53); p.border = RGB(52, 53, 59);
        p.ink = RGB(236, 236, 239); p.muted = RGB(164, 164, 172); p.faint = RGB(110, 110, 118);
        p.accent = RGB(180, 195, 255); p.accentDim = RGB(74, 85, 144); p.accentSoft = RGB(37, 43, 69); p.accentInk = RGB(242, 246, 255);
        p.warn = RGB(242, 169, 59); p.danger = RGB(255, 107, 99);
        p.selected = RGB(37, 43, 69); p.selectedInk = p.ink;
        p.keyTop = RGB(49, 50, 56); p.keyBottom = RGB(37, 38, 43);
        p.litTop = RGB(70, 71, 83); p.litMid = RGB(52, 53, 61); p.litBottom = RGB(43, 44, 50);
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
    Gdiplus::Pen line(&rim, 1.4f); line.SetLineJoin(Gdiplus::LineJoinRound);
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
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color, int gap) {
    if (text.empty() || !pixelDigitsWidth(text, unit)) return false;
    gap = std::clamp(gap, 0, unit - 1);
    for (wchar_t c : text) {
        const auto* glyph = digitGlyph(c);
        for (int row = 0; row < 7; ++row) for (int column = 0; column < glyph->width; ++column)
            if (glyph->rows[row][column] == '#')
                rectFill(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit - gap, y + (row + 1) * unit - gap}, color);
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

// ----- Keycap style -----
namespace ui {
namespace {
// Soft rounded-rect halos, worked out once per size, radius, spread and colour: a keycap's
// shadow and a lit key's glow are copies of these, not something computed every frame.
struct SpriteKey {
    int w, h, radius, spread; COLORREF color; BYTE alpha;
    bool operator<(const SpriteKey& o) const { return std::tie(w, h, radius, spread, color, alpha) < std::tie(o.w, o.h, o.radius, o.spread, o.color, o.alpha); }
};
struct Sprite { HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ original = nullptr; };
std::map<SpriteKey, Sprite> sprites;
const Sprite* haloSprite(const SpriteKey& key) {
    if (auto found = sprites.find(key); found != sprites.end()) return &found->second;
    if (sprites.size() > 400) clearSprites(); // sizes change with the window; keep the cache small
    const int w = key.w + key.spread * 2, h = key.h + key.spread * 2;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!bitmap || !dc || !bits) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return nullptr; }
    // Distance from each pixel to the rounded rect; inside is solid, outside fades to nothing.
    const double hw = key.w / 2.0, hh = key.h / 2.0, r = std::min({double(key.radius), hw, hh});
    const double cx = w / 2.0, cy = h / 2.0, spread = std::max(1, key.spread);
    auto* out = static_cast<uint32_t*>(bits);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const double qx = std::abs(x + .5 - cx) - (hw - r), qy = std::abs(y + .5 - cy) - (hh - r);
        const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
        double a = d <= 0 ? 1 : std::max(0.0, 1 - d / spread);
        a = a * a * (3 - 2 * a) * key.alpha / 255.0; // smoothstep: a soft edge without a visible ring
        const uint32_t A = uint32_t(std::lround(a * 255));
        out[size_t(y) * size_t(w) + size_t(x)] = (A << 24) | (uint32_t(GetRValue(key.color) * A / 255) << 16) |
            (uint32_t(GetGValue(key.color) * A / 255) << 8) | uint32_t(GetBValue(key.color) * A / 255);
    }
    Sprite sprite{dc, bitmap, SelectObject(dc, bitmap)};
    return &sprites.emplace(key, sprite).first->second;
}
void solid(HDC dc, const RECT& r, COLORREF c) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    SetDCBrushColor(dc, c); FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
}
void clearSprites() {
    for (auto& [key, sprite] : sprites) { SelectObject(sprite.dc, sprite.original); DeleteObject(sprite.bitmap); DeleteDC(sprite.dc); }
    sprites.clear();
}
void softHalo(HDC dc, const RECT& r, float radius, COLORREF c, BYTE alpha, int spread, int dx, int dy) {
    if (alpha == 0 || r.right <= r.left || r.bottom <= r.top) return;
    const SpriteKey key{int(r.right - r.left), int(r.bottom - r.top), int(std::lround(radius)), std::max(1, spread), c, alpha};
    const Sprite* sprite = haloSprite(key);
    if (!sprite) return;
    const int w = key.w + key.spread * 2, h = key.h + key.spread * 2;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, r.left - key.spread + dx, r.top - key.spread + dy, w, h, sprite->dc, 0, 0, w, h, blend);
}
void keycap(HDC dc, RECT r, float radius, const Palette& p, double lit, double pressed, double hover) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    lit = std::clamp(lit, 0.0, 1.0); pressed = std::clamp(pressed, 0.0, 1.0); hover = std::clamp(hover, 0.0, 1.0);
    if (p.highContrast) {
        fillRound(dc, r, lit > .5 ? p.selected : p.raised, radius);
        strokeRound(dc, r, lit > .5 ? p.selectedInk : p.border, radius, lit > .5 ? 2.0f : 1.0f);
        return;
    }
    const int h = r.bottom - r.top, unit = std::max(1, h / 22);
    // Light first (it spills onto the panel round the key), then the key's shadow on the panel.
    if (lit > 0) softHalo(dc, r, radius, p.halo, BYTE((p.dark ? 115 : 105) * lit), std::max(8, h / 2));
    const int drop = int(std::lround((1 - pressed) * unit * 3));
    softHalo(dc, r, radius, p.shadow, BYTE(p.dark ? 150 : 52), unit * 5 + 2, 0, drop + 1);
    softHalo(dc, r, radius, p.shadow, BYTE(p.dark ? 110 : 34), 2, 0, 1);
    OffsetRect(&r, 0, int(std::lround(pressed * unit)));
    // The face: lighter at the top, a little brighter under the pointer, warm (or cool) when lit.
    const COLORREF lift = RGB(255, 255, 255);
    const double brighten = hover * (p.dark ? .05 : .25);
    const COLORREF top = mix(mix(p.keyTop, p.litTop, lit), lift, brighten);
    const COLORREF middle = mix(mix(mix(p.keyTop, p.keyBottom, .62), p.litMid, lit), lift, brighten);
    const COLORREF bottom = mix(mix(p.keyBottom, p.litBottom, lit), lift, brighten);
    if (!graphicsToken) { gdiRound(dc, r, middle, nullptr, radius); return; }
    Canvas canvas(dc);
    auto& g = canvas.g;
    const auto box = rectF(r);
    Gdiplus::GraphicsPath path; roundPath(path, box, radius);
    Gdiplus::LinearGradientBrush face(Gdiplus::PointF(0, box.Y - 1), Gdiplus::PointF(0, box.Y + box.Height + 1), color(top), color(bottom));
    Gdiplus::Color stops[] = {color(top), color(middle), color(bottom)};
    Gdiplus::REAL positions[] = {0.0f, .62f, 1.0f};
    face.SetInterpolationColors(stops, positions, 3);
    g.FillPath(&face, &path);
    // The edge: a catch light along the top when unlit; lit, the light shows along the rim.
    Gdiplus::GraphicsState state = g.Save();
    g.SetClip(&path);
    const float band = std::max(1.0f, float(unit) * (lit > .5 ? 2.0f : 1.0f));
    const COLORREF catchLight = mix(p.dark ? RGB(80, 82, 92) : RGB(255, 255, 255), p.rim, lit);
    Gdiplus::SolidBrush edge(color(catchLight, BYTE(p.dark ? 40 + 150 * lit : 230)));
    g.FillRectangle(&edge, box.X, box.Y, box.Width, band);
    g.Restore(state);
    if (lit > 0) {
        auto inner = box; inner.Inflate(-.5f, -.5f);
        Gdiplus::GraphicsPath rimPath; roundPath(rimPath, inner, std::max(0.0f, radius - .5f));
        Gdiplus::Pen rim(color(p.rim, BYTE((p.dark ? 235 : 150) * lit)), p.dark ? 1.5f : 1.0f);
        g.DrawPath(&rim, &rimPath);
    }
}
void well(HDC dc, const RECT& r, float radius, const Palette& p) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (p.highContrast) { strokeRound(dc, r, p.border, radius, 1.0f); return; }
    if (!graphicsToken) { gdiRound(dc, r, p.well, nullptr, radius); return; }
    Canvas canvas(dc);
    auto& g = canvas.g;
    const auto box = rectF(r);
    Gdiplus::GraphicsPath path; roundPath(path, box, radius);
    Gdiplus::SolidBrush fill(color(p.well));
    g.FillPath(&fill, &path);
    Gdiplus::GraphicsState state = g.Save();
    g.SetClip(&path);
    // Shade under the top edge, as if the panel overhangs; a faint light on the lower edge.
    const float shade = std::clamp(box.Height * .22f, 3.0f, 9.0f);
    Gdiplus::LinearGradientBrush top(Gdiplus::PointF(0, box.Y - .5f), Gdiplus::PointF(0, box.Y + shade), color(p.shadow, BYTE(p.dark ? 200 : 60)), color(p.shadow, 0));
    g.FillRectangle(&top, box.X, box.Y, box.Width, shade);
    Gdiplus::SolidBrush lower(color(RGB(255, 255, 255), BYTE(p.dark ? 14 : 150)));
    g.FillRectangle(&lower, box.X, box.Y + box.Height - 1, box.Width, 1.0f);
    g.Restore(state);
}
void led(HDC dc, const RECT& r, COLORREF on, COLORREF halo, double lit, COLORREF off) {
    lit = std::clamp(lit, 0.0, 1.0);
    if (lit > 0) softHalo(dc, r, 1.0f, halo, BYTE(150 * lit), std::max(3, int(std::min(r.right - r.left, r.bottom - r.top)) / 2 + 2));
    solid(dc, r, mix(off, on, lit));
}
void pixelIcon(HDC dc, const char* const* rows, int count, int x, int y, int unit, COLORREF c) {
    const int gap = unit >= 3 ? std::max(1, unit / 4) : 0;
    for (int row = 0; row < count; ++row)
        for (int column = 0; rows[row][column]; ++column)
            if (rows[row][column] == '#') solid(dc, {x + column * unit, y + row * unit, x + (column + 1) * unit - gap, y + (row + 1) * unit - gap}, c);
}
}
