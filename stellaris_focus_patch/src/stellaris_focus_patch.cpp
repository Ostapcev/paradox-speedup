// Stellaris Focus Patch 0.1.0 (измерение), Stellaris 4.5.4 x64 — карточки фокуса стран.
//
// Что в игре (stellaris.exe 4.5.4.1b4f):
//   14025e825  call 14096f500 — в DailyUpdate, в цикле по странам (после эдиктов страны 1406f2590):
//              менеджер карточек страны (country + 0x3678). Для каждой активной карточки
//              (ids: +0x18, число: +0x24; база карточек DAT_14311bcb8, запись: +0x10 id, +0x18 тип,
//              +0x20 «завершена») проверяет условие типа (*(тип + 0x158))->vfunc[0x18] и при успехе
//              завершает её (1408f3090, on_success). Имя типа — std::string в тип + 0x20.
//   В 2300 году на TEST5000 — 5,1 % главного потока.
//
// Режимы ([focus] mode=):
//   off     ничего не меняет;
//   shadow  ничего не меняет в логике: засекает время каждого вызова и пишет выборку
//           (страна, мкс, типы ожидающих карточек) в focus_samples.csv рядом с плагином,
//           сводку — в лог раз в report_s секунд.

#include <windows.h>

#include "call_patch.hpp"
#include "stellaris_plugin.hpp"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint64_t kSite = 0x14025e825;
constexpr std::uint8_t kSiteBytes[5] = {0xe8, 0xd6, 0x0c, 0x71, 0x00};  // call 14096f500
constexpr std::uint64_t kCardDb = 0x14311bcb8;

using CardsFn = void (*)(void*);

splug::Mode mode = splug::Mode::off;
double report_s = 60.0;
CardsFn original = nullptr;
splug::CallPatch patch;

LARGE_INTEGER freq{};
std::int64_t last_report = 0;
FILE* samples = nullptr;

struct TypeStat {
    int id = 0;
    std::string name;
    double share_us = 0;  // время вызова, поделённое поровну между ожидающими карточками
    long long seen = 0;
};
std::unordered_map<const void*, TypeStat> types;
long long calls = 0, calls_total = 0;
double total_us = 0;

std::int64_t now() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double us(std::int64_t ticks) { return 1e6 * static_cast<double>(ticks) / static_cast<double>(freq.QuadPart); }

const std::uint8_t* card_by_id(std::uint32_t id) {
    auto* db = *splug::rva<const std::uint8_t*>(kCardDb);
    if (!db) return nullptr;
    const std::uint32_t index = id & 0xffffff;
    if (index >= *reinterpret_cast<const std::uint32_t*>(db + 0x20)) return nullptr;
    const auto* table = *reinterpret_cast<const std::uint8_t* const*>(db + 0x18);
    const auto* card = *reinterpret_cast<const std::uint8_t* const*>(table + 8 + std::size_t(index) * 0x10);
    if (!card || *reinterpret_cast<const std::uint32_t*>(card + 0x10) != id) return nullptr;
    return card;
}

TypeStat& type_of(const void* type) {
    auto it = types.find(type);
    if (it != types.end()) return it->second;
    TypeStat t;
    t.id = static_cast<int>(types.size());
    const auto* p = static_cast<const std::uint8_t*>(type);
    const auto cap = *reinterpret_cast<const std::uint64_t*>(p + 0x38);
    const auto len = *reinterpret_cast<const std::uint64_t*>(p + 0x30);
    const char* s = cap > 0xf ? *reinterpret_cast<const char* const*>(p + 0x20) : reinterpret_cast<const char*>(p + 0x20);
    t.name.assign(s, len < 200 ? len : 200);
    if (samples) std::fprintf(samples, "T,%d,%s\n", t.id, t.name.c_str());
    return types.emplace(type, t).first->second;
}

void report(std::int64_t t) {
    const double span = us(t - last_report) / 1e6;
    if (span < report_s) return;
    splug::log("%.0f s: card checks %lld, %.0f ms (%.1f%% of wall)", span, calls, total_us / 1000,
               total_us / (1e4 * span));
    std::vector<const TypeStat*> v;
    for (auto& kv : types) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->share_us > b->share_us; });
    for (std::size_t i = 0; i < v.size() && i < 12; ++i)
        splug::log("  %6.0f ms  %7lld pending  %6.1f us/check  %s", v[i]->share_us / 1000, v[i]->seen,
                   v[i]->seen ? v[i]->share_us / v[i]->seen : 0.0, v[i]->name.c_str());
    calls = 0;
    total_us = 0;
    for (auto& kv : types) kv.second.share_us = 0, kv.second.seen = 0;
    last_report = t;
    if (samples) std::fflush(samples);
}

void cards_hook(void* manager) {
    // ожидающие карточки до вызова
    const void* pending[16];
    int n = 0;
    const auto* m = static_cast<const std::uint8_t*>(manager);
    const auto* ids = *reinterpret_cast<const std::uint32_t* const*>(m + 0x18);
    const int count = *reinterpret_cast<const std::int32_t*>(m + 0x24);
    for (int i = 0; i < count && n < 16; ++i) {
        const auto* card = card_by_id(ids[i]);
        if (card && card[0x20] == 0) pending[n++] = *reinterpret_cast<const void* const*>(card + 0x18);
    }
    const std::int64_t t0 = now();
    original(manager);
    const std::int64_t t1 = now();
    const double dt = us(t1 - t0);
    ++calls;
    ++calls_total;
    total_us += dt;
    const auto country_id = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(manager) - 0x3678 + 0x20);
    TypeStat* stats[16];
    int k = 0;
    for (int i = 0; i < n; ++i)
        if (pending[i]) stats[k++] = &type_of(pending[i]);  // строки T пишутся до строки S
    if (samples) std::fprintf(samples, "S,%u,%.1f", country_id, dt);
    for (int i = 0; i < k; ++i) {
        stats[i]->share_us += dt / n;
        ++stats[i]->seen;
        if (samples) std::fprintf(samples, ",%d", stats[i]->id);
    }
    if (samples) std::fputc('\n', samples);
    report(t1);
}

double ini_double(const wchar_t* key, double def) {
    const std::wstring s = splug::ini_string(L"focus", key, L"");
    return s.empty() ? def : std::wcstod(s.c_str(), nullptr);
}

void install(HMODULE module) {
    if (!splug::init(module, L"stellaris_focus_patch")) return;
    QueryPerformanceFrequency(&freq);
    mode = splug::ini_mode(L"focus", L"mode", L"off");
    report_s = ini_double(L"report_s", 60.0);
    if (report_s < 1) report_s = 1;
    splug::log("stellaris_focus_patch 0.1.0: build %s, mode=%s", splug::state().build, splug::mode_name(mode));
    if (mode == splug::Mode::off) return;
    if (mode == splug::Mode::on) {
        splug::log("mode=on is not implemented yet; measuring as shadow");
        mode = splug::Mode::shadow;
    }
    auto* site = splug::rva<std::uint8_t>(kSite);
    original = reinterpret_cast<CardsFn>(splug::call_target(site));
    if (const char* e = splug::prepare_call(site, kSiteBytes, reinterpret_cast<const void*>(&cards_hook), patch)) {
        splug::log("REFUSED: %s; no bytes changed", e);
        return;
    }
    const std::wstring path = splug::state().dir + L"\\focus_samples.csv";
    samples = _wfopen(path.c_str(), L"w");
    last_report = now();
    if (!splug::commit_call(patch)) {
        splug::log("REFUSED: cannot write call site");
        return;
    }
    splug::log("hook installed at %llx; samples -> %ls", static_cast<unsigned long long>(kSite), path.c_str());
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        install(module);
    }
    return TRUE;
}
