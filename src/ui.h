#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <map>
#include <string>
#include <vector>

// Drawing toolkit for the panel: theme palettes and antialiased primitives.
// Text stays on GDI (ClearType); shapes use GDI+ when it started, else GDI.
namespace ui {
enum class ThemeChoice { system = 0, dark = 1, light = 2 };

struct Palette {
    COLORREF background, surface, raised, border;
    COLORREF ink, muted, faint;
    COLORREF accent, accentDim, accentSoft, accentInk; // accentInk: text on an accent fill
    COLORREF warn, danger;
    COLORREF selected, selectedInk;
    bool dark = true;
    bool highContrast = false; // Windows contrast theme: system colors and visible outlines
};

bool systemPrefersLight();
bool highContrastOn();
// Resolves "system" through the Windows app theme; a contrast theme always wins.
// `forceContrast` previews the contrast palette from the current system colors.
Palette palette(ThemeChoice choice, bool forceContrast = false);
COLORREF mix(COLORREF a, COLORREF b, double t);

bool startGraphics();
void stopGraphics();
bool graphicsReady();

void fillRound(HDC dc, const RECT& r, COLORREF color, float radius, BYTE alpha = 255);
void strokeRound(HDC dc, const RECT& r, COLORREF color, float radius, float width);
// Polyline segments over a plot, with a vertical gradient fill down to the plot bottom.
void drawSeries(HDC dc, const RECT& plot, const std::vector<std::vector<POINT>>& segments,
    COLORREF line, float width, COLORREF fill);
void drawDashedLine(HDC dc, POINT a, POINT b, COLORREF color, float width);
// Battery outline with a level bar; `bolt` overlays a charging symbol.
void drawBattery(HDC dc, const RECT& r, double fraction, COLORREF outline, COLORREF level,
    bool bolt, COLORREF boltColor, COLORREF boltEdge);
void drawChevron(HDC dc, const RECT& r, COLORREF color, float width);
void drawDots(HDC dc, const RECT& r, COLORREF color, float radius);

// Pixel style. Shapes are drawn without antialiasing on a grid of `unit`
// device pixels; `steps` is the number of stair steps cut into each corner.
void pixelFill(HDC dc, const RECT& r, COLORREF color, int unit, int steps);
void pixelFrame(HDC dc, const RECT& r, COLORREF color, int unit, int steps);
// A filled box with a one-unit outline; a valid `shadow` adds a one-unit drop below.
void pixelBox(HDC dc, const RECT& r, COLORREF fill, COLORREF outline, int unit, int steps, COLORREF shadow = CLR_INVALID);
// Horizontal meter of 2x-unit blocks with one-unit gaps.
void pixelSegments(HDC dc, const RECT& r, double fraction, COLORREF on, COLORREF off, int unit);
// Battery with `cells` level cells and a pixel lightning bolt when charging.
void pixelBattery(HDC dc, const RECT& r, double fraction, COLORREF outline, COLORREF level, COLORREF empty,
    bool bolt, COLORREF boltColor, COLORREF boltEdge, int unit, int cells = 5);
// 3x5 arrow glyphs for icon buttons.
void pixelChevron(HDC dc, const RECT& r, COLORREF color, int unit);
void pixelDots(HDC dc, const RECT& r, COLORREF color, int unit);
void pixelDottedLine(HDC dc, POINT a, POINT b, COLORREF color, int unit);
// 5x7 bitmap digits for 0-9 . % W - and space. Returns false (drawing nothing)
// if `text` holds any other character; width is 0 in that case.
int pixelDigitsWidth(const std::wstring& text, int unit);
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color);

// A square notification-area icon showing `percent` (0-100) in 3x5 pixel
// digits on a badge. The caller owns the icon (DestroyIcon).
HICON makePercentIcon(int size, int percent, COLORREF badge, COLORREF edge, COLORREF digits);

// The embedded pixel UI font (family name below), loaded privately from a resource.
constexpr wchar_t PixelFontFace[] = L"BatteryScope Pixel";
bool loadPixelFont(HMODULE module, int resource);
void unloadPixelFont();
bool pixelFontLoaded();
// True if every character of `text` has a glyph in the pixel font.
bool pixelCovers(const std::wstring& text);

// Executable icons at one pixel size, cached by path (misses are cached too).
class IconCache {
    std::map<std::wstring, HICON> icons_;
    int size_ = 0;
public:
    IconCache() = default;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;
    ~IconCache() { clear(); }
    HICON get(const std::wstring& path, int size);
    void clear();
};
}
