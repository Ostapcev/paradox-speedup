// Stellaris Fleet Patch 0.2.0, Stellaris 4.5.4 x64 — лишний сброс состояния движения флотов.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140e2d7b0  движение флотов (в микротике 140252c40, 10 раз в день); между параллельными
//              фазами — последовательный цикл по всем флотам списка: call 140e2fd80 (140e2d9a8).
//   140e2fd80  для каждого флота: проверка 140c80700, при необходимости — шаг движения, и ВСЕГДА
//              в конце (140e2ff31) сброс структуры entry+0x188 (0x100 байт):
//                  memset(tmp, 0, 0x100); 140e2ca40(tmp) /* значения по умолчанию */;
//                  140e2ff70(entry + 0x188, tmp)       /* поле за полем */;
//              У стоящих флотов структура уже пустая, и сброс ничего не меняет, но стоит
//              ~2,3 % главного потока в 2300 году на TEST5000 (вместе с проверкой — 4,4 %).
//
// В 2300 году на TEST5000 ~15 тыс. флотов за микротик, и у 96,5 % состояние перед сбросом НЕ
// пустое (заполняется параллельными фазами движения), т. е. сброс нужен; дорог он тем, что
// идёт в три прохода по 0x100 байт (memset временной копии, конструктор, копирование полей).
//
// Плагин заменяет блок 140e2ff31..140e2ff5b (сброс) переходом в свой код:
//   on      пишет значения по умолчанию прямо в entry+0x188 — ровно те поля, что пишет
//           140e2ff70, теми же значениями (результат байт в байт тот же, один проход);
//   shadow  сброс как в игре, а рядом наш сброс в копии; 0x100 байт сравниваются
//           (несовпадение — счётчик mismatch в логе);
//   off     ничего не меняет.
//
// Подкачка (prefetch, с 0.2.0, ключ prefetch=on). В том же цикле 140e2d9a0..9b3
// (rbx = i, r15 = n, [rbp+8] = массив записей) проверка 140c80700(fleet = [entry+8]) ждёт память
// на полях флота +0x1280 и +0x44c (~1,3 % главного потока). Плагин заменяет call 140e2fd80
// (140e2d9a8) вызовом своего кода: подкачать строку записи i+a (+8) и строки записи и флота i+b
// (запись +0x78, +0x180; флот +0x32c, +0x44c, +0x1268, +0x1280, +0x1340), затем jmp 140e2fd80
// (rcx не тронут, адрес возврата тот же). Настоящие чтения — только массив и [entry+8], которые
// игра и так читает; остальное — prefetch.
//
// Выход из блока — на 140e2ff5c (эпилог fd80); единственный вызывающий (140e2d9a8) не
// использует возвращаемое значение. rbx в блоке = entry (сохраняется по ABI).

#include <windows.h>

#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>

#include "call_patch.hpp"

extern "C" {
std::uint64_t sfp_fd80 = 0;           // 140e2fd80
std::int64_t sfp_a = 8, sfp_b = 4;    // на сколько записей вперёд
void fleet_prefetch_stub();
}

// вход по call из 140e2d9a8: rcx = запись i, rbx = i, r15 = n, [rbp+8] = массив
__attribute__((naked)) void fleet_prefetch_stub() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    mov rax, qword ptr [rbp + 8]\n"
        "    mov rdx, qword ptr [rip + sfp_a]\n"
        "    add rdx, rbx\n"
        "    cmp rdx, r15\n"
        "    jge 1f\n"
        "    mov rdx, qword ptr [rax + rdx*8]\n"
        "    prefetcht0 [rdx + 8]\n"
        "1:\n"
        "    mov rdx, qword ptr [rip + sfp_b]\n"
        "    add rdx, rbx\n"
        "    cmp rdx, r15\n"
        "    jge 2f\n"
        "    mov rdx, qword ptr [rax + rdx*8]\n"
        "    prefetcht0 [rdx + 0x78]\n"
        "    prefetcht0 [rdx + 0x180]\n"
        "    mov rdx, qword ptr [rdx + 8]\n"
        "    prefetcht0 [rdx + 0x32c]\n"
        "    prefetcht0 [rdx + 0x44c]\n"
        "    prefetcht0 [rdx + 0x1268]\n"
        "    prefetcht0 [rdx + 0x1280]\n"
        "    prefetcht0 [rdx + 0x1340]\n"
        "2:\n"
        "    jmp qword ptr [rip + sfp_fd80]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kSite = 0x140e2ff31;
constexpr std::uint64_t kResume = 0x140e2ff5c;
constexpr std::uint64_t kInitDefault = 0x140e2ca40;  // void* (void* tmp)
constexpr std::uint64_t kAssign = 0x140e2ff70;       // void* (void* dst, const void* src)
constexpr std::size_t kStateOffset = 0x188;
constexpr std::size_t kStateSize = 0x100;

// 140e2ff31: xor edx,edx; mov r8d,0x100; lea rcx,[rsp+0x68]; call memset; lea rcx,[rsp+0x68];
//            call 140e2ca40; mov rdx,rax; lea rcx,[rbx+0x188]; call 140e2ff70
constexpr std::uint8_t kSiteBytes[] = {0x33, 0xd2, 0x41, 0xb8, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8d,
    0x4c, 0x24, 0x68, 0xe8, 0x3d, 0xbd, 0x1f, 0x01, 0x48, 0x8d, 0x4c, 0x24, 0x68, 0xe8, 0xf3, 0xca,
    0xff, 0xff, 0x48, 0x8b, 0xd0, 0x48, 0x8d, 0x8b, 0x88, 0x01, 0x00, 0x00, 0xe8, 0x14, 0x00, 0x00,
    0x00};

using InitFn = void* (*)(void*);
using AssignFn = void* (*)(void*, const void*);
InitFn init_default = nullptr;
AssignFn assign = nullptr;

splug::Mode mode = splug::Mode::off;
double report_s = 60.0;

// цикл 140e2d9a8 выполняет только главный поток — счётчики без атомарности (lock add
// ~300 тыс. раз в секунду съел бы часть выигрыша)
long long calls = 0, already_default = 0, mismatch = 0, not_default_after = 0;
LARGE_INTEGER freq{};
std::int64_t last_report = 0;

template <class T>
inline T at(const std::uint8_t* p, std::size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

// Все поля, которые пишет 140e2ff70, против значений из memset(0) + 140e2ca40.
bool is_default(const std::uint8_t* s) {
    constexpr std::uint32_t none = 0xffffffffu;
    if (at<std::uint32_t>(s, 0x00) != none || s[0x04] != 0 || s[0x08] != 0) return false;
    // 6 одинаковых блоков: q,q,q = 0; dword = -1; byte, byte = 0
    static constexpr std::size_t blocks[][2] = {
        {0x20, 0x38}, {0x50, 0x68}, {0x88, 0xa0}, {0xb0, 0xc8}, {0xd8, 0xf0}};
    for (const auto& b : blocks) {
        if (at<std::uint64_t>(s, b[0]) | at<std::uint64_t>(s, b[0] + 8) | at<std::uint64_t>(s, b[0] + 16))
            return false;
        if (at<std::uint32_t>(s, b[1]) != none || s[b[1] + 4] != 0 || s[b[1] + 5] != 0) return false;
    }
    if (at<std::uint32_t>(s, 0x78) != 0xau || at<std::uint32_t>(s, 0x7c) != none) return false;
    return s[0xf8] == 0;
}

template <class T>
inline void put(std::uint8_t* p, std::size_t off, T v) {
    std::memcpy(p + off, &v, sizeof(T));
}

// То же, что memset(tmp,0) + 140e2ca40(tmp) + 140e2ff70(s, tmp), одним проходом: пишет ровно
// те поля, что пишет 140e2ff70, значениями по умолчанию. Опциональное поле +8/+0x10: у
// источника оно пустое, поэтому 140e2ff70 только снимает флаг +8 (значение +0x10 не трогает).
void direct_reset(std::uint8_t* s) {
    constexpr std::uint32_t none = 0xffffffffu;
    put<std::uint32_t>(s, 0x00, none);
    s[0x04] = 0;
    s[0x08] = 0;
    static constexpr std::size_t blocks[][2] = {
        {0x20, 0x38}, {0x50, 0x68}, {0x88, 0xa0}, {0xb0, 0xc8}, {0xd8, 0xf0}};
    for (const auto& b : blocks) {
        put<std::uint64_t>(s, b[0], 0);
        put<std::uint64_t>(s, b[0] + 8, 0);
        put<std::uint64_t>(s, b[0] + 16, 0);
        put<std::uint32_t>(s, b[1], none);
        s[b[1] + 4] = 0;
        s[b[1] + 5] = 0;
    }
    put<std::uint32_t>(s, 0x78, 0xau);
    put<std::uint32_t>(s, 0x7c, none);
    s[0xf8] = 0;
}

void original_reset(std::uint8_t* state) {
    alignas(16) std::uint8_t tmp[kStateSize];
    std::memset(tmp, 0, sizeof(tmp));
    assign(state, init_default(tmp));
}

void report() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    if (static_cast<double>(v.QuadPart - last_report) < report_s * static_cast<double>(freq.QuadPart)) return;
    last_report = v.QuadPart;
    const long long c = calls, d = already_default;
    calls = already_default = 0;
    splug::log("%s: resets %lld, already default %lld (%.1f%%), mismatch %lld, not default after %lld",
               splug::mode_name(mode), c, d, c ? 100.0 * static_cast<double>(d) / static_cast<double>(c) : 0.0,
               mismatch, not_default_after);
}

void hook(std::uint8_t* entry) {
    std::uint8_t* state = entry + kStateOffset;
    ++calls;
    if (mode == splug::Mode::on) {
        direct_reset(state);
    } else {
        // shadow: сброс как в игре; рядом — наш сброс в копии, байт в байт сравнить
        if (is_default(state)) ++already_default;
        alignas(16) std::uint8_t ours[kStateSize];
        std::memcpy(ours, state, kStateSize);
        direct_reset(ours);
        original_reset(state);
        if (std::memcmp(ours, state, kStateSize) != 0) ++mismatch;
        if (!is_default(state)) ++not_default_after;
    }
    if ((calls & 4095) == 0) report();
}

constexpr std::uint64_t kLoopCall = 0x140e2d9a8;
constexpr std::uint64_t kFd80 = 0x140e2fd80;
// 140e2d9a0: mov rcx,[rbp+8]; mov rcx,[rcx+rbx*8]; call 140e2fd80; inc rbx; cmp rbx,r15; jl
constexpr std::uint8_t kLoopBytes[] = {0x48, 0x8b, 0x4d, 0x08, 0x48, 0x8b, 0x0c, 0xd9, 0xe8, 0xd3,
                                       0x23, 0x00, 0x00, 0x48, 0xff, 0xc3, 0x49, 0x3b, 0xdf, 0x7c, 0xeb};

void install_prefetch() {
    if (splug::ini_mode(L"fleet", L"prefetch", L"off") != splug::Mode::on) return;
    sfp_a = splug::ini_int(L"fleet", L"prefetch_entry", 8);
    sfp_b = splug::ini_int(L"fleet", L"prefetch_fleet", 4);
    if (sfp_b < 1 || sfp_a < sfp_b || sfp_a > 64) {
        splug::log("prefetch REFUSED: need 64 >= prefetch_entry >= prefetch_fleet >= 1");
        return;
    }
    if (!splug::bytes_match(0x140e2d9a0, kLoopBytes)) {
        splug::log("prefetch REFUSED: loop bytes differ; no bytes changed");
        return;
    }
    sfp_fd80 = reinterpret_cast<std::uint64_t>(splug::rva(kFd80));
    static const std::uint8_t expected[5] = {0xe8, 0xd3, 0x23, 0x00, 0x00};
    static splug::CallPatch patch;
    if (const char* err = splug::prepare_call(splug::rva(kLoopCall), expected,
                                              reinterpret_cast<const void*>(&fleet_prefetch_stub), patch)) {
        splug::log("prefetch REFUSED: %s", err);
        return;
    }
    if (!splug::commit_call(patch)) {
        splug::log("prefetch REFUSED: cannot patch call");
        return;
    }
    splug::log("prefetch installed at %llx, ahead %lld/%lld", static_cast<unsigned long long>(kLoopCall),
               static_cast<long long>(sfp_a), static_cast<long long>(sfp_b));
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_fleet_patch")) return;
    QueryPerformanceFrequency(&freq);
    mode = splug::ini_mode(L"fleet", L"mode", L"off");
    report_s = splug::ini_int(L"fleet", L"report_s", 60);
    if (report_s < 1) report_s = 1;
    splug::log("stellaris_fleet_patch 0.2.0: build %s, mode=%s", splug::state().build, splug::mode_name(mode));
    install_prefetch();
    if (mode == splug::Mode::off) return;
    if (!splug::bytes_match(kSite, kSiteBytes)) {
        splug::log("REFUSED: bytes differ at %llx; no bytes changed", static_cast<unsigned long long>(kSite));
        return;
    }
    init_default = reinterpret_cast<InitFn>(splug::rva(kInitDefault));
    assign = reinterpret_cast<AssignFn>(splug::rva(kAssign));
    // переходник: mov rcx, rbx; mov rax, hook; call rax; mov rax, resume; jmp rax
    // (в точке 140e2ff31 rsp выровнен на 16, область [rsp, rsp+0x20) — теневая для вызовов fd80)
    auto* stub = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        splug::log("REFUSED: cannot allocate stub");
        return;
    }
    std::uint8_t code[] = {0x48, 0x8b, 0xcb, 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
                           0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    const auto h = reinterpret_cast<std::uint64_t>(&hook);
    const auto r = reinterpret_cast<std::uint64_t>(splug::rva(kResume));
    std::memcpy(code + 5, &h, 8);
    std::memcpy(code + 17, &r, 8);
    std::memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&last_report));
    if (!splug::write_jump(splug::rva(kSite), stub)) {
        splug::log("REFUSED: cannot write jump");
        return;
    }
    splug::log("hook installed at %llx -> stub %p", static_cast<unsigned long long>(kSite), stub);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
