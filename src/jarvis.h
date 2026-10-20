//======================================================================
//  JARVIS SEARCH  -  shared types and module contracts.
//
//  A JARVIS themed, always-on-top desktop search bar for Windows 11.
//  Typing a query shows the matching installed applications in the left
//  pane and the available web search providers in the right pane.
//  Clicking an entry performs the operation.
//
//  This header is the single source of truth for every module.  Each
//  .cpp file implements the declarations below and nothing else; no
//  module may define a symbol that is not declared here.
//
//  Build (MSYS2 UCRT64):
//    g++ -std=c++17 -O2 -municode -mwindows -o JarvisSearch.exe
//        src/*.cpp -lgdiplus -luser32 -lgdi32 -lole32 -luuid -lshell32
//        -ladvapi32 -static
//======================================================================
#ifndef JARVIS_SEARCH_H_
#define JARVIS_SEARCH_H_

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace js {

//======================================================================
//  1.  Colour
//======================================================================
struct Rgba {
    BYTE a, r, g, b;
};

//======================================================================
//  2.  Configuration
//
//  A flat key=value text file, read once at start-up from config.txt
//  next to the executable.  Unknown keys are ignored so an old file
//  stays valid across upgrades.
//======================================================================
struct Config {
    //  Appearance
    std::wstring font          = L"Cascadia Mono";
    int         fontSize       = 15;    //  logical px, scaled by DPI
    int         labelFontSize  = 10;    //  logical px, scaled by DPI
    std::wstring statusText    = L"SYSTEM ONLINE";

    //  Geometry
    //  The sibling JARVIS HUD greeting card lives in the top left
    //  corner and the desktop icons sit along the top, so the bar is
    //  put just below the icons and to the right of the card.  The
    //  exact place can be given in pos_x / pos_y; when they are left
    //  at -1 the named corner in "position" is used instead.
    std::wstring position      = L"top-right";
    int         margin         = 24;    //  logical px from the work-area edge
    int         posX           = -1;    //  logical px from the work-area left
    int         posY           = -1;    //  logical px from the work-area top
    int         barWidth       = 680;   //  logical px
    int         barHeight      = 48;    //  logical px
    int         maxAppResults  = 9;     //  rows shown in the left pane
    int         maxProviderResults = 12; //  rows shown in the right pane
    int         rowHeight      = 30;    //  logical px per result row

    //  Behaviour
    //  The bar rests on the desktop as a solid card.  This is how
    //  bright its contents are while it is resting there; it is drawn
    //  at full brightness while it is being used.
    int         idleBrightness = 220;
    bool        showClock      = true;
    bool        showTray       = true;
    bool        autostart      = true;   //  register the Run entry on start-up
    bool        loadUwp        = true;   //  also enumerate Store / MSIX apps
    bool        showReactor    = true;   //  arc reactor motif left of the prompt
    std::wstring hotkey        = L"Ctrl+Alt+Space";
    std::wstring defaultBrowser;        //  optional: explicit browser exe
};

//  Reads config.txt.  Returns false only when the file cannot be read at
//  all; a missing file is not an error, the defaults are kept.
bool LoadConfig(const std::wstring& path, Config& out);

//  Writes a single key back to the config file (used by the tray menu to
//  toggle autostart).  Creates the file if it is missing.
bool SaveConfigKey(const std::wstring& path, const std::wstring& key,
                   const std::wstring& value);

//======================================================================
//  3.  Applications
//======================================================================
enum class AppKind {
    Shortcut,   //  a .lnk in the Start Menu
    Executable, //  a bare .exe found on PATH or in a program folder
    Uwp,        //  a Store / MSIX app, addressed by AUMID
};

struct AppEntry {
    std::wstring name;        //  display name, e.g. "Visual Studio Code"
    std::wstring target;      //  .lnk path, .exe path, or empty for UWP
    std::wstring aumid;       //  Application User Model ID, UWP only
    AppKind      kind = AppKind::Shortcut;

    //  Usage statistics, persisted in usage.txt next to the executable.
    int          uses    = 0;
    long long    lastUse = 0; //  unix seconds, 0 = never used
};

//  An installed application the user can launch.
class AppIndex {
public:
    //  Scans the Start Menu folders (per-user and all-users) for .lnk
    //  files, and optionally the shell:AppsFolder namespace for Store
    //  apps.  Safe to call from a worker thread.
    void Scan(bool includeUwp);

    //  Adds one entry manually, used by the tests.
    void Add(const AppEntry& e);

    //  A copy of the list, safe to take while the scan thread is
    //  still filling the index.
    std::vector<AppEntry> Snapshot() const;

    //  The list without copying.  Only safe once the scan is done.
    const std::vector<AppEntry>& All() const { return m_apps; }
    std::size_t Size() const;

    //  Looks up the stored usage counters for one app name.
    int  Uses(const std::wstring& name) const;
    void RecordUse(const std::wstring& name);

    //  Persists the usage counters.  Best effort, never throws.
    void SaveUsage() const;
    void LoadUsage();

private:
    //  The scan runs on a worker thread, so the list is guarded.
    mutable std::mutex      m_mutex;
    std::vector<AppEntry>   m_apps;
};

//  Launches an app.  Shortcuts and executables go through the shell,
//  UWP apps through IApplicationActivationManager.  Returns true when
//  the launch was dispatched.
bool LaunchApp(const AppEntry& app);

//  Extracts the icon of an app.  The caller owns the returned HICON and
//  must DestroyIcon it.  Returns nullptr when no icon can be found.
HICON ExtractAppIcon(const AppEntry& app);

//======================================================================
//  4.  Web search providers
//======================================================================
struct Provider {
    std::wstring key;         //  short handle, e.g. "gg" for Google
    std::wstring label;       //  display label, e.g. "GOOGLE"
    std::wstring hint;        //  one word describing the target
    std::wstring templateUrl; //  URL containing a single %s for the query
};

//  The built-in provider table, in display order.
const std::vector<Provider>& DefaultProviders();

//  Builds the search URL for a provider.  The query is percent encoded
//  so that spaces and non-ASCII characters survive.
std::wstring BuildProviderUrl(const Provider& p, const std::wstring& query);

//  Percent encodes a UTF-8 query into a URL query-string value.
std::wstring UrlEncodeQuery(const std::wstring& query);

//  Opens a URL in the default browser, or in `browserExe` when that is
//  not empty.  Returns true when the shell accepted the request.
bool OpenUrl(const std::wstring& url, const std::wstring& browserExe);

//======================================================================
//  5.  Fuzzy matching
//======================================================================
struct MatchResult {
    bool        matched = false;
    double      score   = 0.0;   //  higher is better
    int         positions[64] = {0};  //  indices into the target that matched
    int         positionCount = 0;
};

//  Scores `query` against `target` (both already normalised to lower
//  case by the caller).  A subsequence match is required; the score
//  rewards, in order: an exact match, a prefix match, a match at a word
//  boundary, consecutive runs, and a match at the start of the string.
//  Non matching input returns matched == false.
MatchResult FuzzyMatch(const std::wstring& query, const std::wstring& target);

//  True when the character at `index` of `target` starts a word, that is
//  it follows a space, a dot, a dash, an underscore or a slash.
bool IsWordBoundary(const std::wstring& target, int index);

//======================================================================
//  6.  Search results
//======================================================================
enum class ResultKind {
    App,
    Web,
};

struct Result {
    ResultKind   kind = ResultKind::App;
    int          index = -1;          //  index into AppIndex or Provider
    std::wstring label;
    std::wstring sublabel;
    double       score = 0.0;
    int          highlight[64] = {0}; //  indices into label that matched
    int          highlightCount = 0;
};

//  Builds the two panes for a query.  `appResults` is the left pane,
//  `webResults` is the right pane.  Both are sorted best first.
void BuildResults(const std::wstring& query, const AppIndex& index,
                  const std::vector<Provider>& providers, int maxApps,
                  int maxProviders, std::vector<Result>& appResults,
                  std::vector<Result>& webResults);

//======================================================================
//  7.  Small utilities, shared by every module
//======================================================================
std::wstring Widen(const std::string& s);
std::string  NarrowUtf8(const std::wstring& w);
std::wstring TrimWs(const std::wstring& s);
std::wstring LowerAscii(const std::wstring& s);
std::wstring UpperAscii(const std::wstring& s);
bool         StartsWithNoCase(const std::wstring& s, const std::wstring& prefix);
bool         EndsWithNoCase(const std::wstring& s, const std::wstring& suffix);
std::vector<std::wstring> SplitLines(const std::string& blob);
std::string  StripUtf8Bom(const std::string& s);
bool         ReadWholeFile(const std::wstring& path, std::string& out);
bool         WriteTextFile(const std::wstring& path, const std::string& utf8);
std::wstring DirNameOf(const std::wstring& path);
std::wstring BaseNameOf(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
int          ParseInt(const std::wstring& s, int fallback);

//  The directory that holds the running executable.
std::wstring ExeDir();

//  The path of a file next to the executable.
std::wstring ExeRelativePath(const wchar_t* name);

//======================================================================
//  8.  Autostart
//======================================================================
//  Reads, writes and removes the per-user Run entry that starts the
//  bar at logon.  All operations are per-user and need no elevation.
bool IsAutostartEnabled();
bool SetAutostartEnabled(bool enabled);

//======================================================================
//  9.  Global hotkey
//======================================================================
//  Parses a modifier string such as "Ctrl+Alt+Space" and registers it
//  with the system.  Returns the modifier/virtual key combination, or
//  0 when the string cannot be parsed.
UINT ParseHotkey(const std::wstring& spec);

}  // namespace js

#endif  //  JARVIS_SEARCH_H_
