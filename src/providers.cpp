//======================================================================
//  JARVIS SEARCH  -  web search providers.
//
//  Three responsibilities:
//    * the built-in provider table,
//    * building and percent encoding the search URL for a provider,
//    * handing the finished URL to the shell.
//
//  The percent encoding is done by hand instead of with UrlEscapeA or
//  EscapeUriString so that the result is identical on every machine and
//  in every locale:  the query is first turned into UTF-8 and then
//  every byte outside the unreserved set is written as %XX.  A space
//  becomes %20 rather than '+' because two of the provider templates
//  (Google Maps, Spotify) put the value into a path segment where '+'
//  would be taken literally.
//
//  Every failure path is silent; OpenUrl simply reports false.
//======================================================================

//  The shared header ends a line of its build comment with a backslash,
//  which GCC reports as a multi-line comment.  The warning belongs to
//  jarvis.h and not to this module, so it is silenced for the duration
//  of the include to keep this file's own diagnostics clean.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#endif
#include "jarvis.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <shellapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace js {
namespace {

//======================================================================
//  1.  The built-in provider table
//======================================================================
const std::vector<Provider>& ProviderTable() {
    //  A function local static so that the table is built once, on first
    //  use, and never copied again.  The order is the display order in
    //  the right hand pane.
    static const std::vector<Provider> table = {
        { L"g",    L"GOOGLE",         L"WEB",         L"https://www.google.com/search?q=%s" },
        { L"gg",   L"GOOGLE IMAGES",  L"IMAGES",      L"https://www.google.com/search?tbm=isch&q=%s" },
        { L"b",    L"BING",           L"WEB",         L"https://www.bing.com/search?q=%s" },
        { L"d",    L"DUCKDUCKGO",     L"WEB",         L"https://duckduckgo.com/?q=%s" },
        { L"w",    L"WIKIPEDIA",      L"ENCYCLOPEDIA", L"https://en.wikipedia.org/w/index.php?search=%s" },
        { L"y",    L"YOUTUBE",        L"VIDEO",       L"https://www.youtube.com/results?search_query=%s" },
        { L"gh",   L"GITHUB",         L"CODE",        L"https://github.com/search?q=%s" },
        { L"so",   L"STACK OVERFLOW", L"Q&A",         L"https://stackoverflow.com/search?q=%s" },
        { L"pm",   L"PERPLEXITY",     L"ANSWER",      L"https://www.perplexity.ai/search?q=%s" },
        { L"c",    L"CHATGPT",        L"ASSISTANT",   L"https://chatgpt.com/?q=%s" },
        { L"a",    L"AMAZON",         L"SHOPPING",    L"https://www.amazon.com/s?k=%s" },
        { L"m",    L"MAPS",           L"MAPS",        L"https://www.google.com/maps/search/%s" },
        { L"wa",   L"WOLFRAM",        L"COMPUTE",     L"https://www.wolframalpha.com/input?i=%s" },
        { L"n",    L"NEWS",           L"NEWS",        L"https://news.google.com/search?q=%s" },
        { L"t",    L"TRANSLATE",      L"TRANSLATE",   L"https://translate.google.com/?sl=auto&tl=en&text=%s" },
        { L"s",    L"SPOTIFY",        L"MUSIC",       L"https://open.spotify.com/search/%s" },
        { L"im",   L"IMDB",           L"MOVIES",      L"https://www.imdb.com/find/?q=%s" },
        { L"r",    L"REDDIT",         L"DISCUSSIONS", L"https://www.reddit.com/search?q=%s" },
        { L"so2",  L"STACKEXCHANGE",  L"Q&A",         L"https://stackexchange.com/search?q=%s" },
    };
    return table;
}

//======================================================================
//  2.  UTF-8 conversion
//
//  Windows wchar_t is UTF-16, so a character outside the basic
//  multilingual plane arrives as a surrogate pair.  The encoder below
//  is written by hand to keep the byte sequence independent of the
//  code page of the machine the program runs on.
//======================================================================
void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back((char)cp);
    }
    else if (cp <= 0x7FFu) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else if (cp <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

//  Turns a UTF-16 string into UTF-8.  A lone surrogate has no UTF-8
//  representation; it is replaced by U+FFFD so that the result stays
//  well formed and the URL does not break the receiving server.
std::string ToUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = (uint32_t)text[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            //  High surrogate: a low surrogate must follow.
            if (i + 1 < n) {
                const uint32_t lo = (uint32_t)text[i + 1];
                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    ++i;
                }
                else {
                    cp = 0xFFFDu;
                }
            }
            else {
                cp = 0xFFFDu;
            }
        }
        else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            //  Unpaired low surrogate.
            cp = 0xFFFDu;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

//======================================================================
//  3.  Percent encoding
//======================================================================
bool IsUnreserved(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '-' || c == '_' || c == '.' || c == '~') return true;
    return false;
}

const char kHexDigits[] = "0123456789ABCDEF";

}  // namespace

//----------------------------------------------------------------------
//  The built-in provider table, in display order.
//----------------------------------------------------------------------
const std::vector<Provider>& DefaultProviders() {
    return ProviderTable();
}

//----------------------------------------------------------------------
//  Percent encodes a query into a URL query-string value.
//
//  The result is pure ASCII: unreserved characters are copied, every
//  other byte of the UTF-8 form becomes %XX with upper case digits.
//  A space is a non unreserved byte, so it becomes %20.
//----------------------------------------------------------------------
std::wstring UrlEncodeQuery(const std::wstring& query) {
    const std::string utf8 = ToUtf8(query);

    std::wstring out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = (unsigned char)utf8[i];
        if (IsUnreserved(c)) {
            out.push_back((wchar_t)c);
            continue;
        }
        //  Everything else, the space included, is escaped.
        out.push_back(L'%');
        out.push_back((wchar_t)kHexDigits[(c >> 4) & 0x0Fu]);
        out.push_back((wchar_t)kHexDigits[c & 0x0Fu]);
    }
    return out;
}

//----------------------------------------------------------------------
//  Builds the search URL of one provider.
//
//  The first "%s" of the template carries the query; a template without
//  a placeholder simply gets the query appended.  An empty query still
//  removes the placeholder, so the result is always a valid template.
//----------------------------------------------------------------------
std::wstring BuildProviderUrl(const Provider& p, const std::wstring& query) {
    const std::wstring encoded = UrlEncodeQuery(query);
    const std::wstring& tmpl = p.templateUrl;

    const size_t hole = tmpl.find(L"%s");
    if (hole == std::wstring::npos) {
        //  No placeholder: the query is appended to the template.
        return tmpl + encoded;
    }

    std::wstring out;
    out.reserve(tmpl.size() + encoded.size());
    out.append(tmpl, 0, hole);
    out.append(encoded);
    out.append(tmpl, hole + 2, std::wstring::npos);
    return out;
}

//----------------------------------------------------------------------
//  Hands a URL to the shell.
//
//  With an explicit browser the URL travels in lpParameters, otherwise
//  the URL itself is the file that is opened.  The shell is told not to
//  raise any UI, so a missing browser is reported by the return value
//  alone and never by an error box on the screen.
//----------------------------------------------------------------------
bool OpenUrl(const std::wstring& url, const std::wstring& browserExe) {
    if (url.empty()) return false;

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    //  SEE_MASK_FLAG_NO_UI is the MinGW spelling of the SDK's
    //  SEE_MASK_NOERRORUI: no error box when the browser is missing.
    //  SEE_MASK_NO_CONSOLE keeps a console browser from flashing a
    //  window next to the always-on-top bar.
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
    info.lpVerb = L"open";
    info.nShow = SW_SHOWNORMAL;

    if (!browserExe.empty()) {
        info.lpFile = browserExe.c_str();
        info.lpParameters = url.c_str();
    }
    else {
        info.lpFile = url.c_str();
        info.lpParameters = nullptr;
    }

    return ShellExecuteExW(&info) != FALSE;
}

}  // namespace js
