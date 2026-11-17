// Stellaris Reach Patch 0.1.0, Stellaris 4.5.4 x64 — повторное использование поиска по сети врат
// в ежедневной проверке достижимости стран (140adac00).
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140adac00(ctx) ежедневно (edict 1406f2590 <- DailyUpdate) для страны C = [ctx] берёт систему S
//   (столицу, 140cd3550 + vcall 0x80) и для каждой записи списка C+0x2b90 (число +0x2b9c) с другой
//   страной O зовёт 140b33360(C, S, D) (вызов 140adadcd), D — система O. Ответ (bool) пишется в
//   флаги записи +0x120 (биты 19/20); если он изменился, игра зовёт 140adaae0 / 140adaf60 и
//   140adb6a0(.., r9d = 0/1) — события. Без изменения 140adb6a0 зовётся с r9d = 2 и сразу выходит.
//   140b33360 -> 140b333f0: Dijkstra (куча, ребро 100000) от S; цель D проверяется только при
//   извлечении узла из кучи (140b3384c: mov rcx,[r13]; cmp rcx,[rbp+0xc0]). Фильтр рёбер
//   140b33e70 зависит только от (C, узел, сосед, ребро), не от D. Значит, порядок извлечения
//   узлов одинаков для любых D, а поиск для D — префикс этого порядка до D.
//   На позднем этапе (2448, TEST5000) 140b333f0 — ~4,4–5,3 % главного потока.
//
// Плагин:
//   * на месте извлечения (140b3384c, 11 байт) записывает извлечённые узлы, пока идёт «свой» поиск;
//   * вызов 140adadcd идёт в reach_call(C, S, D): для той же (C, S) в пределах одного вызова
//     140adac00 и без изменений статуса ответ берётся из записанного префикса: D в префиксе — true;
//     префикс полный (поиск исчерпал кучу) и D нет — false; иначе — обычный вызов игры с записью
//     (префикс удлиняется). Проверка D (vcall [D]+8), как у игры, делается всегда;
//   * кэш сбрасывается на входе в 140adac00 и при вызове 140adb6a0 с r9d != 2 (изменение статуса,
//     возможны события) — вызов 140adaeb1.
//
// [reach] mode=off / shadow / on. rch_mode (rva в логе) на лету: 0 — как игра, 1 — кэш,
// 2 — тень (ответ игры, сверка с кэшем, расхождения в логе).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
std::int64_t rch_mode = 0;
std::uint64_t rch_rec_on = 0;     // 1 — пишется «свой» поиск
std::uint64_t rch_rec_tid = 0;    // поток, который пишет
std::uint64_t rch_pop_resume = 0; // 140b33857
void rch_record(const std::uint8_t* node);
void rch_pop_stub();
bool reach_call(void* country, void* src, void* dst);
void* adac_hook(void* a, void* b, void* c, void* d);
void* adb6_hook(void* a, void* b, void* c, std::int64_t d);
}

// вход jmp из 140b3384c; rsp ≡ 0 mod 16; флаги — от cmp в конце (je 140b33857 у игры)
__attribute__((naked)) void rch_pop_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rcx, qword ptr [r13]\n"
        "    cmp qword ptr [rip + rch_rec_on], 0\n"
        "    je 1f\n"
        "    push rax\n"
        "    mov rax, qword ptr gs:[0x48]\n"
        "    cmp rax, qword ptr [rip + rch_rec_tid]\n"
        "    pop rax\n"
        "    jne 1f\n"
        "    push rax\n"
        "    push rcx\n"
        "    push rdx\n"
        "    push r8\n"
        "    push r9\n"
        "    push r10\n"
        "    push r11\n"
        "    sub rsp, 0x28\n"
        "    call rch_record\n"
        "    add rsp, 0x28\n"
        "    pop r11\n"
        "    pop r10\n"
        "    pop r9\n"
        "    pop r8\n"
        "    pop rdx\n"
        "    pop rcx\n"
        "    pop rax\n"
        "1:\n"
        "    cmp rcx, qword ptr [rbp + 0xc0]\n"
        "    jmp qword ptr [rip + rch_pop_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kPop = 0x140b3384c;
constexpr std::uint64_t kPopResume = 0x140b33857;
constexpr std::uint8_t kPopBytes[] = {0x49, 0x8b, 0x4d, 0x00, 0x48, 0x3b, 0x8d, 0xc0, 0x00, 0x00, 0x00};
constexpr std::uint64_t kCallReach = 0x140adadcd;
constexpr std::uint8_t kCallReachBytes[5] = {0xe8, 0x8e, 0x85, 0x05, 0x00};
constexpr std::uint64_t kCallAdb6 = 0x140adaeb1;
constexpr std::uint8_t kCallAdb6Bytes[5] = {0xe8, 0xea, 0x07, 0x00, 0x00};
constexpr std::uint64_t kAdac = 0x140adac00;
constexpr std::uint8_t kAdacPrologue[] = {0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48};

using ReachFn = bool (*)(void*, void*, void*);
using Fn4 = void* (*)(void*, void*, void*, void*);
using Fn4i = void* (*)(void*, void*, void*, std::int64_t);
ReachFn reach_original = nullptr;  // 140b33360
Fn4 adac_original = nullptr;       // трамплин 140adac00
Fn4i adb6_original = nullptr;      // 140adb6a0

// состояние кэша (один пишущий поток; другие потоки идут мимо кэша)
struct Memo {
    DWORD tid = 0;
    bool valid = false;
    bool complete = false;  // поиск исчерпал кучу
    void* country = nullptr;
    void* src = nullptr;
    std::uint32_t gen = 1;
    std::vector<std::uint32_t> stamp;
    std::vector<const std::uint8_t*> node;
    long long calls = 0, hits = 0, runs = 0, resets = 0, mismatch = 0, other_thread = 0;
} memo;

LARGE_INTEGER freq{};
LONG64 last_report = 0;

inline bool node_valid(void* p) {
    auto* obj = static_cast<std::uint8_t*>(p);
    using V = bool (*)(void*);
    return (*reinterpret_cast<V**>(obj))[1](obj);
}

inline std::uint32_t node_idx(const std::uint8_t* n) {
    std::uint32_t id;
    std::memcpy(&id, n + 8, 4);
    return id & 0xffffffu;
}

void reset() {
    memo.valid = false;
    memo.complete = false;
    if (++memo.gen == 0) {
        std::fill(memo.stamp.begin(), memo.stamp.end(), 0u);
        memo.gen = 1;
    }
}

bool in_prefix(const std::uint8_t* dst) {
    const std::uint32_t i = node_idx(dst);
    return i < memo.stamp.size() && memo.stamp[i] == memo.gen && memo.node[i] == dst;
}

// обычный вызов игры с записью извлечённых узлов (префикс начинается заново)
bool run_recorded(void* c, void* s, void* d) {
    ++memo.runs;
    reset();
    rch_rec_tid = GetCurrentThreadId();
    rch_rec_on = 1;
    const bool r = reach_original(c, s, d);
    rch_rec_on = 0;
    memo.country = c;
    memo.src = s;
    memo.valid = true;
    // пусто — игра вышла до поиска (проверки C/S/D или индекс S): не запоминаем
    memo.complete = !r;
    bool any = false;
    const std::uint32_t si = node_idx(static_cast<const std::uint8_t*>(s));
    any = si < memo.stamp.size() && memo.stamp[si] == memo.gen;
    if (!any) memo.valid = false;
    return r;
}

void maybe_report() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last_report == 0) {
        last_report = now.QuadPart;
        return;
    }
    if (now.QuadPart - last_report < 60 * freq.QuadPart) return;
    last_report = now.QuadPart;
    splug::log("mode %lld: calls %lld, from prefix %lld, game runs %lld, resets %lld, mismatch %lld, other thread %lld",
               static_cast<long long>(rch_mode), memo.calls, memo.hits, memo.runs, memo.resets, memo.mismatch,
               memo.other_thread);
}

bool cached(void* c, void* s, void* d) {
    if (memo.valid && memo.country == c && memo.src == s) {
        const auto* dn = static_cast<const std::uint8_t*>(d);
        if (in_prefix(dn)) {
            if (!node_valid(d)) return false;  // как проверка D в 140b33360
            ++memo.hits;
            return true;
        }
        if (memo.complete) {
            ++memo.hits;
            return false;  // D недостижима (или не прошла бы проверку — тоже false)
        }
    }
    return run_recorded(c, s, d);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_reach_patch")) return;
    QueryPerformanceFrequency(&freq);
    const auto mode = splug::ini_mode(L"reach", L"mode", L"off");
    splug::log("stellaris_reach_patch 0.1.0: build %s, mode=%s, rch_mode at rva %llx", splug::state().build,
               splug::mode_name(mode),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&rch_mode) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(kPop, kPopBytes) || !splug::bytes_match(kAdac, kAdacPrologue)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    memo.stamp.assign(65536, 0u);
    memo.node.assign(65536, nullptr);
    reach_original = reinterpret_cast<ReachFn>(splug::rva(0x140b33360));
    adb6_original = reinterpret_cast<Fn4i>(splug::rva(0x140adb6a0));
    rch_pop_resume = reinterpret_cast<std::uint64_t>(splug::rva(kPopResume));

    // переходники рядом с игрой: [0] — к rch_pop_stub, [16] — к adac_hook, [32] — трамплин 140adac00
    std::uint8_t* site_pop = splug::rva(kPop);
    std::uint8_t* site_adac = splug::rva(kAdac);
    auto* page = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site_pop));
    if (!page) {
        splug::log("REFUSED: no memory near game code");
        return;
    }
    auto put_abs = [](std::uint8_t* at, const void* target) {
        const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
        const auto a = reinterpret_cast<std::uint64_t>(target);
        std::memcpy(at, jmp, 6);
        std::memcpy(at + 6, &a, 8);
    };
    put_abs(page, reinterpret_cast<const void*>(&rch_pop_stub));
    put_abs(page + 16, reinterpret_cast<const void*>(&adac_hook));
    std::memcpy(page + 32, kAdacPrologue, sizeof(kAdacPrologue));
    put_abs(page + 32 + sizeof(kAdacPrologue), site_adac + sizeof(kAdacPrologue));
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 64);
    adac_original = reinterpret_cast<Fn4>(page + 32);

    auto rel_to = [](const std::uint8_t* from_end, const std::uint8_t* to, std::int32_t& out) {
        const auto d = reinterpret_cast<std::intptr_t>(to) - reinterpret_cast<std::intptr_t>(from_end);
        if (d < INT32_MIN || d > INT32_MAX) return false;
        out = static_cast<std::int32_t>(d);
        return true;
    };
    std::int32_t r1 = 0, r2 = 0;
    if (!rel_to(site_pop + 5, page, r1) || !rel_to(site_adac + 5, page + 16, r2)) {
        splug::log("REFUSED: page out of rel32 range");
        return;
    }
    splug::CallPatch cp_reach, cp_adb6;
    const char* e1 = splug::prepare_call(splug::rva(kCallReach), kCallReachBytes,
                                         reinterpret_cast<const void*>(&reach_call), cp_reach);
    const char* e2 = splug::prepare_call(splug::rva(kCallAdb6), kCallAdb6Bytes,
                                         reinterpret_cast<const void*>(&adb6_hook), cp_adb6);
    if (e1 || e2) {
        splug::log("REFUSED: %s", e1 ? e1 : e2);
        return;
    }
    rch_mode = mode == splug::Mode::on ? 1 : 2;
    // порядок: сначала сбросы (вход 140adac00, 140adb6a0), потом запись и сам кэш
    std::uint8_t p_adac[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    std::memcpy(p_adac + 1, &r2, 4);
    std::uint8_t p_pop[11] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    std::memcpy(p_pop + 1, &r1, 4);
    if (!splug::write_bytes(site_adac, p_adac, sizeof(p_adac)) || !splug::commit_call(cp_adb6) ||
        !splug::write_bytes(site_pop, p_pop, sizeof(p_pop)) || !splug::commit_call(cp_reach)) {
        splug::log("FAILED while writing hooks");
        return;
    }
    splug::log("installed: entry %llx, pop %llx, calls %llx/%llx, mode %lld", static_cast<unsigned long long>(kAdac),
               static_cast<unsigned long long>(kPop), static_cast<unsigned long long>(kCallReach),
               static_cast<unsigned long long>(kCallAdb6), static_cast<long long>(rch_mode));
}

}  // namespace

extern "C" void rch_record(const std::uint8_t* n) {
    const std::uint32_t i = node_idx(n);
    if (i < memo.stamp.size()) {
        memo.stamp[i] = memo.gen;
        memo.node[i] = n;
    }
}

extern "C" void* adac_hook(void* a, void* b, void* c, void* d) {
    if (GetCurrentThreadId() == memo.tid || memo.tid == 0) {
        reset();
        ++memo.resets;
    }
    return adac_original(a, b, c, d);
}

extern "C" void* adb6_hook(void* a, void* b, void* c, std::int64_t d) {
    if (static_cast<std::int32_t>(d) != 2 && (GetCurrentThreadId() == memo.tid || memo.tid == 0)) {
        reset();
        ++memo.resets;
    }
    return adb6_original(a, b, c, d);
}

extern "C" bool reach_call(void* c, void* s, void* d) {
    const std::int64_t mode = rch_mode;
    if (mode == 0) return reach_original(c, s, d);
    const DWORD tid = GetCurrentThreadId();
    if (memo.tid == 0) memo.tid = tid;
    if (memo.tid != tid) {
        ++memo.other_thread;
        return reach_original(c, s, d);
    }
    ++memo.calls;
    if ((memo.calls & 0xfff) == 0) maybe_report();
    if (mode == 2) {
        // тень: ответ из кэша (может сам вызвать игру с записью), затем чистый вызов игры
        const bool fast = cached(c, s, d);
        const bool slow = reach_original(c, s, d);
        if (fast != slow) ++memo.mismatch;
        return slow;
    }
    return cached(c, s, d);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
