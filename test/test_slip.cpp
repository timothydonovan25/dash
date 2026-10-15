#include "check.hpp"
#include "core/slip/slip.hpp"

using namespace core::slip;

int main() {
    const Config c;

    CASE("matched wheels at speed are no slip");
    {
        Meter m;
        CHECK(m.update(30.0f, true, 31.0f, true, 30.0f, true, 0, c).kind == Kind::None);
    }

    CASE("the rear running ahead of the road is spin, in steps of five percent");
    {
        Meter m;
        const Reading r = m.update(20.0f, true, 26.0f, true, 20.0f, true, 0, c);
        CHECK(r.kind == Kind::Spin);
        CHECK(r.pct == 30);
        CHECK(m.update(20.0f, true, 26.4f, true, 20.0f, true, 10, c).pct == 30);   // 32%: same step
    }

    CASE("a wheel well below the road is a lock, and which one");
    {
        Meter f, r;
        CHECK(f.update(5.0f, true, 28.0f, true, 30.0f, true, 0, c).kind == Kind::LockFront);
        const Reading rr = r.update(29.0f, true, 0.0f, true, 30.0f, true, 0, c);
        CHECK(rr.kind == Kind::LockRear);
        CHECK(rr.pct == 100);
    }

    CASE("a lock outranks a spin");
    {
        Meter m;
        // Front locked, rear somehow ahead: the front is the emergency.
        CHECK(m.update(10.0f, true, 40.0f, true, 30.0f, true, 0, c).kind == Kind::LockFront);
    }

    CASE("slow is not slip, and a stop is not a lockup");
    {
        Meter m;
        CHECK(m.update(0.0f, true, 5.0f, true, 3.0f, true, 0, c).kind == Kind::None);
        CHECK(m.update(0.0f, true, 0.0f, true, 0.0f, true, 10, c).kind == Kind::None);
    }

    CASE("a short slip is held long enough to read, at its peak");
    {
        Meter m;
        m.update(20.0f, true, 30.0f, true, 20.0f, true, 0, c);              // 50% spin
        m.update(20.0f, true, 24.0f, true, 20.0f, true, 50, c);             // easing: 20%
        CHECK(m.update(20.0f, true, 20.0f, true, 20.0f, true, 100, c).pct == 50);   // held at peak
        CHECK(m.update(20.0f, true, 20.0f, true, 20.0f, true, 50 + c.hold_ms - 1, c).kind == Kind::Spin);
        CHECK(m.update(20.0f, true, 20.0f, true, 20.0f, true, 50 + c.hold_ms, c).kind == Kind::None);
    }

    CASE("with no road speed, spin is read against the front and a lock is not guessed");
    {
        Meter m;
        CHECK(m.update(20.0f, true, 26.0f, true, 0.0f, false, 0, c).kind == Kind::Spin);
        Meter n;
        CHECK(n.update(2.0f, true, 30.0f, true, 0.0f, false, 0, c).kind == Kind::None);   // front 2: below min
        Meter k;
        CHECK(k.update(20.0f, true, 1.0f, true, 0.0f, false, 0, c).kind == Kind::None);   // can't call a lock
    }

    CASE("a wheel sensor already called dead is not a lock, and cannot fake a spin");
    {
        Meter m;
        // Dead front reads 0 at 30 mph: a lock if believed.
        CHECK(m.update(0.0f, true, 30.0f, true, 30.0f, true, 0, c).kind == Kind::LockFront);
        Meter d;
        CHECK(d.update(0.0f, false, 30.0f, true, 30.0f, true, 0, c).kind == Kind::None);
        // The rear still reads normally next to it.
        CHECK(d.update(0.0f, false, 40.0f, true, 30.0f, true, 10, c).kind == Kind::Spin);
        Meter r;
        CHECK(r.update(30.0f, true, 0.0f, false, 30.0f, true, 0, c).kind == Kind::None);
        CHECK(r.update(5.0f, true, 0.0f, false, 30.0f, true, 10, c).kind == Kind::LockFront);
        // No road speed and a dead front: nothing to read a spin against.
        Meter n;
        CHECK(n.update(0.0f, false, 30.0f, true, 0.0f, false, 0, c).kind == Kind::None);
    }

    return check::report("slip");
}
