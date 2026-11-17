// Stellaris Budget Patch 0.1.0, Stellaris 4.5.4 x64 — реже читать таймер в проверке бюджета кадра.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140938370(obj, double slack) -> bool: «истёк ли бюджет времени кадра» — берёт время
//   (vcall +0x28 = 141babcc0 -> 141da1dd0 -> 141defee0 -> QueryPerformanceCounter), вычитает начало
//   кадра [[obj+0xe8]+0xd8] и сравнивает с 1/fps - slack. Вызывается из циклов тика (140b8de80 на
//   каждом элементе, 1402b72f9, 14024bde5, ...); при true игра рисует кадр изнутри тика (14024be40).
//   На BENCH2446 это ~1 млн вызовов/с и ~6,9 % главного потока (сам QPC — 4,0 %).
//
// Плагин: вход 140938370 (6 байт: push rbx; sub rsp,0x30) -> счётчик вызовов. Настоящая проверка
// делается на каждом (mask+1)-м вызове, на остальных ответ false («ещё не истёк»). Состояние игры
// не затрагивается: меняется только момент, когда из тика рисуется кадр (на несколько вызовов позже).
//
// [budget] mode=on/off, every=16 (степень двойки). bgp_mask (rva в логе) на лету: 0 — как в игре,
// 15 — каждый 16-й вызов и т. д.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
std::int64_t bgp_mask = 0;
std::uint64_t bgp_count = 0;
std::uint64_t bgp_resume = 0;  // 140938376
void bgp_stub();
}

// rax на входе функции мёртв; rcx и xmm1 — аргументы, не трогаются
__attribute__((naked)) void bgp_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rax, qword ptr [rip + bgp_count]\n"
        "    inc rax\n"
        "    mov qword ptr [rip + bgp_count], rax\n"
        "    test rax, qword ptr [rip + bgp_mask]\n"
        "    jz 1f\n"
        "    xor eax, eax\n"
        "    ret\n"
        "1:\n"
        "    push rbx\n"
        "    sub rsp, 0x30\n"
        "    jmp qword ptr [rip + bgp_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x140938370;
constexpr std::uint64_t kResume = 0x140938376;
constexpr std::uint8_t kBytes[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x0f, 0x29, 0x74, 0x24, 0x20};

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_budget_patch")) return;
    const auto mode = splug::ini_mode(L"budget", L"mode", L"off");
    int every = splug::ini_int(L"budget", L"every", 16);
    if (every < 1 || every > 4096 || (every & (every - 1)) != 0) every = 16;
    splug::log("stellaris_budget_patch 0.1.0: build %s, mode=%s, every=%d, bgp_mask at rva %llx", splug::state().build,
               splug::mode_name(mode), every,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&bgp_mask) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (mode != splug::Mode::on) return;
    if (!splug::bytes_match(kSite, kBytes)) {
        splug::log("REFUSED: bytes differ; no bytes changed");
        return;
    }
    std::uint8_t* site = splug::rva(kSite);
    std::uint8_t* tramp = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!tramp) {
        splug::log("REFUSED: no memory near site");
        return;
    }
    std::uint8_t code[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&bgp_stub);
    std::memcpy(code + 6, &target, 8);
    std::memcpy(tramp, code, sizeof(code));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, sizeof(code));
    const auto disp = reinterpret_cast<std::intptr_t>(tramp) - reinterpret_cast<std::intptr_t>(site + 5);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        splug::log("REFUSED: trampoline out of rel32 range");
        return;
    }
    bgp_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    bgp_mask = every - 1;
    const auto rel = static_cast<std::int32_t>(disp);
    std::uint8_t patch[6] = {0xe9, 0, 0, 0, 0, 0x90};
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write jump");
        bgp_mask = 0;
        return;
    }
    splug::log("hook installed at %llx, mask %lld", static_cast<unsigned long long>(kSite),
               static_cast<long long>(bgp_mask));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
