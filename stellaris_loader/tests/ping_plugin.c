// Минимальный плагин для проверки загрузчика: пишет строку в свой журнал.
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(hMod, path, MAX_PATH);
        wchar_t* dot = wcsrchr(path, L'.');
        if (dot) wcscpy(dot, L".loaded");
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char msg[] = "ping plugin loaded\r\n";
            DWORD wr;
            WriteFile(h, msg, sizeof(msg) - 1, &wr, NULL);
            CloseHandle(h);
        }
    }
    return TRUE;
}
