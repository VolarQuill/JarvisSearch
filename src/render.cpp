//======================================================================
//  JARVIS SEARCH  -  the view layer implementation.
//
//  The frame is composed on a 32 bit ARGB surface the size of the
//  window and then flattened onto an opaque one: the alpha is resolved
//  against the card colour and the result is mixed towards it by the
//  fade factor.  The bar is an ordinary window, because a layered one
//  would be composited by the desktop window manager in a pass of its
//  own and would float above every application no matter what the z
//  order says.
//
//  Nothing is animated while the bar is idle.  The caret blinks and
//  the clock ticks, and that is the entire idle cost.
//======================================================================

#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace js {
namespace {

//----------------------------------------------------------------------
//  Palette
//----------------------------------------------------------------------
constexpr Rgba kPanelFill   = { 255,   4,  14,  24};  //  near black navy, solid
constexpr Rgba kPanelEdge   = { 150,   0, 197, 232};  //  1px cyan border
constexpr Rgba kBracket     = { 255, 125, 249, 255};  //  bright corner brackets
constexpr Rgba kTextPrimary = { 255, 184, 244, 255};  //  main text
constexpr Rgba kTextBright  = { 255, 232, 253, 255};  //  highlighted text
constexpr Rgba kTextDim     = { 255,  78, 143, 160};  //  labels, hints
constexpr Rgba kAccentAmber = { 255, 255, 179,  71};  //  sparing accent
constexpr Rgba kGridLine    = {  55,  14,  58,  74};  //  faint background grid
constexpr Rgba kReactorCore = { 255, 255, 255, 255};  //  white hot centre
constexpr Rgba kReactorGlow = { 190, 125, 249, 255};  //  inner glow ring
constexpr Rgba kReactorRing = { 220,   0, 200, 224};  //  outer ring
constexpr Rgba kSelection   = {  40,   0, 197, 232};  //  selected row fill
constexpr Rgba kDivider     = { 120,   0, 197, 232};  //  hairline
constexpr Rgba kIconTint    = { 255, 190, 240, 250};  //  app icon tint

//----------------------------------------------------------------------
//  Geometry, logical pixels
//----------------------------------------------------------------------
constexpr float kBarPadX      = 14.0f;
constexpr float kReactorCx    = 33.0f;
constexpr float kReactorR     = 12.0f;
constexpr float kPromptX      = 48.0f;
constexpr float kTextX        = 62.0f;
constexpr float kCornerRadius = 6.0f;
constexpr float kBracketArm   = 18.0f;
constexpr float kBracketInset = 6.0f;
constexpr float kBracketThick = 2.0f;
constexpr float kGridStep     = 26.0f;
constexpr float kPanePadY     = 9.0f;
constexpr float kHeaderH      = 20.0f;
constexpr float kStatusH      = 18.0f;
//  The room that is kept free at the bottom of the panel so that the
//  corner brackets never sit on top of the status line.
constexpr float kBottomReserve = 26.0f;
constexpr float kRowIconSize  = 16.0f;
constexpr float kRowPadX      = 10.0f;
constexpr float kIconGap      = 9.0f;
constexpr int   kRingSlots    = 24;
constexpr int   kReactorTicks = 8;

const wchar_t* const kFontChain[] = {
    L"Cascadia Mono", L"Consolas", L"Lucida Console", L"Courier New", nullptr
};

inline Gdiplus::Color Col(const Rgba& c) {
    return Gdiplus::Color(c.a, c.r, c.g, c.b);
}

inline Gdiplus::Color ColScaled(const Rgba& c, int alpha) {
    int a = (c.a * alpha) / 255;
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return Gdiplus::Color((BYTE)a, c.r, c.g, c.b);
}

//  True when the point lies inside a rounded rectangle.  The window
//  region is cut with the same shape, so the PNG dump can paint the
//  corners the way the screen shows them.
bool InsideRoundedRect(float px, float py, float x, float y, float w, float h, float r) {
    if (px < x || py < y || px > x + w || py > y + h) return false;
    const float dx = std::min(px - x, x + w - px);
    const float dy = std::min(py - y, y + h - py);
    if (dx >= r || dy >= r) return true;
    const float cx = (px < x + r) ? (x + r) : (x + w - r);
    const float cy = (py < y + r) ? (y + r) : (y + h - r);
    const float ex = px - cx, ey = py - cy;
    return (ex * ex + ey * ey) <= r * r;
}

void BuildRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (r < 0.0f) r = 0.0f;
    float d = 2.0f * r;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        path.CloseFigure();
        return;
    }
    path.AddArc(Gdiplus::RectF(x, y, d, d), 180.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x + w - d, y, d, d), 270.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x + w - d, y + h - d, d, d), 0.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x, y + h - d, d, d), 90.0f, 90.0f);
    path.CloseFigure();
}

//----------------------------------------------------------------------
//  A GDI+ session that is started and stopped with the renderer.
//----------------------------------------------------------------------
class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        m_ok = (Gdiplus::GdiplusStartup(&m_token, &input, nullptr) == Gdiplus::Ok);
    }
    ~GdiplusSession() {
        if (m_ok) Gdiplus::GdiplusShutdown(m_token);
    }
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    bool ok() const { return m_ok; }

private:
    ULONG_PTR m_token = 0;
    bool m_ok = false;
};

//----------------------------------------------------------------------
//  Minimal PNG writer, diagnostics only.
//----------------------------------------------------------------------
uint32_t Crc32Update(uint32_t crc, const BYTE* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const BYTE* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PushBe32(std::vector<BYTE>& v, uint32_t x) {
    v.push_back((BYTE)(x >> 24));
    v.push_back((BYTE)(x >> 16));
    v.push_back((BYTE)(x >> 8));
    v.push_back((BYTE)x);
}

void PushChunk(std::vector<BYTE>& out, const char* type, const std::vector<BYTE>& data) {
    PushBe32(out, (uint32_t)data.size());
    std::vector<BYTE> typeBuf(type, type + 4);
    uint32_t crc = Crc32Update(0, typeBuf.data(), 4);
    if (!data.empty()) crc = Crc32Update(crc, data.data(), data.size());
    out.insert(out.end(), typeBuf.begin(), typeBuf.end());
    out.insert(out.end(), data.begin(), data.end());
    PushBe32(out, crc);
}

bool WritePngRgb(const std::wstring& path, const BYTE* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;
    std::vector<BYTE> raw;
    raw.reserve((size_t)h * ((size_t)w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const BYTE* p = bgra + ((size_t)y * (size_t)w + (size_t)x) * 4;
            raw.push_back(p[2]);
            raw.push_back(p[1]);
            raw.push_back(p[0]);
        }
    }
    std::vector<BYTE> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t chunk = std::min<size_t>(65535, raw.size() - off);
        const bool last = (off + chunk) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((BYTE)(chunk & 0xFF));
        z.push_back((BYTE)((chunk >> 8) & 0xFF));
        z.push_back((BYTE)(~chunk & 0xFF));
        z.push_back((BYTE)((~chunk >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + chunk));
        off += chunk;
    }
    PushBe32(z, Adler32(raw.data(), raw.size()));

    std::vector<BYTE> png;
    const BYTE sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    std::vector<BYTE> ihdr;
    PushBe32(ihdr, (uint32_t)w);
    PushBe32(ihdr, (uint32_t)h);
    ihdr.push_back(8);
    ihdr.push_back(2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", z);
    PushChunk(png, "IEND", std::vector<BYTE>());

    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(fh, png.data(), (DWORD)png.size(), &written, nullptr);
    CloseHandle(fh);
    return true;
}

}  // namespace

//======================================================================
//  The window surface
//======================================================================
bool DibSurface::Create(HDC screen, int width, int height) {
    Destroy();
    if (width <= 0 || height <= 0) return false;
    w = width;
    h = height;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    //  A negative height makes the DIB top down, so the rows are
    //  stored the way they are drawn and the window can be painted
    //  with a plain BitBlt.
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!bmp || !bits) { Destroy(); return false; }
    dc = CreateCompatibleDC(screen);
    if (!dc) { Destroy(); return false; }
    old = SelectObject(dc, bmp);
    ZeroMemory(bits, (size_t)w * (size_t)h * 4);
    return true;
}

void DibSurface::Destroy() {
    if (dc && old) SelectObject(dc, old);
    old = nullptr;
    if (dc) { DeleteDC(dc); dc = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    bits = nullptr;
    w = h = 0;
}

//======================================================================
//  Layout helpers
//======================================================================
RectI Layout::AppRowRect(int i) const {
    RectI r;
    r.x = appsPane.x + (int)kRowPadX;
    r.w = appsPane.w - (int)(kRowPadX * 2);
    r.y = appsHeader.y + appsHeader.h + (i - appScroll) * appRowH;
    r.h = appRowH;
    return r;
}

RectI Layout::WebRowRect(int i) const {
    RectI r;
    r.x = webPane.x + (int)kRowPadX;
    r.w = webPane.w - (int)(kRowPadX * 2);
    r.y = webHeader.y + webHeader.h + (i - webScroll) * webRowH;
    r.h = webRowH;
    return r;
}

//======================================================================
//  Renderer
//======================================================================
Renderer::~Renderer() {
    m_fontQuery.reset();
    m_fontRow.reset();
    m_fontLabel.reset();
    m_fontSmall.reset();
    m_family.reset();
    if (m_screenDc) {
        ReleaseDC(nullptr, m_screenDc);
        m_screenDc = nullptr;
    }
}

bool Renderer::Init(const Config& cfg, HWND hwnd) {
    //  GDI+ has to be running before any of its entry points are
    //  touched, including the font family lookup.  The session is a
    //  function local static so that it starts exactly once and stays
    //  alive for the whole run.
    static GdiplusSession g_gdi;
    (void)g_gdi;

    m_cfg = cfg;
    m_hwnd = hwnd;

    //  The scale follows the window's device pixels per logical pixel.
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) {
        HDC dc = GetDC(hwnd);
        if (dc) {
            const int l = GetDeviceCaps(dc, LOGPIXELSX);
            if (l > 0) dpi = (UINT)l;
            ReleaseDC(hwnd, dc);
        }
    }
    if (dpi == 0) dpi = 96;
    m_scale = (float)dpi / 96.0f;

    if (!ResolveFont()) return false;
    if (!PrepareFonts()) return false;
    ProbeGdiAlpha();
    BuildComposeTables();
    return true;
}

bool Renderer::ResolveFont() {
    std::vector<std::wstring> candidates;
    if (!m_cfg.font.empty()) candidates.push_back(m_cfg.font);
    for (int i = 0; kFontChain[i]; ++i) candidates.push_back(kFontChain[i]);

    for (const std::wstring& name : candidates) {
        Gdiplus::FontFamily ff(name.c_str());
        if (ff.IsAvailable() && ff.IsStyleAvailable(Gdiplus::FontStyleRegular)) {
            m_familyName = name;
            return true;
        }
    }
    const Gdiplus::FontFamily* generic = Gdiplus::FontFamily::GenericMonospace();
    if (generic && generic->IsAvailable()) {
        WCHAR name[LF_FACESIZE];
        if (generic->GetFamilyName(name) == Gdiplus::Ok) m_familyName = name;
    }
    if (m_familyName.empty()) m_familyName = L"Courier New";
    return !m_familyName.empty();
}

bool Renderer::PrepareFonts() {
    m_family.reset(new Gdiplus::FontFamily(m_familyName.c_str()));
    if (!m_family || m_family->GetLastStatus() != Gdiplus::Ok || !m_family->IsAvailable())
        return false;

    const float s = m_scale;
    const int q = std::max(7, m_cfg.fontSize);
    const int row = std::max(7, m_cfg.fontSize - 2);
    const int label = std::max(6, m_cfg.labelFontSize);
    const int small = std::max(6, m_cfg.labelFontSize - 1);

    m_fontQuery.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(q * s),
                                        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    m_fontRow.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(row * s),
                                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    m_fontLabel.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(label * s),
                                        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    m_fontSmall.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(small * s),
                                        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    if (!m_fontQuery || !m_fontRow || !m_fontLabel || !m_fontSmall) return false;
    if (m_fontQuery->GetLastStatus() != Gdiplus::Ok) return false;

    //  Every font is measured on its own: the sizes differ, so one
    //  advance cannot stand in for all of them.
    m_advanceQuery = MeasureAdvance(m_fontQuery.get());
    m_advanceRow = MeasureAdvance(m_fontRow.get());
    m_advanceLabel = MeasureAdvance(m_fontLabel.get());
    m_advanceSmall = MeasureAdvance(m_fontSmall.get());
    return m_advanceQuery > 0.0f;
}

float Renderer::MeasureAdvance(const Gdiplus::Font* font) {
    if (!font) return 0.0f;
    //  Measure a run of characters rather than one, so that hinting
    //  rounding does not bias the cell width.
    Gdiplus::Bitmap bmp(8, 8, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return 0.0f;
    Gdiplus::Graphics g(&bmp);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox |
                       Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF out;
    const WCHAR probe[] = L"MMMMMMMMMM";
    g.MeasureString(probe, -1, font, Gdiplus::RectF(0, 0, 4096, 64), &fmt, &out);
    return out.Width / 10.0f;
}

float Renderer::MeasureLineHeight() {
    if (!m_fontQuery) return 0.0f;
    Gdiplus::Bitmap bmp(8, 8, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return 0.0f;
    Gdiplus::Graphics g(&bmp);
    return m_fontQuery->GetHeight(&g);
}

Layout Renderer::ComputeLayout(const UiModel& m) const {
    Layout lay;
    lay.scale = m_scale;
    const float s = m_scale;

    lay.width = (int)std::lround((float)m_cfg.barWidth * s);
    const int barH = (int)std::lround((float)m_cfg.barHeight * s);
    lay.height = barH;
    lay.bar = RectI{0, 0, lay.width, barH};

    if (!m.panelOpen) return lay;

    const float headerH = kHeaderH * s;
    const float rowH = (float)m_cfg.rowHeight * s;
    const float padY = kPanePadY * s;
    const float statusH = kStatusH * s;

    const int appsVisible = (int)std::min((size_t)m_cfg.maxAppResults, m.apps.rows.size());
    const int webVisible = (int)std::min((size_t)m_cfg.maxProviderResults, m.web.rows.size());
    const float rowsH = (float)std::max(appsVisible, webVisible) * rowH;
    const float panelH = padY + headerH + rowsH + statusH + kBottomReserve * s;
    const int panelHpx = (int)std::lround(panelH);

    lay.height = barH + panelHpx;
    lay.panel = RectI{0, barH, lay.width, panelHpx};
    lay.appsPane = RectI{0, barH, lay.width / 2, panelHpx};
    lay.webPane = RectI{lay.width / 2, barH, lay.width - lay.width / 2, panelHpx};
    lay.appsHeader = RectI{0, barH + (int)padY, lay.width / 2, (int)headerH};
    lay.webHeader = RectI{lay.width / 2, barH + (int)padY, lay.width - lay.width / 2, (int)headerH};
    lay.appRowH = (int)rowH;
    lay.webRowH = (int)rowH;
    lay.appRowsVisible = appsVisible;
    lay.webRowsVisible = webVisible;
    lay.appScroll = m.apps.scroll;
    lay.webScroll = m.web.scroll;
    lay.dividerX = lay.width / 2;
    lay.statusLine = RectI{0, barH + (int)(padY + headerH + rowsH), lay.width, (int)statusH};
    return lay;
}

//======================================================================
//  Drawing
//======================================================================
void Renderer::DrawScene(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay) {
    m_iconDraws.clear();

    DrawFrame(g, m, lay);
    DrawBar(g, m, lay);
    if (!m.panelOpen) return;

    DrawPane(g, m, lay, m.apps, true, lay.appScroll, lay.appRowsVisible, m.activePane == 0);
    DrawPane(g, m, lay, m.web, false, lay.webScroll, lay.webRowsVisible, m.activePane == 1);

    //  The status line under the two panes.
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    DrawSpaced(g, m.hint,
               Gdiplus::RectF((float)(lay.statusLine.x + 10), (float)lay.statusLine.y,
                              (float)(lay.statusLine.w - 20), (float)lay.statusLine.h),
               m_fontSmall.get(), ColScaled(kTextDim, 150), m_advanceSmall, 0.8f * m_scale);

    //  The icons are drawn last, in one go, so that the handover from
    //  GDI+ to GDI happens once for the frame instead of once per row.
    FlushIcons(g);
}

//  Draws the collected row icons with a single device context.
//
//  Every GetHDC is a handover from GDI+ to GDI, and it flushes the
//  pipeline both ways.  Doing it once per row cost more than the icons
//  themselves, so the rows only record where their icon goes and the
//  whole set is drawn here.
void Renderer::FlushIcons(Gdiplus::Graphics& g) {
    if (m_iconDraws.empty()) return;
    HDC hdc = g.GetHDC();
    if (hdc) {
        for (const IconDraw& d : m_iconDraws) {
            if (!d.icon) continue;
            DrawIconEx(hdc, d.x, d.y, d.icon, d.size, d.size, 0, nullptr, DI_NORMAL);
        }
        g.ReleaseHDC(hdc);
    }
    m_iconDraws.clear();
}

void Renderer::DrawBracket(Gdiplus::Graphics& g, const RectI& r, const Gdiplus::Color& color) {
    const float s = m_scale;
    const float arm = kBracketArm * s;
    const float inset = kBracketInset * s;
    const float thick = std::max(1.0f, kBracketThick * s);

    Gdiplus::Pen pen(color, thick);
    pen.SetStartCap(Gdiplus::LineCapSquare);
    pen.SetEndCap(Gdiplus::LineCapSquare);

    const float x0 = (float)r.x + inset, y0 = (float)r.y + inset;
    const float x1 = (float)r.x + r.w - inset, y1 = (float)r.y + r.h - inset;
    g.DrawLine(&pen, x0, y0, x0 + arm, y0);
    g.DrawLine(&pen, x0, y0, x0, y0 + arm);
    g.DrawLine(&pen, x1, y0, x1 - arm, y0);
    g.DrawLine(&pen, x1, y0, x1, y0 + arm);
    g.DrawLine(&pen, x0, y1, x0 + arm, y1);
    g.DrawLine(&pen, x0, y1, x0, y1 - arm);
    g.DrawLine(&pen, x1, y1, x1 - arm, y1);
    g.DrawLine(&pen, x1, y1, x1, y1 - arm);
}

void Renderer::DrawReactor(Gdiplus::Graphics& g, float cx, float cy, float r) {
    const float s = m_scale;

    //  The outer ring is a dashed circle: every third slot of the ring is
    //  left dark, which is what gives the reticle its segmented look.
    Gdiplus::Pen ringPen(Col(kReactorRing), std::max(1.0f, 1.3f * s));
    ringPen.SetStartCap(Gdiplus::LineCapFlat);
    ringPen.SetEndCap(Gdiplus::LineCapFlat);
    const float slot = 360.0f / (float)kRingSlots;
    for (int i = 0; i < kRingSlots; ++i) {
        if (i % 3 == 2) continue;
        g.DrawArc(&ringPen, cx - r, cy - r, r * 2.0f, r * 2.0f, (float)i * slot, slot * 0.7f);
    }

    //  A mid ring of four counter placed arcs.
    Gdiplus::Pen midPen(Col(kReactorGlow), std::max(1.0f, 1.0f * s));
    const float rm = r * 0.70f;
    for (int i = 0; i < 4; ++i) {
        g.DrawArc(&midPen, cx - rm, cy - rm, rm * 2.0f, rm * 2.0f, (float)i * 90.0f + 14.0f, 56.0f);
    }

    //  Eight radial ticks, every third one picked out in amber.
    Gdiplus::Pen tickPen(Col(kReactorGlow), std::max(1.0f, 1.0f * s));
    Gdiplus::Pen amberPen(Col(kAccentAmber), std::max(1.0f, 1.2f * s));
    const float t0 = r * 1.10f, t1 = r * 1.36f;
    for (int i = 0; i < kReactorTicks; ++i) {
        const double a = (double)i * (360.0 / kReactorTicks) * 3.14159265358979323846 / 180.0;
        const float dx = (float)std::cos(a), dy = (float)std::sin(a);
        Gdiplus::Pen& p = (i % 3 == 0) ? amberPen : tickPen;
        g.DrawLine(&p, cx + dx * t0, cy + dy * t0, cx + dx * t1, cy + dy * t1);
    }

    //  The hot core: a radial falloff from white through the glow colour
    //  to nothing.  A path gradient is the only GDI+ primitive that can
    //  do this in one pass.
    {
        const float rg = r * 0.56f;
        Gdiplus::GraphicsPath gp;
        gp.AddEllipse(cx - rg, cy - rg, rg * 2.0f, rg * 2.0f);
        Gdiplus::PathGradientBrush br(&gp);
        br.SetCenterPoint(Gdiplus::PointF(cx, cy));
        br.SetFocusScales(0.0f, 0.0f);
        br.SetCenterColor(Gdiplus::Color(255, 255, 255, 255));
        const Gdiplus::Color surround(0, 125, 249, 255);
        INT surroundCount = 1;
        br.SetSurroundColors(&surround, &surroundCount);
        g.FillPath(&br, &gp);
    }
    {
        const float rc = r * 0.17f;
        Gdiplus::SolidBrush core(Col(kReactorCore));
        g.FillEllipse(&core, cx - rc, cy - rc, rc * 2.0f, rc * 2.0f);
    }
}

void Renderer::DrawFrame(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay) {
    const float s = m_scale;
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::GraphicsPath panel;
    BuildRoundedRect(panel, 0.5f, 0.5f, (float)lay.width - 1.0f, (float)lay.height - 1.0f,
                     kCornerRadius * s);

    //  The fill goes down first, then the grid is drawn over it, then the
    //  border and the brackets go on top.
    Gdiplus::SolidBrush fill(Col(kPanelFill));
    g.FillPath(&fill, &panel);

    {
        Gdiplus::GraphicsState state = g.Save();
        g.SetClip(&panel);
        Gdiplus::Pen gridPen(Col(kGridLine), 1.0f);
        const float step = kGridStep * s;
        for (float x = step; x < (float)lay.width; x += step)
            g.DrawLine(&gridPen, x, 0.0f, x, (float)lay.height);
        for (float y = step; y < (float)lay.height; y += step)
            g.DrawLine(&gridPen, 0.0f, y, (float)lay.width, y);
        g.Restore(state);
    }

    Gdiplus::Pen edge(Col(kPanelEdge), 1.0f);
    g.DrawPath(&edge, &panel);

    //  The separator between the bar and the results panel.
    if (m.panelOpen) {
        Gdiplus::Pen sep(ColScaled(kDivider, 130), 1.0f);
        g.DrawLine(&sep, 1.0f, (float)lay.bar.h, (float)lay.width - 1.0f, (float)lay.bar.h);
        //  The divider between the two panes.
        Gdiplus::Pen div(ColScaled(kDivider, 90), 1.0f);
        g.DrawLine(&div, (float)lay.dividerX, (float)lay.panel.y,
                   (float)lay.dividerX, (float)(lay.panel.y + lay.panel.h - 1));
    }

    DrawBracket(g, lay.panel, Col(kBracket));
}

void Renderer::DrawBar(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay) {
    const float s = m_scale;
    const float barH = (float)lay.bar.h;

    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    if (m_cfg.showReactor) {
        DrawReactor(g, kReactorCx * s, barH * 0.5f, kReactorR * s);
    }

    //  The clock is measured first so that the query field can be kept
    //  clear of it.
    float clockW = 0.0f;
    if (m_cfg.showClock && !m.clock.empty())
        clockW = (float)m.clock.size() * m_advanceLabel + 8.0f * s;

    const float promptX = kPromptX * s;
    Gdiplus::SolidBrush promptBrush(Col(kReactorRing));
    g.DrawString(L">", -1, m_fontQuery.get(),
                 Gdiplus::RectF(promptX, 0.0f, 40.0f * s, barH), &fmt, &promptBrush);

    const float textX = kTextX * s;
    const float textW = (float)lay.width - textX - kBarPadX * s - clockW;

    if (m.query.empty()) {
        Gdiplus::SolidBrush dim(ColScaled(kTextDim, 140));
        g.DrawString(L"SEARCH APPS OR THE WEB", -1, m_fontQuery.get(),
                     Gdiplus::RectF(textX, 0.0f, textW, barH), &fmt, &dim);
    } else {
        Gdiplus::SolidBrush primary(Col(kTextPrimary));
        g.DrawString(m.query.c_str(), -1, m_fontQuery.get(),
                     Gdiplus::RectF(textX, 0.0f, textW, barH), &fmt, &primary);
        if (m.caretOn && m.focused) {
            const int caret = std::max(0, std::min((int)m.query.size(), m.caret));
            const float cx = textX + (float)caret * m_advanceQuery;
            const float ch = m_fontQuery->GetHeight(&g) * 0.80f;
            Gdiplus::SolidBrush caretBrush(Col(kReactorRing));
            g.FillRectangle(&caretBrush, cx, barH * 0.5f - ch * 0.5f,
                            std::max(1.0f, 1.6f * s), ch);
        }
    }

    if (m_cfg.showClock && !m.clock.empty()) {
        Gdiplus::SolidBrush clockBrush(Col(kTextDim));
        const float cw = (float)m.clock.size() * m_advanceLabel;
        const float cx = (float)lay.width - kBarPadX * s - cw;
        g.DrawString(m.clock.c_str(), -1, m_fontLabel.get(),
                     Gdiplus::RectF(cx, 0.0f, cw + 4.0f, barH), &fmt, &clockBrush);
    }
}

void Renderer::DrawPane(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay,
                        const UiPane& pane, bool isApps, int firstRow, int rowCount, bool active) {
    const float s = m_scale;
    const RectI paneRect = isApps ? lay.appsPane : lay.webPane;
    const RectI headerRect = isApps ? lay.appsHeader : lay.webHeader;

    DrawSpaced(g, pane.title,
               Gdiplus::RectF((float)(paneRect.x + kRowPadX * s),
                              (float)headerRect.y, (float)(paneRect.w - 2 * kRowPadX * s),
                              (float)headerRect.h),
               m_fontLabel.get(), Col(kTextDim), m_advanceLabel, 1.6f * s);

    Gdiplus::Pen hair(ColScaled(kDivider, 90), 1.0f);
    const float hy = (float)(headerRect.y + headerRect.h);
    g.DrawLine(&hair, (float)(paneRect.x + kRowPadX * s), hy,
               (float)(paneRect.x + paneRect.w - kRowPadX * s), hy);

    for (int i = 0; i < rowCount; ++i) {
        const int idx = firstRow + i;
        if (idx < 0 || idx >= (int)pane.rows.size()) continue;
        const RectI rect = isApps ? lay.AppRowRect(idx) : lay.WebRowRect(idx);
        DrawRow(g, m, lay, pane.rows[(size_t)idx], rect, pane.selected == idx, active);
    }
}

void Renderer::DrawRow(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay,
                       const UiRow& row, const RectI& rect, bool selected, bool active) {
    (void)m;
    (void)lay;
    const float s = m_scale;
    const float rowH = (float)rect.h;

    if (selected) {
        Gdiplus::SolidBrush sel(ColScaled(kSelection, active ? 200 : 80));
        g.FillRectangle(&sel, (float)rect.x, (float)rect.y, (float)rect.w, rowH);
        Gdiplus::SolidBrush edge(Col(active ? kReactorRing : kReactorGlow));
        g.FillRectangle(&edge, (float)rect.x, (float)rect.y, std::max(2.0f, 2.0f * s), rowH);
    }

    float textX = (float)rect.x + 2.0f * s;
    if (row.iconSlot) {
        const float is = kRowIconSize * s;
        const float iy = (float)rect.y + (rowH - is) * 0.5f;
        if (row.icon) {
            //  Only note where the icon goes; the frame draws them all
            //  at once, when it has a device context of its own.
            IconDraw d;
            d.icon = row.icon;
            d.x = (int)((float)rect.x + 2.0f * s);
            d.y = (int)iy;
            d.size = (int)is;
            m_iconDraws.push_back(d);
        }
        textX = (float)rect.x + is + kIconGap * s + 2.0f * s;
    }

    //  The sub label is right aligned and dim; the label takes what is
    //  left of the row.
    float subW = 0.0f;
    if (!row.sublabel.empty()) subW = (float)row.sublabel.size() * m_advanceSmall + 10.0f * s;

    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    DrawHighlighted(g, row.label, row.highlight, row.highlightCount,
                    Gdiplus::RectF(textX, (float)rect.y,
                                   (float)rect.x + (float)rect.w - textX - subW, rowH),
                    m_fontRow.get(),
                    Col(selected && active ? kTextBright : kTextPrimary),
                    Col(kTextBright), m_advanceRow);

    if (!row.sublabel.empty()) {
        Gdiplus::SolidBrush dim(ColScaled(kTextDim, 170));
        const float sw = (float)row.sublabel.size() * m_advanceSmall;
        const float sx = (float)rect.x + (float)rect.w - sw - 2.0f * s;
        g.DrawString(row.sublabel.c_str(), -1, m_fontSmall.get(),
                     Gdiplus::RectF(sx, (float)rect.y, sw + 4.0f, rowH), &fmt, &dim);
    }
}

void Renderer::DrawHighlighted(Gdiplus::Graphics& g, const std::wstring& text,
                               const int* highlight, int count, const Gdiplus::RectF& box,
                               const Gdiplus::Font* font,
                               const Gdiplus::Color& normal, const Gdiplus::Color& hot,
                               float advance) {
    if (text.empty() || !font) return;

    //  The label has to give way to the sub label on the right, so it is
    //  cut to the number of characters that still fit.  The font is
    //  monospaced, so the cell advance gives the exact count.
    std::wstring shown = text;
    if (advance > 0.5f) {
        const size_t fits = (size_t)(box.Width / advance);
        if (fits > 0 && shown.size() > fits) shown.resize(fits);
    }

    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoFitBlackBox);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    Gdiplus::SolidBrush nb(normal);
    g.DrawString(shown.c_str(), -1, font, box, &fmt, &nb);

    if (count <= 0 || !highlight) return;

    //  The matched characters are drawn a second time on top in the hot
    //  colour.  The indices refer to the lower cased text, which has the
    //  same length as the original, so they transfer directly.
    bool inRun = false;
    int runStart = 0;
    for (int i = 0; i <= count; ++i) {
        const bool hit = (i < count);
        if (hit && !inRun) {
            inRun = true;
            runStart = highlight[i];
        } else if (!hit && inRun) {
            inRun = false;
            const int end = highlight[i - 1];
            if (end >= runStart && (size_t)end < shown.size()) {
                //  The font is monospaced and its cell advance was
                //  measured for this very font, so the run lands
                //  exactly on the characters it covers.
                const std::wstring sub = shown.substr((size_t)runStart, (size_t)(end - runStart + 1));
                Gdiplus::RectF r(box.X + (float)runStart * advance, box.Y,
                                 (float)(end - runStart + 1) * advance + 1.0f, box.Height);
                Gdiplus::SolidBrush hb(hot);
                g.DrawString(sub.c_str(), -1, font, r, &fmt, &hb);
            }
        }
    }
}

void Renderer::DrawSpaced(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::RectF& box,
                          const Gdiplus::Font* font, const Gdiplus::Color& color, float advance,
                          float tracking) {
    if (text.empty() || !font) return;

    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    Gdiplus::SolidBrush brush(color);
    float x = box.X;
    for (size_t i = 0; i < text.size(); ++i) {
        const std::wstring ch(1, text[i]);
        Gdiplus::RectF r(x, box.Y, advance + 2.0f, box.Height);
        g.DrawString(ch.c_str(), -1, font, r, &fmt, &brush);
        x += advance + tracking;
    }
}

//======================================================================
//  Composition
//======================================================================
bool Renderer::ProbeGdiAlpha() {
    //  A 32bppARGB surface may store straight alpha or premultiplied
    //  alpha.  UpdateLayeredWindow always wants premultiplied data, so
    //  the convention of the installed GDI+ is measured once instead of
    //  being assumed.
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    if (probe.GetLastStatus() != Gdiplus::Ok) return true;
    {
        Gdiplus::Graphics g(&probe);
        g.Clear(Gdiplus::Color(128, 100, 150, 200));
    }
    Gdiplus::BitmapData bd;
    Gdiplus::Rect rect(0, 0, 1, 1);
    if (probe.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) !=
        Gdiplus::Ok)
        return true;
    const BYTE* p = (const BYTE*)bd.Scan0;
    const int b = p[0], gg = p[1], r = p[2];
    probe.UnlockBits(&bd);

    const int straightDist = std::abs(r - 100) + std::abs(gg - 150) + std::abs(b - 200);
    const int premulDist = std::abs(r - 50) + std::abs(gg - 75) + std::abs(b - 100);
    m_gdiPremultiplies = (premulDist < straightDist);
    return true;
}

void Renderer::BuildComposeTables() {
    //  What the card shows through an alpha.  In the premultiplied
    //  case that is the share of the card colour that the pixel does
    //  not cover; in the straight case it is the part of the card
    //  colour that is mixed in, before the division.
    const int cr = kPanelFill.r, cg = kPanelFill.g, cb = kPanelFill.b;
    for (int a = 0; a < 256; ++a) {
        const int inv = 255 - a;
        if (m_gdiPremultiplies) {
            m_cardB[a] = (WORD)((cb * inv + 127) / 255);
            m_cardG[a] = (WORD)((cg * inv + 127) / 255);
            m_cardR[a] = (WORD)((cr * inv + 127) / 255);
        } else {
            m_cardB[a] = (WORD)(cb * inv);
            m_cardG[a] = (WORD)(cg * inv);
            m_cardR[a] = (WORD)(cr * inv);
        }
    }

    //  The straight case needs the division by 255 as well.  The sum
    //  that is divided never passes 65025, so the table covers it.
    if (!m_gdiPremultiplies) {
        if (m_div255.size() != 65536) m_div255.resize(65536);
        for (int t = 0; t < 65536; ++t) m_div255[(size_t)t] = (BYTE)((t + 127) / 255);
    } else {
        m_div255.clear();
        m_div255.shrink_to_fit();
    }
}

void Renderer::ComposeOpaque(Gdiplus::Bitmap& src, BYTE* dst, int dstStride, int fade) {
    const int w = (int)src.GetWidth();
    const int h = (int)src.GetHeight();
    if (w <= 0 || h <= 0) return;

    Gdiplus::BitmapData bd;
    Gdiplus::Rect rect(0, 0, w, h);
    if (src.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) !=
        Gdiplus::Ok)
        return;

    //  The card colour is what the panel is made of, and it is also
    //  what the frame dissolves into.  The rounded corners of the
    //  card are therefore filled with it as well, and the window
    //  region cuts them out again.
    const int cr = kPanelFill.r, cg = kPanelFill.g, cb = kPanelFill.b;
    int f = fade;
    if (f < 0)   f = 0;
    if (f > 255) f = 255;
    const int finv = 255 - f;

    //  The fade tables are short and are rebuilt for the frame.  The
    //  fade mixes the finished pixel back towards the card, which is
    //  how the resting bar looks softer than the one being used.
    for (int v = 0; v < 256; ++v) {
        m_fadeB[v] = (BYTE)((v * f + cb * finv + 127) / 255);
        m_fadeG[v] = (BYTE)((v * f + cg * finv + 127) / 255);
        m_fadeR[v] = (BYTE)((v * f + cr * finv + 127) / 255);
    }

    const WORD* cardB = m_cardB;
    const WORD* cardG = m_cardG;
    const WORD* cardR = m_cardR;
    const BYTE* fadeB = m_fadeB;
    const BYTE* fadeG = m_fadeG;
    const BYTE* fadeR = m_fadeR;

    if (m_gdiPremultiplies) {
        for (int y = 0; y < h; ++y) {
            const BYTE* s = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            BYTE* d = dst + (size_t)y * dstStride;
            for (int x = 0; x < w; ++x) {
                const int a = s[3];
                //  The colour already carries the alpha, so only the
                //  part of the card that shows through is added.
                int ib = s[0] + cardB[a];
                int ig = s[1] + cardG[a];
                int ir = s[2] + cardR[a];
                if (ib > 255) ib = 255;
                if (ig > 255) ig = 255;
                if (ir > 255) ir = 255;
                d[0] = fadeB[ib];
                d[1] = fadeG[ig];
                d[2] = fadeR[ir];
                d[3] = 255;
                s += 4;
                d += 4;
            }
        }
    } else {
        const BYTE* div = m_div255.data();
        for (int y = 0; y < h; ++y) {
            const BYTE* s = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
            BYTE* d = dst + (size_t)y * dstStride;
            for (int x = 0; x < w; ++x) {
                const int a = s[3];
                d[0] = fadeB[div[s[0] * a + cardB[a]]];
                d[1] = fadeG[div[s[1] * a + cardG[a]]];
                d[2] = fadeR[div[s[2] * a + cardR[a]]];
                d[3] = 255;
                s += 4;
                d += 4;
            }
        }
    }
    src.UnlockBits(&bd);
}

bool Renderer::Present(HWND hwnd, const UiModel& model, const Layout& layout, int fade) {
    if (!hwnd || layout.width <= 0 || layout.height <= 0) return false;

    //  The frame, the surface and the window region only change when
    //  the bar changes size, which is when the results panel opens and
    //  closes.  They are kept between frames: a fresh two megabyte
    //  bitmap, a fresh device context and a fresh window region on
    //  every keystroke is a lot of work for nothing, and the bar is
    //  redrawn for every letter that is typed.
    const bool sizeChanged = (layout.width != m_lastWidth || layout.height != m_lastHeight);
    if (sizeChanged) {
        m_frame.reset(new Gdiplus::Bitmap((INT)layout.width, (INT)layout.height,
                                           PixelFormat32bppARGB));
        if (!m_frame || m_frame->GetLastStatus() != Gdiplus::Ok) {
            m_frame.reset();
            return false;
        }
    }
    if (!m_frame) return false;

    {
        Gdiplus::Graphics g(m_frame.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        //  The high quality compositing costs about twenty five
        //  milliseconds a frame on the panel, which is more than
        //  everything else in the frame put together, and the panel is
        //  opaque: there is nothing for it to composite carefully.
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        DrawScene(g, model, layout);
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    //  The frame is flattened onto the window surface, so the panel is
    //  a solid card and only its contents are softened by the fade.
    if (!m_screenDc) m_screenDc = GetDC(nullptr);
    if (!m_screenDc) return false;
    if (sizeChanged || m_surface.w != layout.width || m_surface.h != layout.height) {
        if (!m_surface.Create(m_screenDc, layout.width, layout.height)) return false;
    }
    ComposeOpaque(*m_frame, m_surface.bits, layout.width * 4, fade);

    //  The region is the shape of the card, so it only changes with
    //  the size of the bar.
    if (sizeChanged) ApplyRegion(hwnd, layout);
    InvalidateRect(hwnd, nullptr, FALSE);
    //  The frame has to go on the screen now.  The system only sends
    //  WM_PAINT when there is nothing else in the message queue, so
    //  without this a letter typed while the bar was busy would wait
    //  for the typing to stop before it appeared.
    UpdateWindow(hwnd);

    m_lastWidth = layout.width;
    m_lastHeight = layout.height;
    return true;
}

void Renderer::Blit(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (dc && m_surface.dc && m_surface.w > 0 && m_surface.h > 0) {
        BitBlt(dc, 0, 0, m_surface.w, m_surface.h, m_surface.dc, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

void Renderer::ApplyRegion(HWND hwnd, const Layout& layout) {
    if (!hwnd || layout.width <= 0 || layout.height <= 0) return;
    const int radius = (int)std::lround(kCornerRadius * m_scale);
    HRGN rgn = CreateRoundRectRgn(0, 0, layout.width + 1, layout.height + 1, radius * 2,
                                  radius * 2);
    if (!rgn) return;
    //  The window keeps the region, so it has to be deleted only after
    //  the call has taken it over.
    if (SetWindowRgn(hwnd, rgn, TRUE) == 0) DeleteObject(rgn);
}

bool Renderer::DumpPng(const UiModel& model, const Layout& layout, const std::wstring& path) {
    if (layout.width <= 0 || layout.height <= 0) return false;

    Gdiplus::Bitmap frame((INT)layout.width, (INT)layout.height, PixelFormat32bppARGB);
    if (frame.GetLastStatus() != Gdiplus::Ok) return false;

    {
        Gdiplus::Graphics g(&frame);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        DrawScene(g, model, layout);
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    //  The dump is taken from the very same buffer that is painted
    //  into the window, so a screenshot proves what the screen shows.
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    DibSurface dib;
    if (!dib.Create(screen, layout.width, layout.height)) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    ComposeOpaque(frame, dib.bits, layout.width * 4, 255);
    ReleaseDC(nullptr, screen);

    //  WritePngRgb expects a premultiplied BGRA buffer; the surface
    //  is already opaque, so the bytes are copied as they are.  The
    //  corners are painted black, because the window region cuts them
    //  out and the screen never shows them.
    std::vector<BYTE> bgra((size_t)layout.width * (size_t)layout.height * 4);
    memcpy(bgra.data(), dib.bits, bgra.size());
    const float radius = kCornerRadius * m_scale;
    for (int y = 0; y < (int)layout.height; ++y) {
        for (int x = 0; x < (int)layout.width; ++x) {
            if (InsideRoundedRect((float)x + 0.5f, (float)y + 0.5f, 0.5f, 0.5f,
                                  (float)layout.width - 1.0f, (float)layout.height - 1.0f,
                                  radius))
                continue;
            BYTE* p = &bgra[((size_t)y * (size_t)layout.width + (size_t)x) * 4];
            p[0] = p[1] = p[2] = 0;
        }
    }
    return WritePngRgb(path, bgra.data(), layout.width, layout.height);
}

}  // namespace js

