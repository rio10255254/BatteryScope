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
