#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <iostream>
#include <string>

// Patchable target — fixed 260-char buffer at a known offset

#pragma section(".patch", read, write)
__declspec(allocate(".patch"))
volatile wchar_t g_exeName[260] = {
    // MAGIC: "PATCHME:" as wide chars — 8 wchars = 16 bytes
    L'P',L'A',L'T',L'C',L'H',L'M',L'E',L':',
    // Default name (skate.exe) — overwritten by build tool
    L's',L'k',L'a',L't',L'e',L'.',L'e',L'x',L'e',L'\0'
};

// Error handler

static void HandleFailure(const std::wstring& exePath, const std::wstring& args)
{
    DWORD lastError = GetLastError();

    std::string errorMsg;
    if (lastError == 2)
        errorMsg = "Try reinstalling the game.";
    else if (lastError == 740)
        errorMsg = "Do not run the game as admin.";

    std::wcout << L"[-] CreateProcess failed, error code " << lastError << L"\n[-] ";
    std::wcout << std::wstring(errorMsg.begin(), errorMsg.end()) << L"\n";
    std::wcout << L"[-] Trying 'runas'...\n";

    ShellExecuteW(
        nullptr,
        L"runas",
        exePath.c_str(),
        args.empty() ? nullptr : args.c_str(),
        nullptr,
        SW_SHOWDEFAULT
    );

    MessageBoxA(nullptr, errorMsg.c_str(), "Launcher failed", MB_ICONERROR);
}

// Entry point

int main(int argc, const char** argv)
{
    std::cout << "[+] Fake EA Anticheat Launcher is running the game without the EA Anticheat\n";
    std::cout << "[+] Bypassing the Anticheat - Online functions will be disabled\n";

    // Skip the 8-wchar MAGIC prefix to get the actual exe name
    std::wstring exeName(const_cast<wchar_t*>(g_exeName) + 8);

    // Build forwarded argument string from argv
    std::wstring args;
    if (argc > 1)
    {
        std::wcout << L"[+] Args (";
        for (int i = 1; i < argc; i++)
        {
            std::wstring warg(argv[i], argv[i] + strlen(argv[i]));
            if (i > 1) args += L" ";
            args += warg;
        }
        std::wcout << args << L"):\n";
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // lpApplicationName = NULL, full quoted path goes in lpCommandLine only
    std::wstring cmdLine = L"\"" + exeName + L"\"";
    if (!args.empty()) { cmdLine += L" "; cmdLine += args; }

    BOOL success = CreateProcessW(
        nullptr,            // lpApplicationName = NULL
        cmdLine.data(),     // lpCommandLine
        nullptr,            // lpProcessAttributes
        nullptr,            // lpThreadAttributes
        FALSE,              // bInheritHandles
        0,                  // dwCreationFlags = 0
        nullptr,            // lpEnvironment
        nullptr,            // lpCurrentDirectory = NULL
        &si,
        &pi
    );

    if (success)
    {
        std::wcout << L"[+] OK, PID: " << pi.dwProcessId << L"\n";
        std::wcout << L"[+] exe: " << exeName << L"\n";
        std::wcout << L"[+] cmd: " << cmdLine << L"\n";
        Sleep(0x700);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else
    {
        HandleFailure(exeName, args);
    }

    return 0;
}