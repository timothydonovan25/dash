Below is a quick “live‑code‑review” of the snippet you posted.  
I kept the same algorithmic intent but highlighted a few style, safety, and 
maintainability points that could be tweaked. Feel free to cherry‑pick the changes 
that fit your codebase.

---

## 1. General style / safety notes

| Area | What you have | Why it might matter | Suggested tweak |
|------|---------------|---------------------|-----------------|
| **`const` qualification** | Most of the functions are not `const`. | Adding 
`const` (or `constexpr` where appropriate) lets the compiler know that the function 
has no side‑effects and can be called from constant expressions. | `float 
vin_from_pin_mv(uint32_t pin_mv, const Divider& d) const` (or just `constexpr` if 
no runtime data). |
| **`noexcept`** | None of the functions specify exception guarantees. | If the 
functions are truly exception‑free, `noexcept` can improve performance and 
communicate intent. | `float vin_from_pin_mv(...) noexcept` etc. |
| **`constexpr`** | Some trivial functions could be evaluated at compile‑time. | 
`constexpr` encourages constant‑expression usage and enables more aggressive 
inlining. | Mark `vin_from_pin_mv`, `battery_v`, `classify`, `lamp_master_pct` as 
`constexpr`. |
| **`std::clamp` / `std::min`** | Manual clamping (`want > 100 ? 100 : want`). | 
`std::clamp` is clearer, and you avoid magic numbers. | `const auto want = 
std::clamp(setting_pct, uint8_t{0}, uint8_t{100});` |
| **Magic numbers** | 100, 0xFFFF, etc. | Hard‑to‑read constants can be made 
explicit. | `constexpr std::uint16_t kMax16 = 0xFFFF; constexpr std::uint8_t 
kPctMax = 100;` |
| **Explicit casts** | `static_cast<float>(pin_mv)` after multiplication. | The 
cast is applied after the whole expression, which is fine, but you can cast once at 
the start to make intent obvious. | `const float pinV = 
static_cast<float>(pin_mv);` |
| **Return type of `FailDetector::feed`** | `Event` is returned from every branch, 
but the function is defined in the class body. | Nothing wrong, but a 
`[[nodiscard]]` attribute might be useful to catch unused return values. | 
`[[nodiscard]] FailDetector::Event FailDetector::feed(float v) noexcept` |

---

## 2. Function‑by‑function review

### `vin_from_pin_mv`

```cpp
float vin_from_pin_mv(uint32_t pin_mv, const Divider& d) {
    if (d.bottom_kohm <= 0.0f) return 0.0f;
    const float ratio = (d.top_kohm + d.bottom_kohm) / d.bottom_kohm;
    return static_cast<float>(pin_mv) * ratio / 1000.0f;
}
```

* **Overflow check** – `pin_mv` is 32 bit; converting to `float` can lose precision 
for values > 2²⁴ (~16 M). If that range is possible, consider using `double` or a 
64‑bit float (`long double` on some platforms).  
* **Zero division** – already guarded by the `bottom_kohm <= 0.0f` check, so fine.  

* **Clarity** – break into two lines: cast first, then compute.

```cpp
constexpr float vin_from_pin_mv(uint32_t pin_mv, const Divider& d) noexcept {
    if (d.bottom_kohm <= 0.0f) return 0.0f;
    const float pinV = static_cast<float>(pin_mv);
    const float ratio = (d.top_kohm + d.bottom_kohm) / d.bottom_kohm;
    return pinV * ratio / 1000.0f;
}
```

### `battery_v`

```cpp
float battery_v(float vin_v, float trim_v) {
    if (vin_v <= 0.0f) return 0.0f;
    const float v = vin_v + trim_v;
    return v > 0.0f ? v : 0.0f;
}
```

* **Redundant check** – the second guard (`v > 0.0f`) already covers `vin_v <= 
0.0f`.  
* **NaN handling** – the first guard also discards NaN; the second guard does the 
same. It’s fine but a bit repetitive.

```cpp
constexpr float battery_v(float vin_v, float trim_v) noexcept {
    const float v = vin_v + trim_v;
    return (v > 0.0f) ? v : 0.0f;   // NaN will fall through to 0.0f
}
```

### `classify`

```cpp
Source classify(float vin_v, const Thresholds& t) {
    if (!(vin_v >= t.present_v)) return Source::Unknown;   // also catches NaN
    return vin_v >= t.bike_v ? Source::Bike : Source::Usb;
}
```

* **Logical clarity** – `!(vin_v >= t.present_v)` is equivalent to `vin_v < 
t.present_v`. Writing it that way is easier to read.  
* **NaN propagation** – the comment is good; the condition correctly treats NaN as 
`Unknown`.

```cpp
Source classify(float vin_v, const Thresholds& t) noexcept {
    if (vin_v < t.present_v) return Source::Unknown;  // NaN → true
    return (vin_v >= t.bike_v) ? Source::Bike : Source::Usb;
}
```

### `lamp_master_pct`

```cpp
uint8_t lamp_master_pct(bool on_bike, uint8_t setting_pct, uint8_t usb_cap_pct) {
    const uint8_t want = setting_pct > 100 ? 100 : setting_pct;
    if (on_bike) return want;
    return want < usb_cap_pct ? want : usb_cap_pct;
}
```

* **Clamping** – use `std::clamp`.  
* **Logic** – could be expressed in a single line but the two‑step form is fine.

```cpp
constexpr uint8_t lamp_master_pct(bool on_bike, uint8_t setting_pct, uint8_t 
usb_cap_pct) noexcept {
    const uint8_t want = std::clamp(setting_pct, uint8_t{0}, uint8_t{100});
    return on_bike ? want : std::min(want, usb_cap_pct);
}
```

### `FailDetector::feed`

```cpp
FailDetector::Event FailDetector::feed(float v) {
    switch (state_) {
        case State::Idle:
            run_ = v >= cfg_.arm_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.arm_n) { state_ = State::Armed; run_ = 0; }
            return Event::None;

        case State::Armed:
            run_ = v < cfg_.trip_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.trip_n) {
                state_ = State::Tripped;
                low_run_ = run_;
                run_ = 0;
                since_trip_ = 0;
                return Event::Tripped;
            }
            return Event::None;

        case State::Tripped:
            if (!(v < cfg_.trip_v)) low_run_ = 0;
            else if (low_run_ < 0xFFFF) ++low_run_;
            run_ = v >= cfg_.ok_v ? static_cast<uint16_t>(run_ + 1) : 0;
            if (run_ >= cfg_.ok_n) {
                state_ = State::Armed;
                run_ = 0;
                return Event::Recovered;
            }
            if (since_trip_ < 0xFFFF) ++since_trip_;
            if (since_trip_ >= cfg_.usb_n) {
                state_ = State::Idle;      // re-arms only on a bike supply
                run_ = 0;
                return Event::Recovered;
            }
            return Event::None;
    }
    return Event::None;
}
```

* **Overflow safety** – `run_`, `low_run_`, `since_trip_` are `uint16_t`. The check 
`low_run_ < 0xFFFF` before `++low_run_` prevents overflow, but you could just let 
`++low_run_` wrap around (it wraps to 0) or use `std::uint32_t` if you truly need 
the max value.  
* **Readability** – the nested ternary/if‑else is a bit dense; consider extracting 
helper lambdas.  
* **State reset logic** – the `Idle` → `Armed` transition clears `run_`, but 
`Tripped` → `Idle` does not reset `low_run_` or `since_trip_`. That seems 
intentional, but a comment explaining why would help future maintainers.  
* **`noexcept`** – this function is pure logic, so declare it `noexcept`.  
* **Return value** – use `[[nodiscard]]` to warn if the caller forgets to inspect 
the event.

```cpp
[[nodiscard]] FailDetector::Event FailDetector::feed(float v) noexcept {
    switch (state_) {
    case State::Idle:
        run_ = (v >= cfg_.arm_v) ? run_ + 1 : 0;
        if (run_ >= cfg_.arm_n) {
            state_ = State::Armed;
            run_ = 0;
        }
        return Event::None;

    case State::Armed:
        run_ = (v < cfg_.trip_v) ? run_ + 1 : 0;
        if (run_ >= cfg_.trip_n) {
            state_ = State::Tripped;
            low_run_   = run_;
            run_       = 0;
            since_trip_= 0;
            return Event::Tripped;
        }
        return Event::None;

    case State::Tripped:
        if (v < cfg_.trip_v) {
            if (low_run_ < 0xFFFF) ++low_run_;
        } else {
            low_run_ = 0;
        }

        run_ = (v >= cfg_.ok_v) ? run_ + 1 : 0;
        if (run_ >= cfg_.ok_n) {
            state_ = State::Armed;
            run_ = 0;
            return Event::Recovered;
        }

        if (since_trip_ < 0xFFFF) ++since_trip_;
        if (since_trip_ >= cfg_.usb_n) {
            state_ = State::Idle;      // re-arms only on a bike supply
            run_ = 0;
            return Event::Recovered;
        }
        return Event::
