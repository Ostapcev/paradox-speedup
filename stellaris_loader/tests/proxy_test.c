// Тест прокси VERSION.dll: имитирует вызовы игры.
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HMODULE m = LoadLibraryExW(L"VERSION.dll", NULL, 0);
    printf("VERSION.dll module = %p\n", (void*)m);
    if (!m) return 1;

    DWORD (WINAPI *GetSizeA)(LPCSTR, LPDWORD) = (void*)GetProcAddress(m, "GetFileVersionInfoSizeA");
    BOOL (WINAPI *GetInfoA)(LPCSTR, DWORD, DWORD, LPVOID) = (void*)GetProcAddress(m, "GetFileVersionInfoA");
    BOOL (WINAPI *QueryA)(LPCVOID, LPCSTR, LPVOID*, PUINT) = (void*)GetProcAddress(m, "VerQueryValueA");
    printf("procs: %p %p %p\n", (void*)GetSizeA, (void*)GetInfoA, (void*)QueryA);
    if (!GetSizeA || !GetInfoA || !QueryA) return 2;

    const char* exe = "C:\\Stellaris\\Stellaris\\stellaris.exe";
    DWORD handle = 0;
    DWORD sz = GetSizeA(exe, &handle);
    printf("GetFileVersionInfoSizeA(stellaris.exe) = %lu\n", (unsigned long)sz);
    if (sz == 0) return 3;

    void* buf = malloc(sz);
    if (!GetInfoA(exe, handle, sz, buf)) return 4;

    VS_FIXEDFILEINFO* ffi = NULL;
    UINT len = 0;
    if (QueryA(buf, "\\", (LPVOID*)&ffi, &len) && ffi) {
        printf("stellaris.exe file version: %lu.%lu.%lu.%lu\n",
               (unsigned long)(ffi->dwFileVersionMS >> 16),
               (unsigned long)(ffi->dwFileVersionMS & 0xffff),
               (unsigned long)(ffi->dwFileVersionLS >> 16),
               (unsigned long)(ffi->dwFileVersionLS & 0xffff));
    }
    free(buf);
    printf("PROXY TEST PASSED\n");
    return 0;
}
