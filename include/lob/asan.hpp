#pragma once
//-----------------------------------------------------------------------------
// Whether this build runs under AddressSanitizer. GCC defines
// __SANITIZE_ADDRESS__; Clang reports it through __has_feature. LOB_ASAN is for
// preprocessor tests, kAsan for everything else.
//-----------------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__)
#define LOB_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOB_ASAN 1
#endif
#endif

namespace lob {

#if defined(LOB_ASAN)
inline constexpr bool kAsan = true;
#else
inline constexpr bool kAsan = false;
#endif

}
