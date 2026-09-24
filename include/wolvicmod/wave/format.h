#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace wolvicmod {

// Value -> bit-string mapping for waveform dump (§6.3). Built in for bool and
// integral types (two's-complement bit pattern, MSB first). User-defined value
// types can be made dumpable by specializing FstFormat; types without a
// specialization are silently skipped in wave dumps.
template <class T, class = void>
struct FstFormat;

template <class T>
concept FstFormattable = requires(const T& v, std::string& s) {
    { FstFormat<T>::bits } -> std::convertible_to<uint32_t>;
    { FstFormat<T>::format(s, v) };
};

template <>
struct FstFormat<bool, void> {
    static constexpr uint32_t bits = 1;
    static void format(std::string& out, const bool& v) { out = v ? "1" : "0"; }
};

template <std::integral T>
struct FstFormat<T, void> {
    static constexpr uint32_t bits = sizeof(T) * 8;
    static void format(std::string& out, const T& v) {
        using U = std::make_unsigned_t<T>;
        const U u = static_cast<U>(v);
        out.resize(bits);
        for (uint32_t i = 0; i < bits; ++i) out[bits - 1 - i] = ((u >> i) & 1) ? '1' : '0';
    }
};

}  // namespace wolvicmod
