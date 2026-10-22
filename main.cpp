// MCP Activity
// Copyright (c) 2026 D. Matthew Pegg, PeggSoft
// Original source: https://github.com/LuckyElba/MCPActivity
// Licensed under the PeggSoft Permissive Attribution License. See LICENSE.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wbemidl.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
constexpr wchar_t kMainClass[] = L"MCPActivityMainWindow";
constexpr wchar_t kDetailsClass[] = L"MCPActivityDetailsWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 1200;

const GUID kTrayGuid = {0x8A7B5F42, 0xE67A, 0x4F53, {0x9C, 0x27, 0x4C, 0x0E, 0xA7, 0x81, 0xB5, 0x41}};

constexpr int IDC_DETAILS = 1001;
constexpr int IDC_LIST = 1101;
constexpr int IDC_START = 1102;
constexpr int IDC_STOP = 1103;
constexpr int IDC_RESTART = 1104;
constexpr int IDC_START_ALL = 1105;
constexpr int IDC_STOP_ALL = 1106;
constexpr int IDC_EDIT_CONFIG = 1107;
constexpr int IDC_START_WINDOWS = 1108;
constexpr int IDC_LOG = 1109;
constexpr int IDC_COMMUNICATIONS = 1110;
constexpr int IDC_COMM_GRID = 1111;
constexpr UINT_PTR kCommunicationsTimer = 2;

enum class AppState
{
    Clear,
    Green,
    Orange,
    Red
};

struct ProcessInfo
{
    DWORD pid = 0;
    std::wstring name;
    std::wstring commandLine;
};

struct ManagedApp
{
    std::wstring name;
    std::wstring command;
    std::wstring workingDirectory;
    std::wstring match1;
    std::wstring match2;
    bool monitorOnly = false;
    std::wstring matchExecutable;
    std::wstring communicationLog;
    bool autoStart = true;
    bool restartOnFailure = true;
    bool wantedRunning = false;
    AppState state = AppState::Clear;
    std::vector<DWORD> pids;
    std::unordered_map<DWORD, ULONGLONG> previousCpu;
    ULONGLONG lastActivityTick = 0;
    ULONGLONG nextRestartTick = 0;
    std::wstring lastError;
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_detailsWindow = nullptr;
HWND g_list = nullptr;
HWND g_logEdit = nullptr;
HWND g_commGrid = nullptr;
int g_commSortColumn = 0;
bool g_commSortAscending = false;
int g_communicationsProfile = -1;
std::wstring g_lastCommunicationsText;
HFONT g_uiFont = nullptr;
HFONT g_uiFontBold = nullptr;
HBRUSH g_detailsBrush = nullptr;
HBRUSH g_editBrush = nullptr;
HICON g_windowIcon = nullptr;
HICON g_trayIcon = nullptr;
NOTIFYICONDATAW g_tray{};
UINT g_taskbarCreatedMessage = 0;
bool g_trayAdded = false;
bool g_trayIconStateValid = false;
AppState g_trayIconState = AppState::Clear;
IWbemServices *g_wmi = nullptr;
std::vector<ManagedApp> g_apps;
std::deque<std::wstring> g_log;
std::wstring g_iniPath;
std::wstring g_exePath;
bool g_exitRequested = false;
bool g_runAtStartupEnabled = false;

AppState OverallTrayState();
void AddTrayIcon();
void SetTrayStatusIcon(AppState state);

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c)
                   {
                       return static_cast<wchar_t>(towlower(c));
                   });
    return value;
}

std::wstring GetModulePath()
{
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::wstring(buffer.data(), length);
}

std::wstring GetDirectoryName(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring GetEnvironmentString(const wchar_t *name)
{
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed)
        return L"";
    std::vector<wchar_t> value(needed);
    GetEnvironmentVariableW(name, value.data(), needed);
    return value.data();
}

std::wstring GetLastErrorMessage(DWORD error)
{
    wchar_t *raw = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&raw), 0, nullptr);
    std::wstring result = raw ? raw : L"Unknown error";
    if (raw)
        LocalFree(raw);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
        result.pop_back();
    return result;
}

std::wstring TimeStamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t text[32]{};
    swprintf_s(text, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return text;
}

void AddLog(const std::wstring &text)
{
    g_log.push_back(L"[" + TimeStamp() + L"] " + text);
    while (g_log.size() > 250)
        g_log.pop_front();

    if (g_logEdit && IsWindow(g_logEdit))
    {
        std::wstring all;
        for (const auto &line : g_log)
        {
            all += line;
            all += L"\r\n";
        }
        SetWindowTextW(g_logEdit, all.c_str());
        SendMessageW(g_logEdit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
    }
}

const wchar_t *StateText(AppState state)
{
    switch (state)
    {
    case AppState::Green:
        return L"Running";
    case AppState::Orange:
        return L"Active";
    case AppState::Red:
        return L"Error";
    default:
        return L"Not loaded";
    }
}

int ScaleForDpi(int value, UINT dpi);

COLORREF StateColor(AppState state)
{
    switch (state)
    {
    case AppState::Green:
        return RGB(50, 205, 85);
    case AppState::Orange:
        return RGB(245, 166, 35);
    case AppState::Red:
        return RGB(235, 70, 70);
    default:
        return RGB(58, 61, 66);
    }
}

void DrawLed(HDC dc, int cx, int cy, AppState state, UINT dpi)
{
    const int outerRadius = ScaleForDpi(12, dpi);
    const int ledRadius = ScaleForDpi(9, dpi);
    const int shineSize = std::max(2, ScaleForDpi(4, dpi));
    const int penWidth = std::max(1, ScaleForDpi(1, dpi));

    RECT outer{cx - outerRadius, cy - outerRadius, cx + outerRadius, cy + outerRadius};
    HBRUSH ring = CreateSolidBrush(RGB(18, 20, 22));
    FillRect(dc, &outer, ring);
    DeleteObject(ring);

    HBRUSH fill = CreateSolidBrush(StateColor(state));
    HPEN pen =
        CreatePen(PS_SOLID, penWidth, state == AppState::Clear ? RGB(115, 120, 126) : StateColor(state));
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, cx - ledRadius, cy - ledRadius, cx + ledRadius, cy + ledRadius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);

    if (state != AppState::Clear)
    {
        HBRUSH shine = CreateSolidBrush(RGB(230, 235, 240));
        oldBrush = SelectObject(dc, shine);
        oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - ledRadius / 2, cy - ledRadius * 2 / 3, cx - ledRadius / 2 + shineSize,
                cy - ledRadius * 2 / 3 + shineSize);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(shine);
    }
}

bool InitializeWmi()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;

    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
        return false;

    IWbemLocator *locator = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                          reinterpret_cast<void **>(&locator));
    if (FAILED(hr))
        return false;

    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_wmi);
    SysFreeString(ns);
    locator->Release();

    if (FAILED(hr) || !g_wmi)
        return false;

    hr = CoSetProxyBlanket(g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return SUCCEEDED(hr);
}

std::vector<ProcessInfo> QueryProcesses()
{
    std::vector<ProcessInfo> result;
    if (!g_wmi)
        return result;

    IEnumWbemClassObject *enumerator = nullptr;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT ProcessId, Name, CommandLine FROM Win32_Process");
    HRESULT hr = g_wmi->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                  nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(query);

    if (FAILED(hr) || !enumerator)
        return result;

    while (true)
    {
        IWbemClassObject *object = nullptr;
        ULONG returned = 0;
        hr = enumerator->Next(250, 1, &object, &returned);
        if (FAILED(hr) || returned == 0)
            break;

        VARIANT pid{};
        VARIANT name{};
        VARIANT cmd{};
        VariantInit(&pid);
        VariantInit(&name);
        VariantInit(&cmd);

        object->Get(L"ProcessId", 0, &pid, nullptr, nullptr);
        object->Get(L"Name", 0, &name, nullptr, nullptr);
        object->Get(L"CommandLine", 0, &cmd, nullptr, nullptr);

        ProcessInfo info;
        if (pid.vt == VT_I4 || pid.vt == VT_UI4)
            info.pid = pid.ulVal;
        if (name.vt == VT_BSTR && name.bstrVal)
            info.name = name.bstrVal;
        if (cmd.vt == VT_BSTR && cmd.bstrVal)
            info.commandLine = cmd.bstrVal;

        if (info.pid != 0)
            result.push_back(std::move(info));

        VariantClear(&pid);
        VariantClear(&name);
        VariantClear(&cmd);
        object->Release();
    }

    enumerator->Release();
    return result;
}

bool Matches(const ManagedApp &app, const ProcessInfo &process)
{
    if (!app.matchExecutable.empty() && ToLower(process.name) != ToLower(app.matchExecutable))
        return false;
    const std::wstring line = ToLower(process.commandLine);
    const std::wstring first = ToLower(app.match1);
    const std::wstring second = ToLower(app.match2);
    if (!first.empty() && line.find(first) == std::wstring::npos)
        return false;
    if (!second.empty() && line.find(second) == std::wstring::npos)
        return false;
    return !first.empty() || !second.empty();
}

ULONGLONG CpuTimeForProcess(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 0;

    FILETIME create{}, exit{}, kernel{}, user{};
    ULONGLONG total = 0;
    if (GetProcessTimes(process, &create, &exit, &kernel, &user))
    {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        total = k.QuadPart + u.QuadPart;
    }
    CloseHandle(process);
    return total;
}

void WriteDefaultConfigIfNeeded()
{
    if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;

    WritePrivateProfileStringW(L"General", L"RefreshMilliseconds", L"1200", g_iniPath.c_str());

    WritePrivateProfileStringW(L"Desktop Commander", L"Command",
                               L"cmd.exe /d /s /c \"npx @wonderwhy-er/desktop-commander@latest remote\"",
                               g_iniPath.c_str());
    WritePrivateProfileStringW(L"Desktop Commander", L"WorkingDirectory",
                               GetEnvironmentString(L"USERPROFILE").c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Desktop Commander", L"Match1", L"desktop-commander", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Desktop Commander", L"Match2", L" remote", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Desktop Commander", L"AutoStart", L"1", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Desktop Commander", L"RestartOnFailure", L"1", g_iniPath.c_str());

    WritePrivateProfileStringW(L"ScreenShot", L"Command",
                               L"cmd.exe /d /s /c \"\"C:\\WindowCaptureMCP\\.venv\\Scripts\\python.exe\" "
                               L"\"C:\\WindowCaptureMCP\\server.py\"\"",
                               g_iniPath.c_str());
    WritePrivateProfileStringW(L"ScreenShot", L"WorkingDirectory", L"C:\\WindowCaptureMCP",
                               g_iniPath.c_str());
    WritePrivateProfileStringW(L"ScreenShot", L"Match1", L"windowcapturemcp", g_iniPath.c_str());
    WritePrivateProfileStringW(L"ScreenShot", L"Match2", L"server.py", g_iniPath.c_str());
    WritePrivateProfileStringW(L"ScreenShot", L"AutoStart", L"1", g_iniPath.c_str());
    WritePrivateProfileStringW(L"ScreenShot", L"RestartOnFailure", L"1", g_iniPath.c_str());
}

std::wstring ReadIniString(const wchar_t *section, const wchar_t *key, const wchar_t *fallback)
{
    wchar_t buffer[4096]{};
    GetPrivateProfileStringW(section, key, fallback, buffer, static_cast<DWORD>(std::size(buffer)),
                             g_iniPath.c_str());
    return buffer;
}

ManagedApp LoadApp(const wchar_t *section)
{
    ManagedApp app;
    app.name = section;
    app.command = ReadIniString(section, L"Command", L"");
    app.workingDirectory = ReadIniString(section, L"WorkingDirectory", L"");
    app.match1 = ReadIniString(section, L"Match1", L"");
    app.match2 = ReadIniString(section, L"Match2", L"");
    app.matchExecutable = ReadIniString(section, L"MatchExecutable", L"");
    app.communicationLog = ReadIniString(section, L"CommunicationLog", L"");
    app.monitorOnly = GetPrivateProfileIntW(section, L"MonitorOnly", 0, g_iniPath.c_str()) != 0;
    app.autoStart = !app.monitorOnly && GetPrivateProfileIntW(section, L"AutoStart", 1, g_iniPath.c_str()) != 0;
    app.restartOnFailure = !app.monitorOnly && GetPrivateProfileIntW(section, L"RestartOnFailure", 1, g_iniPath.c_str()) != 0;
    return app;
}

void LoadConfiguration()
{
    WriteDefaultConfigIfNeeded();
    g_apps.clear();
    std::vector<wchar_t> sections(4096);
    for (;;)
    {
        DWORD count = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), g_iniPath.c_str());
        if (count < sections.size() - 2)
            break;
        if (sections.size() >= 1024 * 1024)
        {
            AddLog(L"Configuration contains too many sections.");
            return;
        }
        sections.resize(sections.size() * 2);
    }
    for (const wchar_t *section = sections.data(); *section; section += wcslen(section) + 1)
    {
        if (ToLower(section) == L"general")
            continue;
        ManagedApp app = LoadApp(section);
        if (!app.match1.empty() || !app.match2.empty() || !app.command.empty())
            g_apps.push_back(std::move(app));
    }
}

bool LaunchApp(size_t index)
{
    if (index >= g_apps.size())
        return false;

    ManagedApp &app = g_apps[index];
    if (app.monitorOnly)
        return false;
    app.wantedRunning = true;
    app.lastError.clear();

    if (!app.pids.empty())
        return true;

    if (app.command.empty())
    {
        app.lastError = L"No command is configured.";
        app.state = AppState::Red;
        AddLog(app.name + L": " + app.lastError);
        return false;
    }

    std::vector<wchar_t> commandLine(app.command.begin(), app.command.end());
    commandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, app.workingDirectory.empty() ? nullptr : app.workingDirectory.c_str(), &startup, &process);

    if (!ok)
    {
        DWORD error = GetLastError();
        app.lastError = L"Start failed: " + GetLastErrorMessage(error);
        app.state = AppState::Red;
        app.nextRestartTick = GetTickCount64() + 5000;
        AddLog(app.name + L": " + app.lastError);
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    app.nextRestartTick = GetTickCount64() + 5000;
    AddLog(app.name + L": start requested (hidden).");
    return true;
}

void KillAppProcesses(size_t index, bool intentional)
{
    if (index >= g_apps.size())
        return;

    ManagedApp &app = g_apps[index];
    if (app.monitorOnly)
        return;
    if (intentional)
        app.wantedRunning = false;

    size_t killed = 0;
    for (DWORD pid : app.pids)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (process)
        {
            if (TerminateProcess(process, 0))
                ++killed;
            CloseHandle(process);
        }
    }

    app.pids.clear();
    app.previousCpu.clear();
    app.state = intentional ? AppState::Clear : AppState::Red;
    if (killed)
        AddLog(app.name + L": stopped " + std::to_wstring(killed) + L" process(es).");
    else
        AddLog(app.name + L": no running process was found.");
}

void RestartApp(size_t index)
{
    if (index >= g_apps.size())
        return;

    if (g_apps[index].monitorOnly)
        return;
    AddLog(g_apps[index].name + L": restart requested.");
    g_apps[index].wantedRunning = true;
    KillAppProcesses(index, false);
    Sleep(350);
    LaunchApp(index);
}

bool IsRunAtStartupEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD size = sizeof(value);
    LONG result =
        RegQueryValueExW(key, L"MCPActivity", nullptr, &type, reinterpret_cast<BYTE *>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

void SetRunAtStartup(bool enabled)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enabled)
    {
        std::wstring value = L"\"" + g_exePath + L"\"";
        RegSetValueExW(key, L"MCPActivity", 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        AddLog(L"MCP Activity: enabled start with Windows.");
    }
    else
    {
        RegDeleteValueW(key, L"MCPActivity");
        AddLog(L"MCP Activity: disabled start with Windows.");
    }
    RegCloseKey(key);
}

void UpdateTrayTooltip()
{
    int green = 0;
    int orange = 0;
    int red = 0;

    for (const auto &app : g_apps)
    {
        if (app.state == AppState::Green)
        {
            ++green;
        }
        if (app.state == AppState::Orange)
        {
            ++orange;
        }
        if (app.state == AppState::Red)
        {
            ++red;
        }
    }

    std::wstring text = L"MCP Activity - ";
    if (red)
    {
        text += std::to_wstring(red) + (red == 1 ? L" error" : L" errors");
    }
    else if (orange)
    {
        text += L"active";
    }
    else if (green)
    {
        text += std::to_wstring(green) + L" running";
    }
    else
    {
        text += L"not loaded";
    }

    SetTrayStatusIcon(OverallTrayState());

    if (g_trayAdded)
    {
        g_tray.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_GUID;
        g_tray.guidItem = kTrayGuid;
        wcsncpy_s(g_tray.szTip, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    }
}

void UpdateDetailsList()
{
    if (!g_list || !IsWindow(g_list))
    {
        return;
    }

    const int selectedIndex = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    const int focusedIndex = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED);
    const int existingCount = ListView_GetItemCount(g_list);
    const bool rebuildRows = existingCount != static_cast<int>(g_apps.size());

    if (rebuildRows)
    {
        SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_list);

        for (size_t i = 0; i < g_apps.size(); ++i)
        {
            const ManagedApp &app = g_apps[i];

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(i);
            item.lParam = static_cast<LPARAM>(i);
            item.pszText = const_cast<wchar_t *>(app.name.c_str());
            ListView_InsertItem(g_list, &item);
        }
    }

    for (size_t i = 0; i < g_apps.size(); ++i)
    {
        const ManagedApp &app = g_apps[i];
        const int row = static_cast<int>(i);

        ListView_SetItemText(g_list, row, 0, const_cast<wchar_t *>(app.name.c_str()));
        ListView_SetItemText(g_list, row, 1, const_cast<wchar_t *>(StateText(app.state)));

        std::wstring pids;
        for (size_t p = 0; p < app.pids.size(); ++p)
        {
            if (p)
            {
                pids += L", ";
            }
            pids += std::to_wstring(app.pids[p]);
        }

        if (pids.empty())
        {
            pids = L"-";
        }

        ListView_SetItemText(g_list, row, 2, pids.data());

        std::wstring autoText = app.autoStart ? L"Yes" : L"No";
        ListView_SetItemText(g_list, row, 3, autoText.data());

        std::wstring restartText = app.restartOnFailure ? L"Yes" : L"No";
        ListView_SetItemText(g_list, row, 4, restartText.data());

        std::wstring monitorText = app.monitorOnly ? L"Yes" : L"No";
        ListView_SetItemText(g_list, row, 5, monitorText.data());

        const wchar_t *command = app.monitorOnly ? L"Client-managed (monitor only)" : app.command.c_str();
        ListView_SetItemText(g_list, row, 6, const_cast<wchar_t *>(command));
    }

    const bool controllable = selectedIndex >= 0 && selectedIndex < static_cast<int>(g_apps.size()) &&
                              !g_apps[selectedIndex].monitorOnly;
    for (int id : {IDC_START, IDC_STOP, IDC_RESTART})
        EnableWindow(GetDlgItem(g_detailsWindow, id), controllable);
    const bool anyControllable = std::any_of(g_apps.begin(), g_apps.end(),
                                           [](const ManagedApp &app) { return !app.monitorOnly; });
    EnableWindow(GetDlgItem(g_detailsWindow, IDC_START_ALL), anyControllable);
    EnableWindow(GetDlgItem(g_detailsWindow, IDC_STOP_ALL), anyControllable);
    if (rebuildRows)
    {

        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(g_apps.size()))
        {
            ListView_SetItemState(g_list, selectedIndex, LVIS_SELECTED, LVIS_SELECTED);
        }

        if (focusedIndex >= 0 && focusedIndex < static_cast<int>(g_apps.size()))
        {
            ListView_SetItemState(g_list, focusedIndex, LVIS_FOCUSED, LVIS_FOCUSED);
        }

        SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_list, nullptr, TRUE);
    }
}

void RefreshStatus()
{
    const std::vector<ProcessInfo> processes = QueryProcesses();
    const ULONGLONG now = GetTickCount64();

    for (size_t i = 0; i < g_apps.size(); ++i)
    {
        ManagedApp &app = g_apps[i];
        const AppState previousState = app.state;

        std::vector<DWORD> matched;
        std::unordered_map<DWORD, ULONGLONG> newCpu;
        ULONGLONG cpuDelta = 0;

        for (const auto &process : processes)
        {
            if (!Matches(app, process))
                continue;

            matched.push_back(process.pid);
            ULONGLONG current = CpuTimeForProcess(process.pid);
            newCpu[process.pid] = current;
            auto old = app.previousCpu.find(process.pid);
            if (old != app.previousCpu.end() && current >= old->second)
                cpuDelta += current - old->second;
        }

        app.pids = std::move(matched);

        if (!app.pids.empty())
        {
            if (!app.wantedRunning)
            {
                app.wantedRunning = true;
                AddLog(app.name + L": adopted an already-running instance.");
            }

            if (cpuDelta >= 200000ULL)
                app.lastActivityTick = now;

            app.state = (now - app.lastActivityTick < 1800 && app.lastActivityTick != 0) ? AppState::Orange
                                                                                         : AppState::Green;
            app.lastError.clear();
            app.nextRestartTick = 0;
        }
        else if (app.monitorOnly)
        {
            app.state = AppState::Clear;
            app.wantedRunning = false;
            app.lastError.clear();
        }
        else if (app.wantedRunning)
        {
            app.state = AppState::Red;
            if (previousState == AppState::Green || previousState == AppState::Orange)
            {
                app.lastError = L"Process exited unexpectedly.";
                AddLog(app.name + L": process exited unexpectedly.");
                app.nextRestartTick = now + 2500;
            }

            if (app.restartOnFailure && now >= app.nextRestartTick)
            {
                app.nextRestartTick = now + 5000;
                LaunchApp(i);
            }
        }
        else
        {
            app.state = AppState::Clear;
        }

        app.previousCpu = std::move(newCpu);
    }

    InvalidateRect(g_mainWindow, nullptr, FALSE);
    UpdateDetailsList();
    UpdateTrayTooltip();
}

int SelectedAppIndex()
{
    if (!g_list)
        return -1;
    return ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
}

void PositionMainWindow(HWND hwnd)
{
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    RECT window{};
    GetWindowRect(hwnd, &window);
    int width = window.right - window.left;
    int height = window.bottom - window.top;
    int x = work.right - width - 18;
    int y = work.bottom - height - 18;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void ShowMainWindow()
{
    if (!g_mainWindow || !IsWindow(g_mainWindow))
    {
        return;
    }

    ShowWindow(g_mainWindow, SW_RESTORE);
    PositionMainWindow(g_mainWindow);
    BringWindowToTop(g_mainWindow);
    SetForegroundWindow(g_mainWindow);
}

HICON CreateMcpIcon(COLORREF fillColor)
{
    constexpr int width = 32;
    constexpr int height = 32;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
    {
        return nullptr;
    }

    HDC colorDc = CreateCompatibleDC(screenDc);
    HDC maskDc = CreateCompatibleDC(screenDc);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screenDc, width, height);
    HBITMAP maskBitmap = CreateBitmap(width, height, 1, 1, nullptr);
    ReleaseDC(nullptr, screenDc);

    if (!colorDc || !maskDc || !colorBitmap || !maskBitmap)
    {
        if (colorDc)
        {
            DeleteDC(colorDc);
        }
        if (maskDc)
        {
            DeleteDC(maskDc);
        }
        if (colorBitmap)
        {
            DeleteObject(colorBitmap);
        }
        if (maskBitmap)
        {
            DeleteObject(maskBitmap);
        }
        return nullptr;
    }

    HGDIOBJ oldColorBitmap = SelectObject(colorDc, colorBitmap);
    HGDIOBJ oldMaskBitmap = SelectObject(maskDc, maskBitmap);

    RECT iconRect{0, 0, width, height};

    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(colorDc, &iconRect, black);
    DeleteObject(black);

    FillRect(maskDc, &iconRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    HBRUSH colorBrush = CreateSolidBrush(fillColor);
    HPEN colorPen = CreatePen(PS_SOLID, 1, fillColor);
    HGDIOBJ oldColorBrush = SelectObject(colorDc, colorBrush);
    HGDIOBJ oldColorPen = SelectObject(colorDc, colorPen);
    Ellipse(colorDc, 2, 2, width - 2, height - 2);
    SelectObject(colorDc, oldColorBrush);
    SelectObject(colorDc, oldColorPen);
    DeleteObject(colorBrush);
    DeleteObject(colorPen);

    HGDIOBJ oldMaskBrush = SelectObject(maskDc, GetStockObject(BLACK_BRUSH));
    HGDIOBJ oldMaskPen = SelectObject(maskDc, GetStockObject(BLACK_PEN));
    Ellipse(maskDc, 2, 2, width - 2, height - 2);
    SelectObject(maskDc, oldMaskBrush);
    SelectObject(maskDc, oldMaskPen);

    HFONT font =
        CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(colorDc, font);
    SetBkMode(colorDc, TRANSPARENT);
    SetTextColor(colorDc, RGB(255, 255, 255));

    RECT textRect{0, -1, width, height};
    DrawTextW(colorDc, L"M", 1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(colorDc, oldFont);
    DeleteObject(font);

    SelectObject(colorDc, oldColorBitmap);
    SelectObject(maskDc, oldMaskBitmap);
    DeleteDC(colorDc);
    DeleteDC(maskDc);

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmMask = maskBitmap;
    iconInfo.hbmColor = colorBitmap;

    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon;
}

AppState OverallTrayState()
{
    bool hasGreen = false;
    bool hasOrange = false;

    for (const auto &app : g_apps)
    {
        if (app.state == AppState::Red)
        {
            return AppState::Red;
        }

        if (app.state == AppState::Orange)
        {
            hasOrange = true;
        }
        else if (app.state == AppState::Green)
        {
            hasGreen = true;
        }
    }

    if (hasOrange)
    {
        return AppState::Orange;
    }

    if (hasGreen)
    {
        return AppState::Green;
    }

    return AppState::Clear;
}

void SetTrayStatusIcon(AppState state)
{
    if (g_trayIconStateValid && g_trayIconState == state && g_trayIcon)
    {
        return;
    }

    HICON newIcon = CreateMcpIcon(StateColor(state));
    if (!newIcon)
    {
        return;
    }

    HICON oldIcon = g_trayIcon;
    g_trayIcon = newIcon;
    g_trayIconState = state;
    g_trayIconStateValid = true;

    if (g_trayAdded)
    {
        g_tray.uFlags = NIF_ICON | NIF_GUID;
        g_tray.guidItem = kTrayGuid;
        g_tray.hIcon = g_trayIcon;

        if (!Shell_NotifyIconW(NIM_MODIFY, &g_tray))
        {
            g_trayAdded = false;
            AddTrayIcon();
        }
    }

    if (oldIcon)
    {
        DestroyIcon(oldIcon);
    }
}

void AddTrayIcon()
{
    if (!g_trayIcon)
    {
        g_trayIcon = CreateMcpIcon(RGB(45, 112, 170));
    }

    NOTIFYICONDATAW stale{};
    stale.cbSize = sizeof(stale);
    stale.hWnd = g_mainWindow;
    stale.uID = 1;
    stale.uFlags = NIF_GUID;
    stale.guidItem = kTrayGuid;
    Shell_NotifyIconW(NIM_DELETE, &stale);

    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_mainWindow;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    g_tray.guidItem = kTrayGuid;
    wcscpy_s(g_tray.szTip, L"MCP Activity");

    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;

    if (g_trayAdded)
    {
        g_tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    }
}

void RemoveTrayIcon()
{
    if (g_trayAdded)
    {
        g_tray.uFlags = NIF_GUID;
        g_tray.guidItem = kTrayGuid;
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        g_trayAdded = false;
    }
}

void ShowTrayMenu()
{
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 2001, L"Show MCP Activity");
    SetMenuDefaultItem(menu, 2001, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2002, L"Start All");
    AppendMenuW(menu, MF_STRING, 2003, L"Stop All");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2004, L"Exit");

    SetForegroundWindow(g_mainWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_mainWindow, nullptr);
    PostMessageW(g_mainWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

UINT CurrentDpi(HWND hwnd)
{
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi ? dpi : 96;
}

HFONT CreateUiFont(UINT dpi, int pointSize, LONG weight)
{
    return CreateFontW(-MulDiv(pointSize, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void RecreateUiFonts(HWND hwnd)
{
    if (g_uiFont)
    {
        DeleteObject(g_uiFont);
    }
    if (g_uiFontBold)
    {
        DeleteObject(g_uiFontBold);
    }

    const UINT dpi = CurrentDpi(hwnd);
    g_uiFont = CreateUiFont(dpi, 11, FW_NORMAL);
    g_uiFontBold = CreateUiFont(dpi, 11, FW_SEMIBOLD);

    if (hwnd)
    {
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM)
            {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
                return TRUE;
            },
            0);
    }
}

void DrawDarkButton(const DRAWITEMSTRUCT *drawItem)
{
    if (!drawItem || drawItem->CtlType != ODT_BUTTON)
    {
        return;
    }

    RECT rect = drawItem->rcItem;
    const UINT dpi = CurrentDpi(drawItem->hwndItem);

    if (drawItem->CtlID == IDC_START_WINDOWS)
    {
        HBRUSH background = CreateSolidBrush(RGB(31, 33, 36));
        FillRect(drawItem->hDC, &rect, background);
        DeleteObject(background);

        const int boxSize = ScaleForDpi(18, dpi);
        RECT box{rect.left, rect.top + ((rect.bottom - rect.top) - boxSize) / 2, rect.left + boxSize,
                 rect.top + ((rect.bottom - rect.top) - boxSize) / 2 + boxSize};

        const bool checked = g_runAtStartupEnabled;
        const COLORREF boxFillColor = checked ? RGB(45, 112, 170) : RGB(43, 46, 51);
        const COLORREF boxBorderColor = checked ? RGB(135, 195, 245) : RGB(105, 111, 120);

        HBRUSH boxFill = CreateSolidBrush(boxFillColor);
        FillRect(drawItem->hDC, &box, boxFill);
        DeleteObject(boxFill);

        HPEN boxPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi)), boxBorderColor);
        HGDIOBJ oldPen = SelectObject(drawItem->hDC, boxPen);
        HGDIOBJ oldBrush = SelectObject(drawItem->hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(drawItem->hDC, box.left, box.top, box.right, box.bottom);

        if (checked)
        {
            HPEN checkPen = CreatePen(PS_SOLID, std::max(3, ScaleForDpi(2, dpi)), RGB(255, 255, 255));
            SelectObject(drawItem->hDC, checkPen);
            MoveToEx(drawItem->hDC, box.left + ScaleForDpi(4, dpi), box.top + ScaleForDpi(9, dpi), nullptr);
            LineTo(drawItem->hDC, box.left + ScaleForDpi(8, dpi), box.top + ScaleForDpi(13, dpi));
            LineTo(drawItem->hDC, box.left + ScaleForDpi(14, dpi), box.top + ScaleForDpi(5, dpi));
            SelectObject(drawItem->hDC, oldPen);
            DeleteObject(checkPen);
        }
        else
        {
            SelectObject(drawItem->hDC, oldPen);
        }

        SelectObject(drawItem->hDC, oldBrush);
        DeleteObject(boxPen);

        RECT textRect = rect;
        textRect.left += boxSize + ScaleForDpi(10, dpi);

        wchar_t label[256]{};
        GetWindowTextW(drawItem->hwndItem, label, static_cast<int>(std::size(label)));

        SetBkMode(drawItem->hDC, TRANSPARENT);
        SetTextColor(drawItem->hDC, RGB(240, 242, 245));
        HGDIOBJ oldFont = SelectObject(drawItem->hDC, g_uiFont);
        DrawTextW(drawItem->hDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem->hDC, oldFont);
        return;
    }

    const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
    const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
    const bool focused = (drawItem->itemState & ODS_FOCUS) != 0;
    const bool accent = drawItem->CtlID == IDC_DETAILS;

    COLORREF fillColor = accent ? RGB(53, 78, 103) : RGB(49, 53, 59);
    COLORREF borderColor = accent ? RGB(96, 129, 162) : RGB(83, 88, 96);

    if (pressed)
    {
        fillColor = accent ? RGB(42, 64, 85) : RGB(39, 42, 47);
    }
    if (disabled)
    {
        fillColor = RGB(43, 45, 49);
        borderColor = RGB(65, 68, 73);
    }

    HBRUSH fill = CreateSolidBrush(fillColor);
    FillRect(drawItem->hDC, &rect, fill);
    DeleteObject(fill);

    HPEN border = CreatePen(PS_SOLID, focused ? 2 : 1, borderColor);
    HGDIOBJ oldPen = SelectObject(drawItem->hDC, border);
    HGDIOBJ oldBrush = SelectObject(drawItem->hDC, GetStockObject(HOLLOW_BRUSH));
    Rectangle(drawItem->hDC, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(drawItem->hDC, oldBrush);
    SelectObject(drawItem->hDC, oldPen);
    DeleteObject(border);

    wchar_t label[256]{};
    GetWindowTextW(drawItem->hwndItem, label, static_cast<int>(std::size(label)));

    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, disabled ? RGB(135, 139, 145) : RGB(240, 242, 245));
    HGDIOBJ oldFont = SelectObject(drawItem->hDC, accent && g_uiFontBold ? g_uiFontBold : g_uiFont);
    DrawTextW(drawItem->hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(drawItem->hDC, oldFont);
}

void SetDarkWindow(HWND hwnd)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
}

void LayoutDetails(HWND hwnd);

void CreateDetailsWindow()
{
    if (g_detailsWindow && IsWindow(g_detailsWindow))
    {
        ShowWindow(g_detailsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_detailsWindow);
        return;
    }

    const UINT dpi = CurrentDpi(g_mainWindow);
    const int width = ScaleForDpi(1020, dpi);
    const int height = ScaleForDpi(650, dpi);

    g_detailsWindow = CreateWindowExW(WS_EX_APPWINDOW, kDetailsClass, L"MCP Activity - Details",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, width,
                                      height, nullptr, nullptr, g_instance, nullptr);
}

LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR subclassId, DWORD_PTR)
{
    if (message == WM_NOTIFY)
    {
        NMHDR *notification = reinterpret_cast<NMHDR *>(lParam);
        HWND header = ListView_GetHeader(hwnd);

        if (notification && header && notification->hwndFrom == header && notification->code == NM_CUSTOMDRAW)
        {
            NMCUSTOMDRAW *customDraw = reinterpret_cast<NMCUSTOMDRAW *>(lParam);

            if (customDraw->dwDrawStage == CDDS_PREPAINT)
            {
                return CDRF_NOTIFYITEMDRAW;
            }

            if (customDraw->dwDrawStage == CDDS_ITEMPREPAINT)
            {
                HDC dc = customDraw->hdc;
                RECT rect = customDraw->rc;
                const int column = static_cast<int>(customDraw->dwItemSpec);
                const UINT dpi = CurrentDpi(hwnd);

                HBRUSH background = CreateSolidBrush(RGB(45, 48, 53));
                FillRect(dc, &rect, background);
                DeleteObject(background);

                HPEN line = CreatePen(PS_SOLID, 1, RGB(84, 89, 96));
                HGDIOBJ oldPen = SelectObject(dc, line);
                MoveToEx(dc, rect.right - 1, rect.top, nullptr);
                LineTo(dc, rect.right - 1, rect.bottom);
                MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
                LineTo(dc, rect.right, rect.bottom - 1);
                SelectObject(dc, oldPen);
                DeleteObject(line);

                wchar_t label[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT | HDI_FORMAT;
                item.pszText = label;
                item.cchTextMax = static_cast<int>(std::size(label));
                Header_GetItem(header, column, &item);

                RECT textRect = rect;
                textRect.left += ScaleForDpi(7, dpi);
                textRect.right -= ScaleForDpi(7, dpi);

                UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                if (item.fmt & HDF_CENTER)
                {
                    format |= DT_CENTER;
                }
                else if (item.fmt & HDF_RIGHT)
                {
                    format |= DT_RIGHT;
                }
                else
                {
                    format |= DT_LEFT;
                }

                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(240, 242, 245));
                HGDIOBJ oldFont = SelectObject(dc, g_uiFontBold);
                DrawTextW(dc, label, -1, &textRect, format);
                SelectObject(dc, oldFont);

                return CDRF_SKIPDEFAULT;
            }
        }
    }
    else if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, ListViewSubclassProc, subclassId);
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void PopulateCommunicationsGrid(const std::wstring &text);

void ShowCommunications(size_t index)
{
    if (index >= g_apps.size() || !g_logEdit)
        return;

    if (g_communicationsProfile != static_cast<int>(index))
    {
        g_lastCommunicationsText.clear();
        if (g_commGrid) ListView_DeleteAllItems(g_commGrid);
    }
    g_communicationsProfile = static_cast<int>(index);
    const std::wstring &path = g_apps[index].communicationLog;
    HWND logLabel = FindWindowExW(g_detailsWindow, nullptr, L"STATIC", nullptr);
    // The log label is the only STATIC child in the Details view.
    if (logLabel)
    {
        const std::wstring caption = L"Activity / errors";
        SetWindowTextW(logLabel, caption.c_str());
    }
    if (path.empty())
    {
        if (g_commGrid) ShowWindow(g_commGrid, SW_HIDE);
        ShowWindow(g_logEdit, SW_SHOW);
        SetWindowTextW(g_logEdit, L"No CommunicationLog file configured for this profile. Set CommunicationLog to a local JSONL metadata log in mcp-activity.ini.");
        return;
    }

    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
    {
        if (g_commGrid) ShowWindow(g_commGrid, SW_HIDE);
        ShowWindow(g_logEdit, SW_SHOW);
        SetWindowTextW(g_logEdit, L"Communication metadata log is not available.");
        return;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0)
        return;
    const std::streamoff offset = std::max<std::streamoff>(0, length - 32768);
    input.seekg(offset);
    if (offset)
    {
        std::string partial;
        std::getline(input, partial);
    }
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (content.empty())
    {
        if (g_commGrid) ShowWindow(g_commGrid, SW_HIDE);
        ShowWindow(g_logEdit, SW_SHOW);
        SetWindowTextW(g_logEdit, L"No communications recorded.");
        return;
    }
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(),
                                        static_cast<int>(content.size()), nullptr, 0);
    if (required <= 0)
    {
        if (g_commGrid) ShowWindow(g_commGrid, SW_HIDE);
        ShowWindow(g_logEdit, SW_SHOW);
        SetWindowTextW(g_logEdit, L"Metadata log is not valid UTF-8.");
        return;
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(),
                        static_cast<int>(content.size()), wide.data(), required);
    ShowWindow(g_logEdit, SW_HIDE);
    if (g_commGrid) ShowWindow(g_commGrid, SW_SHOW);
    if (wide != g_lastCommunicationsText)
    {
        g_lastCommunicationsText = wide;
        PopulateCommunicationsGrid(wide);
    }
}


std::wstring MetadataField(const std::wstring &line, const wchar_t *key)
{
    const std::wstring needle = std::wstring(L"\"") + key + L"\":";
    size_t pos = line.find(needle);
    if (pos == std::wstring::npos)
        return L"";
    pos += needle.length();
    while (pos < line.size() && iswspace(line[pos])) ++pos;
    if (pos == line.size()) return L"";
    if (line[pos] == L'"')
    {
        ++pos;
        std::wstring value;
        while (pos < line.size())
        {
            if (line[pos] == L'\\' && pos + 1 < line.size())
            {
                ++pos;
                value += line[pos++];
            }
            else if (line[pos] == L'"')
                break;
            else
                value += line[pos++];
        }
        return value;
    }
    size_t end = line.find_first_of(L",}", pos);
    return line.substr(pos, end == std::wstring::npos ? end : end - pos);
}

std::wstring FormatCommunicationTimestamp(const std::wstring &value)
{
    // Proxy timestamps are UTC ISO-8601, with Z or +00:00 offsets.
    if (value.size() < 19 || value[4] != L'-' || value[10] != L'T')
        return value;
    if (value.find(L"+00:00") == std::wstring::npos &&
        value.find(L'Z') == std::wstring::npos)
        return value;

    SYSTEMTIME utc{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (swscanf_s(value.c_str(), L"%d-%d-%dT%d:%d:%d",
                  &year, &month, &day, &hour, &minute, &second) != 6)
        return value;
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59)
        return value;
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return value;
    wchar_t formatted[64]{};
    swprintf_s(formatted, L"%02u/%02u/%04u %02u:%02u",
               local.wDay, local.wMonth, local.wYear,
               local.wHour, local.wMinute);
    return formatted;
}

void PopulateCommunicationsGrid(const std::wstring &content)
{
    if (!g_commGrid) return;
    std::vector<std::vector<std::wstring>> rows;
    std::wistringstream stream(content);
    std::wstring line;
    const wchar_t *keys[] = {L"timestamp", L"direction", L"method", L"id_hash",
                             L"duration_ms", L"bytes", L"error"};
    while (std::getline(stream, line))
    {
        if (line.find(L'{') == std::wstring::npos) continue;
        std::vector<std::wstring> row;
        for (const wchar_t *key : keys) row.push_back(MetadataField(line, key));
        row[0] = FormatCommunicationTimestamp(row[0]);
        row.push_back(g_communicationsProfile >= 0 && g_communicationsProfile < static_cast<int>(g_apps.size())
                          ? g_apps[g_communicationsProfile].communicationLog : L"");
        rows.push_back(std::move(row));
    }
    const int column = g_commSortColumn;
    std::stable_sort(rows.begin(), rows.end(), [column](const auto &a, const auto &b)
    {
        const int cmp = a[column].compare(b[column]);
        return g_commSortAscending ? cmp < 0 : cmp > 0;
    });
    SendMessageW(g_commGrid, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_commGrid);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = rows[i][0].data();
        ListView_InsertItem(g_commGrid, &item);
        for (int col = 1; col < 8; ++col)
            ListView_SetItemText(g_commGrid, static_cast<int>(i), col, rows[i][col].data());
    }
    SendMessageW(g_commGrid, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_commGrid, nullptr, FALSE);
}

void InitializeDetailsControls(HWND hwnd)
{
    RecreateUiFonts(hwnd);
    const UINT dpi = CurrentDpi(hwnd);

    g_list = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), g_instance, nullptr);

    SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    ListView_SetBkColor(g_list, RGB(31, 33, 36));
    ListView_SetTextBkColor(g_list, RGB(31, 33, 36));
    ListView_SetTextColor(g_list, RGB(236, 238, 241));
    SetWindowSubclass(g_list, ListViewSubclassProc, 1, 0);

    HWND header = ListView_GetHeader(g_list);
    if (header)
    {
        SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFontBold), TRUE);
        SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
    }

    struct Column
    {
        const wchar_t *name;
        int width;
    };

    const Column columns[] = {{L"MCP application", 165}, {L"Status", 100}, {L"PID(s)", 200},
                              {L"Auto start", 90},       {L"Restart", 90}, {L"Monitor only", 132},
                              {L"Command", 390}};

    for (int i = 0; i < static_cast<int>(std::size(columns)); ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t *>(columns[i].name);
        column.cx = ScaleForDpi(columns[i].width, dpi);
        ListView_InsertColumn(g_list, i, &column);
    }

    const struct
    {
        int id;
        const wchar_t *text;
    } buttons[] = {{IDC_START, L"Start"},       {IDC_STOP, L"Stop"},
                   {IDC_RESTART, L"Restart"},   {IDC_START_ALL, L"Start All"},
                   {IDC_STOP_ALL, L"Stop All"}, {IDC_EDIT_CONFIG, L"Edit Config..."},
                   {IDC_COMMUNICATIONS, L"Communications"}};

    for (const auto &button : buttons)
    {
        HWND control =
            CreateWindowExW(0, L"BUTTON", button.text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), g_instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    }

    HWND startWindows = CreateWindowExW(
        0, L"BUTTON", L"Start MCP Activity with Windows", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_START_WINDOWS)), g_instance, nullptr);
    SendMessageW(startWindows, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    g_runAtStartupEnabled = IsRunAtStartupEnabled();
    SetWindowTheme(startWindows, L"DarkMode_Explorer", nullptr);

    HWND logLabel = CreateWindowExW(0, L"STATIC", L"Activity / errors", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 0, 0, 0, 0,
                                    hwnd, nullptr, g_instance, nullptr);
    SendMessageW(logLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFontBold), TRUE);

    g_logEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0,
        0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), g_instance, nullptr);
    SendMessageW(g_logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SetWindowTheme(g_logEdit, L"DarkMode_Explorer", nullptr);

    g_commGrid = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMM_GRID)), g_instance, nullptr);
    SendMessageW(g_commGrid, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SetWindowTheme(g_commGrid, L"DarkMode_Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_commGrid, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(g_commGrid, RGB(31, 33, 36));
    ListView_SetTextBkColor(g_commGrid, RGB(31, 33, 36));
    ListView_SetTextColor(g_commGrid, RGB(236, 238, 241));
    const wchar_t *names[] = {L"Timestamp", L"Direction", L"Method", L"Request ID", L"Latency ms", L"Bytes", L"Error", L"Log file (double-click)"};
    const int widths[] = {145, 100, 120, 125, 85, 65, 60, 220};
    for (int i = 0; i < 8; ++i)
    {
        LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t *>(names[i]); col.cx = ScaleForDpi(widths[i], dpi);
        ListView_InsertColumn(g_commGrid, i, &col);
    }
    LayoutDetails(hwnd);
    AddLog(L"Details opened.");
    UpdateDetailsList();
}

void LayoutDetails(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);

    const UINT dpi = CurrentDpi(hwnd);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int margin = ScaleForDpi(18, dpi);
    const int listHeight = ScaleForDpi(255, dpi);
    const int buttonY = ScaleForDpi(286, dpi);
    const int buttonHeight = ScaleForDpi(38, dpi);
    const int gap = ScaleForDpi(8, dpi);

    if (g_list)
    {
        MoveWindow(g_list, margin, margin, width - margin * 2, listHeight, TRUE);
    }

    int x = margin;
    const struct
    {
        int id;
        int width;
    } buttons[] = {{IDC_START, 88},      {IDC_STOP, 88},      {IDC_RESTART, 92},
                   {IDC_START_ALL, 100}, {IDC_STOP_ALL, 100}, {IDC_EDIT_CONFIG, 125},
                   {IDC_COMMUNICATIONS, 140}};

    for (const auto &button : buttons)
    {
        HWND control = GetDlgItem(hwnd, button.id);
        const int buttonWidth = ScaleForDpi(button.width, dpi);
        if (control)
        {
            MoveWindow(control, x, buttonY, buttonWidth, buttonHeight, TRUE);
        }
        x += buttonWidth + gap;
    }

    HWND startWindows = GetDlgItem(hwnd, IDC_START_WINDOWS);
    if (startWindows)
    {
        const int checkWidth = ScaleForDpi(275, dpi);
        MoveWindow(startWindows, width - margin - checkWidth, ScaleForDpi(344, dpi), checkWidth,
                   ScaleForDpi(28, dpi), TRUE);
    }

    HWND logLabel = FindWindowExW(hwnd, nullptr, L"STATIC", L"Activity / errors");
    const int labelY = ScaleForDpi(344, dpi);
    if (logLabel)
    {
        MoveWindow(logLabel, margin, labelY, std::max(ScaleForDpi(220, dpi), width - margin * 2 - ScaleForDpi(280, dpi)), ScaleForDpi(28, dpi), TRUE);
    }

    const int logY = ScaleForDpi(374, dpi);
    if (g_commGrid)
    {
        MoveWindow(g_commGrid, margin, logY, width - margin * 2,
                   std::max(ScaleForDpi(130, dpi), height - logY - margin), TRUE);
    }
    if (g_logEdit)
    {
        MoveWindow(g_logEdit, margin, logY, width - margin * 2,
                   std::max(ScaleForDpi(130, dpi), height - logY - margin), TRUE);
    }
}

void PaintMain(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    const UINT dpi = CurrentDpi(hwnd);
    const auto scale = [dpi](int value)
    {
        return ScaleForDpi(value, dpi);
    };

    HBRUSH background = CreateSolidBrush(RGB(36, 38, 42));
    FillRect(dc, &rc, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 237, 240));

    HFONT titleFont = CreateUiFont(dpi, 18, FW_SEMIBOLD);
    HFONT rowFont = CreateUiFont(dpi, 13, FW_NORMAL);
    HFONT statusFont = CreateUiFont(dpi, 10, FW_NORMAL);

    HGDIOBJ old = SelectObject(dc, titleFont);
    TextOutW(dc, scale(20), scale(17), L"MCP Activity", 12);

    SelectObject(dc, rowFont);
    for (size_t i = 0; i < g_apps.size(); ++i)
    {
        const int rowY = scale(68 + 40 * static_cast<int>(i));
        TextOutW(dc, scale(22), rowY - scale(11), g_apps[i].name.c_str(),
                 static_cast<int>(g_apps[i].name.size()));

        DrawLed(dc, scale(355), rowY, g_apps[i].state, dpi);

        SelectObject(dc, statusFont);
        SetTextColor(dc,
                     g_apps[i].state == AppState::Clear ? RGB(155, 160, 166) : StateColor(g_apps[i].state));
        const wchar_t *status = StateText(g_apps[i].state);
        TextOutW(dc, scale(375), rowY - scale(8), status, static_cast<int>(wcslen(status)));
        SetTextColor(dc, RGB(235, 237, 240));
        SelectObject(dc, rowFont);
    }

    HPEN linePen = CreatePen(PS_SOLID, std::max(1, scale(1)), RGB(66, 69, 74));
    HGDIOBJ oldPen = SelectObject(dc, linePen);
    MoveToEx(dc, scale(20), scale(57 + 40 * static_cast<int>(g_apps.size())), nullptr);
    LineTo(dc, rc.right - scale(20), scale(57 + 40 * static_cast<int>(g_apps.size())));
    SelectObject(dc, oldPen);
    DeleteObject(linePen);

    SelectObject(dc, old);
    DeleteObject(titleFont);
    DeleteObject(rowFont);
    DeleteObject(statusFont);

    EndPaint(hwnd, &ps);
}

struct ForegroundWindowSearch
{
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindProcessWindow(HWND hwnd, LPARAM lParam)
{
    auto *search = reinterpret_cast<ForegroundWindowSearch *>(lParam);
    if (!search || !IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId)
    {
        return TRUE;
    }

    if (GetWindow(hwnd, GW_OWNER) != nullptr)
    {
        return TRUE;
    }

    search->window = hwnd;
    return FALSE;
}

void OpenConfigEditor(HWND owner)
{
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = owner;
    execute.lpVerb = L"open";
    execute.lpFile = g_iniPath.c_str();
    execute.lpParameters = nullptr;
    execute.lpDirectory = GetDirectoryName(g_iniPath).c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute))
    {
        AddLog(L"MCP Activity: failed to open configuration editor: " + GetLastErrorMessage(GetLastError()));
        return;
    }

    if (!execute.hProcess)
    {
        return;
    }

    WaitForInputIdle(execute.hProcess, 3000);

    ForegroundWindowSearch search{};
    search.processId = GetProcessId(execute.hProcess);

    for (int attempt = 0; attempt < 30 && !search.window; ++attempt)
    {
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.window)
        {
            Sleep(50);
        }
    }

    if (search.window)
    {
        ShowWindow(search.window, SW_RESTORE);

        SetWindowPos(search.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(search.window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        BringWindowToTop(search.window);
        SetForegroundWindow(search.window);
    }

    CloseHandle(execute.hProcess);
}

LRESULT CALLBACK DetailsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetDarkWindow(hwnd);
        InitializeDetailsControls(hwnd);
        SetTimer(hwnd, kCommunicationsTimer, 1200, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == kCommunicationsTimer && g_communicationsProfile >= 0)
            ShowCommunications(static_cast<size_t>(g_communicationsProfile));
        return 0;

    case WM_SIZE:
        LayoutDetails(hwnd);
        return 0;

    case WM_DPICHANGED:
    {
        RECT *suggested = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateUiFonts(hwnd);
        LayoutDetails(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_DRAWITEM:
        DrawDarkButton(reinterpret_cast<DRAWITEMSTRUCT *>(lParam));
        return TRUE;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(236, 238, 241));
        SetBkColor(dc, RGB(31, 33, 36));
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_detailsBrush);
    }

    case WM_CTLCOLOREDIT:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(236, 238, 241));
        SetBkColor(dc, RGB(24, 26, 29));
        return reinterpret_cast<LRESULT>(g_editBrush);
    }

    case WM_NOTIFY:
    {
        NMHDR *notification = reinterpret_cast<NMHDR *>(lParam);
        if (notification && notification->hwndFrom == g_list && notification->code == LVN_ITEMCHANGED)
        {
            const NMLISTVIEW *change = reinterpret_cast<const NMLISTVIEW *>(lParam);
            if (g_communicationsProfile >= 0 &&
                (change->uChanged & LVIF_STATE) &&
                (change->uNewState & LVIS_SELECTED) &&
                !(change->uOldState & LVIS_SELECTED) &&
                change->iItem >= 0 && change->iItem < static_cast<int>(g_apps.size()))
                ShowCommunications(static_cast<size_t>(change->iItem));
        }
        if (notification && notification->hwndFrom == g_commGrid && notification->code == NM_DBLCLK)
        {
            const int row = reinterpret_cast<NMITEMACTIVATE *>(lParam)->iItem;
            if (row >= 0 && g_communicationsProfile >= 0 &&
                g_communicationsProfile < static_cast<int>(g_apps.size()))
            {
                const std::wstring &path = g_apps[g_communicationsProfile].communicationLog;
                if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        if (notification && notification->hwndFrom == g_commGrid && notification->code == LVN_COLUMNCLICK)
        {
            const int column = reinterpret_cast<NMLISTVIEW *>(lParam)->iSubItem;
            g_commSortAscending = (column == g_commSortColumn) ? !g_commSortAscending : true;
            g_commSortColumn = column;
            PopulateCommunicationsGrid(g_lastCommunicationsText);
            return 0;
        }
        HWND listHeader = g_list ? ListView_GetHeader(g_list) : nullptr;

        if (notification && g_list && notification->hwndFrom == g_list && notification->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW *customDraw = reinterpret_cast<NMLVCUSTOMDRAW *>(lParam);

            if (customDraw->nmcd.dwDrawStage == CDDS_PREPAINT)
            {
                return CDRF_NOTIFYITEMDRAW;
            }

            if (customDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const int item = static_cast<int>(customDraw->nmcd.dwItemSpec);
                const bool selected =
                    (ListView_GetItemState(g_list, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                if (selected)
                {
                    customDraw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                    customDraw->clrText = RGB(255, 255, 255);
                    customDraw->clrTextBk = RGB(49, 78, 107);
                }
                else
                {
                    customDraw->clrText = RGB(236, 238, 241);
                    customDraw->clrTextBk = RGB(31, 33, 36);
                }

                return CDRF_NEWFONT;
            }
        }

        if (notification && listHeader && notification->hwndFrom == listHeader &&
            notification->code == NM_CUSTOMDRAW)
        {
            NMCUSTOMDRAW *customDraw = reinterpret_cast<NMCUSTOMDRAW *>(lParam);

            if (customDraw->dwDrawStage == CDDS_PREPAINT)
            {
                return CDRF_NOTIFYITEMDRAW;
            }

            if (customDraw->dwDrawStage == CDDS_ITEMPREPAINT)
            {
                HDC dc = customDraw->hdc;
                RECT rect = customDraw->rc;
                const int column = static_cast<int>(customDraw->dwItemSpec);
                const UINT dpi = CurrentDpi(hwnd);

                HBRUSH background = CreateSolidBrush(RGB(45, 48, 53));
                FillRect(dc, &rect, background);
                DeleteObject(background);

                HPEN line = CreatePen(PS_SOLID, 1, RGB(84, 89, 96));
                HGDIOBJ oldPen = SelectObject(dc, line);
                MoveToEx(dc, rect.right - 1, rect.top, nullptr);
                LineTo(dc, rect.right - 1, rect.bottom);
                MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
                LineTo(dc, rect.right, rect.bottom - 1);
                SelectObject(dc, oldPen);
                DeleteObject(line);

                wchar_t label[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT | HDI_FORMAT;
                item.pszText = label;
                item.cchTextMax = static_cast<int>(std::size(label));
                Header_GetItem(listHeader, column, &item);

                RECT textRect = rect;
                textRect.left += ScaleForDpi(7, dpi);
                textRect.right -= ScaleForDpi(7, dpi);

                UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                if (item.fmt & HDF_CENTER)
                {
                    format |= DT_CENTER;
                }
                else if (item.fmt & HDF_RIGHT)
                {
                    format |= DT_RIGHT;
                }
                else
                {
                    format |= DT_LEFT;
                }

                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(240, 242, 245));
                HGDIOBJ oldFont = SelectObject(dc, g_uiFontBold);
                DrawTextW(dc, label, -1, &textRect, format);
                SelectObject(dc, oldFont);
                return CDRF_SKIPDEFAULT;
            }
        }

        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int selected = SelectedAppIndex();

        if (id == IDC_START && selected >= 0)
        {
            LaunchApp(static_cast<size_t>(selected));
        }
        else if (id == IDC_STOP && selected >= 0)
        {
            KillAppProcesses(static_cast<size_t>(selected), true);
        }
        else if (id == IDC_RESTART && selected >= 0)
        {
            RestartApp(static_cast<size_t>(selected));
        }
        else if (id == IDC_START_ALL)
        {
            for (size_t i = 0; i < g_apps.size(); ++i)
            {
                LaunchApp(i);
            }
        }
        else if (id == IDC_STOP_ALL)
        {
            for (size_t i = 0; i < g_apps.size(); ++i)
            {
                KillAppProcesses(i, true);
            }
        }
        else if (id == IDC_EDIT_CONFIG)
        {
            OpenConfigEditor(hwnd);
        }
        else if (id == IDC_COMMUNICATIONS && selected >= 0)
        {
            ShowCommunications(static_cast<size_t>(selected));
        }
        else if (id == IDC_START_WINDOWS)
        {
            HWND check = reinterpret_cast<HWND>(lParam);
            g_runAtStartupEnabled = !g_runAtStartupEnabled;
            SetRunAtStartup(g_runAtStartupEnabled);
            InvalidateRect(check, nullptr, TRUE);
        }

        RefreshStatus();
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kCommunicationsTimer);
        g_communicationsProfile = -1;
        g_lastCommunicationsText.clear();
        g_detailsWindow = nullptr;
        g_list = nullptr;
        g_logEdit = nullptr;
        g_commGrid = nullptr;
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_detailsBrush);
        return 1;
    }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void LayoutMain(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT dpi = CurrentDpi(hwnd);
    const int buttonWidth = ScaleForDpi(120, dpi);
    const int buttonHeight = ScaleForDpi(36, dpi);
    const int x = ((rc.right - rc.left) - buttonWidth) / 2;
    const int y = ScaleForDpi(71 + 40 * static_cast<int>(g_apps.size()), dpi);

    HWND details = GetDlgItem(hwnd, IDC_DETAILS);
    if (details)
    {
        MoveWindow(details, x, y, buttonWidth, buttonHeight, TRUE);
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage)
    {
        g_trayAdded = false;
        AddTrayIcon();
        UpdateTrayTooltip();
        return 0;
    }

    switch (message)
    {
    case WM_CREATE:
    {
        g_mainWindow = hwnd;
        SetDarkWindow(hwnd);
        RecreateUiFonts(hwnd);

        HWND details = CreateWindowExW(
            0, L"BUTTON", L"Details...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DETAILS)), g_instance, nullptr);
        SendMessageW(details, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFontBold), TRUE);

        LayoutMain(hwnd);
        AddTrayIcon();

        if (!RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'M'))
        {
            AddLog(L"MCP Activity: Ctrl+Alt+Shift+M hotkey could not be registered.");
        }

        SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
        return 0;
    }

    case WM_SIZE:
        LayoutMain(hwnd);
        return 0;

    case WM_DPICHANGED:
    {
        RECT *suggested = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateUiFonts(hwnd);
        LayoutMain(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_DRAWITEM:
        DrawDarkButton(reinterpret_cast<DRAWITEMSTRUCT *>(lParam));
        return TRUE;

    case WM_TIMER:
        if (wParam == kRefreshTimer)
        {
            RefreshStatus();
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == 1)
        {
            ShowMainWindow();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE)
        {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_DETAILS:
            CreateDetailsWindow();
            break;

        case 2001:
            ShowMainWindow();
            break;

        case 2002:
            for (size_t i = 0; i < g_apps.size(); ++i)
            {
                LaunchApp(i);
            }
            RefreshStatus();
            break;

        case 2003:
            for (size_t i = 0; i < g_apps.size(); ++i)
            {
                KillAppProcesses(i, true);
            }
            RefreshStatus();
            break;

        case 2004:
            g_exitRequested = true;
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case kTrayMessage:
    {
        const UINT event = LOWORD(lParam);

        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK || event == NIN_SELECT ||
            event == NIN_KEYSELECT)
        {
            ShowMainWindow();
        }
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
        {
            ShowTrayMenu();
        }
        return 0;
    }

    case WM_PAINT:
        PaintMain(hwnd);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        UnregisterHotKey(hwnd, 1);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RegisterClasses()
{
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = g_instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = g_windowIcon ? g_windowIcon : LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hIconSm = mainClass.hIcon;
    mainClass.hbrBackground = nullptr;
    mainClass.lpszClassName = kMainClass;
    RegisterClassExW(&mainClass);

    WNDCLASSEXW detailsClass = mainClass;
    detailsClass.lpfnWndProc = DetailsProc;
    detailsClass.hbrBackground = g_detailsBrush;
    detailsClass.lpszClassName = kDetailsClass;
    RegisterClassExW(&detailsClass);
}

bool EnsureSingleInstance()
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MCPActivity_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex)
            CloseHandle(mutex);
        return false;
    }
    return true;
}
int VerifyMonitoring(const wchar_t *output)
{
    LoadConfiguration();
    if (!InitializeWmi())
        return 2;
    const auto processes = QueryProcesses();
    bool passed = !g_apps.empty();
    unsigned monitored = 0;
    for (size_t i = 0; i < g_apps.size(); ++i)
    {
        auto &app = g_apps[i];
        unsigned matches = 0;
        for (const auto &process : processes)
            if (Matches(app, process)) ++matches;
        WritePrivateProfileStringW(app.name.c_str(), L"MatchedProcesses", std::to_wstring(matches).c_str(), output);
        if (!app.monitorOnly)
            continue;
        ++monitored;
        app.pids = {GetCurrentProcessId()};
        passed = passed && !app.autoStart && !app.restartOnFailure && !LaunchApp(i);
        KillAppProcesses(i, true);
        RestartApp(i);
        passed = passed && app.pids == std::vector<DWORD>{GetCurrentProcessId()} && !app.wantedRunning;
        ProcessInfo wrongExecutable{1, L"unrelated.exe", app.match1 + L" " + app.match2};
        if (!app.matchExecutable.empty()) passed = passed && !Matches(app, wrongExecutable);
        WritePrivateProfileStringW(app.name.c_str(), L"MonitorOnlyGuards", passed ? L"passed" : L"failed", output);
    }
    WritePrivateProfileStringW(L"Verification", L"Profiles", std::to_wstring(g_apps.size()).c_str(), output);
    WritePrivateProfileStringW(L"Verification", L"MonitorOnlyProfiles", std::to_wstring(monitored).c_str(), output);
    WritePrivateProfileStringW(L"Verification", L"Passed", passed ? L"1" : L"0", output);
    g_wmi->Release();
    g_wmi = nullptr;
    return passed ? 0 : 3;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_instance = instance;
    int argumentCount = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount == 3 && wcscmp(arguments[1], L"--verify-monitoring") == 0)
    {
        g_exePath = GetModulePath();
        g_iniPath = GetDirectoryName(g_exePath) + L"\\mcp-activity.ini";
        const int result = VerifyMonitoring(arguments[2]);
        LocalFree(arguments);
        return result;
    }
    const bool startInTray = arguments && argumentCount == 2 && wcscmp(arguments[1], L"--tray") == 0;
    if (arguments) LocalFree(arguments);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!EnsureSingleInstance())
    {
        MessageBoxW(nullptr, L"MCP Activity is already running.", L"MCP Activity",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g_detailsBrush = CreateSolidBrush(RGB(31, 33, 36));
    g_editBrush = CreateSolidBrush(RGB(24, 26, 29));

    WSADATA winsock{};
    WSAStartup(MAKEWORD(2, 2), &winsock);

    g_exePath = GetModulePath();
    g_iniPath = GetDirectoryName(g_exePath) + L"\\mcp-activity.ini";
    LoadConfiguration();

    if (!InitializeWmi())
    {
        AddLog(L"WARNING: WMI process monitoring could not be initialized.");
    }
    else
    {
        AddLog(L"MCP Activity started.");
    }

    g_windowIcon = CreateMcpIcon(RGB(45, 112, 170));
    RegisterClasses();

    const UINT startupDpi = GetDpiForSystem();
    const int mainWidth = ScaleForDpi(470, startupDpi);
    const int mainHeight = ScaleForDpi(170 + 40 * static_cast<int>(g_apps.size()), startupDpi);

    g_mainWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kMainClass, L"MCP Activity",
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                   mainWidth, mainHeight, nullptr, nullptr, g_instance, nullptr);

    if (!g_mainWindow)
        return 1;

    RefreshStatus();

    for (size_t i = 0; i < g_apps.size(); ++i)
    {
        if (g_apps[i].pids.empty() && g_apps[i].autoStart)
        {
            g_apps[i].wantedRunning = true;
            LaunchApp(i);
        }
        else if (!g_apps[i].pids.empty())
        {
            g_apps[i].wantedRunning = true;
        }
    }

    if (!startInTray)
        ShowMainWindow();
    UpdateWindow(g_mainWindow);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_wmi)
    {
        g_wmi->Release();
        g_wmi = nullptr;
    }

    if (g_uiFont)
    {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
    }
    if (g_uiFontBold)
    {
        DeleteObject(g_uiFontBold);
        g_uiFontBold = nullptr;
    }
    if (g_detailsBrush)
    {
        DeleteObject(g_detailsBrush);
        g_detailsBrush = nullptr;
    }
    if (g_editBrush)
    {
        DeleteObject(g_editBrush);
        g_editBrush = nullptr;
    }
    if (g_trayIcon)
    {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
    if (g_windowIcon)
    {
        DestroyIcon(g_windowIcon);
        g_windowIcon = nullptr;
    }

    WSACleanup();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
