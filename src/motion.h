#pragma once
#include <algorithm>
#include <cmath>

namespace bs {
// Finite cubic Hermite easing: zero endpoint velocity, and a bounded tangent
// carried into same-direction retargets. Reversals start at the current value
// without overshooting the new target or moving away from the user's input.
struct Transition {
    double from = 0, to = 0, started = 0, duration = 0, tangent = 0;
    bool active(double now) const { return duration > 0 && now < started + duration; }
    double value(double now) const {
        if (!active(now)) return to;
        const double t = std::clamp((now - started) / duration, 0.0, 1.0);
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
