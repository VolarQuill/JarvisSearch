//======================================================================
//  JARVIS SEARCH  -  shared string, file and configuration helpers.
//
//  Everything in this translation unit is deliberately dependency free:
//  only the Win32 kernel32 APIs are used, so the helpers may be called
//  from any thread at any time without a COM apartment.
//
//  Every failure path is silent.  A malformed file yields an empty
//  result, a failed conversion yields replacement characters, and a
//  failed write reports false to the caller.
//======================================================================

//  jarvis.h carries a build comment whose lines end in a backslash,
//  which the compiler reports as a multi-line comment.  The header is
//  shared and must not be edited, so the warning is muted here for the
//  duration of the include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "jarvis.h"
#pragma GCC diagnostic pop

#include <cerrno>
#include <climits>
#include <cwchar>
#include <string>
#include <vector>

namespace js {

namespace {

//  Anything bigger than this is not a config or a data file, it is a
//  mistake; refuse to load it rather than allocate the world.
constexpr long long kMaxFileBytes = 4LL * 1024 * 1024;

//----------------------------------------------------------------------
//  UTF-8 <-> UTF-16 conversion used as a fallback
//----------------------------------------------------------------------
//  The Win32 converters reject the entire buffer when a single byte is
//  malformed.  That is not the behaviour we want for a text file: the
//  routines below resynchronise after a bad byte and emit U+FFFD in its
//  place, so one broken byte never costs us the rest of the file.

std::wstring DecodeUtf8(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());

    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char lead = (unsigned char)s[i];
        unsigned int cp = 0;
        int extra = 0;

        if (lead < 0x80) {
            cp = lead;
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07u;
            extra = 3;
        } else {
            //  A continuation byte or a forbidden lead byte.
            out.append(1, L'\uFFFD');
            ++i;
            continue;
        }

        if (i + (size_t)extra >= n) {
            out.append(1, L'\uFFFD');
            ++i;
            continue;
        }

        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char c = (unsigned char)s[i + (size_t)k];
            if ((c & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (!ok) {
            out.append(1, L'\uFFFD');
            ++i;
            continue;
        }

        //  Reject overlong encodings, UTF-16 surrogates and out of
        //  range code points; they are all security hazards.
        const bool overlong = (extra == 1 && cp < 0x80) ||
                              (extra == 2 && cp < 0x800) ||
                              (extra == 3 && cp < 0x10000);
        const bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
        if (overlong || surrogate || cp > 0x10FFFF) {
            out.append(1, L'\uFFFD');
            ++i;
            continue;
        }

        if (cp > 0xFFFF) {
            //  wchar_t is 16 bit on Windows, so anything above the BMP
            //  needs a UTF-16 surrogate pair.
            const unsigned int v = cp - 0x10000u;
            out.append(1, (wchar_t)(0xD800u + (v >> 10)));
            out.append(1, (wchar_t)(0xDC00u + (v & 0x3FFu)));
        } else {
            out.append(1, (wchar_t)cp);
        }
        i += (size_t)extra + 1;
    }
    return out;
}

std::string EncodeUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());

    for (size_t i = 0; i < w.size(); ++i) {
        unsigned int cp = (unsigned int)(wchar_t)w[i];

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            //  A high surrogate must be followed by a low surrogate;
            //  a lone one is malformed and becomes U+FFFD.
            unsigned int low = 0;
            if (i + 1 < w.size()) {
                low = (unsigned int)(wchar_t)w[i + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    ++i;
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  //  a lone low surrogate
        }

        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0u | (cp >> 6)));
            out.push_back((char)(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0u | (cp >> 12)));
            out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back((char)(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back((char)(0xF0u | (cp >> 18)));
            out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back((char)(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

//----------------------------------------------------------------------
//  ASCII case folding
//----------------------------------------------------------------------
//  Only A-Z and a-z are folded.  Application and provider names are
//  compared case insensitively, but a full Unicode case fold could
//  change the length of a string (the Turkish dotless i, the German
//  sharp s) and would destroy characters we want to display as they
//  were typed.

wchar_t FoldAscii(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

wchar_t UnfoldAscii(wchar_t c) {
    if (c >= L'a' && c <= L'z') return (wchar_t)(c - L'a' + L'A');
    return c;
}

bool IsWsChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

//----------------------------------------------------------------------
//  Configuration file parsing
//----------------------------------------------------------------------
struct ConfigLine {
    bool         hasKey = false;  //  a "key=value" pair was found
    std::wstring key;             //  trimmed, original case
    std::wstring value;           //  trimmed
};

ConfigLine ParseConfigLine(const std::wstring& line) {
    ConfigLine cl;
    const std::wstring t = TrimWs(line);
    if (t.empty() || t[0] == L'#') return cl;

    //  Only the FIRST '=' separates key from value: a value may
    //  legitimately contain further '=' characters.
    const size_t eq = t.find(L'=');
    if (eq == std::wstring::npos) return cl;

    cl.hasKey = true;
    cl.key = TrimWs(t.substr(0, eq));
    cl.value = TrimWs(t.substr(eq + 1));
    return cl;
}

//  Accepts the usual spellings of a boolean.  Returns false when the
//  text is not a boolean at all, in which case the caller keeps the
//  current value.
bool ParseBool(const std::wstring& raw, bool& out) {
    const std::wstring t = LowerAscii(TrimWs(raw));
    if (t == L"1" || t == L"true" || t == L"yes" || t == L"on") {
        out = true;
        return true;
    }
    if (t == L"0" || t == L"false" || t == L"no" || t == L"off") {
        out = false;
        return true;
    }
    return false;
}

void ApplyConfigKey(const std::wstring& key, const std::wstring& val, Config& cfg) {
    const std::wstring k = LowerAscii(key);

    if (k == L"font") {
        cfg.font = val;
    } else if (k == L"font_size") {
        cfg.fontSize = ParseInt(val, cfg.fontSize);
    } else if (k == L"label_font_size") {
        cfg.labelFontSize = ParseInt(val, cfg.labelFontSize);
    } else if (k == L"status_text") {
        cfg.statusText = val;
    } else if (k == L"position") {
        //  The renderer compares against the lower case spellings
        //  (top-center, bottom-left, ...), so normalise here.
        cfg.position = LowerAscii(val);
    } else if (k == L"margin") {
        cfg.margin = ParseInt(val, cfg.margin);
    } else if (k == L"pos_x") {
        cfg.posX = ParseInt(val, cfg.posX);
    } else if (k == L"pos_y") {
        cfg.posY = ParseInt(val, cfg.posY);
    } else if (k == L"bar_width") {
        cfg.barWidth = ParseInt(val, cfg.barWidth);
    } else if (k == L"bar_height") {
        cfg.barHeight = ParseInt(val, cfg.barHeight);
    } else if (k == L"max_app_results") {
        cfg.maxAppResults = ParseInt(val, cfg.maxAppResults);
    } else if (k == L"max_provider_results") {
        cfg.maxProviderResults = ParseInt(val, cfg.maxProviderResults);
    } else if (k == L"row_height") {
        cfg.rowHeight = ParseInt(val, cfg.rowHeight);
    } else if (k == L"idle_brightness") {
        cfg.idleBrightness = ParseInt(val, cfg.idleBrightness);
    } else if (k == L"show_clock") {
        ParseBool(val, cfg.showClock);
    } else if (k == L"show_tray") {
        ParseBool(val, cfg.showTray);
    } else if (k == L"autostart") {
        ParseBool(val, cfg.autostart);
    } else if (k == L"load_uwp") {
        ParseBool(val, cfg.loadUwp);
    } else if (k == L"show_reactor") {
        ParseBool(val, cfg.showReactor);
    } else if (k == L"hotkey") {
        cfg.hotkey = val;
    } else if (k == L"default_browser") {
        cfg.defaultBrowser = val;
    }
    //  Unknown keys are ignored on purpose so that an old config file
    //  stays valid across upgrades.
}

//----------------------------------------------------------------------
//  Directory creation
//----------------------------------------------------------------------
//  A key or a value must never contain a line break: it would split
//  the config file into two broken lines.  Fold the break into a
//  space instead of corrupting the file.
std::wstring SanitizeField(const std::wstring& s) {
    std::wstring o = s;
    for (size_t i = 0; i < o.size(); ++i) {
        if (o[i] == L'\r' || o[i] == L'\n') o[i] = L' ';
    }
    return o;
}

//  Creates every missing directory leading to the target file.  The
//  path is treated as a plain backslash separated chain, so no shell
//  or COM machinery is involved and no extra library is needed.
bool EnsureParentDir(const std::wstring& path) {
    const std::wstring dir = DirNameOf(path);
    if (dir.empty() || dir == L".") return true;

    std::wstring acc;
    size_t i = 0;
    while (i < dir.size()) {
        size_t j = dir.find(L'\\', i);
        if (j == std::wstring::npos) j = dir.size();
        if (j > i) {
            if (acc.empty()) {
                acc = dir.substr(0, j);
            } else {
                acc += L"\\";
                acc.append(dir, i, j - i);
            }
            //  Failures are ignored: the directory may already exist
            //  (ERROR_ALREADY_EXISTS) or belong to the system root
            //  ("C:", "\\server").
            CreateDirectoryW(acc.c_str(), nullptr);
        }
        i = j + 1;
    }
    return true;
}

}  // namespace

//======================================================================
//  1.  String helpers
//======================================================================

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();

    //  WC_ERR_INVALID_CHARS makes the converter reject the whole
    //  buffer instead of silently substituting, which is what we want
    //  to detect; the hand written decoder then repairs it.
    int n = MultiByteToWideChar(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), nullptr, 0);
    if (n > 0) {
        std::wstring w;
        w.resize((size_t)n);
        if (MultiByteToWideChar(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), &w[0], n) == n) {
            return w;
        }
    }
    return DecodeUtf8(s);
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();

    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string s;
        s.resize((size_t)n);
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                (int)w.size(), &s[0], n, nullptr, nullptr) == n) {
            return s;
        }
    }
    return EncodeUtf8(w);
}

std::wstring TrimWs(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsWsChar(s[b])) ++b;
    while (e > b && IsWsChar(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring LowerAscii(const std::wstring& s) {
    std::wstring o = s;
    for (size_t i = 0; i < o.size(); ++i) o[i] = FoldAscii(o[i]);
    return o;
}

std::wstring UpperAscii(const std::wstring& s) {
    std::wstring o = s;
    for (size_t i = 0; i < o.size(); ++i) o[i] = UnfoldAscii(o[i]);
    return o;
}

bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (FoldAscii(s[i]) != FoldAscii(prefix[i])) return false;
    }
    return true;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (FoldAscii(s[off + i]) != FoldAscii(suffix[i])) return false;
    }
    return true;
}

std::vector<std::wstring> SplitLines(const std::string& blob) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (size_t i = 0; i <= blob.size(); ++i) {
        if (i == blob.size() || blob[i] == '\n') {
            size_t end = i;
            //  A CR immediately before the LF belongs to the line
            //  ending, not to the text.
            if (end > start && blob[end - 1] == '\r') --end;
            lines.push_back(Widen(blob.substr(start, end - start)));
            start = i + 1;
        }
    }
    return lines;
}

std::string StripUtf8Bom(const std::string& s) {
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        return s.substr(3);
    }
    return s;
}

//======================================================================
//  2.  File helpers
//======================================================================

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    out.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        size.QuadPart > kMaxFileBytes) {
        CloseHandle(h);
        return false;
    }

    if (size.QuadPart > 0) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr)) {
            out.clear();
            CloseHandle(h);
            return false;
        }
        out.resize(got);
    }

    CloseHandle(h);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& utf8) {
    if (path.empty()) return false;
    if (!EnsureParentDir(path)) return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    size_t done = 0;
    while (done < utf8.size()) {
        DWORD written = 0;
        const DWORD chunk = (DWORD)((utf8.size() - done) > 0x10000u
                                        ? 0x10000u
                                        : (utf8.size() - done));
        if (!WriteFile(h, utf8.data() + done, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
            break;
        }
        done += written;
    }

    CloseHandle(h);
    return ok;
}

//======================================================================
//  3.  Path helpers - pure string surgery, no filesystem calls
//======================================================================

std::wstring DirNameOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    //  Keep the separator that follows a drive letter, otherwise
    //  "C:\file.txt" would collapse to the useless "C:".
    if (p == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, p);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

std::wstring BaseNameOf(const std::wstring& path) {
    const std::wstring file = FileNameOf(path);
    const size_t dot = file.find_last_of(L'.');
    //  A leading dot marks a hidden file, not an extension.
    if (dot == std::wstring::npos || dot == 0) return file;
    return file.substr(0, dot);
}

int ParseInt(const std::wstring& s, int fallback) {
    const std::wstring t = TrimWs(s);
    if (t.empty()) return fallback;

    errno = 0;
    wchar_t* end = nullptr;
    const long v = wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return fallback;          //  no digits at all
    if (errno == ERANGE) return fallback;           //  out of range

    if (v < (long)INT_MIN) return INT_MIN;
    if (v > (long)INT_MAX) return INT_MAX;
    return (int)v;
}

//======================================================================
//  4.  Executable paths
//======================================================================

std::wstring ExeDir() {
    //  The extended length path limit is 32767 characters, and the
    //  buffer needs room for the terminating null.
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return std::wstring();

    buf[n < (sizeof(buf) / sizeof(buf[0])) ? n : (sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
    return DirNameOf(std::wstring(buf));
}

std::wstring ExeRelativePath(const wchar_t* name) {
    const std::wstring dir = ExeDir();
    if (!name || !*name) return dir;
    if (dir.empty()) return std::wstring(name);
    return dir + L"\\" + name;
}

//======================================================================
//  5.  Configuration
//======================================================================

bool LoadConfig(const std::wstring& path, Config& out) {
    std::string blob;
    if (!ReadWholeFile(path, blob)) {
        //  A missing or unreadable file is not an error: the defaults
        //  that the caller constructed stay in place.
        return false;
    }

    blob = StripUtf8Bom(blob);
    for (const std::wstring& raw : SplitLines(blob)) {
        const ConfigLine cl = ParseConfigLine(raw);
        if (!cl.hasKey || cl.key.empty()) continue;
        ApplyConfigKey(cl.key, cl.value, out);
    }
    return true;
}

bool SaveConfigKey(const std::wstring& path, const std::wstring& key,
                   const std::wstring& value) {
    if (path.empty() || key.empty()) return false;

    const std::wstring wanted = LowerAscii(TrimWs(key));

    //  Split the existing file into raw lines so that comments and
    //  the original bytes of untouched lines survive the round trip.
    std::vector<std::string> raw;
    bool crlf = false;
    {
        std::string blob;
        if (ReadWholeFile(path, blob)) {
            blob = StripUtf8Bom(blob);
            if (!blob.empty()) {
                size_t start = 0;
                for (size_t i = 0; i <= blob.size(); ++i) {
                    if (i == blob.size() || blob[i] == '\n') {
                        size_t end = i;
                        if (end > start && blob[end - 1] == '\r') {
                            --end;
                            crlf = true;
                        }
                        raw.push_back(blob.substr(start, end - start));
                        start = i + 1;
                    }
                }
                //  A trailing newline does not start a new empty line.
                if (!raw.empty() && raw.back().empty() &&
                    blob[blob.size() - 1] == '\n') {
                    raw.pop_back();
                }
            }
        }
    }

    const std::string eol = crlf ? "\r\n" : "\n";
    const std::string newLine = NarrowUtf8(SanitizeField(TrimWs(key))) + "=" +
                                NarrowUtf8(SanitizeField(value));

    bool replaced = false;
    for (std::string& line : raw) {
        const ConfigLine cl = ParseConfigLine(Widen(line));
        if (!cl.hasKey || cl.key.empty()) continue;
        if (LowerAscii(cl.key) != wanted) continue;
        line = newLine;
        replaced = true;
        break;
    }
    if (!replaced) raw.push_back(newLine);

    std::string outBlob;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i) outBlob += eol;
        outBlob += raw[i];
    }
    outBlob += eol;

    return WriteTextFile(path, outBlob);
}

}  // namespace js
