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
// While one of these lives, light and shadow drawn into `dc` stay inside `area` (logical
// coordinates): near an edge they are squeezed so they are gone when they reach it, and never
// cut. A window's own area, one row of a list. `shadowsOnly` holds in only the shadows: keys
// standing in a well keep their shadows inside it while their light still spills over its rim.
// They nest.
class LightBounds {
public:
    LightBounds(HDC dc, const RECT& area, bool shadowsOnly = false);
    LightBounds(const LightBounds&) = delete;
    LightBounds& operator=(const LightBounds&) = delete;
    ~LightBounds();
private:
    bool had_[2]; RECT previous_[2];
};
void clearSprites();
// A keycap. `lit` 0-1 lights it from inside (the palette's light), `pressed` 0-1 sinks it,
// `hover` 0-1 brightens it a little. Contrast themes get a flat face and an outline.
// `opacity` 0-1 fades the whole key in over what is already drawn (a key rising out of a flat
// row under the pointer, the light moving from one choice to the next).
void keycap(HDC dc, RECT r, float radius, const Palette& p, double lit, double pressed = 0, double hover = 0, double opacity = 1);
// A well: the panel sunk in, shaded along its top inner edge, lit along its lower edge.
void well(HDC dc, const RECT& r, float radius, const Palette& p);
// An LED cell: `lit` 0-1 blends `off` to `on` and adds a halo in `halo`.
void led(HDC dc, const RECT& r, COLORREF on, COLORREF halo, double lit, COLORREF off);
// A pool of light on a key's face under the pointer (`at`), clipped to the key.
void spotlight(HDC dc, const RECT& key, float radius, POINT at, COLORREF color, double strength, int reach);
// A ring of light spreading over a key from where it was pressed, clipped to the key. progress 0-1.
void ripple(HDC dc, const RECT& key, float radius, POINT at, COLORREF color, double progress);
// A dot-matrix pixel icon: rows of '#' cells, each cell `unit` px with a small gap.
// `gap` is the space left inside each cell (-1: a small one at larger sizes).
void pixelIcon(HDC dc, const char* const* rows, int count, int x, int y, int unit, COLORREF color, int gap = -1);

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
// The cells of `text` in the 5x7 digits: seven rows of '#' (lit) and '.' (unlit), with a column
// of ' ' between glyphs. False (rows untouched) if the text holds a character the digits lack.
bool pixelDigitRows(const std::wstring& text, std::string rows[7]);
// `ghost`, when given, is the colour of a glyph's unlit cells (a display's dark dots).
bool drawPixelDigits(HDC dc, const std::wstring& text, int x, int y, int unit, COLORREF color, int gap = 0, COLORREF ghost = CLR_INVALID);

// A square notification-area icon showing `percent` (0-100) in 3x5 pixel
// digits on a badge. The caller owns the icon (DestroyIcon).
HICON makePercentIcon(int size, int percent, COLORREF badge, COLORREF edge, COLORREF digits);
// The same badge holding a level instead of digits, for the tray icon's short shows: filled
// from the bottom to `fraction` (0-1), or with `steps` > 0 as that many bars of rising height,
// lit up to `fraction` of them. The caller owns the icon (DestroyIcon).
HICON makeLevelIcon(int size, double fraction, COLORREF badge, COLORREF edge, COLORREF fill, int steps = 0);

// The battery mascot: a 16x14-cell pixel sprite whose body fills with the
// battery level, with a face for each mood. `frame` 1 blinks.
enum class Mood { calm, sleepy, busy, charging, tired, critical, full };
// `face` is used on the filled part of the body, `faceLight` on the empty part.
struct MascotColors { COLORREF outline, empty, level, face, faceLight, cheek, sweat, accent; };
constexpr int MascotCells = 16, MascotRows = 14;
// `look` -1 or 1 turns its eyes a cell to the left or the right.
void drawMascot(HDC dc, int x, int y, int unit, Mood mood, double level, int frame, const MascotColors& colors, int look = 0);
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
