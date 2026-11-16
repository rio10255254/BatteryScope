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
    COLORREF accent, accentDim, accentSoft, accentInk; // accent: the light's colour as text and strokes; accentInk: text on a lit key
    COLORREF warn, danger;
    COLORREF selected, selectedInk;
    // The keycap materials: a key's face from top to bottom, unlit and lit; the lit rim; a well
    // sunk into the panel; the light itself (LEDs, lit dots) and its halo; an unlit LED; shadow.
    COLORREF keyTop, keyBottom, litTop, litMid, litBottom, rim, well, glow, halo, ledOff, shadow;
    bool dark = true;
    bool highContrast = false; // Windows contrast theme: system colors and visible outlines
};

bool systemPrefersLight();
bool highContrastOn();
// Resolves "system" through the Windows app theme; a contrast theme always wins.
// `forceContrast` previews the contrast palette from the current system colors.
// Day: warm ivory keys lit amber from inside. Night: smoked keys lit with an ice-blue neon.
Palette palette(ThemeChoice choice, bool forceContrast = false);
COLORREF mix(COLORREF a, COLORREF b, double t);
// Every colour of `a` moved toward `b` by `t` (the day/night switch blends the whole panel).
Palette mixPalette(const Palette& a, const Palette& b, double t);
// A light along a rounded rect's edge, brightest toward `angle` (radians: 0 right, pi/2 down)
// and fading round the rest (the motion learned from the reference clip). Strength 0-1. Needs GDI+.
void drawRimLight(HDC dc, const RECT& r, float radius, COLORREF color, double angle, double strength);

// ----- Keycap style (0.30) -----
// The panel as a small device, after a reference clip: matte keycaps standing up from the panel,
// wells sunk into it, and light coming from inside whatever is active. Shadows and halos are
// soft rounded-rect sprites worked out once per size and colour, then only copied.
// A soft halo (or shadow) around r, fading out over `spread` px; moved by (dx, dy).
void softHalo(HDC dc, const RECT& r, float radius, COLORREF color, BYTE alpha, int spread, int dx = 0, int dy = 0);
void clearSprites();
// A keycap. `lit` 0-1 lights it from inside (the palette's light), `pressed` 0-1 sinks it,
// `hover` 0-1 brightens it a little. Contrast themes get a flat face and an outline.
void keycap(HDC dc, RECT r, float radius, const Palette& p, double lit, double pressed = 0, double hover = 0);
// A well: the panel sunk in, shaded along its top inner edge, lit along its lower edge.
void well(HDC dc, const RECT& r, float radius, const Palette& p);
// An LED cell: `lit` 0-1 blends `off` to `on` and adds a halo in `halo`.
void led(HDC dc, const RECT& r, COLORREF on, COLORREF halo, double lit, COLORREF off);
// A dot-matrix pixel icon: rows of '#' cells, each cell `unit` px with a small gap.
void pixelIcon(HDC dc, const char* const* rows, int count, int x, int y, int unit, COLORREF color);

bool startGraphics();
void stopGraphics();
bool graphicsReady();

void fillRound(HDC dc, const RECT& r, COLORREF color, float radius, BYTE alpha = 255);
void strokeRound(HDC dc, const RECT& r, COLORREF color, float radius, float width);

// Dot-matrix glyphs (digits and icons) carried over from the pixel heritage.
// 5x7 bitmap digits for 0-9 . % W - and space. Returns false (drawing nothing)
// if `text` holds any other character; width is 0 in that case.
// `gap` leaves that many pixels between cells, so the digits read as a dot matrix.
int pixelDigitsWidth(const std::wstring& text, int unit);
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color, int gap = 0);

// A square notification-area icon showing `percent` (0-100) in 3x5 pixel
// digits on a badge. The caller owns the icon (DestroyIcon).
HICON makePercentIcon(int size, int percent, COLORREF badge, COLORREF edge, COLORREF digits);

// The battery mascot: a 16x14-cell pixel sprite whose body fills with the
// battery level, with a face for each mood. `frame` 1 blinks.
enum class Mood { calm, sleepy, busy, charging, tired, critical, full };
// `face` is used on the filled part of the body, `faceLight` on the empty part.
struct MascotColors { COLORREF outline, empty, level, face, faceLight, cheek, sweat, accent; };
constexpr int MascotCells = 16, MascotRows = 14;
void drawMascot(HDC dc, int x, int y, int unit, Mood mood, double level, int frame, const MascotColors& colors);
// Saves a bitmap as PNG through GDI+.
bool saveBitmapPng(HBITMAP bitmap, const std::wstring& path);

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
