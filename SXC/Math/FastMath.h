#pragma once
// SXC (FlashWOW): std::fmax, std::fmin and std::floor without the call into the C runtime. MSVC doesn't inline
// them under /fp:precise: each is a call into ucrtbase, fmax/fmin with a NaN test (_fdtest) on top. In a
// Goldshire profile (24 players) they were about 2.7% of the client's CPU, most of it in the fog's wakes,
// build and flow. These give the same results: fmax/fmin return the other argument for one NaN; floor
// returns its argument as is when it is already whole (so -0.0, NaN, infinities and |x| >= 2^23 or 2^52).
// The return type is the standard function's for the same arguments.
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace fastmath {
    template <class A, class B>
    inline auto FastMax(A a, B b) -> decltype(std::fmax(a, b)) {
        using T = decltype(std::fmax(a, b));
        const T x = static_cast<T>(a), y = static_cast<T>(b);
        return (x >= y || y != y) ? x : y;
    }

    template <class A, class B>
    inline auto FastMin(A a, B b) -> decltype(std::fmin(a, b)) {
        using T = decltype(std::fmin(a, b));
        const T x = static_cast<T>(a), y = static_cast<T>(b);
        return (x <= y || y != y) ? x : y;
    }

    inline float FastFloor(float f) {
        if (!(f > -8388608.0f && f < 8388608.0f)) {
            return f;   // NaN, infinite, or already whole
        }

        const float t = static_cast<float>(static_cast<int32_t>(f));   // toward zero
        return t == f ? f : (t > f ? t - 1.0f : t);
    }

    inline double FastFloor(double f) {
        if (!(f > -4503599627370496.0 && f < 4503599627370496.0)) {
            return f;
        }

        const double t = static_cast<double>(static_cast<int64_t>(f));
        return t == f ? f : (t > f ? t - 1.0 : t);
    }

    template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
    inline double FastFloor(I i) {
        return static_cast<double>(i);   // std::floor of an integer: a double
    }
}
