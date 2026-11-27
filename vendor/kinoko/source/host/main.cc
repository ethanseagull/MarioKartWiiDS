// POSIX signal declarations under strict C++ (crash report below).
#if defined(__CYGWIN__) || defined(__unix__)
#define _GNU_SOURCE 1
#endif
#include <cstdio>
#include <cstdlib>
#include "host/KReplaySystem.hh"
#include "host/KTestSystem.hh"
#include "host/Option.hh"

#include <egg/core/ExpHeap.hh>

using namespace Kinoko;

#if defined(__arm64__) || defined(__aarch64__)
static void FlushDenormalsToZero() {
    uint64_t fpcr;
    asm("mrs %0,   fpcr" : "=r"(fpcr));
    asm("msr fpcr, %0" ::"r"(fpcr | (1 << 24)));
}
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>

static void FlushDenormalsToZero() {
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}
#endif

static void *s_memorySpace = nullptr;
static EGG::Heap *s_rootHeap = nullptr;

#ifdef _WIN32
#include <windows.h>
// A hard crash prints its code and address (resolve with addr2line against the
// exe; the image base is printed with it).
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *e) {
    std::fprintf(stderr, "CRASH code %08lx at %p (image base %p)\n", e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress, static_cast<void *>(GetModuleHandle(nullptr)));
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#if defined(__CYGWIN__) || defined(__unix__)
#include <signal.h>
#include <ucontext.h>
// MSYS2 g++ is Cygwin-based: a hard crash prints the faulting instruction
// address; resolve with addr2line against the exe.
static void CrashSignal(int sig, siginfo_t *info, void *context) {
    void *pc = nullptr;
#if defined(__x86_64__)
    pc = reinterpret_cast<void *>(static_cast<ucontext_t *>(context)->uc_mcontext.rip);
#endif
    std::fprintf(stderr, "CRASH signal %d at pc %p (data %p)\n", sig, pc, info ? info->si_addr : nullptr);
    std::fflush(stderr);
    std::_Exit(128 + sig);
}
static void InstallCrashSignals() {
    struct sigaction sa {};
    sa.sa_sigaction = CrashSignal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#else
static void InstallCrashSignals() {}
#endif
static void InitMemory() {
    // KINOKO_ARENA_MB sizes the arena as the 3DS does; default as upstream.
    size_t size = MEMORY_SPACE_SIZE;
    if (const char *mb = std::getenv("KINOKO_ARENA_MB")) {
        size = static_cast<size_t>(std::atoi(mb)) << 20;
    }
    s_memorySpace = malloc(size);
    s_rootHeap = EGG::ExpHeap::create(s_memorySpace, size, DEFAULT_OPT);
    s_rootHeap->setName("EGGRoot");
    s_rootHeap->becomeCurrentHeap();

    EGG::SceneManager::SetRootHeap(s_rootHeap);
}

int main(int argc, char **argv) {
    // Unbuffered: an ASSERT's message reaches a pipe before abort().
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    InstallCrashSignals();
#ifdef _WIN32
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS *e) -> LONG {
        const DWORD c = e->ExceptionRecord->ExceptionCode;
        if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_INT_DIVIDE_BY_ZERO) CrashFilter(e);
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    FlushDenormalsToZero();
    InitMemory();

    // The hashmap cannot be constexpr, as it heap-allocates
    // Therefore, it cannot be static, as memory needs to be initialized first
    // TODO: Allow memory initialization before any other static initializers
    const std::unordered_map<std::string, std::function<KSystem *()>> modeMap = {
            {"test", []() -> KSystem * { return KTestSystem::CreateInstance(); }},
            {"replay", []() -> KSystem * { return KReplaySystem::CreateInstance(); }},
    };

    if (argc < 2) {
        PANIC("Too few arguments!");
    }

    KSystem *sys = nullptr;

    // The first argument is the executable, so we ignore it
    // The second argument is the mode
    const std::string mode = argv[1];

    auto it = modeMap.find(mode);
    if (it != modeMap.end()) {
        sys = it->second();
    } else {
        PANIC("Invalid mode!");
    }

    sys->parseOptions(argc - 2, argv + 2);
    sys->init();
    return sys->run() ? 0 : 1;
}
