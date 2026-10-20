//======================================================================
//  JARVIS SEARCH  -  the view layer.
//
//  Everything the user sees is drawn from code with GDI+ primitives.
//  No bitmap or sprite asset is shipped.
//
//  The frame is composed on a 32 bit ARGB surface and then flattened
//  onto an opaque one, because the bar is an ordinary window that has
//  to sit in the desktop band below every application.  An ordinary
//  window cannot be translucent, so the panel is solid and the parts
//  of it that are meant to be soft are mixed towards the card colour
//  instead.
//
//  The renderer owns the layout: it computes where every pane, header
//  and row sits, and hands that layout back to the controller so that
//  hit testing can never drift away from what is actually drawn.
//======================================================================
#ifndef JARVIS_SEARCH_RENDER_H_
#define JARVIS_SEARCH_RENDER_H_

#include "jarvis.h"

#include <gdiplus.h>

#include <memory>
#include <string>
#include <vector>

namespace js {

//======================================================================
//  View model
//======================================================================
struct UiRow {
    std::wstring label;
    std::wstring sublabel;
    HICON        icon = nullptr;   //  borrowed, never owned by the renderer
    int          highlight[64] = {0};
    int          highlightCount = 0;
};

struct UiPane {
    std::wstring       title;
    std::vector<UiRow> rows;
    int                selected = 0;
    int                scroll = 0;
};

struct UiModel {
    std::wstring query;
    std::wstring clock;
    std::wstring hint;         //  status line at the bottom of the panel
    bool        focused = false;
    bool        panelOpen = false;
    bool        caretOn = true;
    int         caret = 0;      //  caret index inside the query
    int         activePane = 0;  //  0 = applications, 1 = web search
    UiPane      apps;
    UiPane      web;
};

//======================================================================
//  Layout, in device pixels
//======================================================================
struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
    bool Contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Layout {
    int   width = 0;
    int   height = 0;
    float scale = 1.0f;

    RectI bar;             //  the input bar
    RectI panel;           //  the results panel, empty when closed
    RectI appsPane;
    RectI webPane;
    RectI appsHeader;
    RectI webHeader;
    RectI statusLine;

    int   appRowH = 0;
    int   webRowH = 0;
    int   appRowsVisible = 0;
    int   webRowsVisible = 0;
    int   appScroll = 0;
    int   webScroll = 0;
    int   dividerX = 0;

    RectI AppRowRect(int i) const;
    RectI WebRowRect(int i) const;
};

//======================================================================
//  A 32 bit DIB section plus the memory DC that holds it.
//
//  The surface is top down, so the image rows are stored the way they
//  are drawn and the window can be painted with a plain BitBlt.
//======================================================================
struct DibSurface {
    HDC      dc   = nullptr;
    HBITMAP  bmp  = nullptr;
    HGDIOBJ  old  = nullptr;
    BYTE*    bits = nullptr;
    int      w = 0, h = 0;

    bool Create(HDC screen, int width, int height);
    void Destroy();

    ~DibSurface() { Destroy(); }
    DibSurface() = default;
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;
};

//======================================================================
//  Renderer
//======================================================================
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    //  Starts GDI+ and resolves the font.  Returns false when the
    //  graphics stack is unavailable, in which case nothing can be
    //  drawn and the caller should give up quietly.
    bool Init(const Config& cfg, HWND hwnd);

    //  The device pixel ratio of the monitor the window is on.
    float Scale() const { return m_scale; }

    //  Recomputes the layout for a model.  The result is in device
    //  pixels and is the single source of truth for hit testing.
    Layout ComputeLayout(const UiModel& model) const;

    //  Draws the model into the window surface and asks for a repaint.
    //  `fade` mixes the finished frame towards the card colour: 255
    //  is the bar at full brightness, less makes it dissolve into the
    //  desktop while it rests there.
    bool Present(HWND hwnd, const UiModel& model, const Layout& layout, int fade);

    //  Paints the surface that Present prepared.  Called from WM_PAINT.
    void Blit(HWND hwnd);

    //  Clips the window to a rounded rectangle, so that the corners of
    //  the card are cut out of an ordinary, opaque window.
    void ApplyRegion(HWND hwnd, const Layout& layout);

    //  Writes the current model to a PNG file.  Used by the self test
    //  and by the screenshot tooling; never called at runtime.
    bool DumpPng(const UiModel& model, const Layout& layout, const std::wstring& path);

    //  The surface height the last Present used, so the controller can
    //  resize the window to fit the panel.
    int  LastWidth() const { return m_lastWidth; }
    int  LastHeight() const { return m_lastHeight; }

private:
    //  Draws the complete scene: frame, bar, both panes and the
    //  status line.  The live path and the PNG dump share it so that
    //  the screenshot can never drift away from the screen.
    void DrawScene(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay);

    //  Drawing helpers
    void DrawFrame(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay);
    void DrawBar(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay);
    void DrawReactor(Gdiplus::Graphics& g, float cx, float cy, float r);
    void DrawPane(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay,
                  const UiPane& pane, bool isApps, int firstRow, int rowCount, bool active);
    void DrawRow(Gdiplus::Graphics& g, const UiModel& m, const Layout& lay,
                 const UiRow& row, const RectI& rect, bool selected, bool active);
    void DrawHighlighted(Gdiplus::Graphics& g, const std::wstring& text,
                         const int* highlight, int count, const Gdiplus::RectF& box,
                         const Gdiplus::Font* font,
                         const Gdiplus::Color& normal, const Gdiplus::Color& hot,
                         float advance);
    void DrawSpaced(Gdiplus::Graphics& g, const std::wstring& text, const Gdiplus::RectF& box,
                    const Gdiplus::Font* font, const Gdiplus::Color& color, float advance,
                    float tracking);
    void DrawBracket(Gdiplus::Graphics& g, const RectI& r, const Gdiplus::Color& color);

    //  Converts the composed surface into an opaque one: the alpha is
    //  resolved against the card colour and the result is mixed
    //  towards it by `fade`.  GDI+ may store either straight or
    //  premultiplied samples in a 32bppARGB surface, so the convention
    //  is probed once at start-up.
    void ComposeOpaque(Gdiplus::Bitmap& src, BYTE* dst, int dstStride, int fade);
    bool ProbeGdiAlpha();

    //  Type
    bool ResolveFont();
    bool PrepareFonts();
    float MeasureAdvance(const Gdiplus::Font* font);
    float MeasureLineHeight();

    Config                    m_cfg;
    HWND                      m_hwnd = nullptr;
    float                     m_scale = 1.0f;
    std::wstring              m_familyName;
    std::unique_ptr<Gdiplus::FontFamily> m_family;
    std::unique_ptr<Gdiplus::Font> m_fontQuery;
    std::unique_ptr<Gdiplus::Font> m_fontRow;
    std::unique_ptr<Gdiplus::Font> m_fontLabel;
    std::unique_ptr<Gdiplus::Font> m_fontSmall;
    float                     m_advanceQuery = 0.0f;
    float                     m_advanceRow = 0.0f;
    float                     m_advanceLabel = 0.0f;
    float                     m_advanceSmall = 0.0f;
    bool                      m_gdiPremultiplies = true;
    int                       m_lastWidth = 0;
    int                       m_lastHeight = 0;
    DibSurface                m_surface;
};

}  // namespace js

#endif  //  JARVIS_SEARCH_RENDER_H_
