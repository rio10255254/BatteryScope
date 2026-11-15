#pragma once
#include <algorithm>
#include <cmath>

namespace bs {
// Finite cubic Hermite easing: zero endpoint velocity, and a bounded tangent
// carried into same-direction retargets. Reversals start at the current value
// without overshooting the new target or moving away from the user's input.
// A lightly damped spring, as macOS animates switches and selections: it settles at 1 with
// one small overshoot (about 4%) and starts with velocity, so it feels quick but soft.
inline double springEase(double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double v = 1 - std::exp(-7.0 * t) * (std::cos(8.0 * t) + .55 * std::sin(8.0 * t));
    return t >= 1 ? 1 : v;
}
struct Transition {
    double from = 0, to = 0, started = 0, duration = 0, tangent = 0;
    bool spring = false; // springEase instead of the Hermite curve
    bool active(double now) const { return duration > 0 && now < started + duration; }
    double value(double now) const {
        if (!active(now)) return to;
        const double t = std::clamp((now - started) / duration, 0.0, 1.0);
        if (spring) return from + (to - from) * springEase(t);
        return from + (to - from) * (t * t * (3 - 2 * t)) + tangent * t * (1 - t) * (1 - t);
    }
    double velocity(double now) const {
        if (!active(now)) return 0;
        const double t = std::clamp((now - started) / duration, 0.0, 1.0);
        return ((to - from) * 6 * t * (1 - t) + tangent * (1 - t) * (1 - 3 * t)) / duration;
    }
    void move(double target, double now, double ms) {
        const double speed = velocity(now), current = value(now), distance = target - current;
        from = current; to = target; started = now;
        duration = std::abs(distance) > .000001 ? ms : 0;
        tangent = duration > 0 && speed * distance > 0 ? distance * std::clamp(speed * duration / distance, 0.0, 3.0) : 0;
    }
};
}
