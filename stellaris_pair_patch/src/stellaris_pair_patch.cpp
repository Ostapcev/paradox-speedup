// Stellaris Pair Patch 0.1.0, Stellaris 4.5.4 x64 — попарная проверка объектов 140645290 (ветка 3).
//
// Что в игре (stellaris.exe 4.5.4.1b4f): 1406450c0(a, out, flag) для каждого объекта b из того же
// контейнера зовёт 140645290(a, b, flag). При [[a+0x50]+0x61c] == 3 (1406453a0):
//   A = система [a+0x78], B = система [b+0x78] (таблица [14311bed8], сверка id по [obj+8], иначе объект
//   по умолчанию [14311b6f0]); затем 14086f6e0(A, B) — есть ли id B ([B+8]) среди гиперлиний A
//   ([A+0x590], шаг 32, id +8, число [A+0x59c]); нет → ответ «нет» (14064546b).
// Почти всё время функции — два промаха: запись таблицы и строка объекта B (сверка id).
//
// Плагин (jmp в 1406453e7, сразу после поиска A): id B равен либо запрошенному [b+0x78] (поиск удался),
// либо id объекта по умолчанию. Если ни тот, ни другой не встречается среди гиперлиний A, 14086f6e0
// вернёт «нет» при любом исходе поиска — отвечаем «нет», не трогая таблицу и B. Иначе — код игры.
// [pair] mode=off / on / shadow (всегда путь игры; быстрый ответ сверяется с полным поиском).
// ppp_mode (rva в логе): 0 — игра, 1 — быстрый ответ, 2 — тень, 3 — замер: вызовы 1406450c0 (вызов 140647e92)
// по очереди на каждом потоке без/с быстрым ответом, время каждого вызова по TSC.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

extern "C" {
std::int64_t ppp_mode = 0;
std::uint64_t ppp_resume = 0;  // 1406453ee
std::uint64_t ppp_false = 0;   // 14064546b
int ppp_quick(const std::uint8_t* sysA, std::uint32_t qid, const std::uint8_t* def);
void ppp_stub();
std::uint64_t ppp_outer(void* a, void* out, std::uint64_t flag, void* d);
}

// вход jmp из 1406453e7; живые: r10 (A), rbx (по умолчанию), rdi (таблица), r9d (размер), rsi (b), rbp (a);
// rsp ≡ 0 mod 16. Затёртые инструкции: mov r8d,[rsi+0x78]; mov eax,r8d.
__attribute__((naked)) void ppp_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    push r9\n"
        "    push r10\n"
        "    sub rsp, 0x20\n"
        "    mov rcx, r10\n"
        "    mov edx, dword ptr [rsi + 0x78]\n"
        "    mov r8, rbx\n"
        "    call ppp_quick\n"
        "    add rsp, 0x20\n"
        "    pop r10\n"
        "    pop r9\n"
        "    test eax, eax\n"
        "    jnz 1f\n"
        "    mov r8d, dword ptr [rsi + 0x78]\n"
        "    mov eax, r8d\n"
        "    jmp qword ptr [rip + ppp_resume]\n"
        "1:\n"
        "    jmp qword ptr [rip + ppp_false]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x1406453e7;
constexpr std::uint64_t kResume = 0x1406453ee;
constexpr std::uint64_t kFalse = 0x14064546b;
constexpr std::uint8_t kSiteBytes[] = {0x44, 0x8b, 0x46, 0x78, 0x41, 0x8b, 0xc0};
// контекст: 1406453a0 (таблица и объект по умолчанию), вызов 14086f6e0, выход
constexpr std::uint8_t kCtx1[] = {0x48, 0x8b, 0x3d, 0x31, 0x6b, 0xad, 0x02, 0x48, 0x8b, 0x1d, 0x42, 0x63, 0xad, 0x02};
constexpr std::uint8_t kFalseBytes[] = {0x32, 0xc0, 0x48, 0x8b, 0x5c, 0x24, 0x30};

std::uint8_t** g_tab = nullptr;  // 14311bed8
constexpr std::uint64_t kOuter = 0x140647e92;
constexpr std::uint8_t kOuterBytes[5] = {0xe8, 0x29, 0xd2, 0xff, 0xff};
using Fn4 = std::uint64_t (*)(void*, void*, std::uint64_t, void*);
Fn4 outer_orig = nullptr;
thread_local int t_alt = 0;
thread_local unsigned t_cnt = 0;
thread_local long long t_n[2], t_t[2];
std::atomic<long long> o_n[2], o_t[2];
std::atomic<long long> c_calls{0}, c_false{0}, c_full{0}, c_mismatch{0};
std::atomic<long long> rep_qpc{0};
LARGE_INTEGER freq{};

inline bool lanes_have(const std::uint8_t* sysA, std::uint32_t id) {
    const std::int32_t n = *reinterpret_cast<const std::int32_t*>(sysA + 0x59c);
    if (n <= 0) return false;
    const std::uint8_t* l = *reinterpret_cast<std::uint8_t* const*>(sysA + 0x590) + 8;
    for (std::int32_t i = 0; i < n; ++i, l += 0x20)
        if (*reinterpret_cast<const std::uint32_t*>(l) == id) return true;
    return false;
}

const std::uint8_t* lookup(std::uint32_t id, const std::uint8_t* def) {
    const std::uint8_t* t = *g_tab;
    if (t) {
        const std::uint32_t idx = id & 0xffffffu;
        if (idx < *reinterpret_cast<const std::uint32_t*>(t + 0x20)) {
            const std::uint8_t* o = *reinterpret_cast<std::uint8_t* const*>(
                *reinterpret_cast<std::uint8_t* const*>(t + 0x18) + idx * 16ull + 8);
            if (o && *reinterpret_cast<const std::uint32_t*>(o + 8) == id) return o;
        }
    }
    return def;
}

void report() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    long long last = rep_qpc.load();
    if (last == 0) {
        rep_qpc.compare_exchange_strong(last, q.QuadPart);
        return;
    }
    if (q.QuadPart - last < 60 * freq.QuadPart || !rep_qpc.compare_exchange_strong(last, q.QuadPart)) return;
    {
        const long long n0 = o_n[0].exchange(0), t0 = o_t[0].exchange(0), n1 = o_n[1].exchange(0), t1 = o_t[1].exchange(0);
        if (n0 || n1)
            splug::log("1406450c0 calls: game %lld avg %.2f kcyc, early 'no' %lld avg %.2f kcyc", n0,
                       n0 ? t0 / 1000.0 / n0 : 0.0, n1, n1 ? t1 / 1000.0 / n1 : 0.0);
    }
    splug::log("mode %lld: checks %lld, answered 'no' early %lld, full path %lld, mismatches %lld",
               static_cast<long long>(ppp_mode), c_calls.exchange(0), c_false.exchange(0), c_full.exchange(0),
               c_mismatch.load());
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_pair_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"pair", L"mode", L"off");
    splug::log("stellaris_pair_patch 0.1.0: build %s, mode=%s, ppp_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&ppp_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(kSite, kSiteBytes) || !splug::bytes_match(0x1406453a0, kCtx1) ||
        !splug::bytes_match(kFalse, kFalseBytes)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    // 140645414: mov rcx, r10; call 14086f6e0
    {
        constexpr std::uint8_t kCall[] = {0x49, 0x8b, 0xca, 0xe8, 0xc4, 0xa2, 0x22, 0x00};
        if (!splug::bytes_match(0x140645414, kCall)) {
            splug::log("REFUSED: adjacency call differs; no bytes changed");
            return;
        }
    }
    g_tab = splug::rva<std::uint8_t*>(0x14311bed8);
    outer_orig = reinterpret_cast<Fn4>(splug::call_target(splug::rva(kOuter)));
    splug::CallPatch op;
    if (reinterpret_cast<std::uintptr_t>(outer_orig) != reinterpret_cast<std::uintptr_t>(splug::rva(0x1406450c0))) {
        splug::log("REFUSED: outer call target differs");
        return;
    }
    if (const char* e = splug::prepare_call(splug::rva(kOuter), kOuterBytes, reinterpret_cast<const void*>(&ppp_outer), op)) {
        splug::log("REFUSED: %s", e);
        return;
    }
    std::uint8_t* site = splug::rva(kSite);
    auto* page = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!page) {
        splug::log("REFUSED: no memory near game code");
        return;
    }
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&ppp_stub);
    std::memcpy(page, jmp, 6);
    std::memcpy(page + 6, &target, 8);
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 16);
    const auto d = reinterpret_cast<std::intptr_t>(page) - reinterpret_cast<std::intptr_t>(site + 5);
    if (d < INT32_MIN || d > INT32_MAX) {
        splug::log("REFUSED: page out of rel32 range");
        return;
    }
    ppp_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    ppp_false = reinterpret_cast<std::uint64_t>(splug::rva(kFalse));
    ppp_mode = mode == splug::Mode::on ? 1 : 2;
    std::uint8_t patch[sizeof(kSiteBytes)];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xe9;
    const std::int32_t rel = static_cast<std::int32_t>(d);
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        ppp_mode = 0;
        splug::log("FAILED: cannot write jump");
        return;
    }
    if (!splug::commit_call(op)) splug::log("outer call not written (mode 3 unavailable)");
    splug::log("installed at %llx, mode %lld", static_cast<unsigned long long>(kSite),
               static_cast<long long>(ppp_mode));
}

}  // namespace

extern "C" int ppp_quick(const std::uint8_t* sysA, std::uint32_t qid, const std::uint8_t* def) {
    std::int64_t mode = ppp_mode;
    if (mode == 3) mode = t_alt;
    if (mode == 0) return 0;
    const std::uint32_t defid = *reinterpret_cast<const std::uint32_t*>(def + 8);
    const bool no = !lanes_have(sysA, qid) && !lanes_have(sysA, defid);
    if (mode == 2) {
        c_calls.fetch_add(1, std::memory_order_relaxed);
        const std::uint8_t* B = lookup(qid, def);
        const bool game_adj = lanes_have(sysA, *reinterpret_cast<const std::uint32_t*>(B + 8));
        if (no && game_adj) {
            const long long k = c_mismatch.fetch_add(1);
            if (k < 10) splug::log("MISMATCH: qid %08x defid %08x B id %08x", qid, defid,
                                   *reinterpret_cast<const std::uint32_t*>(B + 8));
        }
        (no ? c_false : c_full).fetch_add(1, std::memory_order_relaxed);
        report();
        return 0;
    }
    // в рабочем режиме без общих счётчиков (одна строка кэша на все ядра дороже самой проверки)
    return no ? 1 : 0;
}

extern "C" std::uint64_t ppp_outer(void* a, void* out, std::uint64_t flag, void* d) {
    if (ppp_mode != 3) return outer_orig(a, out, flag, d);
    const int alt = static_cast<int>(++t_cnt & 1);
    t_alt = alt;
    const unsigned long long t0 = __rdtsc();
    const std::uint64_t r = outer_orig(a, out, flag, d);
    t_t[alt] += static_cast<long long>(__rdtsc() - t0);
    ++t_n[alt];
    t_alt = 0;
    if ((t_cnt & 255) == 0) {
        for (int k = 0; k < 2; ++k) {
            o_n[k].fetch_add(t_n[k]);
            o_t[k].fetch_add(t_t[k]);
            t_n[k] = t_t[k] = 0;
        }
        report();
    }
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
