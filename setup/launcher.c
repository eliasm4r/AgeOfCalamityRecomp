// "AgeOfCalamityRecomp Setup.exe": a small program that carries the game icon and starts the setup with the signed
// Python beside it (python/Setup (if blocked).exe, pythonw.exe under another name), as WondersBeyondRecomp's
// installer/launcher.c does. It is the one program of the package without a signature, so Smart App Control may refuse
// it; the README then points at python/Setup (if blocked).exe, which is the same setup.
#include <windows.h>

#define TARGET L"python\\Setup (if blocked).exe"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    WCHAR folder[MAX_PATH + 1];
    WCHAR path[MAX_PATH + 64];
    WCHAR command[MAX_PATH + 72];
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    DWORD length = GetModuleFileNameW(NULL, folder, MAX_PATH);
    (void)instance;
    (void)previous;
    (void)arguments;
    (void)show;
    while (length > 0 && folder[length - 1] != L'\\') {
        length--;
    }
    folder[length] = 0;
    lstrcpyW(path, folder);
    lstrcatW(path, TARGET);
    lstrcpyW(command, L"\"");
    lstrcatW(command, path);
    lstrcatW(command, L"\"");
    if (!CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process)) {
        MessageBoxW(NULL, L"The setup could not start. Open \"" TARGET L"\" instead.", L"Setup", MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
