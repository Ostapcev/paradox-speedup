// Stellaris Tick Patch 0.3.0, Stellaris 4.5.4 x64 — подкачка памяти в двух циклах микротика 140252c40.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   140252c40 (из 140251780) дважды проходит по массиву id [14280e828] (число [rsp+0x40]); для каждого id
//   объект берётся из таблицы [1431199e8] (слот +0x18 + (id & 0xffffff) * 16 + 8, проверка obj+0x30 == id).
//   Цикл 1 (голова 1402530f0, индекс rdi, предел r15): vcall obj+0x18 -> +0x20, [obj+0x810] -> +0x1268,
//     +0x32c, obj+0x7b4, вставка в хэш-таблицу 1434f5740 по ключу obj+0x30 (1402c7440).
//   Цикл 2 (голова 1402536e2, индекс r15, предел r12): поиск того же ключа в 1434f5740 (1402b4910),
//     копирование корзины, 14083f6d0(obj+0x930), 14080a4c0(obj+0x910) (читает obj+0x92c).
//   Таблица 1434f5740: база +8, маска +0x14; корзина 0xc0 байт (Robin Hood), хэш FNV-1a по 4 байтам id.
//   На BENCH2300 (14 рабочих потоков) промахи на этих чтениях — ~5 % главного потока: 1402b4910 +0x64,
//   1402c7440 +0x4d, 14080a4c0 +0x10, 14025313b (ожидание vtable) почти целиком — ожидание памяти.
//
// [tick] prefetch=1 — на голове каждого цикла (7 байт mov rdx,[1431199e8] -> jmp) вызывается stp_pf(i, n,
//   цикл): за far элементов вперёд — слот таблицы, за mid — поля объекта, за near — корзина 1434f5740
//   (3 строки) и для цикла 1 объект [obj+0x810]. Логику не меняет: только prefetch того, что игра
//   и так прочитает; явные чтения — лишь слот и obj+0x30/+0x810 живых объектов, как в самой игре.
//   0.2.0: ещё цикл A (голова 140253552, id по указателю, таблица [14311bd30], vcall [obj]+0x20).
//   Цикл B (1402535e0, корабли) уже подкачивает stellaris_ship_patch. stp_on — маска: бит 0 — циклы 1/2,
//   бит 1 — цикл A.
//   0.3.0: цикл P (голова 140645120 в 1406450c0, бит 3 = 8): в позднем этапе 140645290 ждёт объект
//   (+0x50..+0x78) и страну по obj+0x78 (таблица 14311bed8) — ~2–3 % главного потока.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

extern "C" {
std::int64_t stp_on = 0;
std::int64_t stp_far = 16, stp_mid = 8, stp_near = 4;
std::int64_t stp_pfar = 64, stp_pmid = 32, stp_pnear = 16, stp_pobj = 8;  // цикл P: тело короткое, дистанции больше
std::uint64_t stp_db_global = 0;  // адрес глобала 1431199e8
std::uint64_t stp_resume1 = 0;    // 1402530f7
std::uint64_t stp_resume2 = 0;    // 1402536e9
void stp_pf(std::int64_t i, std::int64_t n, int loop, const std::uint32_t* ids);
void stp_stub1();
void stp_stub2();
std::uint64_t stp_dbA_global = 0;   // адрес глобала 14311bd30 (таблица объектов цикла A)
std::uint64_t stp_resumeA = 0;      // 140253559
void stp_stubA();
std::uint64_t stp_dbP_global = 0;   // адрес глобала 14311bdb8 (объекты цикла P)
std::uint64_t stp_resumeP = 0;      // 140645127
void stp_stubP();
}

// Голова цикла: исходная инструкция mov rdx,[1431199e8] (7 байт) заменена переходом сюда.
// Сохраняем изменчивые регистры (в голове цикла они не живы — тело цикла содержит вызовы — но так надёжнее),
// 7 push + 0x28 = 0x60: стек остаётся выровнен на 16.
#define STP_STUB(NAME, IDX, CNT, LOOP, RESUME, BIT, DEST, GLOBAL) \
    __attribute__((naked)) void NAME() {                         \
        asm volatile(                                            \
            ".intel_syntax noprefix\n"                           \
            "    test qword ptr [rip + stp_on], " BIT "\n"       \
            "    jz 1f\n"                                        \
            "    push rax\n"                                     \
            "    push rcx\n"                                     \
            "    push rdx\n"                                     \
            "    push r8\n"                                      \
            "    push r9\n"                                      \
            "    push r10\n"                                     \
            "    push r11\n"                                     \
            "    sub rsp, 0x28\n"                                \
            "    mov rcx, " IDX "\n"                             \
            "    mov rdx, " CNT "\n"                             \
            "    mov r8d, " LOOP "\n"                            \
            "    call stp_pf\n"                                  \
            "    add rsp, 0x28\n"                                \
            "    pop r11\n"                                      \
            "    pop r10\n"                                      \
            "    pop r9\n"                                       \
            "    pop r8\n"                                       \
            "    pop rdx\n"                                      \
            "    pop rcx\n"                                      \
            "    pop rax\n"                                      \
            "1:\n"                                               \
            "    mov " DEST ", qword ptr [rip + " GLOBAL "]\n"   \
            "    mov " DEST ", qword ptr [" DEST "]\n"           \
            "    jmp qword ptr [rip + " RESUME "]\n"             \
            ".att_syntax\n");                                    \
    }

STP_STUB(stp_stub1, "rdi", "r15", "1", "stp_resume1", "1", "rdx", "stp_db_global")
STP_STUB(stp_stub2, "r15", "r12", "2", "stp_resume2", "1", "rdx", "stp_db_global")
// цикл A: rdi — указатель на текущий id, r15 — конец; исходная инструкция mov r8, [14311bd30]
STP_STUB(stp_stubA, "rdi", "r15", "3", "stp_resumeA", "2", "r8", "stp_dbA_global")

// цикл P (1406450c0, голова 140645120): r15 — индекс, r12 — число, [r13+8] — массив id;
// исходная инструкция mov rdx, [14311bdb8]. Вызов stp_pf(i, n, 4, ids) — ids в r9.
__attribute__((naked)) void stp_stubP() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "    test qword ptr [rip + stp_on], 8\n"
        "    jz 1f\n"
        "    push rax\n"
        "    push rcx\n"
        "    push rdx\n"
        "    push r8\n"
        "    push r9\n"
        "    push r10\n"
        "    push r11\n"
        "    sub rsp, 0x28\n"
        "    mov rcx, r15\n"
        "    mov rdx, r12\n"
        "    mov r8d, 4\n"
        "    mov r9, qword ptr [r13 + 8]\n"
        "    call stp_pf\n"
        "    add rsp, 0x28\n"
        "    pop r11\n"
        "    pop r10\n"
        "    pop r9\n"
        "    pop r8\n"
        "    pop rdx\n"
        "    pop rcx\n"
        "    pop rax\n"
        "1:\n"
        "    mov rdx, qword ptr [rip + stp_dbP_global]\n"
        "    mov rdx, qword ptr [rdx]\n"
        "    jmp qword ptr [rip + stp_resumeP]\n"
        ".att_syntax\n");
}

namespace {

constexpr std::uint64_t kIds = 0x14280e828;
constexpr std::uint64_t kDb = 0x1431199e8;
constexpr std::uint64_t kTable = 0x1434f5740;

const std::uint32_t* const* g_ids = nullptr;
const std::uint8_t* const* g_db = nullptr;
const std::uint8_t* g_table = nullptr;

inline void pf(const void* p) { _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0); }

inline const std::uint8_t* slot_addr(const std::uint8_t* db, std::uint32_t id) {
    const std::uint32_t idx = id & 0xffffff;
    std::uint32_t size;
    std::memcpy(&size, db + 0x20, 4);
    if (idx >= size) return nullptr;
    const std::uint8_t* slots;
    std::memcpy(&slots, db + 0x18, 8);
    return slots + static_cast<std::uint64_t>(idx) * 16 + 8;
}

inline const std::uint8_t* bucket_of(std::uint32_t id) {
    std::uint32_t h = 0x811c9dc5u;
    for (int b = 0; b < 4; ++b) {
        h ^= (id >> (8 * b)) & 0xff;
        h *= 0x1000193u;
    }
    std::int32_t mask;
    const std::uint8_t* base;
    std::memcpy(&mask, g_table + 0x14, 4);
    std::memcpy(&base, g_table + 8, 8);
    if (!base) return nullptr;
    const std::int64_t idx = static_cast<std::int64_t>(static_cast<std::int32_t>(h)) & mask;
    return base + idx * 0xc0;
}

}  // namespace

namespace {
const std::uint8_t* const* g_dbA = nullptr;

inline const std::uint8_t* obj_checked(const std::uint8_t* db, std::uint32_t id, int id_off) {
    const auto* s = slot_addr(db, id);
    if (!s) return nullptr;
    const std::uint8_t* obj;
    std::memcpy(&obj, s, 8);
    if (!obj) return nullptr;
    std::uint32_t oid;
    std::memcpy(&oid, obj + id_off, 4);
    return oid == id ? obj : nullptr;
}

// цикл A: перебор id по указателю [p, end); объект из [14311bd30] (id на +0x18), vcall [obj] +0x20.
void pf_loopA(const std::uint32_t* p, const std::uint32_t* end) {
    const std::uint8_t* db = *g_dbA;
    if (!db) return;
    if (p + stp_far < end)
        if (const auto* s = slot_addr(db, p[stp_far])) pf(s);
    if (p + stp_mid < end)
        if (const auto* s = slot_addr(db, p[stp_mid])) {
            const std::uint8_t* obj;
            std::memcpy(&obj, s, 8);
            if (obj) pf(obj);
        }
    if (p + stp_near < end)
        if (const auto* obj = obj_checked(db, p[stp_near], 0x18)) {
            const std::uint8_t* vt;
            std::memcpy(&vt, obj, 8);
            if (vt) pf(vt + 0x20);
        }
}

// цикл P: объекты id из [14311bdb8] (id на +8): 140645290 читает obj+0x50/+0x68/+0x78 и страну
// [14311bed8] по obj+0x78 (id на +8), затем 14086f6e0 (две страны).
const std::uint8_t* const* g_dbP = nullptr;
const std::uint8_t* const* g_dbC = nullptr;

void pf_loopP(std::int64_t i, std::int64_t n, const std::uint32_t* ids) {
    const std::uint8_t* db = *g_dbP;
    const std::uint8_t* dc = *g_dbC;
    if (!db || !ids) return;
    if (i + stp_pfar < n)
        if (const auto* s = slot_addr(db, ids[i + stp_pfar])) pf(s);
    if (i + stp_pmid < n)
        if (const auto* s = slot_addr(db, ids[i + stp_pmid])) {
            const std::uint8_t* obj;
            std::memcpy(&obj, s, 8);
            if (obj) {
                pf(obj);
                pf(obj + 0x40);
            }
        }
    if (!dc) return;
    if (i + stp_pnear < n)
        if (const auto* obj = obj_checked(db, ids[i + stp_pnear], 8)) {
            std::uint32_t cid;
            std::memcpy(&cid, obj + 0x78, 4);
            if (const auto* s = slot_addr(dc, cid)) pf(s);
        }
    const std::int64_t k = stp_pobj;
    if (i + k < n)
        if (const auto* obj = obj_checked(db, ids[i + k], 8)) {
            std::uint32_t cid;
            std::memcpy(&cid, obj + 0x78, 4);
            if (const auto* s = slot_addr(dc, cid)) {
                const std::uint8_t* c;
                std::memcpy(&c, s, 8);
                if (c) pf(c);
            }
        }
}

}  // namespace

extern "C" void stp_pf(std::int64_t i, std::int64_t n, int loop, const std::uint32_t* idsP) {
    if (loop == 4) {
        pf_loopP(i, n, idsP);
        return;
    }
    if (loop == 3) {
        pf_loopA(reinterpret_cast<const std::uint32_t*>(i), reinterpret_cast<const std::uint32_t*>(n));
        return;
    }
    const std::uint8_t* db = *g_db;
    const std::uint32_t* ids = *g_ids;
    if (!db || !ids) return;
    if (i + stp_far < n) {
        if (const auto* s = slot_addr(db, ids[i + stp_far])) pf(s);
    }
    if (i + stp_mid < n) {
        if (const auto* s = slot_addr(db, ids[i + stp_mid])) {
            const std::uint8_t* obj;
            std::memcpy(&obj, s, 8);
            if (obj) {
                pf(obj);         // +0x18 vtable, +0x30 id
                if (loop == 1) {
                    pf(obj + 0x7b4);
                    pf(obj + 0x810);
                    // бит 2: строка +0x7c0..+0x7ff — 140d79ea0 / 140d7f8e0 (через 140c67440) читают
                    // [fleet+0x798+0x50] = +0x7e8 и vtable компонента +0x7d0
                    if (stp_on & 4) pf(obj + 0x7c0);
                } else {
                    pf(obj + 0x910);  // +0x910..+0x93f: 14080a4c0 (+0x92c), 14083f6d0 (+0x930)
                }
            }
        }
    }
    if (i + stp_near < n) {
        const std::uint32_t id = ids[i + stp_near];
        if (const auto* s = slot_addr(db, id)) {
            const std::uint8_t* obj;
            std::memcpy(&obj, s, 8);
            std::uint32_t oid = 0;
            if (obj) std::memcpy(&oid, obj + 0x30, 4);
            if (obj && oid == id) {
                const std::uint8_t* vt;
                std::memcpy(&vt, obj + 0x18, 8);
                if (vt) pf(vt + 0x20);
                if (const auto* b = bucket_of(id)) {
                    pf(b);
                    pf(b + 0x40);
                    pf(b + 0x80);
                }
                if (loop == 1) {
                    const std::uint8_t* q;
                    std::memcpy(&q, obj + 0x810, 8);
                    if (q) {
                        pf(q + 0x32c);
                        pf(q + 0x1268);
                    }
                }
            }
        }
    }
}

namespace {

struct Site {
    std::uint64_t va;
    std::uint64_t resume;
    void (*stub)();
    std::uint64_t* resume_var;
    std::uint8_t bytes[7];
};

bool hook_site(const Site& s) {
    if (!splug::bytes_match(s.va, s.bytes)) {
        splug::log("REFUSED: bytes at %llx differ", static_cast<unsigned long long>(s.va));
        return false;
    }
    *s.resume_var = reinterpret_cast<std::uint64_t>(splug::rva(s.resume));
    auto* site = splug::rva<std::uint8_t>(s.va);
    std::uint8_t* tramp = splug::allocate_near(reinterpret_cast<std::uintptr_t>(site));
    if (!tramp) {
        splug::log("REFUSED: no memory near %llx", static_cast<unsigned long long>(s.va));
        return false;
    }
    const std::uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(tramp, jmp, sizeof(jmp));
    const auto stub = reinterpret_cast<std::uint64_t>(s.stub);
    std::memcpy(tramp + 6, &stub, sizeof(stub));
    DWORD old = 0;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, 14);
    std::uint8_t patch[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(tramp) -
                                               reinterpret_cast<std::intptr_t>(site + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    if (!splug::write_bytes(site, patch, sizeof(patch))) {
        splug::log("REFUSED: cannot write at %llx", static_cast<unsigned long long>(s.va));
        return false;
    }
    return true;
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_tick_patch")) return;
    const int on = splug::ini_int(L"tick", L"prefetch", 0);
    stp_far = splug::ini_int(L"tick", L"far", 16);
    stp_mid = splug::ini_int(L"tick", L"mid", 8);
    stp_near = splug::ini_int(L"tick", L"near", 4);
    splug::log("stellaris_tick_patch 0.3.0: build %s, prefetch=%d, far/mid/near %lld/%lld/%lld; stp_on at rva %llx",
               splug::state().build, on, static_cast<long long>(stp_far), static_cast<long long>(stp_mid),
               static_cast<long long>(stp_near),
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&stp_on) -
                                               reinterpret_cast<std::uintptr_t>(module)));
    if (on < 0) return;
    g_ids = splug::rva<const std::uint32_t*>(kIds);
    g_db = splug::rva<const std::uint8_t*>(kDb);
    g_table = splug::rva<const std::uint8_t>(kTable);
    stp_db_global = reinterpret_cast<std::uint64_t>(splug::rva(kDb));
    stp_on = on;  // бит 0 — циклы 1 и 2, бит 1 — циклы A и B
    // mov rdx, [rip + disp32] -> 1431199e8 в обеих головах
    const Site s1{0x1402530f0, 0x1402530f7, &stp_stub1, &stp_resume1, {0x48, 0x8b, 0x15, 0xf1, 0x68, 0xec, 0x02}};
    const Site s2{0x1402536e2, 0x1402536e9, &stp_stub2, &stp_resume2, {0x48, 0x8b, 0x15, 0xff, 0x62, 0xec, 0x02}};
    // обе головы проверяются до записи, чтобы не ставить половину
    if (!splug::bytes_match(s1.va, s1.bytes) || !splug::bytes_match(s2.va, s2.bytes)) {
        splug::log("REFUSED: loop head bytes differ; nothing changed");
        stp_on = 0;
        return;
    }
    const bool a = hook_site(s1), b = hook_site(s2);
    splug::log("loop 1 head %s, loop 2 head %s", a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked");
    if (a || b) splug::log("installed");
    g_dbA = splug::rva<const std::uint8_t*>(0x14311bd30);
    stp_dbA_global = reinterpret_cast<std::uint64_t>(splug::rva(0x14311bd30));
    // mov r8, [rip+disp] -> 14311bd30. Цикл B (1402535e0) уже подкачивает stellaris_ship_patch.
    const Site sa{0x140253552, 0x140253559, &stp_stubA, &stp_resumeA, {0x4c, 0x8b, 0x05, 0xd7, 0x87, 0xec, 0x02}};
    splug::log("loop A head %s", hook_site(sa) ? "hooked" : "NOT hooked");
    g_dbP = splug::rva<const std::uint8_t*>(0x14311bdb8);
    g_dbC = splug::rva<const std::uint8_t*>(0x14311bed8);
    stp_dbP_global = reinterpret_cast<std::uint64_t>(splug::rva(0x14311bdb8));
    // 0.3.0: цикл P (1406450c0, позднее время: попарная проверка объектов по странам), бит 3
    const Site sp{0x140645120, 0x140645127, &stp_stubP, &stp_resumeP, {0x48, 0x8b, 0x15, 0x91, 0x6c, 0xad, 0x02}};
    stp_pfar = splug::ini_int(L"tick", L"p_far", 64);
    stp_pmid = splug::ini_int(L"tick", L"p_mid", 32);
    stp_pnear = splug::ini_int(L"tick", L"p_near", 16);
    stp_pobj = splug::ini_int(L"tick", L"p_obj", 8);
    splug::log("loop P head %s; p distances at rva %llx", hook_site(sp) ? "hooked" : "NOT hooked",
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&stp_pfar) -
                                               reinterpret_cast<std::uintptr_t>(module)));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
