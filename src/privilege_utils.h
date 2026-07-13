#pragma once
#include <windows.h>

#pragma comment(lib, "advapi32.lib")

// Enables SE_DEBUG_NAME in the current process token so that OpenProcess /
// DebugActiveProcess can target elevated or protected processes.  Requires the
// process to already hold the privilege (i.e. run as Administrator); on a
// non-elevated process the call fails gracefully and returns false without any
// side effects, leaving existing fallbacks in place.
//
// AdjustTokenPrivileges returns nonzero even when it only partially applies the
// privilege, so GetLastError() must be checked for ERROR_SUCCESS to confirm
// full success.
inline bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL adjusted = AdjustTokenPrivileges(hToken, FALSE, &tp,
                                          sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);

    return adjusted && err == ERROR_SUCCESS;
}
