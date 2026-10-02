#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace x2::obf
{
    constexpr std::uint8_t obf_key(std::size_t i, std::uint32_t seed)
    {
        std::uint32_t h = seed + static_cast<std::uint32_t>(i) * 0x9E3779B9u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return static_cast<std::uint8_t>(h);
    }

    inline std::uint8_t opaque_byte(std::uint8_t v)
    {
#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : "+r"(v)::);
#else
        volatile std::uint8_t w = v;
        v = w;
#endif
        return v;
    }

    template <std::size_t N, std::uint32_t Seed>
    inline void obf_decode(const char *enc, char *out)
    {
        const volatile char *ve = enc;
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::uint8_t c = opaque_byte(static_cast<std::uint8_t>(ve[i]));
            out[i] = static_cast<char>(c ^ obf_key(i, Seed));
        }
    }

    template <std::size_t N, std::uint32_t Seed>
    struct ObfString
    {
        char enc[N];
        consteval ObfString(const char (&s)[N]) : enc{}
        {
            for (std::size_t i = 0; i < N; ++i)
                enc[i] = static_cast<char>(static_cast<std::uint8_t>(s[i]) ^ obf_key(i, Seed));
        }

        struct Guard
        {
            char buf[N];

            explicit Guard(const char *e) { obf_decode<N, Seed>(e, buf); }

            ~Guard()
            {
                volatile char *p = buf;
                for (std::size_t i = 0; i < N; ++i)
                    p[i] = 0;
            }

            Guard(const Guard &) = delete;
            Guard &operator=(const Guard &) = delete;
            [[nodiscard]] const char *c_str() const { return buf; }
            operator const char *() const { return buf; }
        };

        [[nodiscard]] __attribute__((noinline)) Guard decrypt() const { return Guard(enc); }

        [[nodiscard]] __attribute__((noinline)) const char *lazy() const
        {
            static char buf[N];
            static bool ready = false;
            if (!ready)
            {
                obf_decode<N, Seed>(enc, buf);
                ready = true;
            }
            return buf;
        }
    };
}

#define X2OBF(s) \
    (::x2::obf::ObfString<sizeof(s), ((__COUNTER__ + 1u) * 2654435761u) ^ 0x1234ABCDu>{s})
#define OBFCSTR(s) (static_cast<const char *>(X2OBF(s).decrypt()))
#define OBFLAZY(s) (X2OBF(s).lazy())
