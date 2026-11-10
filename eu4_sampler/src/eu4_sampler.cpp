#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t default_duration_seconds = 30;
constexpr std::uint64_t default_interval_ms = 5;
// Глубина стека EU4 на дневном тике доходит до ~40 кадров; 32 обрезали корень.
constexpr unsigned max_frames = 128;

struct Options {
    DWORD process_id = 0;
    std::uint64_t duration_seconds = default_duration_seconds;
    std::uint64_t interval_ms = default_interval_ms;
    fs::path output = L"eu4_sampler_latest";
    bool wall = false;          // сэмплировать главный поток по реальному времени (ожидания тоже)
    DWORD wall_thread = 0;      // явный поток для --wall; 0 — выбрать по CPU
    bool wall_all = false;      // --wall-all: все потоки по реальному времени (ожидания тоже)
};

struct ModuleInfo {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::wstring name;
    std::wstring path;
};

struct Stat {
    std::uint64_t cpu_100ns = 0;
    std::uint64_t samples = 0;
};

struct FunctionStat {
    std::uint64_t leaf_100ns = 0;
    std::uint64_t inclusive_100ns = 0;
    std::uint64_t leaf_samples = 0;
    std::uint64_t inclusive_samples = 0;
};

using CallerStats = std::unordered_map<std::uint64_t, Stat>;
using ExternalCallerStats = std::unordered_map<std::uint64_t, CallerStats>;

// Полный стек (лист первым) + id потока; одинаковые стеки складываются.
struct StackKey {
    DWORD thread_id = 0;
    std::vector<std::uint64_t> frames;
    bool operator==(const StackKey& other) const {
        return thread_id == other.thread_id && frames == other.frames;
    }
};

struct StackKeyHash {
    std::size_t operator()(const StackKey& key) const {
        std::uint64_t hash = 1469598103934665603ull ^ key.thread_id;
        for (const auto frame : key.frames) {
            hash ^= frame;
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }
};

using StackStats = std::unordered_map<StackKey, Stat, StackKeyHash>;

struct ThreadState {
    HANDLE handle = nullptr;
    std::uint64_t previous_cpu = 0;
    std::uint64_t total_cpu = 0;
    std::uint64_t attributed_cpu = 0;
    std::uint64_t samples = 0;
    Clock::time_point next_sample{};
    Clock::time_point previous_wall{};
    bool initialized = false;
};

struct CapturedStack {
    std::vector<std::uint64_t> frames;
    bool contains_eu4 = false;
};

std::unordered_map<DWORD, ThreadState> threads;
volatile LONG stop_requested = 0;

std::uint64_t file_time_value(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

std::uint64_t thread_cpu(HANDLE thread, bool& valid) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    valid = GetThreadTimes(thread, &created, &exited, &kernel, &user) != 0;
    return file_time_value(kernel) + file_time_value(user);
}

void close_threads() {
    for (auto& [id, state] : threads) {
        if (state.handle) CloseHandle(state.handle);
    }
    threads.clear();
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

DWORD find_eu4_process() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"eu4.exe") == 0) {
                if (result) {
                    std::fwprintf(stderr, L"Multiple eu4.exe processes found; specify --pid.\n");
                    result = 0;
                    break;
                }
                result = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::vector<ModuleInfo> enumerate_modules(DWORD process_id) {
    std::vector<ModuleInfo> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return modules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            ModuleInfo info;
            info.base = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
            info.size = entry.modBaseSize;
            info.name = entry.szModule;
            info.path = entry.szExePath;
            modules.push_back(std::move(info));
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

const ModuleInfo* find_module(const std::vector<ModuleInfo>& modules, std::uint64_t address) {
    for (const auto& module : modules) {
        if (address >= module.base && address < module.base + module.size) return &module;
    }
    return nullptr;
}

BOOL CALLBACK read_target_memory(
    HANDLE process,
    DWORD64 address,
    PVOID buffer,
    DWORD size,
    LPDWORD bytes_read) {
    SIZE_T completed = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<const void*>(address), buffer, size, &completed)) {
        return FALSE;
    }
    if (bytes_read) *bytes_read = static_cast<DWORD>(completed);
    return completed == size ? TRUE : FALSE;
}

CapturedStack capture_stack(
    HANDLE process,
    HANDLE thread,
    const ModuleInfo& eu4,
    const std::vector<ModuleInfo>& modules) {
    CapturedStack result;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return result;

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    const bool have_context = GetThreadContext(thread, &context) != 0;
    if (have_context) {
        result.frames.push_back(context.Rip);
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        bool first_walk = true;
        for (unsigned index = 1; index < max_frames; ++index) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                    read_target_memory, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            // Первый вызов StackWalk64 возвращает текущий кадр (PC == RIP), а не
            // вызывающий; раньше он попадал в стек как лишний кадр «RIP-1».
            if (first_walk) {
                first_walk = false;
                if (frame.AddrPC.Offset == result.frames.front()) {
                    --index;
                    continue;
                }
            }
            // Non-leaf PCs are return addresses. Step back into the CALL instruction
            // so that RVA-to-function lookup cannot cross a function boundary.
            const auto address = frame.AddrPC.Offset - 1;
            if (!result.frames.empty() && result.frames.back() == address) break;
            result.frames.push_back(address);
        }
    }
    ResumeThread(thread);

    for (const auto address : result.frames) {
        const auto* module = find_module(modules, address);
        if (module && address >= eu4.base && address < eu4.base + eu4.size) {
            result.contains_eu4 = true;
            break;
        }
    }
    return result;
}

void refresh_threads(DWORD process_id) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id || threads.count(entry.th32ThreadID)) continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!handle) continue;
            ThreadState state;
            state.handle = handle;
            state.next_sample = Clock::now();
            threads.emplace(entry.th32ThreadID, state);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void discard_dead_threads() {
    for (auto it = threads.begin(); it != threads.end();) {
        if (!it->second.handle || WaitForSingleObject(it->second.handle, 0) == WAIT_OBJECT_0) {
            if (it->second.handle) CloseHandle(it->second.handle);
            it = threads.erase(it);
        } else {
            ++it;
        }
    }
}

void add_stat(std::unordered_map<std::uint64_t, Stat>& stats,
    std::uint64_t address, std::uint64_t cpu_delta) {
    auto& stat = stats[address];
    stat.cpu_100ns += cpu_delta;
    ++stat.samples;
}

void write_csv_header(FILE* file, const char* value) {
    std::fprintf(file, "%s\n", value);
}

std::string narrow_ascii(std::wstring value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 0 && character <= 127 ? static_cast<char>(character) : '_');
    }
    return result;
}

bool write_reports(
    const Options& options,
    DWORD process_id,
    const std::vector<ModuleInfo>& modules,
    const ModuleInfo& eu4,
    const std::unordered_map<std::uint64_t, Stat>& leaf,
    const ExternalCallerStats& external_callers,
    const std::unordered_map<std::uint64_t, FunctionStat>& functions,
    const StackStats& stacks,
    std::uint64_t elapsed_ms,
    std::uint64_t total_cpu,
    std::uint64_t attributed_cpu,
    std::uint64_t captured_samples) {
    std::error_code error;
    fs::create_directories(options.output, error);
    if (error) return false;

    std::vector<std::pair<std::uint64_t, Stat>> sorted_leaf(leaf.begin(), leaf.end());
    std::sort(sorted_leaf.begin(), sorted_leaf.end(), [](const auto& left, const auto& right) {
        return left.second.cpu_100ns > right.second.cpu_100ns;
    });
    const auto leaf_path = options.output / L"leaf.csv";
    FILE* file = _wfopen(leaf_path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "module,address,rva,estimated_cpu_ms,percent_of_attributed,samples");
    for (const auto& [address, stat] : sorted_leaf) {
        const auto* module = find_module(modules, address);
        const auto name = module ? narrow_ascii(module->name) : "unknown";
        const auto rva = module ? address - module->base : 0;
        std::fprintf(file, "%s,0x%llx,0x%llx,%.3f,%.6f,%llu\n", name.c_str(),
            static_cast<unsigned long long>(address), static_cast<unsigned long long>(rva),
            stat.cpu_100ns / 10000.0,
            attributed_cpu ? stat.cpu_100ns * 100.0 / attributed_cpu : 0.0,
            static_cast<unsigned long long>(stat.samples));
    }
    std::fclose(file);

    std::vector<std::pair<std::uint64_t, FunctionStat>> sorted_functions(functions.begin(), functions.end());
    std::sort(sorted_functions.begin(), sorted_functions.end(), [](const auto& left, const auto& right) {
        return left.second.leaf_100ns > right.second.leaf_100ns;
    });
    const auto functions_path = options.output / L"functions.csv";
    file = _wfopen(functions_path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "rva,address,leaf_cpu_ms,inclusive_cpu_ms,leaf_percent,inclusive_percent,leaf_samples,inclusive_samples");
    for (const auto& [address, stat] : sorted_functions) {
        std::fprintf(file, "0x%llx,0x%llx,%.3f,%.3f,%.6f,%.6f,%llu,%llu\n",
            static_cast<unsigned long long>(address - eu4.base),
            static_cast<unsigned long long>(address),
            stat.leaf_100ns / 10000.0, stat.inclusive_100ns / 10000.0,
            attributed_cpu ? stat.leaf_100ns * 100.0 / attributed_cpu : 0.0,
            attributed_cpu ? stat.inclusive_100ns * 100.0 / attributed_cpu : 0.0,
            static_cast<unsigned long long>(stat.leaf_samples),
            static_cast<unsigned long long>(stat.inclusive_samples));
    }
    std::fclose(file);

    struct ExternalCallerRow {
        std::uint64_t leaf_address;
        std::uint64_t caller_address;
        Stat stat;
    };
    std::vector<ExternalCallerRow> sorted_external_callers;
    for (const auto& [leaf_address, callers] : external_callers) {
        for (const auto& [caller_address, stat] : callers) {
            sorted_external_callers.push_back({leaf_address, caller_address, stat});
        }
    }
    std::sort(sorted_external_callers.begin(), sorted_external_callers.end(),
        [](const auto& left, const auto& right) {
            return left.stat.cpu_100ns > right.stat.cpu_100ns;
        });
    const auto external_callers_path = options.output / L"external_callers.csv";
    file = _wfopen(external_callers_path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "leaf_module,leaf_address,leaf_rva,caller_address,caller_rva,estimated_cpu_ms,percent_of_attributed,samples");
    for (const auto& row : sorted_external_callers) {
        const auto* module = find_module(modules, row.leaf_address);
        const auto name = module ? narrow_ascii(module->name) : "unknown";
        const auto leaf_rva = module ? row.leaf_address - module->base : 0;
        std::fprintf(file, "%s,0x%llx,0x%llx,0x%llx,0x%llx,%.3f,%.6f,%llu\n", name.c_str(),
            static_cast<unsigned long long>(row.leaf_address), static_cast<unsigned long long>(leaf_rva),
            static_cast<unsigned long long>(row.caller_address),
            static_cast<unsigned long long>(row.caller_address - eu4.base),
            row.stat.cpu_100ns / 10000.0,
            attributed_cpu ? row.stat.cpu_100ns * 100.0 / attributed_cpu : 0.0,
            static_cast<unsigned long long>(row.stat.samples));
    }
    std::fclose(file);

    // stacks.csv: один ряд на уникальный стек; кадры от корня к листу,
    // каждый кадр "модуль+0xRVA" (для возвратов RVA уже указывает внутрь CALL).
    std::vector<std::pair<const StackKey*, Stat>> sorted_stacks;
    sorted_stacks.reserve(stacks.size());
    for (const auto& [key, stat] : stacks) sorted_stacks.push_back({&key, stat});
    std::sort(sorted_stacks.begin(), sorted_stacks.end(), [](const auto& left, const auto& right) {
        return left.second.cpu_100ns > right.second.cpu_100ns;
    });
    const auto stacks_path = options.output / L"stacks.csv";
    file = _wfopen(stacks_path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "thread_id,estimated_cpu_ms,samples,frames");
    std::unordered_map<std::uint64_t, std::string> frame_names;
    for (const auto& [key, stat] : sorted_stacks) {
        std::fprintf(file, "%lu,%.3f,%llu,", key->thread_id, stat.cpu_100ns / 10000.0,
            static_cast<unsigned long long>(stat.samples));
        for (auto it = key->frames.rbegin(); it != key->frames.rend(); ++it) {
            auto& name = frame_names[*it];
            if (name.empty()) {
                const auto* module = find_module(modules, *it);
                char buffer[320];
                if (module) {
                    std::snprintf(buffer, sizeof(buffer), "%s+0x%llx", narrow_ascii(module->name).c_str(),
                        static_cast<unsigned long long>(*it - module->base));
                } else {
                    std::snprintf(buffer, sizeof(buffer), "?+0x%llx", static_cast<unsigned long long>(*it));
                }
                name = buffer;
            }
            std::fprintf(file, "%s%s", it == key->frames.rbegin() ? "" : ";", name.c_str());
        }
        std::fputc('\n', file);
    }
    std::fclose(file);

    std::vector<std::pair<DWORD, ThreadState>> sorted_threads(threads.begin(), threads.end());
    std::sort(sorted_threads.begin(), sorted_threads.end(), [](const auto& left, const auto& right) {
        return left.second.total_cpu > right.second.total_cpu;
    });
    const auto threads_path = options.output / L"threads.csv";
    file = _wfopen(threads_path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "thread_id,estimated_cpu_ms,attributed_cpu_ms,samples");
    for (const auto& [id, state] : sorted_threads) {
        std::fprintf(file, "%lu,%.3f,%.3f,%llu\n", id, state.total_cpu / 10000.0,
            state.attributed_cpu / 10000.0, static_cast<unsigned long long>(state.samples));
    }
    std::fclose(file);

    const auto metadata_path = options.output / L"metadata.txt";
    file = _wfopen(metadata_path.c_str(), L"wb");
    if (!file) return false;
    std::fprintf(file, "process_id=%lu\nexecutable=%ls\nimage_base=0x%llx\nimage_size=0x%llx\n",
        process_id, eu4.path.c_str(), static_cast<unsigned long long>(eu4.base),
        static_cast<unsigned long long>(eu4.size));
    std::fprintf(file, "elapsed_ms=%llu\ninterval_ms=%llu\ntotal_cpu_ms=%.3f\nattributed_cpu_ms=%.3f\ncaptured_samples=%llu\n",
        static_cast<unsigned long long>(elapsed_ms), static_cast<unsigned long long>(options.interval_ms),
        total_cpu / 10000.0, attributed_cpu / 10000.0,
        static_cast<unsigned long long>(captured_samples));
    std::fclose(file);
    return true;
}

// symbols.csv: имена для кадров вне eu4.exe (ntdll, kernel32, pops_api, драйвер) по экспортам/PDB
// через dbghelp. Нужны, чтобы различать ожидания: ZwWaitForSingleObject, ZwDelayExecution и т. п.
bool write_external_symbols(const Options& options, HANDLE process,
    const std::vector<ModuleInfo>& modules, const ModuleInfo& eu4, const StackStats& stacks) {
    std::unordered_set<std::uint64_t> addresses;
    for (const auto& [key, stat] : stacks) {
        for (const auto frame : key.frames) {
            if (frame < eu4.base || frame >= eu4.base + eu4.size) addresses.insert(frame);
        }
    }
    const auto path = options.output / L"symbols.csv";
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return false;
    write_csv_header(file, "frame,symbol,offset");
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (const auto address : addresses) {
        const auto* module = find_module(modules, address);
        if (!module) continue;
        std::memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (!SymFromAddr(process, address, &displacement, symbol)) continue;
        std::fprintf(file, "%s+0x%llx,%s,0x%llx\n", narrow_ascii(module->name).c_str(),
            static_cast<unsigned long long>(address - module->base), symbol->Name,
            static_cast<unsigned long long>(displacement));
    }
    std::fclose(file);
    return true;
}

int run(const Options& options) {
    const DWORD process_id = options.process_id ? options.process_id : find_eu4_process();
    if (!process_id) {
        std::fwprintf(stderr, L"eu4.exe was not found. Start the game or specify --pid.\n");
        return 2;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_VM_READ, FALSE, process_id);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed for PID %u: error %lu\n", process_id, GetLastError());
        return 3;
    }

    auto modules = enumerate_modules(process_id);
    if (modules.empty()) {
        std::fwprintf(stderr, L"Cannot enumerate game modules: error %lu\n", GetLastError());
        CloseHandle(process);
        return 4;
    }
    wchar_t executable_path[MAX_PATH]{};
    DWORD path_size = MAX_PATH;
    std::wstring executable_name;
    if (QueryFullProcessImageNameW(process, 0, executable_path, &path_size)) {
        executable_name = fs::path(executable_path).filename().wstring();
    }
    auto eu4_it = std::find_if(modules.begin(), modules.end(), [&](const ModuleInfo& module) {
        return _wcsicmp(module.name.c_str(), L"eu4.exe") == 0 ||
            (!executable_name.empty() && _wcsicmp(module.name.c_str(), executable_name.c_str()) == 0);
    });
    if (eu4_it == modules.end()) {
        std::fwprintf(stderr, L"The target executable is not present in its module list.\n");
        CloseHandle(process);
        return 5;
    }
    const ModuleInfo eu4 = *eu4_it;

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_UNDNAME);
    if (!SymInitializeW(process, nullptr, FALSE)) {
        std::fwprintf(stderr, L"SymInitialize failed: error %lu\n", GetLastError());
        CloseHandle(process);
        return 6;
    }
    for (const auto& module : modules) {
        if (!SymLoadModuleExW(process, nullptr, module.path.c_str(), module.name.c_str(),
                module.base, static_cast<DWORD>(module.size), nullptr, 0)) {
            const auto error = GetLastError();
            if (error != ERROR_SUCCESS) {
                std::fwprintf(stderr, L"Warning: cannot load unwind data for %ls: %lu\n",
                    module.name.c_str(), error);
            }
        }
    }

    refresh_threads(process_id);
    if (threads.empty()) {
        std::fwprintf(stderr, L"No accessible target threads found.\n");
        SymCleanup(process);
        CloseHandle(process);
        return 7;
    }

    std::unordered_map<std::uint64_t, Stat> leaf;
    ExternalCallerStats external_callers;
    std::unordered_map<std::uint64_t, FunctionStat> functions;
    StackStats stacks;
    DWORD wall_thread = options.wall_thread;
    std::uint64_t total_cpu = 0;
    std::uint64_t attributed_cpu = 0;
    std::uint64_t captured_samples = 0;
    const auto started = Clock::now();
    auto next_refresh = started + std::chrono::seconds(2);
    auto next_progress = started + std::chrono::seconds(5);
    std::wprintf(L"Profiling PID %u for %llu seconds; output: %ls\n", process_id,
        static_cast<unsigned long long>(options.duration_seconds), options.output.c_str());
    std::wprintf(L"Do not close this window. Press Ctrl+C to finish and write reports early.\n");

    while (!InterlockedCompareExchange(&stop_requested, 0, 0)) {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (static_cast<std::uint64_t>(elapsed) >= options.duration_seconds * 1000) break;
        if (now >= next_refresh) {
            refresh_threads(process_id);
            next_refresh = now + std::chrono::seconds(2);
        }
        if (now >= next_progress) {
            std::wprintf(L"  %lld/%llu seconds, %llu CPU stacks captured\n",
                static_cast<long long>(elapsed / 1000),
                static_cast<unsigned long long>(options.duration_seconds),
                static_cast<unsigned long long>(captured_samples));
            std::fflush(stdout);
            next_progress = now + std::chrono::seconds(5);
        }

        for (auto& [id, state] : threads) {
            if (now < state.next_sample) continue;
            state.next_sample = now + std::chrono::milliseconds(options.interval_ms);
            bool valid = false;
            const auto current_cpu = thread_cpu(state.handle, valid);
            if (!valid) continue;
            if (!state.initialized) {
                state.previous_cpu = current_cpu;
                state.previous_wall = now;
                state.initialized = true;
                continue;
            }
            if (current_cpu < state.previous_cpu) {
                state.previous_cpu = current_cpu;
                continue;
            }
            const auto cpu_delta = current_cpu - state.previous_cpu;
            state.previous_cpu = current_cpu;
            state.total_cpu += cpu_delta;
            total_cpu += cpu_delta;
            const auto wall_delta = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - state.previous_wall).count() * 10);
            state.previous_wall = now;
            std::uint64_t delta = options.wall_all ? wall_delta : cpu_delta;
            if (options.wall) {
                // --wall: через секунду выбираем поток с наибольшим CPU (главный поток игры)
                // и снимаем только его — и когда он считает, и когда ждёт. Вес = реальное время.
                if (!wall_thread && elapsed >= 1000) {
                    const auto best = std::max_element(threads.begin(), threads.end(),
                        [](const auto& left, const auto& right) {
                            return left.second.total_cpu < right.second.total_cpu;
                        });
                    wall_thread = best->first;
                    std::wprintf(L"  wall mode: sampling thread %lu\n", wall_thread);
                }
                if (id != wall_thread) continue;
                delta = wall_delta;
            }
            if (!delta) continue;

            auto stack = capture_stack(process, state.handle, eu4, modules);
            if (!stack.contains_eu4 || stack.frames.empty()) continue;
            ++state.samples;
            state.attributed_cpu += delta;
            ++captured_samples;
            attributed_cpu += delta;
            add_stat(leaf, stack.frames.front(), delta);
            {
                auto& stat = stacks[StackKey{id, stack.frames}];
                stat.cpu_100ns += delta;
                ++stat.samples;
            }
            const auto* leaf_module = find_module(modules, stack.frames.front());
            if (leaf_module && leaf_module->base != eu4.base) {
                const auto caller = std::find_if(stack.frames.begin(), stack.frames.end(),
                    [&](std::uint64_t address) {
                        return address >= eu4.base && address < eu4.base + eu4.size;
                    });
                if (caller != stack.frames.end()) {
                    add_stat(external_callers[stack.frames.front()], *caller, delta);
                }
            }

            std::unordered_set<std::uint64_t> visited;
            for (const auto address : stack.frames) {
                if (address < eu4.base || address >= eu4.base + eu4.size) continue;
                auto& stat = functions[address];
                if (address == stack.frames.front()) {
                    stat.leaf_100ns += delta;
                    ++stat.leaf_samples;
                }
                if (visited.insert(address).second) {
                    stat.inclusive_100ns += delta;
                    ++stat.inclusive_samples;
                }
            }
        }
        discard_dead_threads();
        Sleep(1);
    }

    discard_dead_threads();
    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    bool written = write_reports(options, process_id, modules, eu4, leaf, external_callers,
        functions, stacks, elapsed_ms, total_cpu, attributed_cpu, captured_samples);
    if (written) written = write_external_symbols(options, process, modules, eu4, stacks);
    close_threads();
    SymCleanup(process);
    CloseHandle(process);
    if (!written) {
        std::fwprintf(stderr, L"Failed to write report files: error %lu\n", GetLastError());
        return 8;
    }
    std::wprintf(L"Done: %llu stacks, %.3f seconds of attributed CPU. Reports: %ls\n",
        static_cast<unsigned long long>(captured_samples), attributed_cpu / 10000000.0,
        options.output.c_str());
    return captured_samples ? 0 : 9;
}

void print_usage() {
    std::wprintf(L"EU4 native CPU stack sampler\n"
        L"Usage: eu4_sampler.exe [--pid N] [--seconds N] [--interval-ms N] [--output DIR]\n"
        L"                       [--wall | --wall-thread TID]\n"
        L"Defaults: find eu4.exe, sample for 30 seconds every 5 ms, weight by thread CPU time.\n"
        L"--wall: sample only the busiest thread by wall-clock time, including waits.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto next_value = [&]() -> wchar_t* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        if (argument == L"--help" || argument == L"-h") {
            print_usage();
            return 0;
        }
        if (argument == L"--wall-all") {
            options.wall_all = true;
            continue;
        }
        if (argument == L"--wall") {
            options.wall = true;
            continue;
        }
        wchar_t* value = next_value();
        if (!value) {
            print_usage();
            return 1;
        }
        if (argument == L"--pid") options.process_id = std::wcstoul(value, nullptr, 10);
        else if (argument == L"--seconds") options.duration_seconds = _wcstoui64(value, nullptr, 10);
        else if (argument == L"--interval-ms") options.interval_ms = _wcstoui64(value, nullptr, 10);
        else if (argument == L"--output") options.output = value;
        else if (argument == L"--wall-thread") {
            options.wall = true;
            options.wall_thread = std::wcstoul(value, nullptr, 10);
        }
        else {
            std::fwprintf(stderr, L"Unknown option: %ls\n", argument.c_str());
            return 1;
        }
    }
    if (!options.duration_seconds || options.duration_seconds > 3600 ||
        !options.interval_ms || options.interval_ms > 1000) {
        std::fwprintf(stderr, L"Use 1-3600 seconds and an interval of 1-1000 ms.\n");
        return 1;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);
    return run(options);
}
