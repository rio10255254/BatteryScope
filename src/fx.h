#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <memory>
#include <vector>

// Effects that leave the panel's window: sparks, rings of light, a halo round the panel, the
// old theme wiped away in a growing circle, and light running round the edge of the screen.
// Everything is drawn in software into premultiplied BGRA pixels and shown in click-through
// layered windows that exist only while something is moving. Nothing here knows about the
// panel: positions are screen coordinates, colours and sizes are given by the caller, and
// randomness is seeded, so a frame at a given time is always the same picture (tests and
// frame dumps rely on it).
namespace bs::fx {

// Premultiplied BGRA pixels, top-down. Drawing takes screen coordinates (the canvas's top left
// pixel is at `origin`), clips to the canvas and grows `dirty` (canvas coordinates).
struct Canvas {
    uint32_t* px = nullptr; int w = 0, h = 0;
    POINT origin{};
    RECT dirty{};
    bool clean() const { return dirty.right <= dirty.left || dirty.bottom <= dirty.top; }
    void clear(const RECT& area);                                  // canvas coordinates, to transparent
    void fill(double x0, double y0, double x1, double y1, COLORREF color, double alpha);
    void glow(double cx, double cy, double radius, COLORREF color, double alpha);            // a soft disc
    void ring(double cx, double cy, double radius, double width, COLORREF color, double alpha); // a soft circle line
};

class Effect {
public:
    double start = 0, life = 0; // ms
    virtual ~Effect() = default;
    virtual void draw(Canvas& canvas, double t) const = 0; // t: 0 to 1
};
// Dot-matrix sparks thrown from a point: `count` cells of `cell` px that fly up to `reach` px,
// snapped to the cell grid, glowing in `halo` with a `core` centre. `direction` and `fan`
// (radians) aim them; `gravity` (px) pulls them down over their life.
std::unique_ptr<Effect> burst(POINT origin, COLORREF core, COLORREF halo, int count, double reach, double cell, double life,
    unsigned seed, double direction = -1.5707963, double fan = 6.2831853, double gravity = 0);
// A circle of light growing from r0 to r1.
std::unique_ptr<Effect> ring(POINT center, COLORREF color, double r0, double r1, double width, double life, double alpha = .8);
// A halo round a rounded rect that blooms quickly and settles away.
std::unique_ptr<Effect> bloom(RECT r, double corner, double spread, COLORREF color, double life, double alpha = .6);
// A band of light `band` px tall crossing a rect from its bottom to its top.
std::unique_ptr<Effect> sweep(RECT r, COLORREF color, double band, double life, double alpha = .3);
// Sparks travelling from one point to another along gentle curves, each with a short trail.
std::unique_ptr<Effect> streak(POINT from, POINT to, COLORREF core, COLORREF halo, int count, double cell, double life, unsigned seed);

// The effects over one area of the screen.
class Stage {
public:
    // Draws into `pixels` (w*h, the overlay's bitmap) for the screen area whose top left is `origin`.
    // Changing the area drops whatever was running.
    void bind(uint32_t* pixels, int w, int h, POINT origin);
    void own(const RECT& screenArea); // or into its own memory (tests, frame dumps)
    void add(std::unique_ptr<Effect> effect, double now, double delay = 0);
    // The picture `pixels` (w*h, opaque BGRA, screen position `at`) is shown and wiped away in a
    // circle growing from `center` to `radius` over `life` ms, a ring of light on its edge.
    void reveal(std::vector<uint32_t> pixels, int w, int h, POINT at, POINT center, double radius, double life, COLORREF ringColor, double ringWidth, double now);
    bool active() const { return !effects_.empty() || revealing_; }
    // Draws the frame for `now`. False when nothing is left (the canvas is then clear).
    // `changed` receives what differs from the previous frame (canvas coordinates).
    bool render(double now, RECT* changed = nullptr);
    void stop(); // drops everything and clears the canvas
    void feather(int px) { feather_ = px; } // everything fades out over this many pixels at the canvas's border
    const Canvas& canvas() const { return canvas_; }
private:
    Canvas canvas_;
    std::vector<uint32_t> memory_;
    std::vector<std::unique_ptr<Effect>> effects_;
    RECT last_{};
    int feather_ = 0;
    bool revealing_ = false;
    std::vector<uint32_t> picture_; int pictureW_ = 0, pictureH_ = 0; POINT pictureAt_{}, revealCenter_{};
    double revealRadius_ = 0, revealStart_ = 0, revealLife_ = 0, revealWidth_ = 0; COLORREF revealColor_ = 0;
};

// Light running round the border of a screen, drawn in four strips (top, right, bottom, left).
// Positions along the border are measured from the bottom centre (0) both ways up to the top
// centre (1).
enum class EdgeShow {
    connect,    // two lights run from the bottom centre round both sides, meet at the top, and the border glows and fades
    disconnect, // the border glows and drains from the top down to the bottom centre
    burst,      // a burst from the bottom centre outward to `reach` of the way
    pulse,      // the whole border pulses `pulses` times
    mode        // a performance mode change: two lights run round the whole border and meet at the top, the
                // meeting blooms and sends an echo back down, and the border breathes out. `level` 0 (silent,
                // calm), 1 (balanced) or 2 (turbo, quicker and hotter) sets its character
};
class EdgeLights {
public:
    // `allocate` false leaves the strips without memory until bind() gives each its bitmap.
    void layout(const RECT& monitor, int thickness, bool allocate = true);
    RECT strip(int index) const { return strips_[index]; } // screen coordinates
    void bind(int index, uint32_t* pixels);                // the strip's bitmap; else own memory
    void play(EdgeShow show, COLORREF color, double life, double now, double reach = 1, int pulses = 1, int level = 0);
    bool active() const { return playing_; }
    bool render(double now); // false when the show is over (the strips are then clear)
    void stop();
    const Canvas& canvas(int index) const { return canvases_[index]; }
    RECT changed(int index) const; // the part of strip `index` the last render changed (canvas coordinates)
    double intensity(double u, double t) const; // light at border position u (0-1) at time t (0-1)
    double depth(double light, double t) const;  // how far in from the edge the glow reaches (0-1 of the strip)
private:
    RECT monitor_{}, strips_[4]{};
    int thickness_ = 0;
    Canvas canvases_[4];
    std::vector<uint32_t> memory_[4];
    bool playing_ = false, drawn_ = false;
    int drawnDepth_[4]{}, shownDepth_[4]{}; // how far in from the screen's edge this frame and the one before drew, per strip
    EdgeShow show_ = EdgeShow::pulse; COLORREF color_ = 0; double start_ = 0, life_ = 0, reach_ = 1; int pulses_ = 1, level_ = 0;
};

// A click-through, always-on-top layered window holding one bitmap with per-pixel alpha. It
// never takes focus or input and is hidden (and its bitmap freed) when nothing is shown.
class Overlay {
public:
    Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    ~Overlay();
    bool show(const RECT& screenArea); // creates the window and a cleared bitmap of that size
    uint32_t* pixels() const { return static_cast<uint32_t*>(bits_); }
    SIZE size() const { return size_; }
    POINT origin() const { return origin_; }
    bool visible() const { return bitmap_ != nullptr; }
    void present(const RECT* changed = nullptr); // pushes the bitmap (or just the changed part) to the screen
    void hide();
    HWND window() const { return window_; }
private:
    HWND window_ = nullptr; HDC dc_ = nullptr; HBITMAP bitmap_ = nullptr; HGDIOBJ original_ = nullptr; void* bits_ = nullptr;
    SIZE size_{}; POINT origin_{};
};
}
