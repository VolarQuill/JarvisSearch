//======================================================================
//  JARVIS SEARCH  -  application index, launcher and icon extraction.
//
//  This module owns the installed-application half of the search bar:
//  the Start Menu walk, the Store / MSIX enumeration, the usage
//  statistics, the launcher and the icon lookup.
//
//  Notes
//  -----
//   * The Start Menu walk is plain Win32 (FindFirstFileW).  It needs no
//     COM and therefore runs on a worker thread without any ceremony.
//   * The Store enumeration is shell COM code, so it initialises COM
//     itself and releases it again.  A failure there never aborts the
//     scan - the Start Menu results are returned either way.
//   * The usage counters live in their own small database (usage.txt
//     next to the executable) so that a counter survives a rescan and
//     so that the counters of an app that was uninstalled are not lost
//     right away.
//   * Every failure path is silent - an empty list, a null icon or a
//     false result, never a dialog and never an exception.
//======================================================================

#include "jarvis.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace js {
namespace {

//======================================================================
//  1.  Constants
//======================================================================

//  The activation manager GUIDs are spelled out here so that the
//  launch path does not depend on an import library.  The values are
//  the documented ones and match what libuuid.a exports.
const GUID kClsidAppActivationManager = {
    0x45ba127d, 0x10a8, 0x46ea, {0x8a, 0xb7, 0x56, 0xea, 0x90, 0x78, 0x94, 0x3c}};
const GUID kIidIApplicationActivationManager = {
    0x2e941141, 0x7f97, 0x4756, {0xba, 0x1d, 0x9d, 0xec, 0xde, 0x89, 0x4a, 0x3d}};

//  The modern SHCONTF_* flags live in shobjidl_core.h in the Windows
//  SDK.  mingw only carries the legacy shlobj.h set whose values differ
//  (SHCONTF_FOLDERS is 0x20 there, not 0x01) and the shell only
//  understands the modern ones, so they are spelled out here.
constexpr SHCONTF kShconfFolders       = 0x0001;
constexpr SHCONTF kShconfItems        = 0x0040;
constexpr SHCONTF kShconfShell        = 0x0008;
constexpr SHCONTF kShconfNonBrowsable = 0x0004;
constexpr SHCONTF kShconfNaviSortBox  = 0x0010;

//  There is no SIGDN_PARSINGNAME in the SDK.  SIGDN_DESKTOPABSOLUTEPARSING
//  is the property that yields the full parsing name of a shell item,
//  which for a Store app reads "shell:AppsFolder\<family>!<app>".
constexpr SIGDN kSigdnParsingName = SIGDN_DESKTOPABSOLUTEPARSING;

//  The behaviour handler that yields the children of a folder as
//  shell items.  shobjidl.h in the SDK declares it as BHID_EnumItems;
//  mingw only carries it in shlguid.h, so the value is spelled out.
const GUID kBhidEnumItems = {
    0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};

//  How deep the Start Menu walk is allowed to go.  The real tree is
//  three levels deep, the limit only guards against a pathological
//  folder tree.
constexpr int kMaxProgramFolderDepth = 8;

//======================================================================
//  2.  Small helpers
//======================================================================

//  Case folding for the usage keys.  LCMapStringEx with the invariant
//  locale covers the whole Unicode range, unlike the ASCII-only
//  LowerAscii of the shared utilities.
std::wstring FoldKey(const std::wstring& s) {
    if (s.empty()) return s;

    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(),
                                 (int)s.size(), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return LowerAscii(s);

    std::wstring out;
    out.resize((size_t)n);
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), (int)s.size(),
                      &out[0], n, nullptr, nullptr, 0) != n)
        return LowerAscii(s);
    return out;
}

//  Case insensitive comparison of two strings, used for the sorting and
//  for the duplicate check.
int CompareNoCase(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE);
    if (r == CSTR_LESS_THAN)    return -1;
    if (r == CSTR_GREATER_THAN) return 1;
    if (r == CSTR_EQUAL)        return 0;
    return a < b ? -1 : (a == b ? 0 : 1);
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareNoCase(a, b) == 0;
}

//  Removes a trailing extension when it matches, case insensitively.
std::wstring StripExtension(const std::wstring& fileName, const wchar_t* ext) {
    const size_t n = wcslen(ext);
    if (fileName.size() <= n) return fileName;
    if (CompareStringOrdinal(fileName.c_str() + fileName.size() - n, (int)n, ext, (int)n, TRUE) !=
        CSTR_EQUAL)
        return fileName;
    return fileName.substr(0, fileName.size() - n);
}

//  The Start Menu names a shortcut "<app> - Shortcut" when the program
//  was not installed by an installer.  The suffix is noise in a search
//  result and is removed.
std::wstring CleanDisplayName(std::wstring name) {
    name = TrimWs(name);

    static const wchar_t kShortcutSuffix[] = L" - Shortcut";
    const size_t n = sizeof(kShortcutSuffix) / sizeof(kShortcutSuffix[0]) - 1;
    if (name.size() > n &&
        CompareStringOrdinal(name.c_str() + name.size() - n, (int)n, kShortcutSuffix, (int)n, TRUE) ==
            CSTR_EQUAL)
        name = TrimWs(name.substr(0, name.size() - n));

    return name;
}

//  Current time in unix seconds.
long long UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    const unsigned long long ticks =
        ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
    //  the file time counts 100 ns intervals since 1601-01-01
    return (long long)(ticks / 10000000ULL) - 11644473600LL;
}

//  Strict integer parse.  A malformed field makes the whole usage line
//  unusable, so the caller can drop the line.
bool ParseNumber(const std::wstring& s, long long& out) {
    if (s.empty()) return false;

    size_t i = 0;
    bool negative = false;
    if (s[0] == L'-' || s[0] == L'+') {
        negative = (s[0] == L'-');
        i = 1;
    }
    if (i >= s.size()) return false;

    long long value = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        value = value * 10 + (s[i] - L'0');
        if (value > 1000000000000LL) value = 1000000000000LL;  //  clamp
    }

    out = negative ? -value : value;
    return true;
}

//  COM guard.  The scan may run on a worker thread that knows nothing
//  about apartments, so the apartment is set up here and taken down
//  again when the scope ends.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_owned  = (hr == S_OK || hr == S_FALSE);
        m_usable = m_owned || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (m_owned) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool Usable() const { return m_usable; }

private:
    bool m_owned  = false;
    bool m_usable = false;
};

//======================================================================
//  3.  The usage database
//
//  The counters are kept outside of the index so that they survive a
//  rescan and so that the counters of an app that is no longer
//  installed are not thrown away.  The lock keeps the worker thread
//  that scans apart from the thread that records a use.
//======================================================================
struct UsageRecord {
    std::wstring name;      //  the original spelling, the key is folded
    int          uses    = 0;
    long long    lastUse = 0;   //  unix seconds
};

SRWLOCK g_usageLock = SRWLOCK_INIT;
std::map<std::wstring, UsageRecord> g_usage;   //  keyed by FoldKey(name)

//  Copies the stored counters onto one entry.  The entry keeps the
//  larger of the two values, so a manually prepared entry is never
//  overwritten by an empty database.
void ApplyStoredUsageTo(AppEntry& entry) {
    if (entry.name.empty()) return;

    AcquireSRWLockShared(&g_usageLock);
    const auto it = g_usage.find(FoldKey(entry.name));
    if (it != g_usage.end()) {
        if (it->second.uses > entry.uses)         entry.uses = it->second.uses;
        if (it->second.lastUse > entry.lastUse)   entry.lastUse = it->second.lastUse;
    }
    ReleaseSRWLockShared(&g_usageLock);
}

void ApplyStoredUsage(std::vector<AppEntry>& apps) {
    for (AppEntry& entry : apps) ApplyStoredUsageTo(entry);
}

//======================================================================
//  4.  The Start Menu
//======================================================================

//  Resolves one of the Start Menu program folders.  The known folder
//  API is the modern one, the CSIDL call is the fallback for the
//  unlikely case that it is unavailable.
bool ResolveProgramFolder(const GUID& folderId, int csidl, std::wstring& out) {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, 0, nullptr, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
        if (!out.empty()) return true;
    }

    wchar_t buffer[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buffer)) &&
        buffer[0]) {
        out = buffer;
        return true;
    }

    out.clear();
    return false;
}

//  Walks one program folder.  Shortcuts are picked up on every level,
//  bare executables only in the folder itself.
void CollectProgramFolder(const std::wstring& root, int depth, std::vector<AppEntry>& out) {
    if (root.empty() || depth > kMaxProgramFolderDepth) return;

    WIN32_FIND_DATAW fd;
    const std::wstring pattern = root + L"\\*";

    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring fileName = fd.cFileName;
        if (fileName == L"." || fileName == L"..") continue;

        const bool isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory) {
            //  a junction could point back into the tree, so links are
            //  not followed
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            CollectProgramFolder(root + L"\\" + fileName, depth + 1, out);
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        AppEntry entry;
        if (EndsWithNoCase(fileName, L".lnk")) {
            entry.kind   = AppKind::Shortcut;
            entry.name   = CleanDisplayName(StripExtension(fileName, L".lnk"));
            entry.target = root + L"\\" + fileName;
        } else if (depth == 0 && EndsWithNoCase(fileName, L".exe")) {
            entry.kind   = AppKind::Executable;
            entry.name   = CleanDisplayName(StripExtension(fileName, L".exe"));
            entry.target = root + L"\\" + fileName;
        } else {
            continue;
        }

        if (entry.name.empty()) continue;
        out.push_back(entry);
    } while (FindNextFileW(find, &fd));

    FindClose(find);
}

//======================================================================
//  5.  The Store / MSIX applications
//======================================================================

//  Turns the parsing name of a shell item into an AUMID.  Only a name
//  that carries the "!" separator of an Application User Model ID is
//  accepted - that is what tells an app apart from an ordinary folder
//  such as an app group.
std::wstring AumidFromParsingName(const std::wstring& parsing) {
    std::wstring aumid = parsing;

    static const wchar_t kPrefix[] = L"shell:AppsFolder\\";
    const size_t prefixLen = sizeof(kPrefix) / sizeof(kPrefix[0]) - 1;
    if (aumid.size() > prefixLen &&
        CompareStringOrdinal(aumid.c_str(), (int)prefixLen, kPrefix, (int)prefixLen, TRUE) ==
            CSTR_EQUAL)
        aumid = aumid.substr(prefixLen);

    //  an AUMID is "<package family>!<application>", nothing else
    if (aumid.find(L'!') == std::wstring::npos) return std::wstring();
    if (aumid.find(L'\\') != std::wstring::npos) return std::wstring();
    if (aumid.find(L'/') != std::wstring::npos)  return std::wstring();

    return aumid;
}

//  Turns one child of the AppsFolder into an app entry.  The AUMID comes
//  from the parsing name of the item, the display name from its normal
//  display name.
bool BuildStoreEntry(IShellItem* item, AppEntry& entry) {
    if (!item) return false;

    IShellItem2* item2 = nullptr;
    if (FAILED(item->QueryInterface(IID_IShellItem2, reinterpret_cast<void**>(&item2))) || !item2)
        return false;

    std::wstring displayName;
    std::wstring aumid;

    LPWSTR text = nullptr;
    if (SUCCEEDED(item2->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) && text) {
        displayName = text;
        CoTaskMemFree(text);
        text = nullptr;
    }

    if (SUCCEEDED(item2->GetDisplayName(kSigdnParsingName, &text)) && text) {
        aumid = AumidFromParsingName(text);
        CoTaskMemFree(text);
        text = nullptr;
    }
    //  the absolute parsing name of an app that sits in an app group
    //  folder still carries the group, the parent relative one is the
    //  plain AUMID
    if (aumid.empty() &&
        SUCCEEDED(item2->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &text)) && text) {
        aumid = AumidFromParsingName(text);
        CoTaskMemFree(text);
    }

    item2->Release();

    //  no AUMID, no Store app - the All Apps folder holds the plain
    //  desktop programs as well
    if (aumid.empty()) return false;

    entry.kind   = AppKind::Uwp;
    entry.target.clear();
    entry.aumid  = aumid;
    entry.name   = CleanDisplayName(displayName);
    if (entry.name.empty()) entry.name = aumid;
    return true;
}

//  Asks the shell for the items of the All Apps folder.  This is the
//  modern way: the behaviour handler hands out an IEnumShellItems, so
//  the children arrive as shell items and can be asked for their
//  display name and their parsing name.
bool EnumerateAllApps(std::vector<AppEntry>& out) {
    IShellItem* folder = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&folder))) || !folder)
        return false;

    IEnumShellItems* enumerator = nullptr;
    const HRESULT hr = folder->BindToHandler(nullptr, kBhidEnumItems, IID_IEnumShellItems,
                                              reinterpret_cast<void**>(&enumerator));
    folder->Release();
    if (FAILED(hr) || !enumerator) return false;

    for (;;) {
        IShellItem* item = nullptr;
        ULONG fetched = 0;
        if (FAILED(enumerator->Next(1, &item, &fetched)) || fetched == 0 || !item) break;

        AppEntry entry;
        if (BuildStoreEntry(item, entry)) out.push_back(entry);
        item->Release();
    }

    enumerator->Release();
    return true;
}

//  The older route through the shell folder of the namespace.  It is
//  only used when the behaviour handler is not available; on Windows 11
//  the parent of the AppsFolder is the desktop, so nothing useful comes
//  out of it - but on an older shell it is the folder that holds the
//  Store applications.
void EnumerateStoreAppsLegacy(std::vector<AppEntry>& out) {
    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &folderPidl, 0, nullptr)) ||
        !folderPidl)
        return;

    IShellFolder* folder = nullptr;
    if (FAILED(SHBindToParent(folderPidl, IID_IShellFolder, reinterpret_cast<void**>(&folder), nullptr)) ||
        !folder) {
        ILFree(folderPidl);
        return;
    }

    const SHCONTF flagSets[] = {
        kShconfFolders | kShconfNonBrowsable,
        kShconfItems | kShconfNonBrowsable,
        kShconfFolders | kShconfNonBrowsable | kShconfItems,
        kShconfNaviSortBox,
    };

    for (const SHCONTF flags : flagSets) {
        IEnumIDList* enumerator = nullptr;
        if (FAILED(folder->EnumObjects(nullptr, flags, &enumerator)) || !enumerator) continue;

        size_t found = 0;
        for (;;) {
            PITEMID_CHILD child = nullptr;
            ULONG fetched = 0;
            if (FAILED(enumerator->Next(1, &child, &fetched)) || fetched == 0 || !child) break;

            //  the child is relative to the folder, it only becomes a
            //  shell item together with the folder
            PIDLIST_ABSOLUTE absolute = ILCombine(folderPidl, child);
            if (!absolute) continue;

            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromIDList(absolute, IID_IShellItem,
                                                 reinterpret_cast<void**>(&item))) &&
                item) {
                AppEntry entry;
                if (BuildStoreEntry(item, entry)) {
                    out.push_back(entry);
                    ++found;
                }
                item->Release();
            }
            ILFree(absolute);
        }

        enumerator->Release();
        if (found > 0) break;
    }

    folder->Release();
    ILFree(folderPidl);
}

//  Enumerates the Store / MSIX applications.
void EnumerateStoreApps(std::vector<AppEntry>& out) {
    //  COM is initialised here because the scan may run on a worker
    //  thread that knows nothing about apartments
    ComScope com;
    if (!com.Usable()) return;

    const size_t before = out.size();
    if (EnumerateAllApps(out) && out.size() > before) return;

    EnumerateStoreAppsLegacy(out);
}

//======================================================================
//  6.  AppIndex
//======================================================================

}  // namespace

void AppIndex::Scan(bool includeUwp) {
    std::vector<AppEntry> found;

    std::wstring folder;
    if (ResolveProgramFolder(FOLDERID_Programs, CSIDL_PROGRAMS, folder))
        CollectProgramFolder(folder, 0, found);
    if (ResolveProgramFolder(FOLDERID_CommonPrograms, CSIDL_COMMON_PROGRAMS, folder))
        CollectProgramFolder(folder, 0, found);

    if (includeUwp) EnumerateStoreApps(found);

    //  sort by name, case insensitively
    const auto byName = [](const AppEntry& a, const AppEntry& b) {
        const int c = CompareNoCase(a.name, b.name);
        if (c != 0) return c < 0;
        return CompareNoCase(a.target, b.target) < 0;
    };
    std::sort(found.begin(), found.end(), byName);

    //  drop the duplicates: the same name and the same target
    std::vector<AppEntry> unique;
    unique.reserve(found.size());
    for (const AppEntry& e : found) {
        if (!unique.empty()) {
            const AppEntry& previous = unique.back();
            if (FoldKey(previous.name) == FoldKey(e.name) &&
                EqualsNoCase(previous.target, e.target))
                continue;
        }
        unique.push_back(e);
    }

    //  merge into the index; an entry that is already there is kept, so
    //  a second scan only adds what is new
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const AppEntry& e : unique) {
        bool known = false;
        for (const AppEntry& existing : m_apps) {
            if (FoldKey(existing.name) == FoldKey(e.name) &&
                EqualsNoCase(existing.target, e.target)) {
                known = true;
                break;
            }
        }
        if (known) continue;
        m_apps.push_back(e);
    }

    std::sort(m_apps.begin(), m_apps.end(), byName);
    ApplyStoredUsage(m_apps);
}

void AppIndex::Add(const AppEntry& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_apps.push_back(e);
    ApplyStoredUsageTo(m_apps.back());
}

std::vector<AppEntry> AppIndex::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_apps;
}

std::size_t AppIndex::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_apps.size();
}

int AppIndex::Uses(const std::wstring& name) const {
    if (name.empty()) return 0;

    const std::wstring key = FoldKey(name);

    AcquireSRWLockShared(&g_usageLock);
    int result = 0;
    const auto it = g_usage.find(key);
    if (it != g_usage.end()) result = it->second.uses;
    for (const AppEntry& e : m_apps) {
        if (FoldKey(e.name) == key && e.uses > result) result = e.uses;
    }
    ReleaseSRWLockShared(&g_usageLock);

    return result;
}

void AppIndex::RecordUse(const std::wstring& name) {
    if (name.empty()) return;

    const std::wstring key = FoldKey(name);
    const long long now = UnixNow();

    AcquireSRWLockExclusive(&g_usageLock);

    int  uses = 0;
    bool known = false;
    for (AppEntry& e : m_apps) {
        if (FoldKey(e.name) == key) {
            e.uses += 1;
            e.lastUse = now;
            uses = e.uses;
            known = true;
            break;
        }
    }

    UsageRecord& record = g_usage[key];
    if (record.name.empty()) record.name = name;
    record.uses    = known ? uses : record.uses + 1;
    record.lastUse = now;

    ReleaseSRWLockExclusive(&g_usageLock);
}

void AppIndex::SaveUsage() const {
    const std::wstring path = ExeRelativePath(L"usage.txt");
    if (path.empty()) return;

    AcquireSRWLockExclusive(&g_usageLock);
    //  the app list carries the freshest counters, the store keeps the
    //  records of the apps that are no longer installed
    std::map<std::wstring, UsageRecord> merged = g_usage;
    for (const AppEntry& e : m_apps) {
        const std::wstring key = FoldKey(e.name);
        const auto it = merged.find(key);
        if (it == merged.end()) {
            UsageRecord record;
            record.name    = e.name;
            record.uses    = e.uses;
            record.lastUse = e.lastUse;
            merged[key] = record;
        } else {
            if (e.uses > it->second.uses)       it->second.uses = e.uses;
            if (e.lastUse > it->second.lastUse) it->second.lastUse = e.lastUse;
            if (it->second.name.empty())        it->second.name = e.name;
        }
    }
    ReleaseSRWLockExclusive(&g_usageLock);

    std::string text;
    for (const auto& pair : merged) {
        const UsageRecord& record = pair.second;
        if (record.name.empty()) continue;
        text += NarrowUtf8(record.name);
        text += '\t';
        text += std::to_string(record.uses);
        text += '\t';
        text += std::to_string(record.lastUse);
        text += '\n';
    }

    //  the database is written to a side file first and swapped in
    //  afterwards, so that a crash in the middle of the write cannot
    //  destroy it
    const std::wstring temp = path + L".tmp";
    if (!WriteTextFile(temp, text)) return;
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void AppIndex::LoadUsage() {
    const std::wstring path = ExeRelativePath(L"usage.txt");
    if (path.empty()) return;

    std::string blob;
    if (!ReadWholeFile(path, blob)) return;   //  a missing file is not an error
    blob = StripUtf8Bom(blob);

    //  one line per app: name <TAB> uses <TAB> lastUse
    std::map<std::wstring, UsageRecord> loaded;
    for (const std::wstring& line : SplitLines(blob)) {
        const size_t firstTab = line.find(L'\t');
        if (firstTab == std::wstring::npos) continue;
        const size_t secondTab = line.find(L'\t', firstTab + 1);
        if (secondTab == std::wstring::npos) continue;

        const std::wstring name = TrimWs(line.substr(0, firstTab));
        if (name.empty()) continue;

        long long uses    = 0;
        long long lastUse = 0;
        if (!ParseNumber(TrimWs(line.substr(firstTab + 1, secondTab - firstTab - 1)), uses))
            continue;
        if (!ParseNumber(TrimWs(line.substr(secondTab + 1)), lastUse)) continue;
        if (uses < 0)    uses = 0;
        if (lastUse < 0) lastUse = 0;

        const std::wstring key = FoldKey(name);
        const auto it = loaded.find(key);
        if (it == loaded.end()) {
            UsageRecord record;
            record.name    = name;
            record.uses    = (int)uses;
            record.lastUse = lastUse;
            loaded[key] = record;
        } else {
            if (uses > it->second.uses)       it->second.uses = (int)uses;
            if (lastUse > it->second.lastUse) it->second.lastUse = lastUse;
        }
    }

    //  the counters are merged, never lowered, so that a use that was
    //  recorded in this session is not lost by a reload
    AcquireSRWLockExclusive(&g_usageLock);
    for (const auto& pair : loaded) {
        const auto it = g_usage.find(pair.first);
        if (it == g_usage.end()) {
            g_usage[pair.first] = pair.second;
        } else {
            if (pair.second.uses > it->second.uses)       it->second.uses = pair.second.uses;
            if (pair.second.lastUse > it->second.lastUse) it->second.lastUse = pair.second.lastUse;
        }
    }
    ReleaseSRWLockExclusive(&g_usageLock);

    ApplyStoredUsage(m_apps);
}

//======================================================================
//  7.  Launching
//======================================================================

bool LaunchApp(const AppEntry& app) {
    if (app.kind == AppKind::Uwp) {
        if (app.aumid.empty()) return false;

        HRESULT hr = E_FAIL;

        {
            ComScope com;
            if (com.Usable()) {
                IApplicationActivationManager* manager = nullptr;
                if (SUCCEEDED(CoCreateInstance(kClsidAppActivationManager, nullptr,
                                               CLSCTX_INPROC_SERVER,
                                               kIidIApplicationActivationManager,
                                               reinterpret_cast<void**>(&manager))) &&
                    manager) {
                    hr = manager->ActivateApplication(app.aumid.c_str(), L"", AO_NONE, nullptr);
                    manager->Release();
                }
            }
        }

        if (SUCCEEDED(hr)) return true;

        //  The activation manager refuses some AUMIDs, for instance when
        //  the package is still starting up.  The shell knows how to
        //  open the All Apps folder entry, so that is the way out.
        std::wstring shellPath = L"shell:AppsFolder\\";
        shellPath += app.aumid;

        SHELLEXECUTEINFOW info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = shellPath.c_str();
        info.nShow  = SW_SHOWNORMAL;

        if (ShellExecuteExW(&info)) {
            if (info.hProcess) CloseHandle(info.hProcess);
            return true;
        }
        return false;
    }

    if (app.target.empty()) return false;

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = app.target.c_str();
    info.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) return false;

    //  the process is not waited for, the handle is only needed to keep
    //  the process object alive
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

//======================================================================
//  8.  Icons
//======================================================================

HICON ExtractAppIcon(const AppEntry& app) {
    if (app.kind == AppKind::Uwp) {
        if (app.aumid.empty()) return nullptr;

        ComScope com;
        if (!com.Usable()) return nullptr;

        //  the AUMID alone is not a parsing name, the app has to be
        //  addressed through the AppsFolder namespace
        PIDLIST_ABSOLUTE pidl = nullptr;
        const std::wstring qualified = L"shell:AppsFolder\\" + app.aumid;
        if (FAILED(SHParseDisplayName(qualified.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
            if (FAILED(SHParseDisplayName(app.aumid.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
                return nullptr;
        }

        SHFILEINFOW sfi;
        ZeroMemory(&sfi, sizeof(sfi));
        const DWORD_PTR result =
            SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON | SHGFI_PIDL);
        ILFree(pidl);
        if (!result) return nullptr;
        return sfi.hIcon;
    }

    if (app.target.empty()) return nullptr;

    SHFILEINFOW sfi;
    ZeroMemory(&sfi, sizeof(sfi));
    if (!SHGetFileInfoW(app.target.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
        return nullptr;
    return sfi.hIcon;
}

}  // namespace js
