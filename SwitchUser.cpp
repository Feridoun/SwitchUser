// SwitchUser - switch between signed-in Windows users without Start > account picture > Switch user > tile.
//
// One exe, several roles:
//   SwitchUser.exe              per-user helper: tray icon + hotkeys (started at logon from HKLM Run)
//   SwitchUser.exe service      the SYSTEM service that actually moves sessions onto the screen
//   SwitchUser.exe install      copy to Program Files, register the service and Run key (asks for UAC)
//   SwitchUser.exe uninstall    remove all of the above
//   SwitchUser.exe toggle       switch once and exit (for scripts / shortcuts)
//
// Only SYSTEM can bring another user's session to the console without their password
// (WTSConnectSession), so the helper asks the service over a named pipe. The service only obeys
// whoever is currently on the console.
//
// Triggers: Ctrl+Alt+U toggles, Ctrl+Alt+Shift+U opens the picker, tray left-click toggles,
// tray right-click opens the picker. Toggle goes to the other signed-in user; if there is none it
// shows the sign-in screen, and if there are several it shows the picker.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <userenv.h>
#include <string>
#include <vector>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t kSvcName[]     = L"SwitchUserSvc";
static const wchar_t kPipeName[]    = L"\\\\.\\pipe\\SwitchUserSvc";
static const wchar_t kWndClass[]    = L"SwitchUserHelperWnd";
static const wchar_t kRunValue[]    = L"SwitchUser";
static const wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// ---------------------------------------------------------------------------------------------
// Shared helpers

struct Session {
    DWORD id;
    std::wstring user;
    WTS_CONNECTSTATE_CLASS state;
};

static std::vector<Session> UserSessions() {
    std::vector<Session> out;
    PWTS_SESSION_INFOW info = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count)) return out;
    for (DWORD i = 0; i < count; i++) {
        if (info[i].SessionId == 0) continue;
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, info[i].SessionId, WTSUserName, &name, &bytes)) {
            if (name && *name) out.push_back({info[i].SessionId, name, info[i].State});
            WTSFreeMemory(name);
        }
    }
    WTSFreeMemory(info);
    return out;
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static std::wstring InstallDir() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\SwitchUser"; }
static std::wstring InstalledExe() { return InstallDir() + L"\\SwitchUser.exe"; }

static void Log(const std::wstring& msg) {
    std::wstring dir = KnownFolder(FOLDERID_ProgramData) + L"\\SwitchUser";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW((dir + L"\\switch-user.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring line = stamp + msg + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    DWORD written;
    WriteFile(f, utf8.data(), (DWORD)utf8.size() - 1, &written, nullptr);
    CloseHandle(f);
}

static std::wstring LastErrorText(DWORD err = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"unknown error";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + L" (" + std::to_wstring(err) + L")";
}

// Sends one command to the service and returns its reply ("OK ...", "PICK", "ERR ...").
static std::wstring CallService(const std::wstring& cmd) {
    wchar_t reply[512] = {};
    DWORD got = 0;
    if (!CallNamedPipeW(kPipeName, (LPVOID)cmd.c_str(), (DWORD)(cmd.size() * sizeof(wchar_t)),
                        reply, sizeof(reply) - sizeof(wchar_t), &got, 3000))
        return L"ERR The SwitchUser service is not running. Run \"SwitchUser.exe install\".";
    reply[got / sizeof(wchar_t)] = 0;
    return reply;
}

// ---------------------------------------------------------------------------------------------
// Service

static SERVICE_STATUS_HANDLE g_statusHandle;
static SERVICE_STATUS g_status = {SERVICE_WIN32_OWN_PROCESS};
static volatile bool g_stopping = false;

static void SetSvcState(DWORD state) {
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_statusHandle, &g_status);
}

static std::wstring ConnectToConsole(const Session& target, DWORD console) {
    if (WTSConnectSessionW(target.id, console, (PWSTR)L"", TRUE)) {
        Log(L"connect " + target.user + L" (session " + std::to_wstring(target.id) + L")");
        return L"OK";
    }
    std::wstring err = LastErrorText();
    Log(L"connect " + target.user + L" failed: " + err);
    return L"ERR Could not switch to " + target.user + L": " + err;
}

static std::wstring ShowSignInScreen(DWORD console) {
    if (WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, console, FALSE)) {
        Log(L"disconnect session " + std::to_wstring(console) + L" (sign-in screen)");
        return L"OK";
    }
    std::wstring err = LastErrorText();
    Log(L"disconnect failed: " + err);
    return L"ERR Could not show the sign-in screen: " + err;
}

static std::wstring HandleCommand(const std::wstring& cmd, DWORD clientSession) {
    DWORD console = WTSGetActiveConsoleSessionId();
    if (clientSession != console) return L"ERR Only the user on screen can switch.";

    std::vector<Session> others;
    for (auto& s : UserSessions())
        if (s.id != console) others.push_back(s);

    if (cmd == L"SIGNIN") return ShowSignInScreen(console);

    if (cmd == L"TOGGLE") {
        if (others.empty()) return ShowSignInScreen(console);
        if (others.size() == 1) return ConnectToConsole(others[0], console);
        return L"PICK";
    }

    if (cmd.rfind(L"SESSION ", 0) == 0) {
        DWORD id = wcstoul(cmd.c_str() + 8, nullptr, 10);
        for (auto& s : others)
            if (s.id == id) return ConnectToConsole(s, console);
        return L"ERR That user is no longer signed in.";
    }

    return L"ERR Unknown command.";
}

// Starts the helper in every signed-in session, so it works straight after install without signing out.
// The helper's own mutex stops duplicates.
static void LaunchHelperInSessions() {
    std::wstring exe = ExePath();
    std::wstring cmdline = L"\"" + exe + L"\"";
    for (auto& s : UserSessions()) {
        HANDLE token = nullptr;
        if (!WTSQueryUserToken(s.id, &token)) continue;
        void* env = nullptr;
        CreateEnvironmentBlock(&env, token, FALSE);
        STARTUPINFOW si = {sizeof(si)};
        si.lpDesktop = (LPWSTR)L"winsta0\\default";
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
        buf.push_back(0);
        if (CreateProcessAsUserW(token, exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                 CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(token);
    }
}

static DWORD WINAPI SvcCtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_stopping = true;
        SetSvcState(SERVICE_STOP_PENDING);
        // Unblock ConnectNamedPipe.
        HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return NO_ERROR;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kSvcName, SvcCtrlHandler, nullptr);
    if (!g_statusHandle) return;
    SetSvcState(SERVICE_START_PENDING);

    // SYSTEM and admins full access; interactive users may read/write (send commands).
    SECURITY_ATTRIBUTES sa = {sizeof(sa)};
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
                                                         SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    SetSvcState(SERVICE_RUNNING);
    Log(L"service started");
    LaunchHelperInSessions();

    while (!g_stopping) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 1024, 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"CreateNamedPipe failed: " + LastErrorText());
            Sleep(1000);
            continue;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            wchar_t buf[256] = {};
            DWORD got = 0;
            ULONG clientSession = MAXDWORD;
            if (!g_stopping && ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &got, nullptr) &&
                GetNamedPipeClientSessionId(pipe, &clientSession)) {
                buf[got / sizeof(wchar_t)] = 0;
                std::wstring reply = HandleCommand(buf, clientSession);
                DWORD written;
                WriteFile(pipe, reply.c_str(), (DWORD)(reply.size() * sizeof(wchar_t)), &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }

    LocalFree(sa.lpSecurityDescriptor);
    Log(L"service stopped");
    SetSvcState(SERVICE_STOPPED);
}

// ---------------------------------------------------------------------------------------------
// Install / uninstall (run elevated)

static bool IsElevated() {
    HANDLE token;
    TOKEN_ELEVATION elev = {};
    DWORD len;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len);
        CloseHandle(token);
    }
    return elev.TokenIsElevated != 0;
}

static void StopAndDeleteService(SC_HANDLE scm) {
    SC_HANDLE svc = OpenServiceW(scm, kSvcName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) return;
    SERVICE_STATUS st;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
    }
    DeleteService(svc);
    CloseServiceHandle(svc);
}

// A running helper locks the exe, but a locked exe can still be renamed out of the way.
static void MoveAsideAndDelete(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    if (MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && !DeleteFileW(old.c_str()))
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

static int DoInstall() {
    std::wstring dir = InstallDir(), exe = InstalledExe(), self = ExePath();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return MessageBoxW(nullptr, (L"Cannot open the service manager: " + LastErrorText()).c_str(), L"SwitchUser", MB_ICONERROR), 1;
    StopAndDeleteService(scm);

    // Program Files, so a standard user cannot replace the binary the SYSTEM service runs.
    CreateDirectoryW(dir.c_str(), nullptr);
    if (_wcsicmp(self.c_str(), exe.c_str()) != 0) {
        MoveAsideAndDelete(exe);
        if (!CopyFileW(self.c_str(), exe.c_str(), FALSE)) {
            MessageBoxW(nullptr, (L"Cannot copy to " + exe + L": " + LastErrorText()).c_str(), L"SwitchUser", MB_ICONERROR);
            CloseServiceHandle(scm);
            return 1;
        }
    }

    // A just-deleted service can linger as "marked for deletion" for a moment.
    std::wstring binPath = L"\"" + exe + L"\" service";
    SC_HANDLE svc = nullptr;
    for (int i = 0; i < 30 && !svc; i++) {
        svc = CreateServiceW(scm, kSvcName, L"Switch User", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc && GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) Sleep(200);
        else if (!svc) break;
    }
    if (!svc) {
        MessageBoxW(nullptr, (L"Cannot create the service: " + LastErrorText()).c_str(), L"SwitchUser", MB_ICONERROR);
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_DESCRIPTIONW desc = {(LPWSTR)L"Lets the SwitchUser tray icon and Ctrl+Alt+U switch between signed-in users."};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    if (!StartServiceW(svc, 0, nullptr))
        MessageBoxW(nullptr, (L"Service installed but did not start: " + LastErrorText()).c_str(), L"SwitchUser", MB_ICONWARNING);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    std::wstring runCmd = L"\"" + exe + L"\"";
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, kRunValue, REG_SZ, runCmd.c_str(), (DWORD)((runCmd.size() + 1) * sizeof(wchar_t)));

    Log(L"installed " + exe);
    return 0;
}

static int DoUninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm) {
        StopAndDeleteService(scm);
        CloseServiceHandle(scm);
    }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRunKey, kRunValue);
    MoveAsideAndDelete(InstalledExe());
    RemoveDirectoryW(InstallDir().c_str());
    Log(L"uninstalled");
    return 0;
}

static void CloseHelperInThisSession() {
    HWND h = FindWindowW(kWndClass, nullptr);
    if (!h) return;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(h, WM_CLOSE, 0, 0);
    if (proc) {
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

// Runs the elevated half via UAC, then does the per-session part (which must not be elevated).
static int InstallOrUninstall(bool install) {
    if (!IsElevated()) {
        CloseHelperInThisSession();
        std::wstring self = ExePath();
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = self.c_str();
        sei.lpParameters = install ? L"install-elevated" : L"uninstall-elevated";
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) return 1;   // UAC declined
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code != 0) return (int)code;
        if (install) {
            ShellExecuteW(nullptr, nullptr, InstalledExe().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            MessageBoxW(nullptr,
                        L"SwitchUser is installed.\n\n"
                        L"Ctrl+Alt+U\t\tswitch to the other user\n"
                        L"Ctrl+Alt+Shift+U\tpick a user\n"
                        L"Tray icon\t\tleft-click switch, right-click pick",
                        L"SwitchUser", MB_ICONINFORMATION);
        } else {
            MessageBoxW(nullptr, L"SwitchUser is uninstalled.", L"SwitchUser", MB_ICONINFORMATION);
        }
        return 0;
    }
    return install ? DoInstall() : DoUninstall();
}

// ---------------------------------------------------------------------------------------------
// Helper: tray icon + hotkeys

enum : UINT {
    WM_TRAY = WM_APP + 1,
    HK_TOGGLE = 1,
    HK_PICK = 2,
    ID_SIGNIN = 10,
    ID_EXIT = 11,
    ID_SESSION_BASE = 100,
};

static NOTIFYICONDATAW g_nid = {sizeof(g_nid)};
static UINT g_taskbarCreated;

static void Notify(const std::wstring& text) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    wcscpy_s(g_nid.szInfoTitle, L"Switch user");
    g_nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon(HWND hwnd) {
    SHSTOCKICONINFO sii = {sizeof(sii)};
    SHGetStockIconInfo(SIID_USERS, SHGSI_ICON | SHGSI_SMALLICON, &sii);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = sii.hIcon ? sii.hIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Switch user (Ctrl+Alt+U)\nRight-click or Ctrl+Alt+Shift+U to pick");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void ShowPicker(HWND hwnd, POINT pt) {
    DWORD mine = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    std::vector<Session> sessions = UserSessions();

    HMENU menu = CreatePopupMenu();
    int n = 0;
    for (size_t i = 0; i < sessions.size(); i++) {
        auto& s = sessions[i];
        if (s.id == mine) {
            AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, 0, s.user.c_str());
        } else {
            std::wstring label = (n < 9 ? L"&" + std::to_wstring(n + 1) + L"  " : L"   ") + s.user;
            AppendMenuW(menu, MF_STRING, ID_SESSION_BASE + i, label.c_str());
            if (n == 0) SetMenuDefaultItem(menu, ID_SESSION_BASE + (UINT)i, FALSE);
            n++;
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_SIGNIN, L"&Sign-in screen (another user)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"E&xit tray icon");

    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    std::wstring reply;
    if (cmd == ID_EXIT) DestroyWindow(hwnd);
    else if (cmd == ID_SIGNIN) reply = CallService(L"SIGNIN");
    else if (cmd >= ID_SESSION_BASE && cmd - ID_SESSION_BASE < sessions.size())
        reply = CallService(L"SESSION " + std::to_wstring(sessions[cmd - ID_SESSION_BASE].id));
    if (reply.rfind(L"ERR ", 0) == 0) Notify(reply.substr(4));
}

static void Toggle(HWND hwnd) {
    std::wstring reply = CallService(L"TOGGLE");
    if (reply == L"PICK") {
        POINT pt;
        GetCursorPos(&pt);
        ShowPicker(hwnd, pt);
    } else if (reply.rfind(L"ERR ", 0) == 0) {
        Notify(reply.substr(4));
    }
}

static LRESULT CALLBACK HelperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        AddTrayIcon(hwnd);
        std::wstring failed;
        if (!RegisterHotKey(hwnd, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'U')) failed += L"Ctrl+Alt+U ";
        if (!RegisterHotKey(hwnd, HK_PICK, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'U')) failed += L"Ctrl+Alt+Shift+U ";
        if (!failed.empty()) Notify(L"Hotkey already in use by another program: " + failed);
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) Toggle(hwnd);
        else if (wp == HK_PICK) {
            POINT pt;
            GetCursorPos(&pt);
            ShowPicker(hwnd, pt);
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            Toggle(hwnd);
            break;
        case WM_CONTEXTMENU:
            ShowPicker(hwnd, {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarCreated && msg != 0) {   // Explorer restarted
        AddTrayIcon(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunHelper(HINSTANCE inst) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SwitchUserHelper");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = HelperWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    if (!CreateWindowW(kWndClass, L"SwitchUser", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr)) return 1;

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CloseHandle(mutex);
    return 0;
}

// ---------------------------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring arg = argc > 1 ? argv[1] : L"";
    LocalFree(argv);

    if (arg == L"service") {
        SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)kSvcName, ServiceMain}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (arg == L"install") return InstallOrUninstall(true);
    if (arg == L"uninstall") return InstallOrUninstall(false);
    if (arg == L"install-elevated") return DoInstall();
    if (arg == L"uninstall-elevated") return DoUninstall();
    if (arg == L"toggle") {
        std::wstring reply = CallService(L"TOGGLE");
        if (reply.rfind(L"ERR ", 0) == 0) MessageBoxW(nullptr, reply.substr(4).c_str(), L"SwitchUser", MB_ICONWARNING);
        return reply == L"OK" ? 0 : 1;
    }
    return RunHelper(inst);
}
