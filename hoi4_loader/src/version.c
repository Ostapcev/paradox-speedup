// VERSION.dll — прокси-загрузчик плагинов для Hearts of Iron IV (x64).
//
// Игра при запуске сама загружает VERSION.dll из своей папки (импортирует
// GetFileVersionInfoA, VerQueryValueA, GetFileVersionInfoSizeA). Эта DLL
// пробрасывает все вызовы в настоящую системную version.dll и дополнительно
// загружает все *.dll из подпапки "plugins" рядом с hoi4.exe.
//
// Журнал: plugins\_loader.log. Плагин отключается переименованием
// (например foo.dll -> foo.dll.off).
//
// Сборка: zig cc -target x86_64-windows-gnu -shared -O2 -o VERSION.dll src/version.c

#include <windows.h>
#include <stdio.h>

static HMODULE g_real = NULL;

static HMODULE RealVersionDll(void)
{
    if (g_real == NULL) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 12) return NULL;
        wcscat(path, L"\\version.dll");
        g_real = LoadLibraryW(path);   // абсолютный путь: рекурсии в себя не будет
    }
    return g_real;
}

// ---- проброска экспортов системной version.dll ----------------------------

#define FWD0(ret, name) \
    static ret (WINAPI *p_##name)(void) = NULL; \
    __declspec(dllexport) ret WINAPI name(void) { \
        if (!p_##name) { HMODULE m = RealVersionDll(); \
            if (!m || !(p_##name = (void*)GetProcAddress(m, #name))) return (ret)0; } \
        return p_##name(); }

#define FWD(ret, name, ...) \
    static ret (WINAPI *p_##name)(__VA_ARGS__) = NULL; \
    __declspec(dllexport) ret WINAPI name(__VA_ARGS__)

#define FWD_BODY(name, args) { \
        if (!p_##name) { HMODULE m = RealVersionDll(); \
            if (!m || !(p_##name = (void*)GetProcAddress(m, #name))) return 0; } \
        return p_##name args; }

FWD(BOOL,  GetFileVersionInfoA, LPCSTR a, DWORD b, DWORD c, LPVOID d)  FWD_BODY(GetFileVersionInfoA, (a,b,c,d))
FWD(BOOL,  GetFileVersionInfoW, LPCWSTR a, DWORD b, DWORD c, LPVOID d) FWD_BODY(GetFileVersionInfoW, (a,b,c,d))
FWD(DWORD, GetFileVersionInfoSizeA, LPCSTR a, LPDWORD b)               FWD_BODY(GetFileVersionInfoSizeA, (a,b))
FWD(DWORD, GetFileVersionInfoSizeW, LPCWSTR a, LPDWORD b)              FWD_BODY(GetFileVersionInfoSizeW, (a,b))
FWD(BOOL,  VerQueryValueA, LPCVOID a, LPCSTR b, LPVOID* c, PUINT d)    FWD_BODY(VerQueryValueA, (a,b,c,d))
FWD(BOOL,  VerQueryValueW, LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d)   FWD_BODY(VerQueryValueW, (a,b,c,d))

// остальные экспорты version.dll — на случай, если их запросит другой модуль
typedef struct { DWORD dwSIG; DWORD dwStrucVersion; DWORD dwFileVersionMS; DWORD dwFileVersionLS; DWORD dwProductVersionMS; DWORD dwProductVersionLS; DWORD dwFileFlagsMask; DWORD dwFileFlags; DWORD dwFileOS; DWORD dwFileType; DWORD dwFileSubtype; DWORD dwFileDateMS; DWORD dwFileDateLS; } VS_FIXEDFILEINFO_local;

FWD(BOOL,  GetFileVersionInfoExA, DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d)  FWD_BODY(GetFileVersionInfoExA, (f,a,b,c,d))
FWD(BOOL,  GetFileVersionInfoExW, DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d) FWD_BODY(GetFileVersionInfoExW, (f,a,b,c,d))
FWD(DWORD, GetFileVersionInfoSizeExA, DWORD f, LPCSTR a, LPDWORD b)               FWD_BODY(GetFileVersionInfoSizeExA, (f,a,b))
FWD(DWORD, GetFileVersionInfoSizeExW, DWORD f, LPCWSTR a, LPDWORD b)              FWD_BODY(GetFileVersionInfoSizeExW, (f,a,b))
FWD(BOOL,  GetFileVersionInfoByHandle, DWORD f, HANDLE h, LPVOID d, DWORD c)      FWD_BODY(GetFileVersionInfoByHandle, (f,h,d,c))

// VerLanguageNameA/W — редко нужны, но пробрасываем для полноты.
// (VerFindFile*/VerInstallFile* не пробрасываем: они не нужны современному коду.)
FWD(DWORD, VerLanguageNameA, DWORD a, LPSTR b, DWORD c)  FWD_BODY(VerLanguageNameA, (a,b,c))
FWD(DWORD, VerLanguageNameW, DWORD a, LPWSTR b, DWORD c) FWD_BODY(VerLanguageNameW, (a,b,c))

// ---- загрузчик плагинов ----------------------------------------------------

static void LogLine(HANDLE log, const char* s)
{
    if (log == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(log, s, (DWORD)strlen(s), &wr, NULL);
}

static int __cdecl CmpNames(const void* a, const void* b)
{
    return _wcsicmp(*(const wchar_t* const*)a, *(const wchar_t* const*)b);
}

static void LoadPlugins(HMODULE self)
{
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    slash[1] = L'\0';                     // dir = папка игры с '\' на конце

    wchar_t plugdir[MAX_PATH], mask[MAX_PATH], logpath[MAX_PATH];
    swprintf(plugdir, MAX_PATH, L"%splugins", dir);
    swprintf(mask,    MAX_PATH, L"%s\\*.dll", plugdir);
    swprintf(logpath, MAX_PATH, L"%s\\_loader.log", plugdir);

    HANDLE log = CreateFileW(logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log != INVALID_HANDLE_VALUE) SetFilePointer(log, 0, NULL, FILE_END);
    LogLine(log, "---- hoi4 plugin loader start ----\r\n");

    // собрать список и отсортировать, чтобы порядок загрузки был предсказуемым
    wchar_t* names[256];
    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.cFileName[0] == L'_') continue;   // служебные/отключённые
            if (count < 256) {
                names[count] = _wcsdup(fd.cFileName);
                if (names[count]) count++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(names, count, sizeof(wchar_t*), CmpNames);

    char line[MAX_PATH * 2 + 64];
    for (int i = 0; i < count; i++) {
        wchar_t full[MAX_PATH];
        swprintf(full, MAX_PATH, L"%s\\%s", plugdir, names[i]);
        HMODULE m = LoadLibraryExW(full, NULL, 0);
        DWORD err = m ? 0 : GetLastError();
        snprintf(line, sizeof(line), "%s %ls%s (err=%lu)\r\n",
                 m ? "OK   " : "FAIL ", names[i],
                 m ? "" : " (plugin not loaded)", (unsigned long)err);
        LogLine(log, line);
        free(names[i]);
    }
    if (count == 0) LogLine(log, "(no plugins found)\r\n");

    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        RealVersionDll();
        LoadPlugins(hMod);
    }
    return TRUE;
}
