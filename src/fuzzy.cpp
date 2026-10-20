//======================================================================
//  JARVIS SEARCH  -  fuzzy matching and result building.
//
//  The search bar ranks installed applications and web providers against
//  what the user typed.  This module owns that ranking:
//
//   * FuzzyMatch scores a query against a target as a subsequence and
//     reports the target indices that matched, so the renderer can
//     highlight them.
//   * IsWordBoundary tells whether a character starts a word, which is
//     what separates the "Code" of "Visual Studio Code" from a mid-word
//     "c".
//   * BuildResults turns one query into the two panes: applications on
//     the left, web providers on the right.
//
//  Every failure path is silent.  A query that does not match simply
//  produces no result; nothing is logged and nothing is reported.
//======================================================================

#include "jarvis.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace js {
namespace {

//---------------------------------------------------------------------
//  Score weights.  The scale is fixed so that scores stay comparable
//  between the two panes and across releases.
//---------------------------------------------------------------------
constexpr double kScoreExact       = 1000.0;  //  query == target
constexpr double kScoreExactNoCase = 900.0;   //  query == target, case folded
constexpr double kScorePrefix      = 600.0;   //  target starts with query
constexpr double kScoreSubstring   = 400.0;   //  query sits inside the target
constexpr double kScoreBoundary    = 60.0;    //  matched on a word boundary
constexpr double kScoreConsecutive = 30.0;    //  matched the previous character
constexpr double kScoreSkipPerChar = 10.0;    //  per skipped character
constexpr double kScorePerChar     = 5.0;     //  base cost of one match
constexpr double kScoreEmptyQuery  = 1.0;     //  empty query matches everything

//  Web provider tiers, relative to each other only.
constexpr double kWebLabelPrefix = 300.0;
constexpr double kWebKeyPrefix   = 150.0;
constexpr double kWebLabelWord   = 60.0;
constexpr double kWebBaseline    = 10.0;

//  Usage and recency bonuses for applications.
constexpr int       kUsesCounted   = 20;      //  uses above this add nothing
constexpr int       kUsesWeight    = 4;
constexpr long long kRecencyHour   = 3600;
constexpr long long kRecencyDay    = 86400;
constexpr long long kRecencyWeek   = 604800;

constexpr int kMaxPositions = 64;

//---------------------------------------------------------------------
//  Character helpers.  The comparison is deliberately ASCII only: the
//  Windows wide character API is locale dependent and would fold non
//  ASCII letters in a way the renderer never sees.
//---------------------------------------------------------------------
inline wchar_t AsciiLower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

inline bool IsAsciiUpper(wchar_t c) {
    return c >= L'A' && c <= L'Z';
}

inline bool IsAsciiLower(wchar_t c) {
    return c >= L'a' && c <= L'z';
}

inline bool SameChar(wchar_t a, wchar_t b) {
    return AsciiLower(a) == AsciiLower(b);
}

inline bool IsSpaceChar(wchar_t c) {
    return c == L' ' || c == L'\t';
}

//  Equality ignoring ASCII case.
bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!SameChar(a[i], b[i])) return false;
    }
    return true;
}

//  Prefix test ignoring ASCII case.
bool StartsWithNoCaseLocal(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (!SameChar(text[i], prefix[i])) return false;
    }
    return true;
}

//  Name ordering for the result list: case insensitive first so that the
//  order does not depend on how an app capitalised its name, then a
//  plain comparison so that the order is total and reproducible.
int CompareName(const std::wstring& a, const std::wstring& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const wchar_t ca = AsciiLower(a[i]);
        const wchar_t cb = AsciiLower(b[i]);
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return (a.size() < b.size()) ? -1 : 1;
}

//  True when `word` appears in `text` as a whitespace delimited word.
bool ContainsWord(const std::wstring& text, const std::wstring& word) {
    if (word.empty()) return false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && IsSpaceChar(text[i])) ++i;
        const size_t start = i;
        while (i < text.size() && !IsSpaceChar(text[i])) ++i;
        const size_t len = i - start;
        if (len == word.size()) {
            bool same = true;
            for (size_t k = 0; k < len; ++k) {
                if (!SameChar(text[start + k], word[k])) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
    }
    return false;
}

//  Leftmost subsequence search of `query` inside `label`, ignoring
//  ASCII case.  Returns the number of matched characters, or zero when
//  the query is not contained in the label at all; the indices are
//  written to `out` either way.
int FindSubsequence(const std::wstring& query, const std::wstring& label,
                    int* out, int maxOut) {
    int found = 0;
    size_t j = 0;
    for (size_t i = 0; i < query.size(); ++i) {
        //  The cursor walks on until the next character of the query is
        //  found; the leftmost match is the one worth highlighting.
        while (j < label.size() && !SameChar(query[i], label[j])) ++j;
        if (j >= label.size()) break;
        if (found < maxOut) out[found] = (int)j;
        ++found;
        ++j;
    }
    if (found != (int)query.size()) return 0;
    return std::min(found, maxOut);
}

//  Finds the query inside the target as one piece, ignoring case.
size_t FindSubstringNoCase(const std::wstring& target, const std::wstring& needle) {
    if (needle.empty() || target.size() < needle.size()) return std::wstring::npos;
    for (size_t i = 0; i + needle.size() <= target.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && SameChar(target[i + k], needle[k])) ++k;
        if (k == needle.size()) return i;
    }
    return std::wstring::npos;
}


//  Records a contiguous run of matches starting at zero.
void FillContiguous(MatchResult& r, size_t count) {
    r.positionCount = (int)std::min(count, (size_t)kMaxPositions);
    for (int i = 0; i < r.positionCount; ++i) r.positions[i] = i;
}

//  Records a contiguous run of matches that starts at a given offset.
void FillContiguousAt(MatchResult& r, size_t offset, size_t count) {
    r.positionCount = (int)std::min(count, (size_t)kMaxPositions);
    for (int i = 0; i < r.positionCount; ++i) r.positions[i] = (int)offset + i;
}

//---------------------------------------------------------------------
//  Application scoring helpers
//---------------------------------------------------------------------

//  Frequently used applications float to the top of the list.
double UsageBonus(const AppEntry& app) {
    const int uses = (app.uses > 0) ? app.uses : 0;
    const int counted = std::min(uses, kUsesCounted);
    return (double)counted * (double)kUsesWeight;
}

//  Recently used applications float to the top as well, with a steep
//  falloff: what was opened a minute ago matters far more than what
//  was opened last Tuesday.
double RecencyBonus(long long lastUse) {
    if (lastUse <= 0) return 0.0;
    const long long now = (long long)std::time(nullptr);
    const long long age = now - lastUse;
    if (age < 0) return 60.0;                 //  clock skew: treat as just used
    if (age <= kRecencyHour) return 60.0;
    if (age <= kRecencyDay) return 30.0;
    if (age <= kRecencyWeek) return 10.0;
    return 0.0;
}

//  A short, human readable origin of an application.
std::wstring DescribeSource(const AppEntry& app) {
    switch (app.kind) {
        case AppKind::Uwp:
            return L"STORE";
        case AppKind::Shortcut:
            return L"START MENU";
        case AppKind::Executable:
            break;
    }
    if (app.target.empty()) return L"PROGRAM";
    const std::wstring file = FileNameOf(app.target);
    return file.empty() ? std::wstring(L"PROGRAM") : file;
}

}  // namespace

//======================================================================
//  Fuzzy matching
//======================================================================

//  True when the character at `index` starts a word, that is it follows
//  a space, a dot, a dash, an underscore or a slash.  A capital that
//  follows a lower case letter starts a word as well, so the caller
//  must hand in the original spelling of the name, not the folded one.
bool IsWordBoundary(const std::wstring& target, int index) {
    if (index < 0) return false;
    if (index == 0) return !target.empty();
    if ((size_t)index >= target.size()) return false;

    const wchar_t prev = target[(size_t)index - 1];
    switch (prev) {
        case L' ':
        case L'\t':
        case L'.':
        case L'-':
        case L'_':
        case L'/':
        case L'\\':
        case L'(':
        case L')':
        case L'[':
        case L']':
        case L':':
            return true;
        default:
            break;
    }

    //  camelCase: a capital directly after a lower case letter opens a
    //  new word inside a name such as "PowerShell".
    return IsAsciiUpper(target[(size_t)index]) && IsAsciiLower(prev);
}

//  Scores `query` against `target`.  The query has to appear in the
//  target as a subsequence; the score rewards an exact match, a prefix
//  match, matches that land on word boundaries and matches that form
//  consecutive runs, and it penalises the characters skipped in
//  between.  The comparison ignores ASCII case, so a caller may hand in
//  the target in its original spelling.
MatchResult FuzzyMatch(const std::wstring& query, const std::wstring& target) {
    MatchResult r;

    //  An empty query matches everything but says nothing, so it
    //  scores just above zero.
    if (query.empty()) {
        r.matched = true;
        r.score = kScoreEmptyQuery;
        return r;
    }
    if (target.empty()) return r;

    const size_t qn = query.size();

    //  Whole target matched.
    if (query == target) {
        r.matched = true;
        r.score = kScoreExact;
        FillContiguous(r, qn);
        return r;
    }
    if (EqualNoCase(query, target)) {
        r.matched = true;
        r.score = kScoreExactNoCase;
        FillContiguous(r, qn);
        return r;
    }

    //  The target opens with the query.
    if (StartsWithNoCaseLocal(target, query)) {
        r.matched = true;
        r.score = kScorePrefix;
        FillContiguous(r, qn);
        return r;
    }

    //  The query sits inside the target as one piece.
    {
        const size_t at = FindSubstringNoCase(target, query);
        if (at != std::wstring::npos) {
            r.matched = true;
            r.score = kScoreSubstring;
            FillContiguousAt(r, at, qn);
            return r;
        }
    }

    //  Greedy leftmost subsequence search.
    std::vector<int> hits;
    hits.reserve(qn);
    size_t j = 0;
    for (size_t i = 0; i < qn; ++i) {
        size_t k = j;
        while (k < target.size() && !SameChar(query[i], target[k])) ++k;
        if (k >= target.size()) return r;      //  not a subsequence
        hits.push_back((int)k);
        j = k + 1;
    }

    //  Refinement: a match that landed in the middle of a word is moved
    //  to an earlier occurrence that starts a word, so that the
    //  "Code" of "Visual Studio Code" is preferred over a mid-word
    //  "c".  The match order is never broken: a position may only move
    //  to one that still follows the previous match.
    for (size_t i = hits.size(); i-- > 0;) {
        const int cur = hits[i];
        const int limit = (i == 0) ? 0 : hits[i - 1] + 1;
        for (int k = cur - 1; k >= limit; --k) {
            if (!SameChar(target[(size_t)k], target[(size_t)cur])) continue;
            if (!IsWordBoundary(target, k)) continue;
            hits[i] = k;
            break;
        }
    }

    double score = 0.0;
    for (size_t i = 0; i < hits.size(); ++i) {
        const int at = hits[i];
        if (IsWordBoundary(target, at)) score += kScoreBoundary;
        if (i > 0) {
            const int gap = at - hits[i - 1] - 1;
            if (gap == 0) score += kScoreConsecutive;
            score -= kScoreSkipPerChar * (double)gap;
        }
        score += kScorePerChar;
    }
    if (score > kScoreExact) score = kScoreExact;

    r.matched = true;
    r.score = score;
    r.positionCount = (int)std::min(hits.size(), (size_t)kMaxPositions);
    for (int i = 0; i < r.positionCount; ++i) r.positions[i] = hits[(size_t)i];
    return r;
}

//======================================================================
//  Result building
//======================================================================

//  Builds the two panes for a query.  The left pane holds the matching
//  applications, the right pane the web providers; both are sorted best
//  first and truncated to the given row counts.
void BuildResults(const std::wstring& query, const AppIndex& index,
                  const std::vector<Provider>& providers, int maxApps,
                  int maxProviders, std::vector<Result>& appResults,
                  std::vector<Result>& webResults) {
    appResults.clear();
    webResults.clear();
    if (maxApps < 0) maxApps = 0;
    if (maxProviders < 0) maxProviders = 0;

    const bool haveQuery = !query.empty();
    const std::wstring qLower = LowerAscii(query);

    //-----------------------------------------------------------------
    //  Applications
    //-----------------------------------------------------------------
    if (haveQuery) {
        //  A copy, because the scan may still be running on its own
        //  thread while the pane is being built.
        const std::vector<AppEntry> apps = index.Snapshot();
        for (size_t i = 0; i < apps.size(); ++i) {
            const AppEntry& app = apps[i];

            //  The match runs against the folded name; the indices are
            //  the same in the original spelling because folding only
            //  changes the case of a character, never its position.
            const std::wstring folded = LowerAscii(app.name);
            const MatchResult m = FuzzyMatch(query, folded);
            if (!m.matched) continue;

            Result r;
            r.kind = ResultKind::App;
            r.index = (int)i;
            r.label = app.name;
            r.sublabel = DescribeSource(app);
            r.score = m.score + UsageBonus(app) + RecencyBonus(app.lastUse);
            r.highlightCount = std::min(m.positionCount, kMaxPositions);
            for (int k = 0; k < r.highlightCount; ++k) r.highlight[k] = m.positions[k];
            appResults.push_back(r);
        }

        std::stable_sort(appResults.begin(), appResults.end(),
                         [](const Result& a, const Result& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return CompareName(a.label, b.label) < 0;
                         });

        if ((int)appResults.size() > maxApps) appResults.resize((size_t)maxApps);
    }

    //-----------------------------------------------------------------
    //  Web providers
    //
    //  The query is the search term, so every provider is a candidate
    //  and only the label decides how high it floats.
    //-----------------------------------------------------------------
    {
        struct WebHit {
            int    index;
            double score;
        };

        std::vector<WebHit> hits;
        hits.reserve(providers.size());
        for (size_t i = 0; i < providers.size(); ++i) {
            const Provider& p = providers[i];
            double score = kWebBaseline;
            if (haveQuery) {
                const std::wstring labelLower = LowerAscii(p.label);
                const std::wstring keyLower = LowerAscii(p.key);
                if (StartsWithNoCaseLocal(labelLower, qLower)) {
                    score = kWebLabelPrefix;
                } else if (StartsWithNoCaseLocal(keyLower, qLower)) {
                    score = kWebKeyPrefix;
                } else if (ContainsWord(labelLower, qLower)) {
                    score = kWebLabelWord;
                } else {
                    score = kWebBaseline;
                }
            }
            WebHit h;
            h.index = (int)i;
            h.score = score;
            hits.push_back(h);
        }

        //  Ties keep the order of the provider table.
        std::stable_sort(hits.begin(), hits.end(),
                         [](const WebHit& a, const WebHit& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return a.index < b.index;
                         });

        const size_t count = std::min(hits.size(), (size_t)maxProviders);
        for (size_t i = 0; i < count; ++i) {
            const Provider& p = providers[(size_t)hits[i].index];
            Result r;
            r.kind = ResultKind::Web;
            r.index = hits[i].index;
            r.label = p.label;
            r.sublabel = p.hint;
            r.score = hits[i].score;
            if (haveQuery && hits[i].score > kWebBaseline) {
                //  The label matched: mark the characters of the query
                //  inside the original label.
                r.highlightCount = FindSubsequence(qLower, p.label, r.highlight, kMaxPositions);
            }
            webResults.push_back(r);
        }
    }
}

}  // namespace js
