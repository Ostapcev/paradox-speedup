// Stellaris Ship Patch 0.3.0, Stellaris 4.5.4 x64 — подкачка данных в цикле по кораблям.
//
// Что в игре (stellaris.exe 4.5.4.1b4f), микротик 140252c40, цикл 1402535e0..1402536b1:
//   for (i = 0; i < n; ++i) {            // rdi = i, r12 = n
//       ship = ships[i];                 // массив, указатель на него в глобале 14280e7f8
//       fleet = db_lookup(ship+0x998);   // таблица *(1431199e8): +0x18 слоты, +0x20 размер;
//                                        // слот = [+0x18][idx*16+8], верен если [слот+0x30]==id,
//                                        // иначе флот по умолчанию *(143119ec0)
//       if (fleet->vfn... && ship->vfn... && флаги +0x450) 140ca4ca0(ship);
//       ...
//   }
//   140ca4ca0 на входе снова ищет флот и читает [[fleet+0x810]+0x32c] и [fleet+0x7b4].
// В 2300 году на TEST5000 ожидание памяти на этих чтениях — ~2,3 % главного потока
// (промахи: слот таблицы, fleet+0x30, fleet+0x810 -> +0x32c).
//
// Плагин ставит в начало тела цикла (1402535e0, mov rax,[rip+...] — 7 байт) переход в свой код,
// который конвейером подкачивает (prefetcht0) данные для кораблей i+a, i+b, i+c, i+d:
//   i+a: строки корабля (+0x20, +0x450, +0x998, +0x9bc, +0xa5c);
//   i+b: слот таблицы флотов;
//   i+c: строки флота (+0x18, +0x30, +0x7b4, +0x810);
//   i+d: владелец: [fleet+0x810] + 0x32c.
// 0.3.0 ([ship] components=1): в позднем этапе (2448) 140ca4ca0 ждёт ещё массив компонентов корабля
//   [ship+0x9b0] и поле comp+0x7c (~3,8 % главного потока). Ступени e1 (i+ahead_comp_list) — строки
//   массива, e2 (i+ahead_comp) — строки компонентов (+0x58, +0x7c), до 24 на корабль.
// Настоящие чтения — только того, что игра и так читает в этом цикле или в 140ca4ca0
// (корабль+0x998, слот, [слот+0x30], [fleet+0x810]); остальное — prefetch, который не падает
// даже на неверном адресе. Затем выполняется вытесненная инструкция и возврат на 1402535e7.
// Регистры rax, rcx, rdx, r8–r11 и флаги в этой точке мертвы (переписываются ниже до чтения).

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
// заполняются в install(); читаются из ship_prefetch_stub
std::uint64_t ssp_ships_global = 0;    // адрес глобала с указателем на массив кораблей
std::uint64_t ssp_db_global = 0;       // адрес глобала с указателем на таблицу флотов
std::uint64_t ssp_default_global = 0;  // адрес глобала с флотом по умолчанию
std::uint64_t ssp_resume = 0;          // 1402535e7
std::int64_t ssp_a = 6, ssp_b = 4, ssp_c = 2, ssp_d = 1;
std::int64_t ssp_comp = 0;              // 1 — ступени e1/e2 (компоненты корабля); меняется на лету
std::int64_t ssp_e1 = 4, ssp_e2 = 2;    // на сколько кораблей вперёд: массив ship+0x9b0 / сами компоненты
void ship_prefetch_stub();
}

__attribute__((naked)) void ship_prefetch_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov r9, qword ptr [rip + ssp_ships_global]\n"
        "    mov r9, qword ptr [r9]\n"
        "    mov r10, qword ptr [rip + ssp_db_global]\n"
        "    mov r10, qword ptr [r10]\n"
        // ступень a: строки корабля
        "    mov rax, qword ptr [rip + ssp_a]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 1f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    prefetcht0 [rcx + 0x998]\n"
        "    prefetcht0 [rcx + 0x450]\n"
        "    prefetcht0 [rcx + 0x20]\n"
        "1:\n"
        "    test r10, r10\n"
        "    jz 3f\n"
        "    mov r11, qword ptr [r10 + 0x18]\n"
        // ступень b: слот таблицы
        "    mov rax, qword ptr [rip + ssp_b]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 2f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    mov r8d, dword ptr [rcx + 0x998]\n"
        "    and r8d, 0xffffff\n"
        "    cmp r8d, dword ptr [r10 + 0x20]\n"
        "    jae 2f\n"
        "    shl r8, 4\n"
        "    prefetcht0 [r11 + r8 + 8]\n"
        "2:\n"
        // ступень c: строки флота (слот без проверки id — только prefetch)
        "    mov rax, qword ptr [rip + ssp_c]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 3f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    mov r8d, dword ptr [rcx + 0x998]\n"
        "    and r8d, 0xffffff\n"
        "    cmp r8d, dword ptr [r10 + 0x20]\n"
        "    jae 3f\n"
        "    shl r8, 4\n"
        "    mov rdx, qword ptr [r11 + r8 + 8]\n"
        "    prefetcht0 [rdx + 0x30]\n"
        "    prefetcht0 [rdx + 0x7b4]\n"
        "    prefetcht0 [rdx + 0x810]\n"
        "3:\n"
        // ступени e1/e2: компоненты корабля для 140ca4ca0 (массив [ship+0x9b0], число +0x9bc;
        // читает comp+0x58/+0x64/+0x70/+0x7c). Чтения — только массив живого корабля, как в игре.
        "    cmp qword ptr [rip + ssp_comp], 0\n"
        "    je 7f\n"
        "    mov rax, qword ptr [rip + ssp_e1]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 4f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    mov rdx, qword ptr [rcx + 0x9b0]\n"
        "    prefetcht0 [rdx]\n"
        "    prefetcht0 [rdx + 0x40]\n"
        "4:\n"
        "    mov rax, qword ptr [rip + ssp_e2]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 7f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    movsxd r8, dword ptr [rcx + 0x9bc]\n"
        "    test r8, r8\n"
        "    jle 7f\n"
        "    mov eax, 24\n"
        "    cmp r8, rax\n"
        "    cmovg r8, rax\n"
        "    mov rdx, qword ptr [rcx + 0x9b0]\n"
        "    xor eax, eax\n"
        "8:\n"
        "    mov rcx, qword ptr [rdx + rax*8]\n"
        "    prefetcht0 [rcx + 0x58]\n"
        "    prefetcht0 [rcx + 0x7c]\n"
        "    inc rax\n"
        "    cmp rax, r8\n"
        "    jl 8b\n"
        "7:\n"
        // ступень d: владелец флота (поиск как в игре, с проверкой id)
        "    mov rax, qword ptr [rip + ssp_d]\n"
        "    add rax, rdi\n"
        "    cmp rax, r12\n"
        "    jge 9f\n"
        "    mov rcx, qword ptr [r9 + rax*8]\n"
        "    mov r8d, dword ptr [rcx + 0x998]\n"
        "    test r10, r10\n"
        "    jz 5f\n"
        "    mov eax, r8d\n"
        "    and eax, 0xffffff\n"
        "    cmp eax, dword ptr [r10 + 0x20]\n"
        "    jae 5f\n"
        "    mov r11, qword ptr [r10 + 0x18]\n"
        "    shl rax, 4\n"
        "    mov rdx, qword ptr [r11 + rax + 8]\n"
        "    test rdx, rdx\n"
        "    jz 5f\n"
        "    cmp dword ptr [rdx + 0x30], r8d\n"
        "    je 6f\n"
        "5:\n"
        "    mov rdx, qword ptr [rip + ssp_default_global]\n"
        "    mov rdx, qword ptr [rdx]\n"
        "6:\n"
        "    mov rdx, qword ptr [rdx + 0x810]\n"
        "    prefetcht0 [rdx + 0x32c]\n"
        "9:\n"
        "    mov rax, r9\n"  // вытесненная mov rax,[rip+0x25bb211]
        "    jmp qword ptr [rip + ssp_resume]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x1402535e0;
constexpr std::uint64_t kResume = 0x1402535e7;
constexpr std::uint64_t kShips = 0x14280e7f8;
constexpr std::uint64_t kDb = 0x1431199e8;
constexpr std::uint64_t kDefault = 0x143119ec0;

// 1402535e0: mov rax,[rip+0x25bb211]; mov rbx,[rax+rdi*8]; mov rdx,[rip+0x2ec63f6]; test rdx,rdx
constexpr std::uint8_t kSiteBytes[] = {0x48, 0x8b, 0x05, 0x11, 0xb2, 0x5b, 0x02, 0x48, 0x8b, 0x1c, 0xf8,
                                       0x48, 0x8b, 0x15, 0xf6, 0x63, 0xec, 0x02, 0x48, 0x85, 0xd2};

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_ship_patch")) return;
    const auto mode = splug::ini_mode(L"ship", L"mode", L"off");
    ssp_a = splug::ini_int(L"ship", L"ahead_ship", 6);
    ssp_b = splug::ini_int(L"ship", L"ahead_slot", 4);
    ssp_c = splug::ini_int(L"ship", L"ahead_fleet", 2);
    ssp_d = splug::ini_int(L"ship", L"ahead_owner", 1);
    ssp_comp = splug::ini_int(L"ship", L"components", 1);
    ssp_e1 = splug::ini_int(L"ship", L"ahead_comp_list", 4);
    ssp_e2 = splug::ini_int(L"ship", L"ahead_comp", 2);
    splug::log("stellaris_ship_patch 0.3.0: build %s, mode=%s, ahead %lld/%lld/%lld/%lld", splug::state().build,
               splug::mode_name(mode), static_cast<long long>(ssp_a), static_cast<long long>(ssp_b),
               static_cast<long long>(ssp_c), static_cast<long long>(ssp_d));
    if (mode != splug::Mode::on) return;
    splug::log("components=%lld ahead %lld/%lld, ssp_comp at rva %llx", static_cast<long long>(ssp_comp),
               static_cast<long long>(ssp_e1), static_cast<long long>(ssp_e2),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&ssp_comp) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (ssp_d < 1 || ssp_c < ssp_d || ssp_b < ssp_c || ssp_a < ssp_b || ssp_a > 64 || ssp_e2 < 1 ||
        ssp_e1 < ssp_e2 || ssp_e1 > 64) {
        splug::log("REFUSED: need 64 >= ahead_ship >= ahead_slot >= ahead_fleet >= ahead_owner >= 1");
        return;
    }
    if (!splug::bytes_match(kSite, kSiteBytes)) {
        splug::log("REFUSED: bytes differ at %llx; no bytes changed", static_cast<unsigned long long>(kSite));
        return;
    }
    ssp_ships_global = reinterpret_cast<std::uint64_t>(splug::rva(kShips));
    ssp_db_global = reinterpret_cast<std::uint64_t>(splug::rva(kDb));
    ssp_default_global = reinterpret_cast<std::uint64_t>(splug::rva(kDefault));
    ssp_resume = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    std::uint8_t* site = splug::rva(kSite);
    // переходник рядом с сайтом: jmp [rip+0]; dq ship_prefetch_stub
    std::uint8_t* tramp = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!tramp) {
        splug::log("REFUSED: no memory near site");
        return;
    }
    std::uint8_t code[14] = {0xff, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uint64_t>(&ship_prefetch_stub);
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
    const auto rel = static_cast<std::int32_t>(disp);
    std::uint8_t patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    std::memcpy(patch + 1, &rel, 4);
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write jump");
        return;
    }
    splug::log("hook installed at %llx -> tramp %p -> stub %p", static_cast<unsigned long long>(kSite),
               static_cast<void*>(tramp), reinterpret_cast<void*>(target));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
