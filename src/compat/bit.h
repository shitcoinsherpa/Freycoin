// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMPAT_BIT_H
#define BITCOIN_COMPAT_BIT_H

#include <type_traits>

// Provide std::bit_cast compatibility for compilers/libraries lacking full C++20 support.
// MinGW-w64 GCC 13 libstdc++ doesn't define __cpp_lib_bit_cast but has __builtin_bit_cast.
// This header ensures constexpr bit_cast works on all supported compilers (GCC 11+, Clang 9+).

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L
// Full library support - use standard bit_cast
#include <bit>
using std::bit_cast;
#elif defined(__GNUC__) && __GNUC__ >= 11
// GCC 11+ has constexpr __builtin_bit_cast
template <class To, class From>
constexpr To bit_cast(const From& src) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bit_cast requires same size");
    static_assert(std::is_trivially_copyable_v<From>, "source must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<To>, "dest must be trivially copyable");
    return __builtin_bit_cast(To, src);
}
#elif defined(__clang__) && __clang_major__ >= 9
// Clang 9+ has constexpr __builtin_bit_cast
template <class To, class From>
constexpr To bit_cast(const From& src) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bit_cast requires same size");
    static_assert(std::is_trivially_copyable_v<From>, "source must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<To>, "dest must be trivially copyable");
    return __builtin_bit_cast(To, src);
}
#else
#error "Freycoin requires GCC 11+, Clang 9+, or a compiler with std::bit_cast support"
#endif

#endif // BITCOIN_COMPAT_BIT_H
