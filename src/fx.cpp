#include "fx.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bs::fx {
namespace {
constexpr double Pi = 3.14159265358979;
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double easeOut(double t) { t = clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
double easeInOut(double t) { t = clamp01(t); return t < .5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }
double smooth(double t) { t = clamp01(t); return t * t * (3 - 2 * t); }
COLORREF mix(COLORREF a, COLORREF b, double t) {
    t = clamp01(t);
    auto channel = [t](int x, int y) { return int(std::lround(x + (y - x) * t)); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}
// Source over, into a premultiplied pixel. `a` is 0-255.
inline void over(uint32_t& pixel, COLORREF c, uint32_t a) {
    if (!a) return;
    const uint32_t inv = 255 - a;
    const uint32_t sr = GetRValue(c) * a, sg = GetGValue(c) * a, sb = GetBValue(c) * a;
    const uint32_t da = pixel >> 24, dr = (pixel >> 16) & 255, dg = (pixel >> 8) & 255, db = pixel & 255;
    const uint32_t oa = a + (da * inv + 127) / 255;
    const uint32_t orr = (sr + dr * inv + 127) / 255, og = (sg + dg * inv + 127) / 255, ob = (sb + db * inv + 127) / 255;
    pixel = (std::min<uint32_t>(oa, 255) << 24) | (std::min(orr, oa) << 16) | (std::min(og, oa) << 8) | std::min(ob, oa);
}
// A small deterministic generator: the same seed gives the same sparks.
struct Random {
    uint32_t state;
    explicit Random(unsigned seed) : state(seed * 2654435761u + 0x9E3779B9u) {}
    double next() { state = state * 1664525u + 1013904223u; return ((state >> 8) & 0xFFFFFF) / double(0x1000000); }
    double range(double a, double b) { return a + (b - a) * next(); }
};
void grow(RECT& dirty, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) dirty = {x0, y0, x1, y1};
    else { dirty.left = std::min<LONG>(dirty.left, x0); dirty.top = std::min<LONG>(dirty.top, y0); dirty.right = std::max<LONG>(dirty.right, x1); dirty.bottom = std::max<LONG>(dirty.bottom, y1); }
}
RECT join(const RECT& a, const RECT& b) {
    RECT r = a; grow(r, b.left, b.top, b.right, b.bottom); return r;
}
}

void Canvas::clear(const RECT& area) {
    if (!px) return;
    const int x0 = std::max<int>(0, area.left), y0 = std::max<int>(0, area.top), x1 = std::min<int>(w, area.right), y1 = std::min<int>(h, area.bottom);
    if (x1 <= x0) return;
    for (int y = y0; y < y1; ++y) std::memset(px + size_t(y) * size_t(w) + x0, 0, size_t(x1 - x0) * 4);
}
void Canvas::fill(double fx0, double fy0, double fx1, double fy1, COLORREF color, double alpha) {
    if (!px || alpha <= 0) return;
    const int x0 = std::max(0, int(std::lround(fx0 - origin.x))), y0 = std::max(0, int(std::lround(fy0 - origin.y)));
    const int x1 = std::min(w, int(std::lround(fx1 - origin.x))), y1 = std::min(h, int(std::lround(fy1 - origin.y)));
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t a = uint32_t(std::lround(clamp01(alpha) * 255));
    for (int y = y0; y < y1; ++y) { uint32_t* row = px + size_t(y) * size_t(w); for (int x = x0; x < x1; ++x) over(row[x], color, a); }
    grow(dirty, x0, y0, x1, y1);
}
void Canvas::glow(double sx, double sy, double radius, COLORREF color, double alpha) {
    if (!px || alpha <= 0 || radius <= 0) return;
    const double cx = sx - origin.x, cy = sy - origin.y;
    const int x0 = std::max(0, int(std::floor(cx - radius))), y0 = std::max(0, int(std::floor(cy - radius)));
    const int x1 = std::min(w, int(std::ceil(cx + radius)) + 1), y1 = std::min(h, int(std::ceil(cy + radius)) + 1);
    if (x1 <= x0 || y1 <= y0) return;
    const double r2 = radius * radius, strength = clamp01(alpha) * 255;
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = px + size_t(y) * size_t(w);
        const double dy = y + .5 - cy;
        for (int x = x0; x < x1; ++x) {
            const double dx = x + .5 - cx, d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            const double f = 1 - d2 / r2;
            over(row[x], color, uint32_t(strength * f * f));
        }
    }
    grow(dirty, x0, y0, x1, y1);
}
void Canvas::ring(double sx, double sy, double radius, double width, COLORREF color, double alpha) {
    if (!px || alpha <= 0 || radius <= 0 || width <= 0) return;
    const double cx = sx - origin.x, cy = sy - origin.y, half = width / 2, outer = radius + half, inner = std::max(0.0, radius - half);
    const int y0 = std::max(0, int(std::floor(cy - outer))), y1 = std::min(h, int(std::ceil(cy + outer)) + 1);
    const int bx0 = std::max(0, int(std::floor(cx - outer))), bx1 = std::min(w, int(std::ceil(cx + outer)) + 1);
    if (y1 <= y0 || bx1 <= bx0) return;
    const double strength = clamp01(alpha) * 255;
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = px + size_t(y) * size_t(w);
        const double dy = y + .5 - cy, o2 = outer * outer - dy * dy;
        if (o2 <= 0) continue;
        const double xo = std::sqrt(o2), i2 = inner * inner - dy * dy, xi = i2 > 0 ? std::sqrt(i2) : 0;
        // Only the two arcs of this row that lie in the ring.
        const int spans[2][2] = {{int(std::floor(cx - xo)), int(std::ceil(cx - xi)) + 1}, {int(std::floor(cx + xi)), int(std::ceil(cx + xo)) + 1}};
        int done = bx0;
        for (const auto& span : spans) {
            const int a = std::max({span[0], bx0, done}), b = std::min(span[1], bx1);
            for (int x = a; x < b; ++x) {
                const double dx = x + .5 - cx, d = std::sqrt(dx * dx + dy * dy), f = 1 - std::abs(d - radius) / half;
                if (f > 0) over(row[x], color, uint32_t(strength * f * f));
            }
            done = std::max(done, b);
        }
    }
    grow(dirty, bx0, y0, bx1, y1);
}

// ----- Effects -----
namespace {
struct Spark { double angle, speed, delay, size, phase; };
class Burst : public Effect {
public:
    POINT origin{}; COLORREF core = 0, halo = 0; double reach = 0, cell = 1, gravity = 0;
    std::vector<Spark> sparks;
    void draw(Canvas& c, double t) const override {
        for (const auto& s : sparks) {
            const double local = (t - s.delay) / (1 - s.delay);
            if (local <= 0 || local >= 1) continue;
            // Sparks move on the cell grid, as lit dots of a matrix would; two dimmer cells trail each.
            auto place = [&](double at, double& x, double& y) {
                const double travel = reach * s.speed * (1 - (1 - at) * (1 - at));
                x = std::round((origin.x + std::cos(s.angle) * travel) / cell) * cell;
                y = std::round((origin.y + std::sin(s.angle) * travel + gravity * at * at) / cell) * cell;
            };
            const double fade = std::pow(1 - local, 1.2) * (.8 + .2 * std::sin(local * 38 + s.phase));
            const double side = cell * s.size * (local < .75 ? 1 : (1 - local) / .25);
            double x = 0, y = 0;
            for (int k = 2; k >= 1; --k) {
                const double behind = local - k * .07;
                if (behind <= 0) continue;
                place(behind, x, y);
                c.fill(x - side * .4, y - side * .4, x + side * .4, y + side * .4, halo, fade * (.55 - k * .17));
            }
            place(local, x, y);
            c.glow(x, y, cell * s.size * 4.4, halo, fade * .5);
            c.fill(x - side / 2, y - side / 2, x + side / 2, y + side / 2, core, fade);
        }
    }
};
class Ring : public Effect {
public:
    POINT center{}; COLORREF color = 0; double r0 = 0, r1 = 0, width = 1, alpha = .8;
    void draw(Canvas& c, double t) const override {
        // A sharp line of light with a soft, wider glow round it.
        const double radius = r0 + (r1 - r0) * easeOut(t), level = alpha * std::pow(1 - t, 1.2);
        c.ring(center.x, center.y, radius, width * 6, color, level * .38);
        c.ring(center.x, center.y, radius, width * (1 + t), mix(color, RGB(255, 255, 255), .45), level);
    }
};
// How strongly each pixel round a rounded rect is lit (0-255), worked out once: a bloom then
// only scales it frame by frame. The last one is kept, since the panel rarely changes size.
struct HaloMap { int w = 0, h = 0, spread = 0, corner = 0; std::vector<uint8_t> level; };
std::shared_ptr<const HaloMap> haloMap(int w, int h, int corner, int spread) {
    static std::shared_ptr<const HaloMap> last;
    if (last && last->w == w && last->h == h && last->corner == corner && last->spread == spread) return last;
    auto map = std::make_shared<HaloMap>();
    map->w = w; map->h = h; map->corner = corner; map->spread = spread;
    const int W = w + spread * 2, H = h + spread * 2;
    map->level.assign(size_t(W) * size_t(H), 0);
    const double hw = w / 2.0, hh = h / 2.0, r = std::min({double(corner), hw, hh}), cx = W / 2.0, cy = H / 2.0;
    for (int y = 0; y < H; ++y) {
        const double py = std::abs(y + .5 - cy);
        for (int x = 0; x < W; ++x) {
            if (py < hh - r && x >= spread && x < spread + w) { x = spread + w - 1; continue; } // inside the rect: nothing
            const double qx = std::abs(x + .5 - cx) - (hw - r), qy = py - (hh - r);
            const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
            if (d <= 0 || d >= spread) continue;
            const double f = 1 - d / spread;
            map->level[size_t(y) * size_t(W) + size_t(x)] = uint8_t(std::lround(f * f * 255));
        }
    }
    last = map;
    return last;
}
class Bloom : public Effect {
public:
    RECT rect{}; double alpha = .6; COLORREF color = 0;
    std::shared_ptr<const HaloMap> map;
    void draw(Canvas& c, double t) const override {
        if (!c.px || !map) return;
        // Quick to come up (a fifth of its life), slow to settle away.
        const double level = alpha * (t < .2 ? easeOut(t / .2) : 1 - smooth((t - .2) / .8));
        const uint32_t scale = uint32_t(std::lround(clamp01(level) * 256));
        if (!scale) return;
        const int W = map->w + map->spread * 2, H = map->h + map->spread * 2;
        const int ox = rect.left - map->spread - c.origin.x, oy = rect.top - map->spread - c.origin.y;
        const int x0 = std::max(0, ox), x1 = std::min(c.w, ox + W), y0 = std::max(0, oy), y1 = std::min(c.h, oy + H);
        if (x1 <= x0 || y1 <= y0) return;
        const int innerTop = oy + map->spread + map->corner, innerBottom = oy + map->spread + map->h - map->corner;
        const int innerLeft = ox + map->spread, innerRight = ox + map->spread + map->w;
        // The colour at every strength, premultiplied: most of the halo lands on clear pixels and is just written.
        uint32_t lit[256];
        for (uint32_t a = 0; a < 256; ++a) lit[a] = (a << 24) | ((GetRValue(color) * a / 255) << 16) | ((GetGValue(color) * a / 255) << 8) | (GetBValue(color) * a / 255);
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = c.px + size_t(y) * size_t(c.w);
            const uint8_t* levels = map->level.data() + size_t(y - oy) * size_t(W) - ox;
            auto run = [&](int a, int b) {
                for (int x = std::max(a, x0); x < std::min(b, x1); ++x) {
                    if (!levels[x]) continue;
                    const uint32_t strength = (levels[x] * scale) >> 8;
                    if (!row[x]) row[x] = lit[strength]; else over(row[x], color, strength);
                }
            };
            if (y >= innerTop && y < innerBottom) { run(x0, innerLeft); run(innerRight, x1); } // beside the straight sides: two strips
            else run(x0, x1);
        }
        grow(c.dirty, x0, y0, x1, y1);
    }
};
class Sweep : public Effect {
public:
    RECT rect{}; COLORREF color = 0; double band = 1, alpha = .3;
    void draw(Canvas& c, double t) const override {
        // A band of light crossing the rect from its bottom to its top, brightest in the middle.
        if (!c.px) return;
        const double height = rect.bottom - rect.top, middle = rect.bottom + band / 2 - (height + band) * easeInOut(t);
        const int x0 = std::max<int>(0, rect.left - c.origin.x), x1 = std::min<int>(c.w, rect.right - c.origin.x);
        const int y0 = std::max<int>({0, int(rect.top) - int(c.origin.y), int(middle - band / 2) - int(c.origin.y)});
        const int y1 = std::min<int>({c.h, int(rect.bottom) - int(c.origin.y), int(middle + band / 2) - int(c.origin.y) + 1});
        if (x1 <= x0 || y1 <= y0) return;
        const double level = alpha * std::sin(Pi * clamp01(t));
        for (int y = y0; y < y1; ++y) {
            const double f = 1 - std::abs(y + c.origin.y + .5 - middle) / (band / 2);
            if (f <= 0) continue;
            const uint32_t a = uint32_t(std::lround(clamp01(level * f * f) * 255));
            uint32_t* row = c.px + size_t(y) * size_t(c.w);
            for (int x = x0; x < x1; ++x) over(row[x], color, a);
        }
        grow(c.dirty, x0, y0, x1, y1);
    }
};
struct Flight { double delay, bend, size; };
class Streak : public Effect {
public:
    POINT from{}, to{}; COLORREF core = 0, halo = 0; double cell = 1;
    std::vector<Flight> flights;
    void draw(Canvas& c, double t) const override {
        const double dx = to.x - from.x, dy = to.y - from.y, length = std::hypot(dx, dy);
        const double nx = length > 0 ? -dy / length : 0, ny = length > 0 ? dx / length : 0;
        for (const auto& f : flights) {
            for (int k = 0; k < 4; ++k) { // the head, then three dimmer cells behind it
                const double local = (t - f.delay) / (1 - f.delay) - k * .07;
                if (local <= 0 || local >= 1) continue;
                const double p = easeInOut(local), arc = std::sin(Pi * p) * f.bend * length;
                double x = from.x + dx * p + nx * arc, y = from.y + dy * p + ny * arc;
                x = std::round(x / cell) * cell; y = std::round(y / cell) * cell;
                const double fade = (1 - k * .26) * std::min(1.0, (1 - local) * 5) * std::min(1.0, local * 8);
                const double side = cell * f.size * (1 - k * .12);
                if (!k) c.glow(x, y, cell * f.size * 4.6, halo, fade * .55);
                c.fill(x - side / 2, y - side / 2, x + side / 2, y + side / 2, k ? halo : core, fade);
            }
        }
    }
};
}
std::unique_ptr<Effect> burst(POINT origin, COLORREF core, COLORREF halo, int count, double reach, double cell, double life, unsigned seed,
    double direction, double fan, double gravity) {
    auto effect = std::make_unique<Burst>();
    effect->origin = origin; effect->core = core; effect->halo = halo; effect->reach = reach; effect->cell = std::max(1.0, cell);
    effect->gravity = gravity; effect->life = life;
    Random random(seed);
    for (int i = 0; i < count; ++i)
        effect->sparks.push_back({direction + random.range(-fan / 2, fan / 2), random.range(.35, 1), random.range(0, .18), random.range(.8, 1.5), random.range(0, 6.28)});
    return effect;
}
std::unique_ptr<Effect> ring(POINT center, COLORREF color, double r0, double r1, double width, double life, double alpha) {
    auto effect = std::make_unique<Ring>();
    effect->center = center; effect->color = color; effect->r0 = r0; effect->r1 = r1; effect->width = width; effect->life = life; effect->alpha = alpha;
    return effect;
}
std::unique_ptr<Effect> bloom(RECT r, double corner, double spread, COLORREF color, double life, double alpha) {
    auto effect = std::make_unique<Bloom>();
    effect->rect = r; effect->color = color; effect->life = life; effect->alpha = alpha;
    effect->map = haloMap(std::max<int>(1, r.right - r.left), std::max<int>(1, r.bottom - r.top), std::max(0, int(std::lround(corner))), std::max(1, int(std::lround(spread))));
    return effect;
}
std::unique_ptr<Effect> sweep(RECT r, COLORREF color, double band, double life, double alpha) {
    auto effect = std::make_unique<Sweep>();
    effect->rect = r; effect->color = color; effect->band = std::max(2.0, band); effect->life = life; effect->alpha = alpha;
    return effect;
}
std::unique_ptr<Effect> streak(POINT from, POINT to, COLORREF core, COLORREF halo, int count, double cell, double life, unsigned seed) {
    auto effect = std::make_unique<Streak>();
    effect->from = from; effect->to = to; effect->core = core; effect->halo = halo; effect->cell = std::max(1.0, cell); effect->life = life;
    Random random(seed);
    for (int i = 0; i < count; ++i) effect->flights.push_back({random.range(0, .45), random.range(-.28, .28), random.range(.8, 1.4)});
    return effect;
}

// ----- Stage -----
void Stage::bind(uint32_t* pixels, int w, int h, POINT origin) {
    const bool same = canvas_.px == pixels && canvas_.w == w && canvas_.h == h && canvas_.origin.x == origin.x && canvas_.origin.y == origin.y;
    if (!same) { effects_.clear(); revealing_ = false; picture_.clear(); picture_.shrink_to_fit(); }
    memory_.clear(); memory_.shrink_to_fit();
    canvas_.px = pixels; canvas_.w = pixels ? w : 0; canvas_.h = pixels ? h : 0; canvas_.origin = origin; canvas_.dirty = {};
    if (!same) last_ = {};
}
void Stage::own(const RECT& area) {
    effects_.clear(); revealing_ = false; picture_.clear();
    const int w = std::max<int>(0, area.right - area.left), h = std::max<int>(0, area.bottom - area.top);
    memory_.assign(size_t(w) * size_t(h), 0);
    canvas_.px = memory_.empty() ? nullptr : memory_.data(); canvas_.w = w; canvas_.h = h; canvas_.origin = {area.left, area.top}; canvas_.dirty = {};
    last_ = {};
}
void Stage::add(std::unique_ptr<Effect> effect, double now, double delay) {
    if (!effect || !canvas_.px) return;
    effect->start = now + delay;
    effects_.push_back(std::move(effect));
}
void Stage::reveal(std::vector<uint32_t> pixels, int w, int h, POINT at, POINT center, double radius, double life, COLORREF ringColor, double ringWidth, double now) {
    if (!canvas_.px || w <= 0 || h <= 0 || pixels.size() < size_t(w) * size_t(h)) return;
    picture_ = std::move(pixels); pictureW_ = w; pictureH_ = h; pictureAt_ = at; revealCenter_ = center;
    revealRadius_ = radius; revealStart_ = now; revealLife_ = std::max(1.0, life); revealColor_ = ringColor; revealWidth_ = ringWidth;
    revealing_ = true;
}
void Stage::stop() {
    effects_.clear(); revealing_ = false; picture_.clear(); picture_.shrink_to_fit();
    if (canvas_.px) canvas_.clear({0, 0, canvas_.w, canvas_.h});
    canvas_.dirty = {}; last_ = {};
}
bool Stage::render(double now, RECT* changed) {
    if (!canvas_.px) { if (changed) *changed = {}; effects_.clear(); revealing_ = false; return false; }
    canvas_.clear(last_);
    canvas_.dirty = {};
    if (revealing_) {
        const double t = (now - revealStart_) / revealLife_;
        if (t >= 1) { revealing_ = false; picture_.clear(); picture_.shrink_to_fit(); }
        else {
            // The old picture with a hole growing from the centre: each row is copied, then the
            // part inside the circle is cleared.
            const double grown = 1 - (1 - std::max(0.0, t)) * (1 - std::max(0.0, t)); // quick, but slow enough to watch
            const double radius = revealRadius_ * grown;
            const int ox = pictureAt_.x - canvas_.origin.x, oy = pictureAt_.y - canvas_.origin.y;
            const double cx = revealCenter_.x - canvas_.origin.x, cy = revealCenter_.y - canvas_.origin.y;
            const int x0 = std::max(0, ox), x1 = std::min(canvas_.w, ox + pictureW_);
            const int y0 = std::max(0, oy), y1 = std::min(canvas_.h, oy + pictureH_);
            for (int y = y0; y < y1 && x1 > x0; ++y) {
                uint32_t* row = canvas_.px + size_t(y) * size_t(canvas_.w);
                std::memcpy(row + x0, picture_.data() + size_t(y - oy) * size_t(pictureW_) + size_t(x0 - ox), size_t(x1 - x0) * 4);
                const double dy = y + .5 - cy, inside = radius * radius - dy * dy;
                if (inside <= 0) continue;
                const double half = std::sqrt(inside);
                const int a = std::max(x0, int(std::ceil(cx - half))), b = std::min(x1, int(std::floor(cx + half)));
                if (b > a) std::memset(row + a, 0, size_t(b - a) * 4);
            }
            grow(canvas_.dirty, x0, y0, x1, y1);
            canvas_.ring(revealCenter_.x, revealCenter_.y, radius, revealWidth_, revealColor_, .95 * std::pow(1 - t, .7));
            canvas_.ring(revealCenter_.x, revealCenter_.y, radius, revealWidth_ * 4, revealColor_, .35 * std::pow(1 - t, .7));
        }
    }
    for (auto it = effects_.begin(); it != effects_.end();) {
        const Effect& effect = **it;
        if (now < effect.start) { ++it; continue; }
        const double t = effect.life > 0 ? (now - effect.start) / effect.life : 1;
        if (t >= 1) { it = effects_.erase(it); continue; }
        effect.draw(canvas_, t);
        ++it;
    }
    // Near the canvas's own border everything fades out, so light leaving the area is never cut off.
    if (feather_ > 0 && !canvas_.clean()) {
        const RECT d = canvas_.dirty;
        for (int y = d.top; y < d.bottom; ++y) {
            const int edgeY = std::min(y, canvas_.h - 1 - y);
            uint32_t* row = canvas_.px + size_t(y) * size_t(canvas_.w);
            for (int x = d.left; x < d.right; ++x) {
                const int edge = std::min({edgeY, x, canvas_.w - 1 - x});
                if (edge >= feather_) { if (edgeY >= feather_ && x >= feather_) x = std::max(x, std::min<int>(d.right, canvas_.w - feather_) - 1); continue; }
                const uint32_t p = row[x];
                if (!p) continue;
                const uint32_t f = uint32_t(edge * 256 / feather_);
                row[x] = ((((p >> 24) * f) >> 8) << 24) | (((((p >> 16) & 255) * f) >> 8) << 16) | (((((p >> 8) & 255) * f) >> 8) << 8) | (((p & 255) * f) >> 8);
            }
        }
    }
    if (changed) *changed = join(last_, canvas_.dirty);
    last_ = canvas_.dirty;
    return active();
}

// ----- Edge lights -----
void EdgeLights::layout(const RECT& monitor, int thickness, bool allocate) {
    stop();
    monitor_ = monitor; thickness_ = std::max(2, thickness);
    const int t = thickness_;
    strips_[0] = {monitor.left, monitor.top, monitor.right, monitor.top + t};                       // top
    strips_[1] = {monitor.right - t, monitor.top + t, monitor.right, monitor.bottom - t};           // right
    strips_[2] = {monitor.left, monitor.bottom - t, monitor.right, monitor.bottom};                 // bottom
    strips_[3] = {monitor.left, monitor.top + t, monitor.left + t, monitor.bottom - t};             // left
    for (int i = 0; i < 4; ++i) {
        const int w = std::max<int>(0, strips_[i].right - strips_[i].left), h = std::max<int>(0, strips_[i].bottom - strips_[i].top);
        if (allocate) memory_[i].assign(size_t(w) * size_t(h), 0); else { memory_[i].clear(); memory_[i].shrink_to_fit(); }
        canvases_[i] = Canvas{};
        canvases_[i].px = memory_[i].empty() ? nullptr : memory_[i].data(); canvases_[i].w = w; canvases_[i].h = h; canvases_[i].origin = {strips_[i].left, strips_[i].top};
    }
}
void EdgeLights::bind(int index, uint32_t* pixels) {
    memory_[index].clear(); memory_[index].shrink_to_fit();
    canvases_[index].px = pixels;
    if (!pixels) { canvases_[index].w = canvases_[index].h = 0; }
    else { canvases_[index].w = strips_[index].right - strips_[index].left; canvases_[index].h = strips_[index].bottom - strips_[index].top; }
}
void EdgeLights::play(EdgeShow show, COLORREF color, double life, double now, double reach, int pulses) {
    show_ = show; color_ = color; life_ = std::max(1.0, life); start_ = now; reach_ = clamp01(reach); pulses_ = std::max(1, pulses);
    playing_ = true;
}
void EdgeLights::stop() {
    playing_ = false;
    if (drawn_) for (auto& c : canvases_) if (c.px) c.clear({0, 0, c.w, c.h});
    drawn_ = false;
}
double EdgeLights::intensity(double u, double t) const {
    switch (show_) {
    case EdgeShow::connect: {
        // Two lights leave the bottom centre, run round and meet at the top centre at t = .62;
        // what they pass stays softly lit; the meeting flashes the whole border, then all fades.
        const double travel = .62, head = t < travel ? easeInOut(t / travel) : 1;
        const double after = t < travel ? 1 : 1 - smooth((t - travel) / (1 - travel));
        double light = 0;
        if (u <= head) light = std::exp(-(head - u) / .16) * (t < travel ? 1 : std::max(0.0, 1 - (t - travel) / .2)) + .4 * after;
        const double meet = (t - travel) / .3;
        if (meet > 0 && meet < 1) light += .5 * std::sin(Pi * meet);
        return clamp01(light);
    }
    case EdgeShow::disconnect: {
        // The lit border drains from the top centre down both sides to the bottom centre.
        const double cut = 1 - easeInOut(t / .78);
        if (cut <= 0) return 0;
        double light = u <= cut ? .62 * (1 - .4 * t) * clamp01((cut - u) / .06) : 0;
        light += .6 * std::exp(-std::abs(cut - u) / .05) * (1 - t);
        return clamp01(light);
    }
    case EdgeShow::burst: {
        const double front = reach_ * easeOut(t / .6);
        if (u > front) return 0;
        return clamp01(std::pow(1 - t, 1.2) * (.45 + .55 * std::exp(-(front - u) / (.07 + .05 * reach_))));
    }
    case EdgeShow::pulse: {
        const double wave = std::sin(Pi * t * pulses_);
        return clamp01(.9 * wave * wave * (1 - .25 * t));
    }
    }
    return 0;
}
bool EdgeLights::render(double now) {
    if (!playing_) return false;
    const double t = (now - start_) / life_;
    for (auto& c : canvases_) if (c.px && drawn_) c.clear({0, 0, c.w, c.h});
    drawn_ = false;
    if (t >= 1) { playing_ = false; return false; }
    if (t < 0) return true;
    const double W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top, half = W + H;
    if (W <= 0 || H <= 0) { playing_ = false; return false; }
    const int T = thickness_;
    std::vector<double> depth(size_t(T), 0.0); // the light is strongest at the screen's edge and gone at the strip's inner side
    const int line = std::max(2, T / 24); // a bright line on the very edge, a wide soft glow behind it
    for (int k = 0; k < T; ++k) depth[size_t(k)] = k < line ? 1.0 : .6 * std::pow(1 - double(k - line) / (T - line), 2.4);
    const COLORREF hot = mix(color_, RGB(255, 255, 255), .45);
    for (int i = 0; i < 4; ++i) {
        Canvas& c = canvases_[i];
        if (!c.px) continue;
        const bool horizontal = i == 0 || i == 2;
        const int along = horizontal ? c.w : c.h;
        for (int p = 0; p < along; ++p) {
            double u;
            if (i == 2) u = std::abs(p + .5 - W / 2) / half;                                        // bottom: 0 at its centre
            else if (i == 0) u = (W / 2 + H + (W / 2 - std::abs(p + .5 - W / 2))) / half;           // top: 1 at its centre
            else u = (W / 2 + (H - (p + T + .5))) / half;                                          // sides, from the bottom up
            const double light = intensity(u, t);
            if (light <= .004) continue;
            const COLORREF color = mix(color_, hot, light);
            const uint32_t r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
            // In a corner the nearer of the two edges counts, so the glow turns the corner unbroken.
            const int toSide = horizontal ? std::min(p, along - 1 - p) : T;
            for (int k = 0; k < T; ++k) {
                const int d = std::min((i == 0 || i == 3) ? k : T - 1 - k, toSide); // distance from the screen's edge
                const uint32_t a = uint32_t(light * depth[size_t(d)] * 240);
                if (!a) continue;
                // The strips were just cleared and hold nothing else: the premultiplied pixel is written as is.
                (horizontal ? c.px[size_t(k) * size_t(c.w) + size_t(p)] : c.px[size_t(p) * size_t(c.w) + size_t(k)]) =
                    (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
            }
        }
    }
    drawn_ = true;
    return true;
}

// ----- Overlay window -----
namespace {
constexpr wchar_t OverlayClass[] = L"BatteryScope.Fx.v1";
LRESULT CALLBACK overlayProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wp, lp);
}
bool registerOverlay() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = overlayProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = OverlayClass;
    registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}
}
Overlay::~Overlay() {
    hide();
    if (window_) DestroyWindow(window_);
}
bool Overlay::show(const RECT& area) {
    const SIZE size{area.right - area.left, area.bottom - area.top};
    if (size.cx <= 0 || size.cy <= 0 || size.cx > 16384 || size.cy > 16384 || !registerOverlay()) return false;
    if (!window_) {
        // Layered and transparent to the mouse, above everything, never active, absent from the taskbar and Alt+Tab.
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, OverlayClass, L"",
            WS_POPUP, area.left, area.top, size.cx, size.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return false;
    }
    if (!bitmap_ || size.cx != size_.cx || size.cy != size_.cy) {
        if (bitmap_) { SelectObject(dc_, original_); DeleteObject(bitmap_); bitmap_ = nullptr; bits_ = nullptr; }
        if (!dc_) { HDC screen = GetDC(nullptr); dc_ = CreateCompatibleDC(screen); ReleaseDC(nullptr, screen); }
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = -size.cy; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap_ = dc_ ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
        if (!bitmap_ || !bits_) { if (bitmap_) DeleteObject(bitmap_); bitmap_ = nullptr; bits_ = nullptr; return false; }
        original_ = SelectObject(dc_, bitmap_);
        std::memset(bits_, 0, size_t(size.cx) * size_t(size.cy) * 4);
    }
    size_ = size; origin_ = {area.left, area.top};
    SetWindowPos(window_, HWND_TOPMOST, area.left, area.top, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    present();
    return true;
}
void Overlay::present(const RECT* changed) {
    if (!window_ || !bitmap_) return;
    RECT dirty{0, 0, size_.cx, size_.cy};
    if (changed) {
        if (changed->right <= changed->left || changed->bottom <= changed->top) return; // nothing changed
        dirty.left = std::max<LONG>(0, changed->left); dirty.top = std::max<LONG>(0, changed->top);
        dirty.right = std::min<LONG>(size_.cx, changed->right); dirty.bottom = std::min<LONG>(size_.cy, changed->bottom);
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return;
    }
    GdiFlush();
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{}; info.cbSize = sizeof(info);
    info.pptDst = &origin_; info.psize = &size_; info.hdcSrc = dc_; info.pptSrc = &source; info.pblend = &blend; info.dwFlags = ULW_ALPHA;
    info.prcDirty = changed ? &dirty : nullptr;
    if (!UpdateLayeredWindowIndirect(window_, &info)) UpdateLayeredWindow(window_, nullptr, &origin_, &size_, dc_, &source, 0, &blend, ULW_ALPHA);
}
void Overlay::hide() {
    if (window_ && IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
    if (bitmap_) { SelectObject(dc_, original_); DeleteObject(bitmap_); bitmap_ = nullptr; bits_ = nullptr; }
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    size_ = {};
}
}
