//======================================================================
//  JARVIS SEARCH  -  the controller.
//
//  A single borderless, per-pixel transparent window that lives on the
//  desktop.  It is invisible to the taskbar and to Alt+Tab, it never
//  steals the focus when it starts, and it only opens its results panel
//  when it has the focus.
//
//  The left pane lists the applications that are installed on the
//  machine, the right pane lists the web search providers.  Clicking an
//  entry launches the application or opens the search in the browser.
//
//  Build (MSYS2 UCRT64):
//    g++ -std=c++17 -O2 -municode -mwindows -o JarvisSearch.exe 
//        src/*.cpp -lgdiplus -luser32 -lgdi32 -lole32 -luuid -lshell32 
//        -ladvapi32 -static
//======================================================================

#include "jarvis.h"
#include "render.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace js;

//----------------------------------------------------------------------
//  Constants
//----------------------------------------------------------------------
constexpr UINT       kTrayMessage  = WM_APP + 1;
//  Posted by the scan thread when the application list is complete.
constexpr UINT       kScanDoneMsg  = WM_APP + 2;
//  Posted by the icon thread when there is nothing left to fetch.
constexpr UINT       kIconReadyMsg = WM_APP + 3;
constexpr int        kHotkeyId     = 0xB01;
constexpr int        kTimerCaret   = 1;
constexpr int        kTimerClock   = 2;
constexpr int        kTimerBand    = 3;
constexpr int        kTraySearchId = 100;
constexpr int        kTrayAutoId   = 101;
constexpr int        kTrayQuitId   = 102;
constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValueName = L"JarvisSearch";
constexpr const wchar_t* kMutexName = L"Local\\JarvisSearchBar";

struct AppState {
    Config   cfg;
    Renderer renderer;
    AppIndex index;
    std::vector<Provider> providers;
    UiModel  model;

    HWND      hwnd = nullptr;
    HINSTANCE inst = nullptr;
    //  The shell's desktop window.  The bar rests directly on top of
    //  it, which is what keeps it out of the way of every program.
    HWND      desktopHost = nullptr;
    //  The window that had the focus before the bar was summoned, so
    //  that it can be handed back when the bar goes away.
    HWND      prevForeground = nullptr;

    std::wstring query;
    int         caret = 0;

    bool focused = false;
    //  True while the bar is in use: it is on top, it has the focus
    //  and it can be clicked.  When it is false the bar rests on the
    //  desktop.
    bool summoned = false;

    //  Dragging the bar with the left mouse button.  The bar follows
    //  the mouse as soon as it is moved far enough, so a click without
    //  a movement is still a click.
    bool  dragging = false;
    bool  dragMoved = false;
    int   dragStartX = 0, dragStartY = 0;      //  mouse, screen pixels
    int   dragWindowX = 0, dragWindowY = 0;    //  window, screen pixels
    int   dragX = 0, dragY = 0;                //  window, screen pixels

    bool caretOn = true;
    int  activePane = 0;
    int  appSelected = 0;
    int  webSelected = 0;

    std::vector<Result> appResults;
    std::vector<Result> webResults;

    std::unordered_map<std::wstring, HICON> iconCache;

    //  The icons are fetched on a worker thread.
    //
    //  Pulling an icon out of a file costs ten to twenty milliseconds,
    //  and doing that on the thread that draws the bar is what made the
    //  letters appear only when the typing stopped.  The rows ask for
    //  the icon they want, and they are drawn without one until the
    //  worker has it.
    std::mutex                iconMutex;
    std::deque<AppEntry>      iconQueue;    //  asked for by a visible row
    std::deque<AppEntry>      iconWarm;     //  asked for in advance
    std::unordered_set<std::wstring> iconPending;
    std::thread               iconThread;
    std::atomic<bool>         iconStop{false};

    std::thread        scanThread;
    std::atomic<bool>  scanDone{false};

    NOTIFYICONDATAW nid{};
    bool  trayAdded = false;

    std::wstring logPath;
    //  The log file is opened once and kept open: opening and closing
    //  it for every line cost a noticeable share of a keystroke.
    HANDLE      logHandle = INVALID_HANDLE_VALUE;
    std::wstring dumpPath;
    bool  selftest = false;
    bool  listApps = false;
    //  A diagnostic: everything is logged and nothing is really
    //  started, so the test can drive the whole program without
    //  opening anything on the screen of the person at the keyboard.
    bool  dryLaunch = false;
    std::wstring queryArg;
    bool  showPanel = true;
};

AppState* g_app = nullptr;

//  The message that a second copy of the bar uses to ask the first
//  one to show itself.
UINT g_showMessage = 0;

//----------------------------------------------------------------------
//  Small helpers
//----------------------------------------------------------------------
void LogLine(AppState& st, const std::string& line) {
    if (st.logPath.empty()) return;
    if (st.logHandle == INVALID_HANDLE_VALUE) {
        st.logHandle = CreateFileW(st.logPath.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (st.logHandle == INVALID_HANDLE_VALUE) return;
    }
    std::string blob = line + "\r\n";
    DWORD written = 0;
    WriteFile(st.logHandle, blob.data(), (DWORD)blob.size(), &written, nullptr);
    //  The file is kept open, so the end of it has to be pushed out
    //  for anything else that reads the log to see the line.  Without
    //  this the log stays empty until the program closes.
    FlushFileBuffers(st.logHandle);
}


std::wstring SubLabelFor(const AppEntry& app) {
    switch (app.kind) {
        case AppKind::Uwp:        return L"STORE";
        case AppKind::Executable: return L"PROGRAM";
        case AppKind::Shortcut:
        default:                  return L"START MENU";
    }
}

std::wstring ClockText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

//----------------------------------------------------------------------
//  Autostart
//----------------------------------------------------------------------
bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[2048];
    DWORD sz = sizeof(buf), type = 0;
    bool found = false;
    if (RegQueryValueExW(key, kRunValueName, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
        found = (type == REG_SZ || type == REG_EXPAND_SZ);
    RegCloseKey(key);
    return found;
}

bool SetAutostartEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    if (enabled) {
        wchar_t path[32768];
        const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
        if (n == 0) {
            ok = false;
        } else {
            std::wstring cmd = L"\"";
            cmd.append(path, n);
            cmd += L"\"";
            ok = (RegSetValueExW(key, kRunValueName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                                 (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
        }
    } else {
        const LONG rc = RegDeleteValueW(key, kRunValueName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

//----------------------------------------------------------------------
//  Global hotkey
//----------------------------------------------------------------------
UINT ParseHotkey(const std::wstring& spec) {
    if (spec.empty()) return 0;

    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : spec) {
        if (c == L'+') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return 0;

    UINT mods = 0;
    UINT vk = 0;
    for (const std::wstring& raw : parts) {
        const std::wstring p = LowerAscii(raw);
        if (p == L"ctrl" || p == L"control") { mods |= MOD_CONTROL; continue; }
        if (p == L"alt")  { mods |= MOD_ALT; continue; }
        if (p == L"shift") { mods |= MOD_SHIFT; continue; }
        if (p == L"win" || p == L"super" || p == L"cmd") { mods |= MOD_WIN; continue; }

        if (p == L"space") { vk = VK_SPACE; continue; }
        if (p == L"enter" || p == L"return") { vk = VK_RETURN; continue; }
        if (p == L"esc" || p == L"escape") { vk = VK_ESCAPE; continue; }
        if (p == L"tab") { vk = VK_TAB; continue; }
        if (p == L"backspace") { vk = VK_BACK; continue; }
        if (p == L"home") { vk = VK_HOME; continue; }
        if (p == L"end") { vk = VK_END; continue; }
        if (p == L"up") { vk = VK_UP; continue; }
        if (p == L"down") { vk = VK_DOWN; continue; }
        if (p == L"left") { vk = VK_LEFT; continue; }
        if (p == L"right") { vk = VK_RIGHT; continue; }
        if (p.size() == 1) {
            const wchar_t c = p[0];
            if (c >= L'a' && c <= L'z') { vk = (UINT)(c - L'a' + L'A'); continue; }
            if (c >= L'0' && c <= L'9') { vk = (UINT)c; continue; }
            vk = (UINT)c;
            continue;
        }
        if (p.size() >= 2 && p[0] == L'f') {
            const int n = ParseInt(p.substr(1), 0);
            if (n >= 1 && n <= 24) { vk = (UINT)(VK_F1 + n - 1); continue; }
        }
        return 0;   //  an unknown key name makes the whole spec invalid
    }
    if (vk == 0) return 0;
    //  The modifiers and the virtual key are packed into separate
    //  bytes: the modifiers in the high byte, the key in the low one.
    return (mods << 8) | (vk & 0xFF);
}

//----------------------------------------------------------------------
//  Results
//----------------------------------------------------------------------
void RebuildResults(AppState& st) {
    //  The selection keeps its place when the results are rebuilt, so
    //  that the arrow keys are not thrown back to the first row after
    //  every keystroke.
    const int keepApp = st.appSelected;
    const int keepWeb = st.webSelected;

    st.appResults.clear();
    st.webResults.clear();

    if (!st.scanDone.load(std::memory_order_acquire)) {
        //  The scan has not finished yet, so the left pane stays empty
        //  and only the providers are listed.
        BuildResults(st.query, st.index, st.providers, 0, st.cfg.maxProviderResults,
                     st.appResults, st.webResults);
        st.appSelected = 0;
        st.webSelected = 0;
        return;
    }

    BuildResults(st.query, st.index, st.providers, st.cfg.maxAppResults,
                 st.cfg.maxProviderResults, st.appResults, st.webResults);

    if (st.query.empty()) {
        //  With nothing typed the left pane shows the applications that
        //  are used most often, which is a more useful empty state than
        //  an empty box.
        const std::vector<AppEntry> all = st.index.Snapshot();
        std::vector<int> order(all.size());
        for (size_t i = 0; i < all.size(); ++i) order[i] = (int)i;
        std::sort(order.begin(), order.end(), [&all](int x, int y) {
            if (all[(size_t)x].uses != all[(size_t)y].uses)
                return all[(size_t)x].uses > all[(size_t)y].uses;
            return LowerAscii(all[(size_t)x].name) < LowerAscii(all[(size_t)y].name);
        });
        st.appResults.clear();
        int n = 0;
        for (int idx : order) {
            if (n >= st.cfg.maxAppResults) break;
            Result r;
            r.kind = ResultKind::App;
            r.index = idx;
            r.label = all[(size_t)idx].name;
            r.sublabel = SubLabelFor(all[(size_t)idx]);
            r.score = 1000.0 - (double)n;
            st.appResults.push_back(r);
            ++n;
        }
    }

    st.appSelected = keepApp;
    st.webSelected = keepWeb;
    if (st.appSelected >= (int)st.appResults.size())
        st.appSelected = (int)st.appResults.size() - 1;
    if (st.appSelected < 0) st.appSelected = 0;
    if (st.webSelected >= (int)st.webResults.size())
        st.webSelected = (int)st.webResults.size() - 1;
    if (st.webSelected < 0) st.webSelected = 0;
}

//----------------------------------------------------------------------
//  Icons
//
//  An icon is pulled out of a file, and that costs ten to twenty
//  milliseconds.  Doing it while the bar was being drawn held the
//  keyboard back, and the letters only appeared once the typing
//  stopped.  So the icons are fetched by a worker thread and the row
//  is drawn without one until it is ready.
//----------------------------------------------------------------------

//  What an application is known by.  The same key means the same icon.
std::wstring IconKey(const AppEntry& app) {
    if (!app.target.empty()) return app.target;
    if (!app.aumid.empty()) return app.aumid;
    return app.name;
}

//  Asks for the icon of an application, and gives it back if it is
//  already in the cache.  A missing icon is asked for and not waited
//  for.
//
//  The caller holds the lock on the cache, so that a whole pane of rows
//  costs one turn of it and not one per row.
HICON IconFor(AppState& st, const Result& r, const std::vector<AppEntry>& all) {
    if (r.kind != ResultKind::App) return nullptr;
    if (r.index < 0 || r.index >= (int)all.size()) return nullptr;
    const AppEntry& app = all[(size_t)r.index];

    const std::wstring key = IconKey(app);
    auto it = st.iconCache.find(key);
    if (it != st.iconCache.end()) return it->second;
    if (st.iconPending.insert(key).second) st.iconQueue.push_back(app);
    return nullptr;
}

//  Asks for an icon that nobody is waiting for.  The applications that
//  are used most often are fetched as soon as the list is complete, so
//  that the first letter typed already has its icons.
void WarmIcon(AppState& st, const AppEntry& app) {
    const std::wstring key = IconKey(app);
    std::lock_guard<std::mutex> lk(st.iconMutex);
    if (st.iconCache.count(key)) return;
    if (!st.iconPending.insert(key).second) return;
    st.iconWarm.push_back(app);
}

//  The worker.  It takes the icons that a visible row asked for first,
//  and the ones that were warmed in the background afterwards.
void IconWorker(AppState& st) {
    while (!st.iconStop.load(std::memory_order_acquire)) {
        AppEntry app;
        bool urgent = false;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(st.iconMutex);
            if (!st.iconQueue.empty()) {
                app = st.iconQueue.front();
                st.iconQueue.pop_front();
                urgent = true;
                have = true;
            } else if (!st.iconWarm.empty()) {
                app = st.iconWarm.front();
                st.iconWarm.pop_front();
                have = true;
            }
        }
        //  Nothing to do.  The wait is outside the lock: sleeping with
        //  the lock held would hold up every row that asks for an icon.
        if (!have) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        HICON icon = ExtractAppIcon(app);
        const std::wstring key = IconKey(app);
        bool everythingDone = false;
        {
            std::lock_guard<std::mutex> lk(st.iconMutex);
            //  A missing icon is remembered as well, so that it is not
            //  asked for again on every keystroke.
            st.iconCache[key] = icon;
            st.iconPending.erase(key);
            everythingDone = st.iconQueue.empty() && st.iconWarm.empty();
        }

        //  The window is told when there is nothing left to fetch, so
        //  that the rows are drawn once with every icon they can get
        //  instead of once per icon.
        if ((urgent || everythingDone) && st.hwnd)
            PostMessageW(st.hwnd, kIconReadyMsg, 0, 0);
    }
}

void SyncModel(AppState& st) {
    st.model.query = st.query;
    st.model.caret = st.caret;
    st.model.focused = st.focused;
    st.model.panelOpen = st.summoned && st.showPanel;
    st.model.caretOn = st.caretOn;
    st.model.activePane = st.activePane;
    st.model.clock = st.cfg.showClock ? ClockText() : L"";
    //  The status line carries the configured text and then the keys
    //  that are worth remembering.
    st.model.hint = L"ENTER LAUNCH     TAB SWITCH     ESC CLEAR     CTRL+1-9 QUICK";
    if (!st.cfg.statusText.empty())
        st.model.hint = st.cfg.statusText + L"     " + st.model.hint;

    //  A copy of the list, because the scan may still be running on
    //  its own thread while the pane is being drawn.
    const std::vector<AppEntry> all = st.index.Snapshot();

    st.model.apps.rows.clear();
    st.model.apps.title = L"APPLICATIONS";
    {
        std::lock_guard<std::mutex> lk(st.iconMutex);
        for (const Result& r : st.appResults) {
            UiRow row;
            row.label = r.label;
            row.sublabel = r.sublabel;
            row.highlightCount = std::min(r.highlightCount, 64);
            if (row.highlightCount > 0)
                memcpy(row.highlight, r.highlight, sizeof(int) * (size_t)row.highlightCount);
            row.icon = IconFor(st, r, all);
            //  The room for the icon is kept even when the icon has not
            //  arrived yet, so that the text does not jump about while
            //  the icons are coming in.
            row.iconSlot = (r.kind == ResultKind::App);
            st.model.apps.rows.push_back(row);
        }
    }
    st.model.apps.selected = st.appSelected;
    st.model.apps.scroll = 0;

    st.model.web.rows.clear();
    st.model.web.title = L"WEB SEARCH";
    for (const Result& r : st.webResults) {
        UiRow row;
        row.label = r.label;
        row.sublabel = r.sublabel;
        row.highlightCount = std::min(r.highlightCount, 64);
        if (row.highlightCount > 0)
            memcpy(row.highlight, r.highlight, sizeof(int) * (size_t)row.highlightCount);
        st.model.web.rows.push_back(row);
    }
    st.model.web.selected = st.webSelected;
    st.model.web.scroll = 0;
}

//----------------------------------------------------------------------
//  Geometry
//----------------------------------------------------------------------
void ComputePosition(AppState& st, const Layout& lay, int& outX, int& outY) {
    HMONITOR mon = MonitorFromWindow(st.hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;

    const int margin = (int)std::lround((float)st.cfg.margin * st.renderer.Scale());
    const int w = lay.width, h = lay.height;
    const std::wstring pos = LowerAscii(st.cfg.position);

    int x = wa.left + (wa.right - wa.left - w) / 2;
    int y = wa.top + margin;
    if (pos == L"top-left") {
        x = wa.left + margin;  y = wa.top + margin;
    } else if (pos == L"top-right") {
        x = wa.right - margin - w; y = wa.top + margin;
    } else if (pos == L"bottom-left") {
        x = wa.left + margin;  y = wa.bottom - margin - h;
    } else if (pos == L"bottom-right") {
        x = wa.right - margin - w; y = wa.bottom - margin - h;
    } else if (pos == L"bottom-center") {
        x = wa.left + (wa.right - wa.left - w) / 2; y = wa.bottom - margin - h;
    } else if (pos == L"center") {
        x = wa.left + (wa.right - wa.left - w) / 2;
        y = wa.top + (wa.bottom - wa.top - h) / 2;
    }

    //  An exact place wins over the named corner.  The desktop icons
    //  sit along the top of the screen and the greeting card of the
    //  sibling HUD sits in the top left corner, so the bar is put
    //  below the icons and to the right of the card that way.
    const float scale = st.renderer.Scale();
    if (st.cfg.posX >= 0) x = wa.left + (int)std::lround((float)st.cfg.posX * scale);
    if (st.cfg.posY >= 0) y = wa.top  + (int)std::lround((float)st.cfg.posY * scale);

    //  While the bar is being dragged it stays where the mouse has
    //  put it, whatever the config says.
    if (st.dragging) {
        outX = st.dragX;
        outY = st.dragY;
        return;
    }

    outX = x;
    outY = y;
}

//  Remembers where the user has put the bar, so that it comes back in
//  the same place the next time the machine starts.  The place is
//  written into the config file next to the program, as pos_x and
//  pos_y, in the same logical pixels the config uses.
void SaveBarPosition(AppState& st, int screenX, int screenY) {
    if (!st.hwnd) return;
    HMONITOR mon = MonitorFromWindow(st.hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const float scale = st.renderer.Scale();
    const int posX = (int)std::lround((float)(screenX - mi.rcWork.left) / scale);
    const int posY = (int)std::lround((float)(screenY - mi.rcWork.top) / scale);
    st.cfg.posX = posX;
    st.cfg.posY = posY;
    const std::wstring path = ExeRelativePath(L"config.txt");
    SaveConfigKey(path, L"pos_x", std::to_wstring(posX));
    SaveConfigKey(path, L"pos_y", std::to_wstring(posY));
}

void Redraw(AppState& st) {
    if (!st.hwnd) return;
    SyncModel(st);
    const Layout lay = st.renderer.ComputeLayout(st.model);
    int x = 0, y = 0;
    ComputePosition(st, lay, x, y);
    //  The z order is left alone here.  While the bar is in use it is
    //  already on top of everything, and while it rests on the desktop
    //  every repaint would push it back over the open applications.
    //  Moving the foreground window about takes the foreground away
    //  from it again, so it is not touched.
    SetWindowPos(st.hwnd, nullptr, x, y, lay.width, lay.height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    //  The bar that rests on the desktop is a little softer than the
    //  one that is being used.
    const int fade = st.summoned ? 255 : st.cfg.idleBrightness;
    st.renderer.Present(st.hwnd, st.model, lay, fade);
}

//----------------------------------------------------------------------
//  Actions
//----------------------------------------------------------------------
void LaunchAppAt(AppState& st, int index) {
    const std::vector<AppEntry> all = st.index.Snapshot();
    if (index < 0 || index >= (int)all.size()) return;
    const AppEntry& app = all[(size_t)index];
    const bool ok = st.dryLaunch ? true : LaunchApp(app);
    {
        char line[512];
        snprintf(line, sizeof(line),
                 "launch app=%ls kind=%d aumid=%ls target=%ls ok=%d dry=%d",
                 app.name.c_str(), (int)app.kind, app.aumid.c_str(), app.target.c_str(),
                 ok ? 1 : 0, st.dryLaunch ? 1 : 0);
        LogLine(st, line);
    }
    if (ok && !st.dryLaunch) st.index.RecordUse(app.name);
    st.query.clear();
    st.caret = 0;
    RebuildResults(st);
}

void OpenWebAt(AppState& st, int index) {
    if (index < 0 || index >= (int)st.providers.size()) return;
    const std::wstring url = BuildProviderUrl(st.providers[(size_t)index], st.query);
    if (!st.dryLaunch) OpenUrl(url, st.cfg.defaultBrowser);
    {
        char line[512];
        snprintf(line, sizeof(line), "open web provider=%ls url=%ls dry=%d",
                 st.providers[(size_t)index].label.c_str(), url.c_str(),
                 st.dryLaunch ? 1 : 0);
        LogLine(st, line);
    }
    st.query.clear();
    st.caret = 0;
    RebuildResults(st);
}

void ActivateSelection(AppState& st) {
    if (st.activePane == 0) {
        if (st.appSelected >= 0 && st.appSelected < (int)st.appResults.size())
            LaunchAppAt(st, st.appResults[(size_t)st.appSelected].index);
    } else {
        if (st.webSelected >= 0 && st.webSelected < (int)st.webResults.size())
            OpenWebAt(st, st.webResults[(size_t)st.webSelected].index);
    }
}

//----------------------------------------------------------------------
//  The desktop band
//
//  The bar rests on the desktop: above the wallpaper and the icons,
//  below every application.  That is done with the z order.  The
//  window is pushed to the very bottom and the shell's desktop window
//  is then pushed below it, which leaves the bar directly on top of
//  the desktop and under everything else.
//
//  The shell raises its desktop window now and then, so the band is
//  checked once a second and put back when it has been lost.
//----------------------------------------------------------------------
HWND FindDesktopHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) return progman;
    //  A fallback for the shells that hide the icons in a separate
    //  view: the parent of that view is the desktop window.
    HWND defView = FindWindowExW(nullptr, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        HWND parent = GetParent(defView);
        if (parent) return parent;
    }
    return nullptr;
}

void PlaceInDesktopBand(AppState& st) {
    if (!st.hwnd) return;
    if (!st.desktopHost) {
        st.desktopHost = FindDesktopHost();
        if (!st.desktopHost)
            LogLine(st, "no desktop host found, the bar cannot rest on the desktop");
    }
    if (!st.desktopHost) return;

    //  First to the very bottom, then the desktop window goes right
    //  below the bar.
    SetWindowPos(st.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(st.desktopHost, st.hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    LogLine(st, "on the desktop");
}


void RefreshDesktopBand(AppState& st) {
    if (!st.hwnd || st.summoned) return;
    if (!st.desktopHost) { PlaceInDesktopBand(st); return; }

    //  The shell moves its own windows around now and then, and a few
    //  invisible helpers sit in the band as well.  The only thing
    //  that matters is that the desktop stays below the bar: if the
    //  shell puts it on top, the bar is hidden behind the wallpaper
    //  and has to be put back.
    HWND above = st.hwnd;
    for (int i = 0; i < 64 && above; ++i) {
        above = GetWindow(above, GW_HWNDPREV);
        if (!above) break;
        if (above == st.desktopHost) {
            PlaceInDesktopBand(st);
            return;
        }
    }
}

//----------------------------------------------------------------------
//  Summoning and dismissing
//----------------------------------------------------------------------
//  The bar is never click through.  It rests on the desktop, but it
//  has to be possible to take hold of it and drag it somewhere else,
//  and a window that lets the clicks through never hears of the
//  mouse.

//  Brings the bar up: on top of everything, with the focus, ready to
//  be typed into.
void Summon(AppState& st) {
    if (!st.hwnd) return;

    st.summoned = true;
    st.focused = true;
    st.showPanel = true;
    st.activePane = 0;
    st.caret = (int)st.query.size();
    st.caretOn = true;

    //  The bar is clickable whenever it is on the screen: it has to
    //  be possible to take hold of it and drag it somewhere else.


    //  The bar goes into the topmost band before it takes the
    //  foreground.  A window that is the foreground and is moved to
    //  another band afterwards loses the foreground again, and the bar
    //  would be put away again straight away.
    SetWindowPos(st.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    //  The bar is a tool window, so it has to take the foreground
    //  away from whatever the user was working in.  Windows refuses
    //  that when the call comes from a background thread, so the
    //  input queues are attached to the foreground thread for the
    //  duration of the call.
    const HWND foreground = GetForegroundWindow();
    if (foreground && foreground != st.hwnd) st.prevForeground = foreground;
    DWORD fgThread = 0;
    if (foreground) fgThread = GetWindowThreadProcessId(foreground, nullptr);
    const DWORD selfThread = GetCurrentThreadId();
    const bool attached = (fgThread != 0 && fgThread != selfThread) &&
                         AttachThreadInput(selfThread, fgThread, TRUE) != FALSE;

    ShowWindow(st.hwnd, SW_SHOWNA);
    SetForegroundWindow(st.hwnd);
    SetActiveWindow(st.hwnd);
    SetFocus(st.hwnd);
    BringWindowToTop(st.hwnd);

    if (attached) AttachThreadInput(selfThread, fgThread, FALSE);

    //  Letting go of the input queue of the foreground thread can hand
    //  the foreground back to the window that had it, so it is asked
    //  for again.  Without this the bar comes up and is put away again
    //  right away.
    if (st.hwnd == GetForegroundWindow()) {
        SetForegroundWindow(st.hwnd);
        SetFocus(st.hwnd);
    }

    RebuildResults(st);
    Redraw(st);
    SetTimer(st.hwnd, kTimerCaret, 500, nullptr);
    LogLine(st, "summoned");
}

//  Puts the bar back on the desktop: no focus, no panel, no clicks
//  and out of the topmost band.
void Dismiss(AppState& st, const char* reason = "focus") {
    if (!st.hwnd || !st.summoned) return;

    st.summoned = false;
    st.focused = false;
    st.activePane = 0;
    st.showPanel = false;
    KillTimer(st.hwnd, kTimerCaret);

    //  The focus has to go back to the window that had it, otherwise
    //  the keystrokes of the person at the keyboard would still land
    //  in the bar that is no longer there.
    if (GetForegroundWindow() == st.hwnd) {
        HWND back = st.prevForeground;
        if (back && back != st.hwnd && IsWindow(back)) {
            SetForegroundWindow(back);
        } else {
            SetFocus(nullptr);
        }
    }

    //  Out of the topmost band and back onto the desktop.
    SetWindowPos(st.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    PlaceInDesktopBand(st);

    RebuildResults(st);
    Redraw(st);
    {
        char line[160];
        snprintf(line, sizeof(line), "dismissed reason=%s", reason);
        LogLine(st, line);
    }
}

//----------------------------------------------------------------------
//  Hit testing
//----------------------------------------------------------------------
struct Hit {
    enum Kind { None, Bar, App, Web } kind = None;
    int index = -1;
};

Hit HitTest(AppState& st, int px, int py) {
    Hit hit;
    const Layout lay = st.renderer.ComputeLayout(st.model);
    if (lay.panel.h <= 0) {
        if (lay.bar.Contains(px, py)) hit.kind = Hit::Bar;
        return hit;
    }
    if (lay.bar.Contains(px, py)) {
        hit.kind = Hit::Bar;
        return hit;
    }
    for (int i = 0; i < lay.appRowsVisible; ++i) {
        const int idx = lay.appScroll + i;
        if (lay.AppRowRect(idx).Contains(px, py)) {
            hit.kind = Hit::App;
            hit.index = idx;
            return hit;
        }
    }
    for (int i = 0; i < lay.webRowsVisible; ++i) {
        const int idx = lay.webScroll + i;
        if (lay.WebRowRect(idx).Contains(px, py)) {
            hit.kind = Hit::Web;
            hit.index = idx;
            return hit;
        }
    }
    return hit;
}

//----------------------------------------------------------------------
//  Tray icon
//----------------------------------------------------------------------
bool AddTrayIcon(AppState& st) {
    st.nid = {};
    st.nid.cbSize = sizeof(NOTIFYICONDATAW);
    st.nid.hWnd = st.hwnd;
    st.nid.uID = 1;
    st.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st.nid.uCallbackMessage = kTrayMessage;
    wcsncpy(st.nid.szTip, L"JARVIS Search", ARRAYSIZE(st.nid.szTip) - 1);

    HICON icon = nullptr;
    wchar_t path[32768];
    if (GetModuleFileNameW(nullptr, path, 32768) != 0)
        ExtractIconExW(path, 0, nullptr, &icon, 1);
    if (!icon) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    st.nid.hIcon = icon;

    if (Shell_NotifyIconW(NIM_ADD, &st.nid)) {
        st.trayAdded = true;
        return true;
    }
    if (icon) DestroyIcon(icon);
    return false;
}

void RemoveTrayIcon(AppState& st) {
    if (!st.trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &st.nid);
    if (st.nid.hIcon) {
        DestroyIcon(st.nid.hIcon);
        st.nid.hIcon = nullptr;
    }
    st.trayAdded = false;
}

void ShowTrayMenu(AppState& st) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTraySearchId, L"Open search");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_CHECKED,
                kTrayAutoId, L"Start at logon");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayQuitId, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(st.hwnd);
    const UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                          pt.x, pt.y, 0, st.hwnd, nullptr);
    PostMessageW(st.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kTraySearchId) {
        Summon(st);
    } else if (cmd == kTrayAutoId) {
        const bool want = !IsAutostartEnabled();
        if (SetAutostartEnabled(want)) {
            st.cfg.autostart = want;
            SaveConfigKey(ExeRelativePath(L"config.txt"), L"autostart", want ? L"1" : L"0");
        }
    } else if (cmd == kTrayQuitId) {
        DestroyWindow(st.hwnd);
    }
}

//----------------------------------------------------------------------
//  Window procedure
//----------------------------------------------------------------------
//  Every printable character is simply typed.  The keys that are not
//  characters - enter, escape, tab, backspace - are handled in the
//  key down handler, so that a letter can never be a command.
void OnChar(AppState& st, WPARAM wp) {
    const wchar_t ch = (wchar_t)wp;
    if (ch == 13 || ch == 10) return;    //  enter, see OnKeyDown
    {
        char line[128];
        snprintf(line, sizeof(line), "char=%d query=%ls", (int)ch, st.query.c_str());
        LogLine(st, line);
    }
    if (ch == 8) {                                  //  backspace
        if (st.caret > 0) {
            st.query.erase((size_t)st.caret - 1, 1);
            --st.caret;
        }
    } else if (ch >= 32 && ch != 127) {
        st.query.insert((size_t)st.caret, 1, ch);
        ++st.caret;
    } else {
        return;
    }
    st.caretOn = true;
    RebuildResults(st);
    Redraw(st);
}

void OnKeyDown(AppState& st, WPARAM wp) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    //  Ctrl+1 .. Ctrl+9 launch the numbered application directly.
    if (ctrl && wp >= '1' && wp <= '9') {
        const int n = (int)(wp - '1');
        if (n < (int)st.appResults.size()) {
            LaunchAppAt(st, st.appResults[(size_t)n].index);
            Redraw(st);
        }
        return;
    }

    switch (wp) {
        case VK_RETURN:
            ActivateSelection(st);
            Redraw(st);
            return;
        case VK_ESCAPE:
            if (!st.query.empty()) {
                st.query.clear();
                st.caret = 0;
            } else {
                //  Nothing left to clear, so the bar goes back to the
                //  desktop.
                Dismiss(st);
            }
            st.caretOn = true;
            RebuildResults(st);
            Redraw(st);
            return;
        case VK_BACK:
            if (st.caret > 0) {
                st.query.erase((size_t)st.caret - 1, 1);
                --st.caret;
            }
            break;
        case VK_DELETE:
            if (st.caret < (int)st.query.size()) st.query.erase((size_t)st.caret, 1);
            break;
        case VK_LEFT:
            if (st.caret > 0) --st.caret;
            break;
        case VK_RIGHT:
            if (st.caret < (int)st.query.size()) ++st.caret;
            break;
        case VK_HOME:
            st.caret = 0;
            break;
        case VK_END:
            st.caret = (int)st.query.size();
            break;
        case VK_TAB:
            st.activePane = st.activePane ? 0 : 1;
            break;
        case VK_UP:
        case VK_DOWN: {
            const int delta = (wp == VK_DOWN) ? 1 : -1;
            if (st.activePane == 0) {
                const int n = (int)st.appResults.size();
                if (n > 0) st.appSelected = (st.appSelected + delta + n) % n;
            } else {
                const int n = (int)st.webResults.size();
                if (n > 0) st.webSelected = (st.webSelected + delta + n) % n;
            }
            {
                char line[160];
                snprintf(line, sizeof(line), "nav key=%d pane=%d sel=%d apps=%d web=%d",
                         (int)wp, st.activePane,
                         st.activePane == 0 ? st.appSelected : st.webSelected,
                         (int)st.appResults.size(), (int)st.webResults.size());
                LogLine(st, line);
            }
            break;
        }
        case VK_PRIOR:
        case VK_NEXT: {
            const int delta = (wp == VK_NEXT) ? 3 : -3;
            if (st.activePane == 0) {
                const int n = (int)st.appResults.size();
                if (n > 0) st.appSelected = std::max(0, std::min(n - 1, st.appSelected + delta));
            } else {
                const int n = (int)st.webResults.size();
                if (n > 0) st.webSelected = std::max(0, std::min(n - 1, st.webSelected + delta));
            }
            break;
        }
        default:
            return;
    }
    st.caretOn = true;
    RebuildResults(st);
    Redraw(st);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState* st = g_app;

    //  A second copy of the bar asked the first one to come up.
    if (g_showMessage != 0 && msg == g_showMessage) {
        if (st) Summon(*st);
        return 0;
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            st->hwnd = hwnd;
            return 0;

        case WM_ERASEBKGND:
            return 1;   //  the whole surface is painted, there is no background

        case WM_PAINT:
            //  The bar is an ordinary window, so the surface that Present
            //  prepared is put on the screen here.  Without this the window
            //  stays empty and nothing of the bar is ever seen.
            st->renderer.Blit(hwnd);
            return 0;

        case WM_NCHITTEST:
            //  The bar always answers the mouse, so that it can be
            //  taken hold of and dragged somewhere else while it
            //  rests on the desktop.
            return HTCLIENT;

        case WM_MOUSEACTIVATE:
            //  A bar that rests on the desktop must not take the
            //  focus away from the program the person is working in.
            //  It is only brought up when the button is let go
            //  without a drag.
            if (!st->summoned) return MA_NOACTIVATE;
            return DefWindowProcW(hwnd, msg, wp, lp);

        case WM_SETFOCUS:
            st->focused = true;
            st->caretOn = true;
            st->caret = (int)st->query.size();
            RebuildResults(*st);
            Redraw(*st);
            SetTimer(hwnd, kTimerCaret, 500, nullptr);
            return 0;

        case WM_KILLFOCUS:
            //  The query is kept: losing the focus by accident should
            //  not throw away what was typed.  The bar goes back to
            //  the desktop, which is where it belongs when it is not
            //  being used.
            Dismiss(*st);
            return 0;

        case WM_CHAR:
            OnChar(*st, wp);
            return 0;

        case WM_KEYDOWN:
            OnKeyDown(*st, wp);
            return 0;

        case WM_SYSCHAR:
            return 0;   //  the bar is not a menu

        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            {
                char line[256];
                snprintf(line, sizeof(line), "down at=%d,%d focused=%d", x, y, st->focused ? 1 : 0);
                LogLine(*st, line);
            }

            //  The bar can be dragged with the left button.  Where the
            //  mouse went down and where the window is are kept, so
            //  that the window can follow the mouse.
            POINT cur;
            cur.x = cur.y = 0;
            GetCursorPos(&cur);
            RECT wr;
            wr.left = wr.top = wr.right = wr.bottom = 0;
            GetWindowRect(hwnd, &wr);
            st->dragging = true;
            st->dragMoved = false;
            st->dragStartX = cur.x;
            st->dragStartY = cur.y;
            st->dragWindowX = wr.left;
            st->dragWindowY = wr.top;
            st->dragX = wr.left;
            st->dragY = wr.top;
            SetCapture(hwnd);

            //  A bar that is resting on the desktop must not take the
            //  focus away from the program the person is working in.
            //  It is only brought up when the button is let go
            //  without a drag.
            if (!st->summoned) return 0;

            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            const Hit hit = HitTest(*st, x, y);
            if (hit.kind == Hit::Bar) {
                st->caret = (int)st->query.size();
            } else if (hit.kind == Hit::App) {
                st->activePane = 0;
                st->appSelected = hit.index;
            } else if (hit.kind == Hit::Web) {
                st->activePane = 1;
                st->webSelected = hit.index;
            }
            Redraw(*st);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!st->dragging) return 0;
            POINT cur;
            cur.x = cur.y = 0;
            GetCursorPos(&cur);
            const int dx = cur.x - st->dragStartX;
            const int dy = cur.y - st->dragStartY;
            //  A few pixels of jitter are a click, not a drag.
            if (!st->dragMoved && (dx > 4 || dx < -4 || dy > 4 || dy < -4))
                st->dragMoved = true;
            if (!st->dragMoved) return 0;

            RECT wr;
            wr.left = wr.top = wr.right = wr.bottom = 0;
            GetWindowRect(hwnd, &wr);
            const int width = wr.right - wr.left;
            const int height = wr.bottom - wr.top;
            int x = st->dragWindowX + dx;
            int y = st->dragWindowY + dy;

            //  The bar has to stay on the screen of the monitor it is
            //  on.
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                const RECT& work = mi.rcWork;
                if (x < work.left) x = work.left;
                if (y < work.top) y = work.top;
                if (x + width > work.right) x = work.right - width;
                if (y + height > work.bottom) y = work.bottom - height;
            }
            st->dragX = x;
            st->dragY = y;
            SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_CAPTURECHANGED: {
            //  The mouse was taken away, so the drag is over.
            if (st->dragging) {
                st->dragging = false;
                if (st->dragMoved) SaveBarPosition(*st, st->dragX, st->dragY);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (st->dragging) {
                //  The drag is over before the capture is let go:
                //  letting go sends WM_CAPTURECHANGED, which comes
                //  straight back here.
                st->dragging = false;
                ReleaseCapture();
                if (st->dragMoved) {
                    //  The bar was moved: the place it was put in is
                    //  remembered, and the drag is not a click.
                    SaveBarPosition(*st, st->dragX, st->dragY);
                    return 0;
                }
            }
            if (!st->summoned) {
                //  A click on the bar that rests on the desktop brings
                //  it up.
                Summon(*st);
                return 0;
            }
            const Hit hit = HitTest(*st, x, y);
            {
                char line[512];
                std::string label;
                if (hit.kind == Hit::App && hit.index >= 0 &&
                    hit.index < (int)st->appResults.size())
                    label = NarrowUtf8(st->appResults[(size_t)hit.index].label);
                snprintf(line, sizeof(line),
                         "click at=%d,%d kind=%d index=%d apps=%d web=%d query=%ls label=%s",
                         x, y, (int)hit.kind, hit.index,
                         (int)st->appResults.size(), (int)st->webResults.size(),
                         st->query.c_str(), label.c_str());
                LogLine(*st, line);
            }
            if (hit.kind == Hit::App) {
                st->activePane = 0;
                st->appSelected = hit.index;
                if (st->appSelected < (int)st->appResults.size())
                    LaunchAppAt(*st, st->appResults[(size_t)st->appSelected].index);
            } else if (hit.kind == Hit::Web) {
                st->activePane = 1;
                st->webSelected = hit.index;
                if (st->webSelected < (int)st->webResults.size())
                    OpenWebAt(*st, st->webResults[(size_t)st->webSelected].index);
            }
            Redraw(*st);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const Layout lay = st->renderer.ComputeLayout(st->model);
            const bool overWeb = lay.webPane.w > 0 && pt.x >= lay.webPane.x;
            UiPane& pane = overWeb ? st->model.web : st->model.apps;
            const int total = (int)pane.rows.size();
            const int visible = overWeb ? lay.webRowsVisible : lay.appRowsVisible;
            if (total > visible) {
                int scroll = overWeb ? lay.webScroll : lay.appScroll;
                scroll -= delta / WHEEL_DELTA;
                const int maxScroll = std::max(0, total - visible);
                scroll = std::max(0, std::min(maxScroll, scroll));
                if (overWeb) st->webSelected = scroll; else st->appSelected = scroll;
                st->model.web.scroll = overWeb ? scroll : st->model.web.scroll;
                st->model.apps.scroll = overWeb ? st->model.apps.scroll : scroll;
            }
            Redraw(*st);
            return 0;
        }

        case WM_TIMER: {
            if (wp == kTimerCaret) {
                if (st->focused) {
                    st->caretOn = !st->caretOn;
                    Redraw(*st);
                }
            } else if (wp == kTimerClock) {
                const std::wstring now = st->cfg.showClock ? ClockText() : L"";
                if (now != st->model.clock) Redraw(*st);
            } else if (wp == kTimerBand) {
                //  The shell raises its desktop window now and then,
                //  so the band is put back when it has been lost.
                RefreshDesktopBand(*st);
            }
            return 0;
        }


        case WM_HOTKEY:
            if (wp == (WPARAM)kHotkeyId) {
                //  The hotkey is a switch: it brings the bar up, and
                //  it puts it away again when it is already up.
                if (st->summoned) Dismiss(*st, "hotkey");
                else Summon(*st);
            }
            return 0;

        case kScanDoneMsg:
            //  The application list is complete now, so the panes are
            //  built again with everything in them.
            {
                char line[128];
                snprintf(line, sizeof(line), "scan done apps=%d",
                         (int)st->index.Size());
                LogLine(*st, line);
            }
            RebuildResults(*st);
            Redraw(*st);
            return 0;

        case kIconReadyMsg:
            //  The icons that were asked for have arrived, so the rows
            //  are drawn again with them.
            if (st->summoned) Redraw(*st);
            return 0;

        case kTrayMessage: {
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                Summon(*st);
            } else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
                ShowTrayMenu(*st);
            }
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kTraySearchId) Summon(*st);
            else if (id == kTrayAutoId) {
                const bool want = !IsAutostartEnabled();
                if (SetAutostartEnabled(want)) {
                    st->cfg.autostart = want;
                    SaveConfigKey(ExeRelativePath(L"config.txt"), L"autostart", want ? L"1" : L"0");
                }
            } else if (id == kTrayQuitId) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_DESTROY:
            RemoveTrayIcon(*st);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

//----------------------------------------------------------------------
//  Self test
//----------------------------------------------------------------------
int RunSelfTest(AppState& st) {
    const ULONGLONG t0 = GetTickCount64();

    st.providers = DefaultProviders();
    st.model.focused = true;
    st.model.panelOpen = true;
    st.model.query = L"code";
    st.model.caret = 4;
    st.model.caretOn = true;
    st.model.clock = L"09:41";
    st.model.hint = L"ENTER LAUNCH     TAB SWITCH     ESC CLEAR     CTRL+1-9 QUICK";
    st.model.apps.title = L"APPLICATIONS";
    st.model.web.title = L"WEB SEARCH";

    const wchar_t* mockApps[] = {L"Visual Studio Code", L"Google Chrome", L"Windows Terminal",
                                 L"Notepad", L"Code", L"Adobe Photoshop"};
    const wchar_t* mockSubs[] = {L"START MENU", L"START MENU", L"START MENU",
                                 L"START MENU", L"START MENU", L"START MENU"};
    for (int i = 0; i < 6; ++i) {
        UiRow row;
        row.label = mockApps[i];
        row.sublabel = mockSubs[i];
        st.model.apps.rows.push_back(row);
    }
    st.model.apps.selected = 0;

    for (size_t i = 0; i < st.providers.size() && i < 10; ++i) {
        UiRow row;
        row.label = st.providers[i].label;
        row.sublabel = st.providers[i].hint;
        st.model.web.rows.push_back(row);
    }
    st.model.web.selected = 0;

    const Layout lay = st.renderer.ComputeLayout(st.model);
    const std::wstring png = st.dumpPath.empty() ? ExeRelativePath(L"JarvisSearch_frame.png")
                                                 : st.dumpPath;
    const bool ok = st.renderer.DumpPng(st.model, lay, png);

    char line[512];
    snprintf(line, sizeof(line),
             "selftest font=%s scale=%.3f size=%dx%d apps=%d web=%d png=%s ok=%d",
             st.renderer.Scale() > 0 ? "resolved" : "none", (double)st.renderer.Scale(),
             lay.width, lay.height, (int)st.model.apps.rows.size(),
             (int)st.model.web.rows.size(), ok ? "written" : "failed", ok ? 1 : 0);
    LogLine(st, line);
    snprintf(line, sizeof(line), "selftest_ms=%llu", (unsigned long long)(GetTickCount64() - t0));
    LogLine(st, line);
    return 0;
}

//----------------------------------------------------------------------
//  Entry point
//----------------------------------------------------------------------
std::vector<std::wstring> SplitArgs(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < cmd.size()) {
        while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
        if (i >= cmd.size()) break;
        std::wstring tok;
        if (cmd[i] == L'"') {
            ++i;
            while (i < cmd.size() && cmd[i] != L'"') tok.push_back(cmd[i++]);
            if (i < cmd.size()) ++i;
        } else {
            while (i < cmd.size() && cmd[i] != L' ' && cmd[i] != L'\t') tok.push_back(cmd[i++]);
        }
        out.push_back(tok);
    }
    return out;
}

int Run(HINSTANCE instance, const std::wstring& cmdLine) {
    AppState st;
    g_app = &st;
    st.inst = instance;

    const std::vector<std::wstring> args = SplitArgs(cmdLine);
    bool installAuto = false, uninstallAuto = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--selftest") st.selftest = true;
        else if (a == L"--log" && i + 1 < args.size()) st.logPath = args[++i];
        else if (a == L"--dump" && i + 1 < args.size()) st.dumpPath = args[++i];
        else if (a == L"--install-autostart") installAuto = true;
        else if (a == L"--uninstall-autostart") uninstallAuto = true;
        else if (a == L"--list-apps") st.listApps = true;
        else if (a == L"--dry-launch") st.dryLaunch = true;
        else if (a == L"--query" && i + 1 < args.size()) st.queryArg = args[++i];
    }

    if (installAuto) {
        const bool ok = SetAutostartEnabled(true);
        LogLine(st, ok ? "autostart enabled" : "autostart failed");
        return ok ? 0 : 1;
    }
    if (uninstallAuto) {
        const bool ok = SetAutostartEnabled(false);
        LogLine(st, ok ? "autostart disabled" : "autostart failed");
        return ok ? 0 : 1;
    }

    const std::wstring cfgPath = ExeRelativePath(L"config.txt");
    const bool cfgOk = LoadConfig(cfgPath, st.cfg);
    st.providers = DefaultProviders();
    {
        char line[256];
        snprintf(line, sizeof(line), "config ok=%d pos=%ls font=%ls size=%d",
                 cfgOk ? 1 : 0, st.cfg.position.c_str(), st.cfg.font.c_str(),
                 st.cfg.fontSize);
        LogLine(st, line);
    }

    if (st.listApps) {
        //  A diagnostic: write the whole index to a file so that the
        //  names can be checked against what the shell really holds.
        st.index.Scan(st.cfg.loadUwp);
        const std::vector<AppEntry> all = st.index.Snapshot();
        std::string text;
        for (const AppEntry& e : all) {
            text += NarrowUtf8(e.name);
            text += '\t';
            text += (e.kind == AppKind::Uwp ? "uwp" : "lnk");
            text += '\t';
            text += NarrowUtf8(e.target.empty() ? e.aumid : e.target);
            text += '\n';
        }
        const std::wstring out = ExeRelativePath(L"apps.txt");
        WriteTextFile(out, text);
        char line[128];
        snprintf(line, sizeof(line), "list-apps count=%d file=%ls", (int)all.size(), out.c_str());
        LogLine(st, line);
        return 0;
    }

    if (!st.queryArg.empty()) {
        //  A diagnostic: show what a query matches and in what order.
        st.index.Scan(st.cfg.loadUwp);
        st.providers = DefaultProviders();
        std::vector<Result> apps;
        std::vector<Result> web;
        BuildResults(st.queryArg, st.index, st.providers, 50, 50, apps, web);
        std::string text;
        text += "query\t" + NarrowUtf8(st.queryArg) + "\n";
        for (const Result& r : apps) {
            text += "app\t" + NarrowUtf8(r.label) + "\t" + std::to_string(r.index) + "\t" +
                    std::to_string((int)r.score) + "\n";
        }
        for (const Result& r : web) {
            text += "web\t" + NarrowUtf8(r.label) + "\n";
        }
        const std::wstring out = ExeRelativePath(L"query.txt");
        WriteTextFile(out, text);
        return 0;
    }

    if (st.selftest) {
        //  The self test needs a window handle to resolve the scale.
        WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc)); wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"JarvisSearchTest";
        RegisterClassExW(&wc);
        st.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10,
                                  nullptr, nullptr, instance, nullptr);
        if (!st.hwnd) return 1;
        if (!st.renderer.Init(st.cfg, st.hwnd)) return 1;
        return RunSelfTest(st);
    }

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc)); wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"JarvisSearchBar";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return 1;

    g_showMessage = RegisterWindowMessageW(L"JarvisSearchShowBar");

    //  Only one bar may be on the screen.  A second copy asks the
    //  first one to come to the front and then leaves.
    HANDLE single = CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);
    if (alreadyRunning) {
        if (g_showMessage) PostMessageW(HWND_BROADCAST, g_showMessage, 0, 0);
        if (single) CloseHandle(single);
        return 0;
    }

    //  The bar is an ordinary window on purpose.  A layered window is
    //  composited by the desktop window manager in a pass of its own,
    //  after the ordinary windows, so it would float above every
    //  application whatever the z order says.  This one obeys the z
    //  order, which is what lets it rest on the desktop.  It is not
    //  click through, because it has to be possible to drag it.
    DWORD exStyle = WS_EX_TOOLWINDOW;

    st.hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"JARVIS Search", WS_POPUP, 0, 0,
                              (int)st.cfg.barWidth, (int)st.cfg.barHeight, nullptr, nullptr,
                              instance, nullptr);
    if (!st.hwnd) return 1;

    if (!st.renderer.Init(st.cfg, st.hwnd)) return 1;

    //  The global hotkey is a convenience, not a requirement: if it is
    //  taken by another program the bar still works by being clicked.
    const UINT hotkey = ParseHotkey(st.cfg.hotkey);
    if (hotkey) {
        const UINT mods = (hotkey >> 8) & 0xFF;
        const UINT vk = hotkey & 0xFF;
        if (!RegisterHotKey(st.hwnd, kHotkeyId, mods | MOD_NOREPEAT, vk))
            LogLine(st, "hotkey not registered, another program owns it");
    }

    if (st.cfg.showTray) AddTrayIcon(st);

    //  The icons are fetched on their own thread, so that a row never
    //  waits for one while the keyboard does.
    st.iconThread = std::thread([&st]() { IconWorker(st); });

    //  The application list is built on a worker thread so that the bar
    //  is on screen immediately.
    st.scanThread = std::thread([&st]() {
        st.index.Scan(st.cfg.loadUwp);
        st.scanDone.store(true, std::memory_order_release);
        //  The icons of the applications that are used most often are
        //  fetched in advance, so that the first letter typed already
        //  has them.
        {
            const std::vector<AppEntry> all = st.index.Snapshot();
            std::vector<int> order(all.size());
            for (size_t i = 0; i < all.size(); ++i) order[i] = (int)i;
            std::sort(order.begin(), order.end(), [&all](int x, int y) {
                if (all[(size_t)x].uses != all[(size_t)y].uses)
                    return all[(size_t)x].uses > all[(size_t)y].uses;
                return LowerAscii(all[(size_t)x].name) < LowerAscii(all[(size_t)y].name);
            });
            const int n = std::min<int>((int)order.size(), 12);
            for (int i = 0; i < n; ++i) WarmIcon(st, all[(size_t)order[(size_t)i]]);
        }
        //  Tell the window, so that a query typed before the scan
        //  finished is run again against the full list.
        PostMessageW(st.hwnd, kScanDoneMsg, 0, 0);
    });

    st.focused = false;
    st.summoned = false;
    Redraw(st);
    ShowWindow(st.hwnd, SW_SHOWNOACTIVATE);
    //  The bar starts where it belongs: on the desktop, under every
    //  application window.
    PlaceInDesktopBand(st);
    {
        char line[256];
        RECT r{};
        GetWindowRect(st.hwnd, &r);
        snprintf(line, sizeof(line), "ready rect=%ld,%ld,%ld,%ld scale=%.2f hotkey=%ls",
                 (long)r.left, (long)r.top, (long)r.right, (long)r.bottom,
                 (double)st.renderer.Scale(), st.cfg.hotkey.c_str());
        LogLine(st, line);
    }
    SetTimer(st.hwnd, kTimerClock, 1000, nullptr);
    SetTimer(st.hwnd, kTimerBand, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.scanThread.joinable()) st.scanThread.join();
    st.iconStop.store(true, std::memory_order_release);
    if (st.iconThread.joinable()) st.iconThread.join();
    if (st.logHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(st.logHandle);
        st.logHandle = INVALID_HANDLE_VALUE;
    }
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    UnregisterHotKey(st.hwnd, kHotkeyId);
    g_app = nullptr;
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR lpCmdLine, int) {
    //  DPI awareness has to be established before any window or GDI+
    //  work, otherwise a layered window is scaled and misplaced by the
    //  compositor.  SetProcessDpiAwarenessContext lives in user32 on
    //  Windows 10 1703+, so it is resolved dynamically and the binary
    //  still runs on older systems.
    {
        typedef BOOL(WINAPI* SetCtxFn)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            union {
                FARPROC p;
                SetCtxFn f;
            } proc;
            proc.p = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            const SetCtxFn setCtx = proc.f;
            if (setCtx) {
                if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) goto dpiReady;
                if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) goto dpiReady;
            }
        }
        SetProcessDPIAware();
    }
dpiReady:
    const int rc = Run(instance, lpCmdLine ? lpCmdLine : L"");
    return rc;
}
