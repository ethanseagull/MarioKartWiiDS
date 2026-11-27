#pragma once

#include <cstdio>
#include <cstdlib>

#define TOSTRINGIMPL(x) #x
#define TOSTRING(x) TOSTRINGIMPL(x)

#ifdef __FILE_NAME__
#define FILE_NAME __FILE_NAME__
#else
#define FILE_NAME __FILE__
#endif

#define STATIC_ASSERT(e) static_assert(e, "[" FILE_NAME ":" TOSTRING(__LINE__) "] ASSERT: " #e)

#ifdef KINOKO_QUIET
// Release builds: every check still runs and still aborts; nothing is printed.
// The failed check is kept for the port's abort log (DIAG: FAIL abort ... kinoko=).
namespace Kinoko {
inline const char *volatile s_abortWhere = nullptr;
// The code address of the failed check and its function's return address.
inline void *volatile s_abortPc = nullptr;
inline void *volatile s_abortCaller = nullptr;
// The last file asked of the ResourceManager and the last object created,
// for the abort log (a check failing while either is read names it).
inline char s_lastFile[48] = "";
inline volatile int s_lastObject = -1;
}
#define RUNTIME_ASSERT(e) \
    do { \
        if (!(e)) { \
            __label__ kinokoFailed; \
        kinokoFailed: \
            ::Kinoko::s_abortWhere = "[" FILE_NAME ":" TOSTRING(__LINE__) "] ASSERT: " #e; \
            ::Kinoko::s_abortPc = &&kinokoFailed; \
            ::Kinoko::s_abortCaller = __builtin_return_address(0); \
            abort(); \
        } \
    } while (0)
#define ASSERT(e) RUNTIME_ASSERT(e)
#define PANIC(m, ...) \
    do { \
        ::Kinoko::s_abortWhere = "[" FILE_NAME ":" TOSTRING(__LINE__) "] PANIC: " m; \
        abort(); \
    } while (0)
#define WARN(m, ...) \
    do { \
    } while (0)
#define REPORT(m, ...) \
    do { \
    } while (0)
#else
#define RUNTIME_ASSERT(e) \
    do { \
        if (!(e)) { \
            printf("[" FILE_NAME ":" TOSTRING(__LINE__) "] ASSERT: " #e "\n"); \
            abort(); \
        } \
    } while (0)

#define ASSERT(e) RUNTIME_ASSERT(e)

#define PANIC(m, ...) \
    do { \
        printf("[" FILE_NAME ":" TOSTRING(__LINE__) "] PANIC: " m "\n", ##__VA_ARGS__); \
        abort(); \
    } while (0)

#define WARN(m, ...) \
    do { \
        printf("[" FILE_NAME ":" TOSTRING(__LINE__) "] WARN: " m "\n\n", ##__VA_ARGS__); \
    } while (0)

#define REPORT(m, ...) \
    do { \
        printf("[" FILE_NAME ":" TOSTRING(__LINE__) "] REPORT: " m "\n", ##__VA_ARGS__); \
    } while (0)
#endif

#ifdef BUILD_DEBUG
#define DEBUG(m, ...) \
    do { \
        printf("[" FILE_NAME ":" TOSTRING(__LINE__) "] DEBUG: " m "\n", ##__VA_ARGS__); \
    } while (0)
#else
#define DEBUG(m, ...)
#endif
