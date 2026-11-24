#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <vfw.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <climits>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "vfw32.lib")

using namespace Gdiplus;

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* APP_CLASS = L"ScreenshotEditorClass";
static const wchar_t* APP_TITLE = L"Screenshot Editor";

enum class Tool { Select, Pen, Line, Rectangle, Ellipse, Arrow, Eraser, Fill, Crop, Text, Callout, Highlighter, Pixelate, None };
enum class CaptureMode { None, Region };

struct Stroke {
    Tool tool;
    COLORREF color;
    int penWidth;
    std::vector<POINT> points;
    POINT startPt;
    POINT endPt;

    std::wstring text;
    int fontSize = 24;
    std::wstring fontName = L"Arial";
};

// A saved image state for undoing destructive image edits (rotate, filters...).
struct ImgSnap {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    std::vector<Stroke> strokes;   // annotations that were live at the time
    size_t strokeCount = 0;        // redo only: strokes present when it was undone
};

struct TabData {
    std::vector<ImgSnap> imgUndo;
    std::vector<ImgSnap> imgRedo;
    bool fitMode = true;           // keep fitting to the window when it resizes
    HBITMAP hBitmap = nullptr;
    int imgW = 0, imgH = 0;
    bool hasImage = false;
    std::vector<Stroke> strokes;
    std::vector<Stroke> redoStack;
    double zoom = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;
    int selIdx = -1;
    Stroke selBackup;
    int id = 0;
    std::wstring filename;
};

static const UINT WM_DEFER_FONTDLG = WM_APP + 1;

struct App {
    Tool currentTool = Tool::Select;
    COLORREF penColor = RGB(240, 200, 0);
    int penWidth = 3;
    bool isDrawing = false;
    bool isPanning = false;
    POINT panLast = {};
    POINT drawStart = {};
    POINT drawEnd = {};
    CaptureMode capMode = CaptureMode::None;

    HWND hwndMain = nullptr;
    HWND hwndCanvas = nullptr;
    HWND hStatusbar = nullptr;
    HWND hwndTooltip = nullptr;
    HWND hwndTab = nullptr;
    std::vector<HWND> tabCloseBtns;

    int toolbarH = 40;
    int tabH = 24;
    int hoverBtn = -1;

    int fontSize = 32;
    std::wstring fontName = L"Arial";

    bool isDraggingSel = false;
    bool isResizingSel = false;
    int resizeHandle = -1;
    POINT dragStartImg = {};
    Stroke selBackup;

    bool isCropping = false;
    POINT cropStart = {};
    POINT cropEnd = {};

    HWND hwndFontDlg = nullptr;

    std::vector<TabData> tabs;
    int activeTab = -1;
    int nextTabId = 1;

    struct TBButton {
        int id;
        int x, y, w, h;
        bool isSep;
        bool enabled;
    };
    std::vector<TBButton> tbBtns;

    bool isRecording = false;
    int recFrameW = 0, recFrameH = 0;
    int recMonX = 0, recMonY = 0;
    int recFrameCount = 0;
    PAVIFILE aviFile = nullptr;
    PAVISTREAM aviStream = nullptr;
    UINT_PTR recTimer = 0;
    std::vector<BYTE> recBuf24;
    CLSID recJpegClsid;
    std::vector<BYTE> recJpegBuf;
    int recRow24 = 0;

    bool isScrollCapturing = false;
    HWND scrollCapTarget = nullptr;
    std::vector<HBITMAP> scrollCapFrames;
    std::vector<int> scrollCapFrameY;
    std::vector<int> scrollCapFrameScroll;
    int scrollCapClientW = 0, scrollCapClientH = 0;
};


static App g;

static TabData* ActiveTab() {
    return (g.activeTab >= 0 && g.activeTab < (int)g.tabs.size()) ? &g.tabs[g.activeTab] : nullptr;
}
#define T ActiveTab()

static double PtSegDistSq(POINT p, POINT a, POINT b) {
    double dx = (double)(b.x - a.x);
    double dy = (double)(b.y - a.y);
    double lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-6) {
        double ex = (double)(p.x - a.x);
        double ey = (double)(p.y - a.y);
        return ex * ex + ey * ey;
    }
    double t = ((double)(p.x - a.x) * dx + (double)(p.y - a.y) * dy) / lenSq;
    t = max(0.0, min(1.0, t));
    double px = (double)a.x + t * dx;
    double py = (double)a.y + t * dy;
    double ex2 = (double)p.x - px;
    double ey2 = (double)p.y - py;
    return ex2 * ex2 + ey2 * ey2;
}

// ---------------------------------------------------------------------------
// Callout: one layout (in image coordinates) shared by drawing on screen,
// saving/flattening, hit testing and the selection box, so what you see is
// exactly what gets saved.
// ---------------------------------------------------------------------------
struct CalloutLayout {
    RECT bubble = {};                 // image coords
    int pad = 6;
    int lineH = 0;
    int radius = 8;
    std::vector<std::wstring> lines;  // already word-wrapped
    bool hasArrow = false;
    POINT tail = {};                  // where the arrow leaves the bubble
    POINT tip = {};                   // arrow tip (the pointed-at spot)
};

static const int CALLOUT_MAX_TEXT_W = 320;

static HFONT CreateAnnotFont(const std::wstring& name, int pixelHeight) {
    return CreateFont(-max(1, pixelHeight), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name.c_str());
}

static void ComputeCalloutLayout(const Stroke& s, CalloutLayout& L) {
    int fs = max(8, s.fontSize);
    HDC hdc = CreateCompatibleDC(nullptr);
    HFONT hf = CreateAnnotFont(s.fontName, fs);
    HFONT old = (HFONT)SelectObject(hdc, hf);
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    L.lineH = max(1, (int)(tm.tmHeight + tm.tmExternalLeading));
    L.pad = max(6, fs / 3);
    L.radius = max(6, fs / 2);
    L.lines.clear();

    auto width = [&](const std::wstring& str) {
        SIZE sz = { 0, 0 };
        if (!str.empty()) GetTextExtentPoint32(hdc, str.c_str(), (int)str.size(), &sz);
        return (int)sz.cx;
    };

    // Greedy word wrap per paragraph; very long words are broken by character.
    std::wstring text = s.text;
    size_t p = 0;
    int maxW = 0;
    while (true) {
        size_t nl = text.find(L'\n', p);
        std::wstring para = text.substr(p, nl == std::wstring::npos ? std::wstring::npos : nl - p);
        if (!para.empty() && para.back() == L'\r') para.pop_back();
        std::wstring line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(L' ', i);
            std::wstring word = para.substr(i, sp == std::wstring::npos ? std::wstring::npos : sp - i);
            std::wstring cand = line.empty() ? word : line + L" " + word;
            if (width(cand) <= CALLOUT_MAX_TEXT_W || line.empty()) {
                if (line.empty() && width(word) > CALLOUT_MAX_TEXT_W) {
                    // break a long word
                    std::wstring chunk;
                    for (wchar_t ch : word) {
                        if (!chunk.empty() && width(chunk + ch) > CALLOUT_MAX_TEXT_W) {
                            L.lines.push_back(chunk);
                            maxW = max(maxW, width(chunk));
                            chunk.clear();
                        }
                        chunk += ch;
                    }
                    line = chunk;
                } else {
                    line = cand;
                }
            } else {
                L.lines.push_back(line);
                maxW = max(maxW, width(line));
                line = word;
            }
            if (sp == std::wstring::npos) break;
            i = sp + 1;
        }
        L.lines.push_back(line);
        maxW = max(maxW, width(line));
        if (nl == std::wstring::npos) break;
        p = nl + 1;
    }
    SelectObject(hdc, old);
    DeleteObject(hf);
    DeleteDC(hdc);

    int bw = max(maxW, fs) + L.pad * 2;
    int bh = (int)L.lines.size() * L.lineH + L.pad * 2;
    L.bubble = { s.endPt.x - bw / 2, s.endPt.y - bh / 2,
                 s.endPt.x - bw / 2 + bw, s.endPt.y - bh / 2 + bh };

    // Arrow from the bubble edge (on the line toward the tip) to the tip.
    L.tip = s.startPt;
    POINT tipPt = s.startPt;
    L.hasArrow = !PtInRect(&L.bubble, tipPt);
    if (L.hasArrow) {
        double cx = s.endPt.x, cy = s.endPt.y;
        double dx = s.startPt.x - cx, dy = s.startPt.y - cy;
        double hw = bw / 2.0, hh = bh / 2.0;
        double tx = dx != 0 ? hw / fabs(dx) : 1e18;
        double ty = dy != 0 ? hh / fabs(dy) : 1e18;
        double tt = min(tx, ty);
        L.tail = { (int)lround(cx + dx * tt), (int)lround(cy + dy * tt) };
    }
}

// Draws a callout. Image point (x, y) maps to (x * zoom + ox, y * zoom + oy).
static void DrawCallout(HDC hdc, const Stroke& s, double zoom, double ox, double oy) {
    CalloutLayout L;
    ComputeCalloutLayout(s, L);
    auto X = [&](double x) { return (int)lround(x * zoom + ox); };
    auto Y = [&](double y) { return (int)lround(y * zoom + oy); };
    int pw = max(1, (int)lround(max(1, s.penWidth) * zoom));

    HPEN pen = CreatePen(PS_SOLID, pw, s.color);
    HBRUSH colorBr = CreateSolidBrush(s.color);
    HBRUSH whiteBr = CreateSolidBrush(RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, colorBr);

    // Arrow first, so the bubble covers the start of the shaft.
    if (L.hasArrow) {
        double tx = L.tip.x, ty = L.tip.y;
        double dx = tx - L.tail.x, dy = ty - L.tail.y;
        double len = sqrt(dx * dx + dy * dy);
        if (len > 0.5) {
            double nx = dx / len, ny = dy / len;
            double hl = min(len, max(10.0, 3.0 * max(1, s.penWidth) + 6.0));
            double hw = hl * 0.55;
            double bx = tx - nx * hl, by = ty - ny * hl;   // base of the head
            MoveToEx(hdc, X(L.tail.x), Y(L.tail.y), nullptr);
            LineTo(hdc, X(bx + nx * hl * 0.3), Y(by + ny * hl * 0.3));
            POINT head[3] = {
                { X(tx), Y(ty) },
                { X(bx + ny * hw), Y(by - nx * hw) },
                { X(bx - ny * hw), Y(by + nx * hw) }
            };
            Polygon(hdc, head, 3);
        }
    }

    SelectObject(hdc, whiteBr);
    int r = max(2, (int)lround(L.radius * zoom));
    RoundRect(hdc, X(L.bubble.left), Y(L.bubble.top), X(L.bubble.right), Y(L.bubble.bottom), r, r);

    int fsPx = (int)lround(max(8, s.fontSize) * zoom);
    if (fsPx >= 1) {
        HFONT hf = CreateAnnotFont(s.fontName, fsPx);
        HFONT oldF = (HFONT)SelectObject(hdc, hf);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, s.color);
        for (size_t i = 0; i < L.lines.size(); i++) {
            const std::wstring& ln = L.lines[i];
            if (ln.empty()) continue;
            TextOut(hdc, X(L.bubble.left + L.pad), Y(L.bubble.top + L.pad + (int)i * L.lineH),
                    ln.c_str(), (int)ln.size());
        }
        SelectObject(hdc, oldF);
        DeleteObject(hf);
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(pen);
    DeleteObject(colorBr);
    DeleteObject(whiteBr);
}

static void GetBBox(const Stroke& s, POINT& tl, POINT& br) {
    if (s.tool == Tool::Pen || s.tool == Tool::Eraser || s.tool == Tool::Highlighter) {
        if (s.points.empty()) { tl = br = { 0,0 }; return; }
        tl = br = s.points[0];
        for (size_t i = 1; i < s.points.size(); i++) {
            tl.x = min(tl.x, s.points[i].x); tl.y = min(tl.y, s.points[i].y);
            br.x = max(br.x, s.points[i].x); br.y = max(br.y, s.points[i].y);
        }
    } else if (s.tool == Tool::Text) {
        int fs = max(8, s.fontSize);
        HDC hdc = CreateCompatibleDC(nullptr);
        HFONT hf = CreateFont(-fs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
        HFONT old = (HFONT)SelectObject(hdc, hf);
        SIZE sz = { 0, 0 };
        if (!s.text.empty())
            GetTextExtentPoint32(hdc, s.text.c_str(), (int)s.text.size(), &sz);
        SelectObject(hdc, old);
        DeleteObject(hf);
        DeleteDC(hdc);
        int pad = 6;
        tl.x = s.startPt.x - pad; tl.y = s.startPt.y - fs - pad;
        br.x = s.startPt.x + sz.cx + pad; br.y = s.startPt.y + pad;
    } else if (s.tool == Tool::Callout) {
        CalloutLayout L;
        ComputeCalloutLayout(s, L);
        tl = { L.bubble.left, L.bubble.top };
        br = { L.bubble.right, L.bubble.bottom };
        if (L.hasArrow) {
            tl.x = min(tl.x, L.tip.x); tl.y = min(tl.y, L.tip.y);
            br.x = max(br.x, L.tip.x); br.y = max(br.y, L.tip.y);
        }
    } else {
        tl.x = min(s.startPt.x, s.endPt.x); tl.y = min(s.startPt.y, s.endPt.y);
        br.x = max(s.startPt.x, s.endPt.x); br.y = max(s.startPt.y, s.endPt.y);
    }
}

static int HitTestShape(POINT imgPt) {
    TabData* t = ActiveTab();
    if (!t) return -1;
    double tol = 8.0;
    for (int i = (int)t->strokes.size() - 1; i >= 0; i--) {
        const auto& s = t->strokes[i];
        switch (s.tool) {
        case Tool::Rectangle:
        case Tool::Fill: {
            POINT tl, br;
            GetBBox(s, tl, br);
            bool inLeft   = abs(imgPt.x - tl.x) <= tol && imgPt.y >= tl.y - tol && imgPt.y <= br.y + tol;
            bool inRight  = abs(imgPt.x - br.x) <= tol && imgPt.y >= tl.y - tol && imgPt.y <= br.y + tol;
            bool inTop    = abs(imgPt.y - tl.y) <= tol && imgPt.x >= tl.x - tol && imgPt.x <= br.x + tol;
            bool inBottom = abs(imgPt.y - br.y) <= tol && imgPt.x >= tl.x - tol && imgPt.x <= br.x + tol;
            if (s.tool == Tool::Fill) {
                if (imgPt.x >= tl.x && imgPt.x <= br.x && imgPt.y >= tl.y && imgPt.y <= br.y)
                    return i;
            } else {
                if (inLeft || inRight || inTop || inBottom) return i;
                if (imgPt.x >= tl.x && imgPt.x <= br.x && imgPt.y >= tl.y && imgPt.y <= br.y) return i;
            }
            break;
        }
        case Tool::Ellipse: {
            POINT tl, br;
            GetBBox(s, tl, br);
            double cx = (tl.x + br.x) / 2.0;
            double cy = (tl.y + br.y) / 2.0;
            double rx = (br.x - tl.x) / 2.0;
            double ry = (br.y - tl.y) / 2.0;
            if (rx < 1) rx = 1; if (ry < 1) ry = 1;
            double dx = (imgPt.x - cx) / rx;
            double dy = (imgPt.y - cy) / ry;
            double d = dx * dx + dy * dy;
            if (d <= 1.0 && d >= 0.6) return i;
            if (d <= 1.0) return i;
            break;
        }
        case Tool::Line:
        case Tool::Arrow: {
            double dSq = PtSegDistSq(imgPt, s.startPt, s.endPt);
            double penTol = max(tol, s.penWidth / 2.0 + tol);
            if (dSq <= penTol * penTol) return i;
            break;
        }
        case Tool::Pen:
        case Tool::Eraser:
        case Tool::Highlighter: {
            if (s.points.size() < 2) break;
            double penTol = max(tol, s.penWidth / 2.0 + tol);
            for (size_t j = 0; j < s.points.size() - 1; j++) {
                if (PtSegDistSq(imgPt, s.points[j], s.points[j + 1]) <= penTol * penTol)
                    return i;
            }
            break;
        }
        case Tool::Text: {
            POINT tl, br;
            GetBBox(s, tl, br);
            if (imgPt.x >= tl.x && imgPt.x <= br.x && imgPt.y >= tl.y && imgPt.y <= br.y)
                return i;
            break;
        }
        case Tool::Callout: {
            CalloutLayout L;
            ComputeCalloutLayout(s, L);
            RECT bb = L.bubble;
            InflateRect(&bb, 3, 3);
            if (PtInRect(&bb, imgPt)) return i;
            if (L.hasArrow) {
                double dSq = PtSegDistSq(imgPt, L.tail, L.tip);
                double penTol = max(tol, s.penWidth / 2.0 + tol);
                if (dSq <= penTol * penTol) return i;
            }
            break;
        }
        default: break;
        }
    }
    return -1;
}

static const int HANDLE_SIZE = 6;

static int HitTestHandle(POINT scrPt) {
    TabData* t = ActiveTab();
    if (!t || t->selIdx < 0 || t->selIdx >= (int)t->strokes.size()) return -1;
    const auto& s = t->strokes[t->selIdx];
    POINT tl, br;
    GetBBox(s, tl, br);
    POINT handles[8];
    handles[0] = { tl.x, tl.y };
    handles[1] = { (tl.x + br.x) / 2, tl.y };
    handles[2] = { br.x, tl.y };
    handles[3] = { tl.x, (tl.y + br.y) / 2 };
    handles[4] = { br.x, (tl.y + br.y) / 2 };
    handles[5] = { tl.x, br.y };
    handles[6] = { (tl.x + br.x) / 2, br.y };
    handles[7] = { br.x, br.y };

    double half = HANDLE_SIZE / t->zoom + 2.0;
    for (int i = 0; i < 8; i++) {
        double hx = handles[i].x * t->zoom + t->offsetX;
        double hy = handles[i].y * t->zoom + t->offsetY;
        if (abs(scrPt.x - hx) <= half && abs(scrPt.y - hy) <= half)
            return i;
    }
    return -1;
}

static void ResizeStroke(int idx, int handle, POINT newImgPt) {
    TabData* t = ActiveTab();
    if (!t || idx < 0 || idx >= (int)t->strokes.size()) return;
    auto& s = t->strokes[idx];
    const auto& bak = t->selBackup;
    // Start from the box the shape had when the drag began, so edges that
    // aren't being dragged stay put (shapes like callouts don't fill their box
    // exactly, which made them drift when based on the current box).
    POINT tl, br;
    GetBBox(bak, tl, br);

    POINT newTl = tl, newBr = br;
    switch (handle) {
    case 0: newTl = newImgPt; break;
    case 1: newTl.y = newImgPt.y; break;
    case 2: newBr.x = newImgPt.x; newTl.y = newImgPt.y; break;
    case 3: newTl.x = newImgPt.x; break;
    case 4: newBr.x = newImgPt.x; break;
    case 5: newTl.x = newImgPt.x; newBr.y = newImgPt.y; break;
    case 6: newBr.y = newImgPt.y; break;
    case 7: newBr = newImgPt; break;
    }
    if (newBr.x <= newTl.x + 2) { if (handle == 0 || handle == 3 || handle == 5) newTl.x = newBr.x - 2; else newBr.x = newTl.x + 2; }
    if (newBr.y <= newTl.y + 2) { if (handle == 0 || handle == 1 || handle == 2) newTl.y = newBr.y - 2; else newBr.y = newTl.y + 2; }

    POINT origTl, origBr;
    GetBBox(bak, origTl, origBr);

    double sx = origBr.x == origTl.x ? 1.0 : (double)(newBr.x - newTl.x) / (origBr.x - origTl.x);
    double sy = origBr.y == origTl.y ? 1.0 : (double)(newBr.y - newTl.y) / (origBr.y - origTl.y);

    if (s.tool == Tool::Pen || s.tool == Tool::Eraser || s.tool == Tool::Highlighter) {
        if (bak.points.empty()) return;
        for (size_t i = 0; i < s.points.size() && i < bak.points.size(); i++) {
            s.points[i].x = newTl.x + (int)((bak.points[i].x - origTl.x) * sx);
            s.points[i].y = newTl.y + (int)((bak.points[i].y - origTl.y) * sy);
        }
    } else if (s.tool == Tool::Text) {
        int dx = newTl.x - origTl.x;
        int dy = newTl.y - origTl.y;
        s.startPt.x = bak.startPt.x + dx;
        s.startPt.y = bak.startPt.y + dy;
    } else if (s.tool == Tool::Callout) {
        // Stretch the arrow with the box and scale the text with it.
        s.startPt.x = newTl.x + (int)lround((bak.startPt.x - origTl.x) * sx);
        s.startPt.y = newTl.y + (int)lround((bak.startPt.y - origTl.y) * sy);
        s.endPt.x = newTl.x + (int)lround((bak.endPt.x - origTl.x) * sx);
        s.endPt.y = newTl.y + (int)lround((bak.endPt.y - origTl.y) * sy);
        double fscale = sqrt(fabs(sx * sy));
        s.fontSize = max(8, min(200, (int)lround(bak.fontSize * fscale)));
    } else {
        s.startPt.x = newTl.x + (int)((bak.startPt.x - origTl.x) * sx);
        s.startPt.y = newTl.y + (int)((bak.startPt.y - origTl.y) * sy);
        s.endPt.x = newTl.x + (int)((bak.endPt.x - origTl.x) * sx);
        s.endPt.y = newTl.y + (int)((bak.endPt.y - origTl.y) * sy);
    }
}

static HBITMAP LoadImageFile(const wchar_t* path, int& w, int& h) {
    Gdiplus::Bitmap bmp(path);
    if (bmp.GetLastStatus() != Ok) return nullptr;
    w = bmp.GetWidth();
    h = bmp.GetHeight();
    HBITMAP hBmp = nullptr;
    bmp.GetHBITMAP(Color(255, 255, 255), &hBmp);
    return hBmp;
}

static bool SaveImageFile(HBITMAP hBmp, const wchar_t* path) {
    Gdiplus::Bitmap bmp(hBmp, nullptr);
    CLSID clsid;
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    if (_wcsicmp(ext, L".png") == 0)
        clsid = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    else if (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0)
        clsid = { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    else
        clsid = { 0x557cf400, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    return bmp.Save(path, &clsid, nullptr) == Ok;
}

static HBITMAP CaptureScreen() {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC hScr = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hScr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pvBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    HGDIOBJ old = SelectObject(hdc, hBmp);
    BitBlt(hdc, 0, 0, vw, vh, hScr, vx, vy, SRCCOPY);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hScr);
    return hBmp;
}

static void HideAndCapture(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    RedrawWindow(nullptr, nullptr, nullptr,
        RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_ERASE);
    DwmFlush();
    Sleep(1000);
}

static HBITMAP CaptureRegion(int x, int y, int w, int h) {
    HDC hScr = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hScr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pvBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    HGDIOBJ old = SelectObject(hdc, hBmp);
    BitBlt(hdc, 0, 0, w, h, hScr, x, y, SRCCOPY);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hScr);
    return hBmp;
}

static void FlattenToBitmap();
static void SetImage(HBITMAP hBmp, int w, int h);
static void FitToWindow();
static void UpdateStatus();

static HBITMAP CaptureWindowClient(HWND frame, void** outBits = nullptr, bool forceScreen = false) {
    if (!frame) return nullptr;
    RECT wr, cr;
    GetWindowRect(frame, &wr);
    GetClientRect(frame, &cr);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) return nullptr;

    POINT cTL = { 0, 0 };
    ClientToScreen(frame, &cTL);
    int ox = cTL.x - wr.left;
    int oy = cTL.y - wr.top;

    HDC hdcScreen = GetDC(nullptr);

    HDC hdcTmp = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmiFull = {};
    bmiFull.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmiFull.bmiHeader.biWidth = ww;
    bmiFull.bmiHeader.biHeight = -wh;
    bmiFull.bmiHeader.biPlanes = 1;
    bmiFull.bmiHeader.biBitCount = 32;
    bmiFull.bmiHeader.biCompression = BI_RGB;
    void* pvFull = nullptr;
    HBITMAP hBmpFull = CreateDIBSection(hdcTmp, &bmiFull, DIB_RGB_COLORS, &pvFull, nullptr, 0);
    HGDIOBJ oldTmp = SelectObject(hdcTmp, hBmpFull);

    if (forceScreen) {
        HBITMAP hScrFull = CaptureRegion(wr.left, wr.top, ww, wh);
        if (hScrFull) {
            HDC hdcScr = CreateCompatibleDC(nullptr);
            HGDIOBJ oldScr = SelectObject(hdcScr, hScrFull);
            BitBlt(hdcTmp, 0, 0, ww, wh, hdcScr, 0, 0, SRCCOPY);
            SelectObject(hdcScr, oldScr);
            DeleteDC(hdcScr);
            DeleteObject(hScrFull);
        }
    } else {
        BOOL ok = PrintWindow(frame, hdcTmp, PW_RENDERFULLCONTENT);
        if (!ok) {
            HDC hdcWnd = GetDC(frame);
            BitBlt(hdcTmp, 0, 0, ww, wh, hdcWnd, 0, 0, SRCCOPY);
            ReleaseDC(frame, hdcWnd);
        }
    }

    HDC hdcDst = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmiClient = {};
    bmiClient.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmiClient.bmiHeader.biWidth = cw;
    bmiClient.bmiHeader.biHeight = -ch;
    bmiClient.bmiHeader.biPlanes = 1;
    bmiClient.bmiHeader.biBitCount = 32;
    bmiClient.bmiHeader.biCompression = BI_RGB;
    void* pvClient = nullptr;
    HBITMAP hBmpClient = CreateDIBSection(hdcDst, &bmiClient, DIB_RGB_COLORS, &pvClient, nullptr, 0);
    HGDIOBJ oldDst = SelectObject(hdcDst, hBmpClient);
    BitBlt(hdcDst, 0, 0, cw, ch, hdcTmp, ox, oy, SRCCOPY);

    long long ink = 0;
    const BYTE* px = (const BYTE*)pvClient;
    for (long long i = 0; i < (long long)ch * cw; i++)
        ink += 765 - (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]);
    if (ink < (long long)ch * max(4, cw / 20)) {
        HBITMAP hScrBmp = CaptureRegion(cTL.x, cTL.y, cw, ch);
        if (hScrBmp) {
            HDC hdcScr = CreateCompatibleDC(nullptr);
            HGDIOBJ oldScr = SelectObject(hdcScr, hScrBmp);
            BitBlt(hdcDst, 0, 0, cw, ch, hdcScr, 0, 0, SRCCOPY);
            SelectObject(hdcScr, oldScr);
            DeleteDC(hdcScr);
            DeleteObject(hScrBmp);
        }
    }

    SelectObject(hdcTmp, oldTmp);
    SelectObject(hdcDst, oldDst);
    DeleteDC(hdcTmp);
    DeleteDC(hdcDst);
    ReleaseDC(nullptr, hdcScreen);
    DeleteObject(hBmpFull);

    if (outBits) *outBits = pvClient;
    return hBmpClient;
}

static HWND FindScrollableChild(HWND frame) {
    struct { int area; HWND hwnd; } data = { 0, nullptr };
    EnumChildWindows(frame, [](HWND hwnd, LPARAM lParam) -> BOOL {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int area = (rc.right - rc.left) * (rc.bottom - rc.top);
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        auto* d = (decltype(data)*)lParam;
        if (area > 1000 && (style & (WS_VSCROLL | WS_HSCROLL)) && area > d->area) {
            d->area = area;
            d->hwnd = hwnd;
        }
        return TRUE;
    }, (LPARAM)&data);
    if (data.hwnd) return data.hwnd;
    data.area = 0;
    data.hwnd = nullptr;
    EnumChildWindows(frame, [](HWND hwnd, LPARAM lParam) -> BOOL {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int area = (rc.right - rc.left) * (rc.bottom - rc.top);
        auto* d = (decltype(data)*)lParam;
        if (area > 5000 && area > d->area) {
            d->area = area;
            d->hwnd = hwnd;
        }
        return TRUE;
    }, (LPARAM)&data);
    return data.hwnd ? data.hwnd : frame;
}

// ---------------------------------------------------------------------------
// Scrolling capture
//
// Each captured frame is reduced to a per-row "signature" (column-bucket sums).
// Consecutive frames are compared to find the exact pixel shift of the
// scrolling content, ignoring fixed bands (toolbars, sticky headers, status
// bars) at the top/bottom and the scrollbar column. The final image is built
// from those measured shifts instead of guessed scroll amounts.
// ---------------------------------------------------------------------------

struct ShiftResult {
    bool ok = false;     // a consistent shift was found (0 = no scroll)
    int shift = 0;       // rows the content moved up
    int topStatic = 0;   // fixed rows at the top
    int bottomStatic = 0;// fixed rows at the bottom
};

static int GetWindowDpiSafe(HWND hwnd) {
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow p = (PFN_GetDpiForWindow)GetProcAddress(
        GetModuleHandle(L"user32.dll"), "GetDpiForWindow");
    UINT d = p ? p(hwnd) : 0;
    return d ? (int)d : 96;
}

static bool SigRowsEqual(const int* a, const int* b, int nb, int tol) {
    for (int k = 0; k < nb; k++) {
        int d = a[k] - b[k];
        if (d < -tol || d > tol) return false;
    }
    return true;
}

// Finds how far the content moved up between frame A and frame B
// (32bpp top-down pixels, same size). Only columns that actually change
// between the two frames are compared, so window borders, translucent
// (Mica) frames, static sidebars and blank margins are ignored. The shift is
// the one giving the longest consistent run of matching rows, so a changing
// title bar, hover highlight or small animation doesn't break the match.
static ShiftResult FindScrollShift(const BYTE* A, const BYTE* B, int w, int h,
                                   int x0, int x1, int expected) {
    ShiftResult res;
    x0 = max(0, x0);
    x1 = min(w, x1);
    if (h < 48 || x1 - x0 < 8) return res;
    const size_t stride = (size_t)w * 4;

    // 1. Columns whose pixels differ between the frames
    std::vector<char> colAct(w, 0);
    for (int y = 0; y < h; y++) {
        const BYTE* pa = A + (size_t)y * stride;
        const BYTE* pb = B + (size_t)y * stride;
        for (int x = x0; x < x1; x++) {
            if (colAct[x]) continue;
            const BYTE* qa = pa + x * 4;
            const BYTE* qb = pb + x * 4;
            int d = abs(qa[0] - qb[0]) + abs(qa[1] - qb[1]) + abs(qa[2] - qb[2]);
            if (d > 60) colAct[x] = 1;   // ignore faint noise (translucent frames)
        }
    }
    std::vector<int> cols;
    for (int x = x0; x < x1; x++)
        if (colAct[x]) cols.push_back(x);
    if ((int)cols.size() < 8) {
        res.ok = true; res.shift = 0; res.topStatic = h; res.bottomStatic = 0;
        return res;
    }

    // 2. Row signatures over the changing columns only
    const int nAct = (int)cols.size();
    const int nb = min(64, max(8, nAct / 6));
    const int bw = max(1, nAct / nb);
    std::vector<int> SA((size_t)h * nb, 0), SB((size_t)h * nb, 0);
    for (int y = 0; y < h; y++) {
        const BYTE* pa = A + (size_t)y * stride;
        const BYTE* pb = B + (size_t)y * stride;
        int* ra = &SA[(size_t)y * nb];
        int* rb = &SB[(size_t)y * nb];
        for (int i = 0; i < nAct; i++) {
            int k = (int)((long long)i * nb / nAct);
            const BYTE* qa = pa + cols[i] * 4;
            const BYTE* qb = pb + cols[i] * 4;
            ra[k] += qa[0] + qa[1] * 3 + qa[2] * 2;
            rb[k] += qb[0] + qb[1] * 3 + qb[2] * 2;
        }
    }
    auto rowA = [&](int y) { return &SA[(size_t)y * nb]; };
    auto rowB = [&](int y) { return &SB[(size_t)y * nb]; };

    for (int pass = 0; pass < 2; pass++) {
        // pass 0: exact, pass 1: tolerant (smooth scrolling, dithering)
        const int tol = (pass == 0) ? 0 : bw * 6 * 10 + 6 * 60;
        const int infoThresh = max(tol, bw * 6 * 3);

        // Fixed bands at top and bottom
        int t = 0;
        while (t < h && SigRowsEqual(rowA(t), rowB(t), nb, tol)) t++;
        int bs = 0;
        while (bs < h - t && SigRowsEqual(rowA(h - 1 - bs), rowB(h - 1 - bs), nb, tol)) bs++;
        if (h - t - bs < 8) {
            res.ok = true; res.shift = 0; res.topStatic = t; res.bottomStatic = bs;
            return res;
        }

        // Rows with enough detail to be matched unambiguously
        auto informative = [&](const int* r) {
            int mn = INT_MAX, mx = INT_MIN;
            for (int k = 0; k < nb; k++) { mn = min(mn, r[k]); mx = max(mx, r[k]); }
            return (mx - mn) > infoThresh;
        };
        std::vector<char> infoA(h), infoB(h);
        for (int y = 0; y < h; y++) {
            infoA[y] = informative(rowA(y)) ? 1 : 0;
            infoB[y] = informative(rowB(y)) ? 1 : 0;
        }

        // 3. For each shift, the best run of matching rows (+1 match, -3 mismatch)
        // Only rows inside the scrolling band are compared, so a fixed header
        // or footer can't make "no scroll" look like the best match.
        int bestScore = -1, bestShift = -1;
        const int bandEnd = h - bs;
        for (int d = 0; d <= bandEnd - t - 8; d++) {
            int cur = 0, best = 0;
            for (int y = t; y + d < bandEnd; y++) {
                if (!infoA[y + d] && !infoB[y]) continue;
                if (SigRowsEqual(rowA(y + d), rowB(y), nb, tol)) {
                    cur += 1;
                    if (cur > best) best = cur;
                } else {
                    cur -= 3;
                    if (cur < 0) cur = 0;
                }
            }
            bool better = false;
            if (bestShift < 0) better = true;
            else if (expected > 0) {
                // Prefer the shift near the expected step when its run is
                // comparably long (guards against repeated-looking content).
                bool nearD = abs(d - expected) <= max(8, expected / 6);
                bool nearB = abs(bestShift - expected) <= max(8, expected / 6);
                if (nearD && !nearB) better = best * 10 >= bestScore * 6;
                else if (!nearD && nearB) better = best * 6 > bestScore * 10;
                else better = best > bestScore + 2 ||
                    (best >= bestScore - 2 && abs(d - expected) < abs(bestShift - expected));
            } else {
                better = best > bestScore + 2;
            }
            if (better) {
                bestScore = best;
                bestShift = d;
            }
        }

        if (bestShift >= 0 && bestScore >= 8) {
            res.ok = true;
            res.shift = bestShift;
            res.topStatic = t;
            res.bottomStatic = bs;
            return res;
        }
    }
    return res;
}

static HBITMAP CreateDib32(int w, int h, void** bits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// frames[i] moved up by shifts[i] relative to frames[i-1]; `footer` fixed rows at the bottom.
static HBITMAP StitchScrollFrames(const std::vector<HBITMAP>& frames,
                                  const std::vector<int>& shifts, int footer) {
    if (frames.empty()) return nullptr;
    BITMAP bm0;
    GetObject(frames[0], sizeof(bm0), &bm0);
    int width = bm0.bmWidth, frameH = bm0.bmHeight;
    footer = max(0, min(footer, frameH / 2));
    int maxShift = 0;
    for (size_t i = 1; i < frames.size(); i++) maxShift = max(maxShift, shifts[i]);
    footer = min(footer, max(0, frameH - maxShift));
    int b = frameH - footer;

    long long totalH = frameH;
    for (size_t i = 1; i < frames.size(); i++) totalH += shifts[i];
    if (totalH <= 0 || totalH > 0x7FFFFFFF / 4 / max(1, width)) return nullptr;

    void* bits = nullptr;
    HBITMAP hResult = CreateDib32(width, (int)totalH, &bits);
    if (!hResult) return nullptr;

    HDC hdcDst = CreateCompatibleDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldDst = SelectObject(hdcDst, hResult);
    HGDIOBJ oldSrc = SelectObject(hdcSrc, frames[0]);

    // First frame: everything above the fixed footer
    BitBlt(hdcDst, 0, 0, width, b, hdcSrc, 0, 0, SRCCOPY);
    int dstY = b;
    // Each next frame contributes only the rows that newly scrolled into view
    for (size_t i = 1; i < frames.size(); i++) {
        int d = shifts[i];
        if (d <= 0) continue;
        int srcY = max(0, b - d);
        int copyH = b - srcY;
        SelectObject(hdcSrc, frames[i]);
        BitBlt(hdcDst, 0, dstY, width, copyH, hdcSrc, 0, srcY, SRCCOPY);
        dstY += d;
    }
    // Fixed footer from the last frame
    if (footer > 0) {
        SelectObject(hdcSrc, frames.back());
        BitBlt(hdcDst, 0, dstY, width, footer, hdcSrc, 0, b, SRCCOPY);
    }

    SelectObject(hdcSrc, oldSrc);
    SelectObject(hdcDst, oldDst);
    DeleteDC(hdcDst);
    DeleteDC(hdcSrc);
    return hResult;
}

// ESC handling for scrolling capture. A low-level keyboard hook on its own
// thread (with its own message loop) sees ESC no matter which window has
// focus and no matter how long the capture thread is busy.
static volatile LONG g_scrollCancel = 0;
static HHOOK g_scrollKbHook = nullptr;

static LRESULT CALLBACK ScrollKbProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        if (k->vkCode == VK_ESCAPE) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                InterlockedExchange(&g_scrollCancel, 1);
            return 1; // don't pass ESC to the window being captured
        }
    }
    return CallNextHookEx(g_scrollKbHook, nCode, wParam, lParam);
}

static DWORD WINAPI ScrollKbThread(LPVOID param) {
    MSG msg;
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE); // create the message queue
    g_scrollKbHook = SetWindowsHookEx(WH_KEYBOARD_LL, ScrollKbProc, GetModuleHandle(nullptr), 0);
    SetEvent((HANDLE)param);
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (g_scrollKbHook) UnhookWindowsHookEx(g_scrollKbHook);
    g_scrollKbHook = nullptr;
    return 0;
}

struct EscWatcher {
    HANDLE thread = nullptr;
    DWORD tid = 0;
    EscWatcher() {
        InterlockedExchange(&g_scrollCancel, 0);
        HANDLE ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        thread = CreateThread(nullptr, 0, ScrollKbThread, ready, 0, &tid);
        if (thread && ready) WaitForSingleObject(ready, 2000);
        if (ready) CloseHandle(ready);
    }
    ~EscWatcher() {
        if (thread) {
            PostThreadMessage(tid, WM_QUIT, 0, 0);
            WaitForSingleObject(thread, 2000);
            CloseHandle(thread);
        }
    }
};

static bool ScrollCancelled() { return g_scrollCancel != 0; }

static void CancelableSleep(int ms) {
    DWORD start = GetTickCount();
    while (!ScrollCancelled() && (int)(GetTickCount() - start) < ms)
        Sleep(min(10, max(1, ms)));
}

static HHOOK g_scrollPickHook = nullptr;
static HWND g_scrollPickResult = nullptr;
static bool g_scrollPickSwallowUp = false;

static LRESULT CALLBACK ScrollPickProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (wParam == WM_LBUTTONDOWN && !g_scrollPickSwallowUp) {
            MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
            HWND hit = WindowFromPoint(ms->pt);
            HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
            if (hit && root != g.hwndMain && !IsChild(g.hwndMain, hit)) {
                // Eat the click so it does not press a link/button in the target
                g_scrollPickResult = hit;
                g_scrollPickSwallowUp = true;
                return 1;
            }
        } else if (wParam == WM_LBUTTONUP && g_scrollPickSwallowUp) {
            g_scrollPickSwallowUp = false;
            HHOOK hk = g_scrollPickHook;
            g_scrollPickHook = nullptr;
            UnhookWindowsHookEx(hk);
            return 1;
        }
    }
    return CallNextHookEx(g_scrollPickHook, nCode, wParam, lParam);
}

static HWND PickWindowForScrollCapture() {
    g_scrollPickResult = nullptr;
    g_scrollPickSwallowUp = false;
    g_scrollPickHook = SetWindowsHookEx(WH_MOUSE_LL, ScrollPickProc,
        GetModuleHandle(nullptr), 0);
    if (!g_scrollPickHook) return nullptr;
    MSG msg;
    while (g_scrollPickHook) {
        if (ScrollCancelled()) {
            UnhookWindowsHookEx(g_scrollPickHook);
            g_scrollPickHook = nullptr;
            g_scrollPickResult = nullptr;
            break;
        }
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    g_scrollPickSwallowUp = false;
    return g_scrollPickResult;
}

static bool IsWindowOccluded(HWND frame) {
    RECT cr;
    GetClientRect(frame, &cr);
    POINT cTL = { 0, 0 };
    ClientToScreen(frame, &cTL);
    int cw = cr.right, ch = cr.bottom;
    if (cw < 20 || ch < 20) return true;
    const int nx = 7, ny = 7;
    for (int i = 0; i < nx; i++) {
        for (int j = 0; j < ny; j++) {
            POINT pt = {
                cTL.x + (int)((long long)(i + 1) * cw / (nx + 1)),
                cTL.y + (int)((long long)(j + 1) * ch / (ny + 1))
            };
            HWND h = WindowFromPoint(pt);
            if (!h) continue;
            HWND root = GetAncestor(h, GA_ROOT);
            if (root != frame) return true;
        }
    }
    return false;
}

static bool SameFrameBits(HBITMAP a, const void* ab, HBITMAP b, const void* bb) {
    if (!a || !b || !ab || !bb) return false;
    BITMAP ma, mb;
    GetObject(a, sizeof(ma), &ma);
    GetObject(b, sizeof(mb), &mb);
    if (ma.bmWidth != mb.bmWidth || ma.bmHeight != mb.bmHeight) return false;
    return memcmp(ab, bb, (size_t)ma.bmWidth * ma.bmHeight * 4) == 0;
}

// Captures the client area after a scroll request:
//  1. waits for the window to start changing (input is handled asynchronously),
//  2. then waits until it has stopped changing (smooth-scroll animation done).
// With prevBmp == nullptr only step 2 is done.
static HBITMAP CaptureSettledClient(HWND frame, HBITMAP prevBmp, const void* prevBits, void** outBits) {
    CancelableSleep(120);
    void* curBits = nullptr;
    HBITMAP cur = CaptureWindowClient(frame, &curBits, true);

    if (prevBmp) {
        DWORD t0 = GetTickCount();
        while (cur && !ScrollCancelled() && (int)(GetTickCount() - t0) < 800 &&
               SameFrameBits(prevBmp, prevBits, cur, curBits)) {
            CancelableSleep(40);
            void* nb = nullptr;
            HBITMAP n = CaptureWindowClient(frame, &nb, true);
            if (!n) break;
            DeleteObject(cur);
            cur = n;
            curBits = nb;
        }
    }

    int sameCount = 0;
    DWORD t0 = GetTickCount();
    while (cur && !ScrollCancelled() && (int)(GetTickCount() - t0) < 2000) {
        CancelableSleep(50);
        void* nb = nullptr;
        HBITMAP n = CaptureWindowClient(frame, &nb, true);
        if (!n) break;
        bool same = SameFrameBits(cur, curBits, n, nb);
        DeleteObject(cur);
        cur = n;
        curBits = nb;
        if (same) {
            if (++sameCount >= 2) break;
        } else {
            sameCount = 0;
        }
    }
    if (outBits) *outBits = curBits;
    return cur;
}

// Set environment variable SCROLLCAP_DEBUG=<folder> to dump raw frames and a log.
static void ScrollDebugDump(const void* a, const void* b, int w, int h, int cycle, int ticks,
                            int method, const ShiftResult& sr) {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"SCROLLCAP_DEBUG", dir, MAX_PATH)) return;
    CreateDirectoryW(dir, nullptr);
    wchar_t path[MAX_PATH];
    const void* bufs[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        swprintf(path, MAX_PATH, L"%s\\c%03d_%c.raw", dir, cycle, i ? 'b' : 'a');
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD wr;
        int hdr[2] = { w, h };
        WriteFile(f, hdr, sizeof(hdr), &wr, nullptr);
        WriteFile(f, bufs[i], (DWORD)((size_t)w * h * 4), &wr, nullptr);
        CloseHandle(f);
    }
    swprintf(path, MAX_PATH, L"%s\\log.txt", dir);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char line[200];
        int n = sprintf(line, "cycle %d method %d ticks %d -> ok %d shift %d top %d bottom %d\r\n",
                        cycle, method, ticks, sr.ok, sr.shift, sr.topStatic, sr.bottomStatic);
        DWORD wr;
        WriteFile(f, line, n, &wr, nullptr);
        CloseHandle(f);
    }
}

static void ScrollCaptureWindow(HWND hwndMain) {
    FlattenToBitmap();
    MessageBox(hwndMain,
        L"Click on the window you want to capture.\n"
        L"The window must be visible on screen.\n\n"
        L"Press ESC to cancel, or to stop scrolling early.",
        L"Scrolling Capture", MB_OK | MB_ICONINFORMATION);

    EscWatcher escWatcher;

    ShowWindow(hwndMain, SW_HIDE);
    RedrawWindow(nullptr, nullptr, nullptr,
        RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_ERASE);
    DwmFlush();
    Sleep(300);

    HWND target = PickWindowForScrollCapture();
    if (!target || !IsWindow(target) || ScrollCancelled()) {
        ShowWindow(hwndMain, SW_SHOW);
        return;
    }

    HWND frame = GetAncestor(target, GA_ROOT);
    if (!frame || !IsWindow(frame)) frame = target;
    SetForegroundWindow(frame);
    Sleep(150);

    if (IsWindowOccluded(frame)) {
        ShowWindow(hwndMain, SW_SHOW);
        MessageBox(hwndMain,
            L"The chosen window is covered by other windows.\n"
            L"Bring it fully to the front, then start scrolling capture again.",
            L"Scrolling Capture", MB_OK | MB_ICONWARNING);
        return;
    }

    RECT cr;
    GetClientRect(frame, &cr);
    int winH = cr.bottom - cr.top;
    int winW = cr.right - cr.left;
    if (winH <= 0 || winW <= 0) {
        ShowWindow(hwndMain, SW_SHOW);
        return;
    }

    // Point inside the area the user clicked; used for wheel input
    POINT clickPt;
    GetCursorPos(&clickPt);
    POINT wpt = clickPt;
    {
        RECT wr;
        GetWindowRect(frame, &wr);
        if (!PtInRect(&wr, wpt)) {
            wpt = { cr.right / 2, (int)(cr.bottom * 0.55) };
            ClientToScreen(frame, &wpt);
        }
    }
    HWND wheelTarget = WindowFromPoint(wpt);
    if (!wheelTarget || GetAncestor(wheelTarget, GA_ROOT) != frame) wheelTarget = target;

    // Window that owns a native scrollbar, if any
    HWND nativeTarget = nullptr;
    for (HWND cand : { target, FindScrollableChild(frame), frame }) {
        if (!cand) continue;
        SCROLLINFO si = { sizeof(si), SIF_POS | SIF_RANGE | SIF_PAGE };
        if ((GetWindowLong(cand, GWL_STYLE) & WS_VSCROLL) &&
            GetScrollInfo(cand, SB_VERT, &si) && si.nMax > si.nMin &&
            (int)si.nPage <= si.nMax - si.nMin) {
            nativeTarget = cand;
            break;
        }
    }

    // Ignore the scrollbar column when matching frames (its thumb moves)
    int dpi = GetWindowDpiSafe(frame);
    int sbw = min(winW / 8, MulDiv(24, dpi, 96));
    int matchX0 = min(4, winW / 4);
    int matchX1 = max(matchX0 + 1, winW - sbw);

    for (auto& f : g.scrollCapFrames)
        if (f) DeleteObject(f);
    g.scrollCapFrames.clear();
    g.scrollCapFrameY.clear();
    g.scrollCapFrameScroll.clear();

    void* firstBits = nullptr;
    HBITMAP first = CaptureSettledClient(frame, nullptr, nullptr, &firstBits);
    if (!first || !firstBits || ScrollCancelled()) {
        if (first) DeleteObject(first);
        ShowWindow(hwndMain, SW_SHOW);
        if (!ScrollCancelled())
            MessageBox(hwndMain, L"Failed to capture the window.", L"Scrolling Capture", MB_OK | MB_ICONERROR);
        return;
    }
    BITMAP bmFirst;
    GetObject(first, sizeof(bmFirst), &bmFirst);
    const int frameW = bmFirst.bmWidth, frameH = bmFirst.bmHeight;

    std::vector<int> pairTops(1, 0);
    g.scrollCapFrames.push_back(first);
    g.scrollCapFrameScroll.push_back(0);   // shift relative to previous frame
    g.scrollCapFrameY.push_back(0);        // fixed footer rows for that pair
    HBITMAP prevBmp = first;
    void* prevBits = firstBits;

    enum Method { M_NATIVE = 0, M_INPUT, M_MSG, M_COUNT };
    Method order[M_COUNT];
    int nMethods = 0;
    if (nativeTarget) order[nMethods++] = M_NATIVE;
    order[nMethods++] = M_INPUT;
    order[nMethods++] = M_MSG;
    int methodIdx = 0;

    POINT savedCursor = clickPt;
    bool cursorMoved = false;

    auto doScroll = [&](Method m, int ticks, bool down) {
        if (m == M_NATIVE) {
            for (int i = 0; i < ticks; i++)
                SendMessage(nativeTarget, WM_VSCROLL, down ? SB_LINEDOWN : SB_LINEUP, 0);
            SendMessage(nativeTarget, WM_VSCROLL, SB_ENDSCROLL, 0);
        } else if (m == M_INPUT) {
            cursorMoved = true;
            SetCursorPos(wpt.x, wpt.y);
            for (int i = 0; i < ticks; i++) {
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                input.mi.mouseData = (DWORD)(down ? -(int)WHEEL_DELTA : (int)WHEEL_DELTA);
                SendInput(1, &input, sizeof(INPUT));
                Sleep(15);
            }
        } else {
            for (int i = 0; i < ticks; i++) {
                WPARAM wp = MAKEWPARAM(0, (WORD)(short)(down ? -(int)WHEEL_DELTA : (int)WHEEL_DELTA));
                LPARAM lp = MAKELPARAM((short)wpt.x, (short)wpt.y);
                PostMessage(wheelTarget, WM_MOUSEWHEEL, wp, lp);
                Sleep(15);
            }
        }
    };

    // Start with the smallest step, measure how far one step moves, then grow
    // the step so consecutive frames still overlap by about half a screen.
    auto initialTicks = [&](Method m) { return m == M_NATIVE ? 3 : 1; };
    int ticks = initialTicks(order[0]);
    double pxPerTick = 0;
    int stall = 0, dead = 0, fails = 0, backtracks = 0;
    long long totalH = frameH;
    const long long maxTotalH = 60000;
    DWORD captureStart = GetTickCount();

    for (int cycle = 0; cycle < 1000; cycle++) {
        if (ScrollCancelled()) break;
        if (GetTickCount() - captureStart > 5 * 60 * 1000) break;
        if (totalH >= maxTotalH) break;

        Method m = order[methodIdx];
        doScroll(m, ticks, true);

        void* nb = nullptr;
        HBITMAP next = CaptureSettledClient(frame, prevBmp, prevBits, &nb);
        if (!next || !nb) { if (next) DeleteObject(next); break; }
        BITMAP bmNext;
        GetObject(next, sizeof(bmNext), &bmNext);
        if (bmNext.bmWidth != frameW || bmNext.bmHeight != frameH) {
            DeleteObject(next);   // window was resized; keep what we have
            break;
        }

        int expected = pxPerTick > 0 ? (int)(pxPerTick * ticks + 0.5) : 0;
        ShiftResult sr = FindScrollShift((const BYTE*)prevBits, (const BYTE*)nb, frameW, frameH,
                                         matchX0, matchX1, expected);
        ScrollDebugDump(prevBits, nb, frameW, frameH, cycle, ticks, (int)m, sr);

        if (sr.ok && sr.shift > 0) {
            stall = 0; dead = 0; fails = 0;
            g.scrollCapFrames.push_back(next);
            g.scrollCapFrameScroll.push_back(sr.shift);
            g.scrollCapFrameY.push_back(sr.bottomStatic);
            pairTops.push_back(sr.topStatic);
            prevBmp = next;
            prevBits = nb;
            totalH += sr.shift;

            int regionH = max(48, frameH - sr.topStatic - sr.bottomStatic);
            double ppt = (double)sr.shift / ticks;
            if (ppt > pxPerTick) pxPerTick = ppt;
            int maxTicks = (m == M_NATIVE) ? 200 : 10;
            ticks = max(1, min(maxTicks, (int)(regionH * 0.5 / pxPerTick)));
            continue;
        }

        if (sr.ok) {
            DeleteObject(next);
            // Nothing scrolled
            if (g.scrollCapFrames.size() > 1) {
                if (++stall >= 2) break;    // reached the end
            } else if (++dead >= 2) {
                dead = 0;                   // this method doesn't move the window
                if (++methodIdx >= nMethods) break;
                ticks = initialTicks(order[methodIdx]);
            }
            continue;
        }

        // Content changed but no overlap with the previous frame was found.
        DeleteObject(next);
        if (++fails >= 3 || ticks <= 1 || ++backtracks > 4) break;
        // Scrolled too far: go back the same distance and retry with a smaller step.
        doScroll(m, ticks, false);
        HBITMAP settle = CaptureSettledClient(frame, nullptr, nullptr, nullptr);
        if (settle) DeleteObject(settle);
        ticks = max(1, ticks / 2);
    }

    if (cursorMoved)
        SetCursorPos(savedCursor.x, savedCursor.y);

    if (g.scrollCapFrames.size() <= 1 && !ScrollCancelled()) {
        MessageBox(hwndMain,
            L"Could not scroll the selected window.\n"
            L"It may have no scrollable content, or it is not accepting scroll input.",
            L"Scrolling Capture", MB_OK | MB_ICONWARNING);
    }

    // Fixed footer height. Each appended strip (the bottom `shift` rows above the
    // footer) must stay below that frame's fixed header, which limits the footer.
    int footer = 0;
    for (size_t i = 1; i < g.scrollCapFrameY.size(); i++)
        footer = max(footer, g.scrollCapFrameY[i]);
    for (size_t i = 1; i < g.scrollCapFrameScroll.size() && i < pairTops.size(); i++)
        footer = min(footer, frameH - g.scrollCapFrameScroll[i] - pairTops[i]);
    footer = max(0, footer);

    HBITMAP hStitched = StitchScrollFrames(g.scrollCapFrames, g.scrollCapFrameScroll, footer);

    for (auto& f : g.scrollCapFrames)
        if (f) DeleteObject(f);
    g.scrollCapFrames.clear();
    g.scrollCapFrameY.clear();
    g.scrollCapFrameScroll.clear();

    ShowWindow(hwndMain, SW_SHOW);
    SetForegroundWindow(hwndMain);

    if (hStitched) {
        BITMAP bm;
        GetObject(hStitched, sizeof(bm), &bm);
        SetImage(hStitched, bm.bmWidth, bm.bmHeight);
        FitToWindow();
        InvalidateRect(g.hwndCanvas, nullptr, FALSE);
        UpdateStatus();
    }
}

static void UpdateTabControl();
static void RebuildToolbar();

static int AddTab() {
    TabData td = {};
    td.zoom = 1.0;
    td.selIdx = -1;
    td.id = g.nextTabId++;
    g.tabs.push_back(td);
    g.activeTab = (int)g.tabs.size() - 1;
    return g.activeTab;
}

static void SetImage(HBITMAP hBmp, int w, int h) {
    int idx = AddTab();
    TabData* t = ActiveTab();
    t->hBitmap = hBmp;
    t->imgW = w;
    t->imgH = h;
    t->hasImage = true;
    UpdateTabControl();
    TabCtrl_SetCurSel(g.hwndTab, idx);
}

static void UpdateCanvasScrollBars() {
    if (!g.hwndCanvas) return;
    TabData* t = ActiveTab();
    RECT rc;
    GetClientRect(g.hwndCanvas, &rc);
    int cw = rc.right, ch = rc.bottom;

    // The bars are always shown (greyed out when the whole image fits) so the
    // canvas size does not jump when they would appear or disappear.
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;

    if (!t || !t->hasImage || cw < 1 || ch < 1) {
        si.nMin = 0; si.nMax = 0; si.nPage = 1; si.nPos = 0;
        SetScrollInfo(g.hwndCanvas, SB_HORZ, &si, TRUE);
        SetScrollInfo(g.hwndCanvas, SB_VERT, &si, TRUE);
        return;
    }

    int dw = (int)(t->imgW * t->zoom);
    int dh = (int)(t->imgH * t->zoom);

    if (dw > cw) {
        t->offsetX = max(-(double)(dw - cw), min(0.0, t->offsetX));
        si.nMin = 0; si.nMax = dw - 1; si.nPage = cw; si.nPos = (int)lround(-t->offsetX);
    } else {
        t->offsetX = (cw - dw) / 2.0;
        si.nMin = 0; si.nMax = 0; si.nPage = 1; si.nPos = 0;
    }
    SetScrollInfo(g.hwndCanvas, SB_HORZ, &si, TRUE);

    if (dh > ch) {
        t->offsetY = max(-(double)(dh - ch), min(0.0, t->offsetY));
        si.nMin = 0; si.nMax = dh - 1; si.nPage = ch; si.nPos = (int)lround(-t->offsetY);
    } else {
        t->offsetY = (ch - dh) / 2.0;
        si.nMin = 0; si.nMax = 0; si.nPage = 1; si.nPos = 0;
    }
    SetScrollInfo(g.hwndCanvas, SB_VERT, &si, TRUE);
}

static void FitToWindow() {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage || !g.hwndCanvas) return;
    RECT rc;
    GetClientRect(g.hwndCanvas, &rc);
    if (rc.right < 1 || rc.bottom < 1) return;
    double sx = (double)rc.right / t->imgW;
    double sy = (double)rc.bottom / t->imgH;
    t->zoom = min(sx, sy) * 0.92;
    t->offsetX = (rc.right - t->imgW * t->zoom) / 2.0;
    t->offsetY = (rc.bottom - t->imgH * t->zoom) / 2.0;
    t->fitMode = true;
    UpdateCanvasScrollBars();
}

// Zooms the active tab to `newZoom`, keeping the image point under the canvas
// point (cx, cy) fixed. Pass cx = cy = INT_MIN to zoom around the centre.
static void ZoomAt(double newZoom, int cx = INT_MIN, int cy = INT_MIN) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage || !g.hwndCanvas) return;
    if (cx == INT_MIN) {
        RECT rc;
        GetClientRect(g.hwndCanvas, &rc);
        cx = rc.right / 2; cy = rc.bottom / 2;
    }
    newZoom = max(0.02, min(newZoom, 30.0));
    double mx = (cx - t->offsetX) / t->zoom;
    double my = (cy - t->offsetY) / t->zoom;
    t->zoom = newZoom;
    t->offsetX = cx - mx * t->zoom;
    t->offsetY = cy - my * t->zoom;
    t->fitMode = false;
    UpdateCanvasScrollBars();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

// Scrolls the view by (dx, dy) screen pixels (positive = towards bottom/right).
static void ScrollCanvasBy(int dx, int dy) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    t->offsetX -= dx;
    t->offsetY -= dy;
    UpdateCanvasScrollBars();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

static POINT CanvasToImage(POINT pt) {
    TabData* t = ActiveTab();
    if (!t) return { 0, 0 };
    return {
        (int)((pt.x - t->offsetX) / t->zoom),
        (int)((pt.y - t->offsetY) / t->zoom)
    };
}

static void FlattenToBitmap() {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    HDC hdc = CreateCompatibleDC(nullptr);
    SelectObject(hdc, t->hBitmap);
    for (const auto& s : t->strokes) {
        if (s.tool == Tool::Highlighter) {
            if (s.points.size() >= 2) {
                Gdiplus::Graphics gpGraphics(hdc);
                gpGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                BYTE a = 80, r = GetRValue(s.color), gr = GetGValue(s.color), b = GetBValue(s.color);
                Gdiplus::Color hc(a, r, gr, b);
                int hw = max(2, s.penWidth);
                Gdiplus::Pen gpPen(hc, (Gdiplus::REAL)hw);
                gpPen.SetStartCap(Gdiplus::LineCapRound);
                gpPen.SetEndCap(Gdiplus::LineCapRound);
                gpPen.SetLineJoin(Gdiplus::LineJoinRound);
                Gdiplus::Point* pts = new Gdiplus::Point[(int)s.points.size()];
                for (size_t i = 0; i < s.points.size(); i++) {
                    pts[i].X = (int)s.points[i].x;
                    pts[i].Y = (int)s.points[i].y;
                }
                gpGraphics.DrawLines(&gpPen, pts, (int)s.points.size());
                delete[] pts;
            }
            continue;
        }
        COLORREF c = s.tool == Tool::Eraser ? RGB(255, 255, 255) : s.color;
        int pw = max(1, s.penWidth);
        HPEN hPen = CreatePen(PS_SOLID, pw, c);
        HBRUSH hBrOld = nullptr;
        SelectObject(hdc, hPen);
        switch (s.tool) {
        case Tool::Pen:
        case Tool::Eraser:
        case Tool::Highlighter:
            if (s.points.size() >= 2) {
                MoveToEx(hdc, s.points[0].x, s.points[0].y, nullptr);
                for (size_t i = 1; i < s.points.size(); i++)
                    LineTo(hdc, s.points[i].x, s.points[i].y);
            }
            break;
        case Tool::Line:
            MoveToEx(hdc, s.startPt.x, s.startPt.y, nullptr);
            LineTo(hdc, s.endPt.x, s.endPt.y);
            break;
        case Tool::Arrow: {
            MoveToEx(hdc, s.startPt.x, s.startPt.y, nullptr);
            LineTo(hdc, s.endPt.x, s.endPt.y);
            double a = atan2((double)(s.endPt.y - s.startPt.y), (double)(s.endPt.x - s.startPt.x));
            double hl = 15.0;
            double sx2 = (double)s.endPt.x, sy2 = (double)s.endPt.y;
            MoveToEx(hdc, (int)sx2, (int)sy2, nullptr);
            LineTo(hdc, (int)(sx2 - hl * cos(a - 0.4)), (int)(sy2 - hl * sin(a - 0.4)));
            MoveToEx(hdc, (int)sx2, (int)sy2, nullptr);
            LineTo(hdc, (int)(sx2 - hl * cos(a + 0.4)), (int)(sy2 - hl * sin(a + 0.4)));
            break;
        }
        case Tool::Rectangle: {
            hBrOld = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, min(s.startPt.x, s.endPt.x), min(s.startPt.y, s.endPt.y),
                max(s.startPt.x, s.endPt.x), max(s.startPt.y, s.endPt.y));
            SelectObject(hdc, hBrOld);
            break;
        }
        case Tool::Ellipse: {
            hBrOld = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, min(s.startPt.x, s.endPt.x), min(s.startPt.y, s.endPt.y),
                max(s.startPt.x, s.endPt.x), max(s.startPt.y, s.endPt.y));
            SelectObject(hdc, hBrOld);
            break;
        }
        case Tool::Fill: {
            DeleteObject(hPen);
            hPen = nullptr;
            HBRUSH hBr = CreateSolidBrush(s.color);
            SelectObject(hdc, hBr);
            Rectangle(hdc, min(s.startPt.x, s.endPt.x), min(s.startPt.y, s.endPt.y),
                max(s.startPt.x, s.endPt.x), max(s.startPt.y, s.endPt.y));
            DeleteObject(hBr);
            break;
        }
        case Tool::Text: {
            DeleteObject(hPen);
            hPen = nullptr;
            int fs = max(8, s.fontSize);
            HFONT hf = CreateFont(-fs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
            HFONT hof = (HFONT)SelectObject(hdc, hf);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, s.color);
            TextOut(hdc, s.startPt.x, s.startPt.y - fs, s.text.c_str(), (int)s.text.size());
            SelectObject(hdc, hof);
            DeleteObject(hf);
            break;
        }
        case Tool::Callout: {
            DeleteObject(hPen);
            hPen = nullptr;
            DrawCallout(hdc, s, 1.0, 0.0, 0.0);
            break;
        }
        default: break;
        }
        if (hPen) DeleteObject(hPen);
    }
    t->strokes.clear();
    t->redoStack.clear();
    DeleteDC(hdc);
}

static void ShowColorDlg(HWND parent) {
    CHOOSECOLOR cc = {};
    static COLORREF cust[16] = {};
    if (cust[0] == 0) {
        for (int i = 0; i < 16; i++) cust[i] = RGB(128, 128, 128);
    }
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = parent;
    cc.rgbResult = g.penColor;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColor(&cc)) {
        g.penColor = cc.rgbResult;
        TabData* t = ActiveTab();
        if (t && t->selIdx >= 0 && t->selIdx < (int)t->strokes.size()) {
            t->strokes[t->selIdx].color = g.penColor;
        }
    }
}

static void ShowPenWidthDlg(HWND parent) {
    DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_PEN_WIDTH), parent,
        [](HWND hDlg, UINT msg, WPARAM wp, LPARAM) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                wchar_t b[16];
                swprintf_s(b, L"%d", g.penWidth);
                SetDlgItemText(hDlg, 201, b);
                SendDlgItemMessage(hDlg, 201, EM_SETLIMITTEXT, 2, 0);
                SendDlgItemMessage(hDlg, 201, EM_SETSEL, 0, -1);
                SetFocus(GetDlgItem(hDlg, 201));
                return FALSE;
            }
            if (msg == WM_COMMAND) {
                if (LOWORD(wp) == IDOK) {
                    wchar_t b[16];
                    GetDlgItemText(hDlg, 201, b, 16);
                    int w = _wtoi(b);
                    if (w >= 1 && w <= 50) {
                        g.penWidth = w;
                        TabData* t = ActiveTab();
                        if (t && t->selIdx >= 0 && t->selIdx < (int)t->strokes.size()) {
                            t->strokes[t->selIdx].penWidth = w;
                        }
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 2) {
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                }
            }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            return FALSE;
        }, 0);
}

static bool PushImageUndo(TabData* t);

static void ShowResizeDlg(HWND parent) {
    TabData* t = ActiveTab();
    if (!t) return;
    DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_RESIZE), parent,
        [](HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)lp);
                TabData* t = (TabData*)lp;
                wchar_t b[16];
                swprintf_s(b, L"%d", t->imgW);
                SetDlgItemText(hDlg, 101, b);
                swprintf_s(b, L"%d", t->imgH);
                SetDlgItemText(hDlg, 102, b);
                CheckDlgButton(hDlg, 103, BST_CHECKED);
                return FALSE;
            }
            TabData* t = (TabData*)GetWindowLongPtr(hDlg, DWLP_USER);
            if (!t) return FALSE;
            if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
                wchar_t bW[16], bH[16];
                GetDlgItemText(hDlg, 101, bW, 16);
                GetDlgItemText(hDlg, 102, bH, 16);
                bool isPct = IsDlgButtonChecked(hDlg, 104) == BST_CHECKED;
                int nw, nh;
                if (isPct) {
                    double pct = _wtof(bW);
                    if (pct < 1) pct = 100;
                    nw = (int)(t->imgW * pct / 100.0);
                    nh = (int)(t->imgH * pct / 100.0);
                } else {
                    nw = _wtoi(bW);
                    nh = _wtoi(bH);
                }
                if (nw < 1 || nh < 1) {
                    MessageBox(hDlg, L"Invalid dimensions", L"Error", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                bool keepAspect = IsDlgButtonChecked(hDlg, 103) == BST_CHECKED && !isPct;
                if (keepAspect) {
                    double ar = (double)t->imgW / t->imgH;
                    if (nw * ar > nh) nh = (int)(nw / ar);
                    else nw = (int)(nh * ar);
                }
                PushImageUndo(t);
                FlattenToBitmap();
                HDC hdcScr = GetDC(nullptr);
                HBITMAP hNew = CreateCompatibleBitmap(hdcScr, nw, nh);
                ReleaseDC(nullptr, hdcScr);
                HDC hdcN = CreateCompatibleDC(nullptr);
                HDC hdcO = CreateCompatibleDC(nullptr);
                SelectObject(hdcN, hNew);
                SelectObject(hdcO, t->hBitmap);
                HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 255));
                RECT fr = { 0, 0, nw, nh };
                FillRect(hdcN, &fr, hBr);
                DeleteObject(hBr);
                SetStretchBltMode(hdcN, HALFTONE);
                StretchBlt(hdcN, 0, 0, nw, nh, hdcO, 0, 0, t->imgW, t->imgH, SRCCOPY);
                DeleteDC(hdcN);
                DeleteDC(hdcO);
                DeleteObject(t->hBitmap);
                t->hBitmap = hNew;
                t->imgW = nw;
                t->imgH = nh;
                FitToWindow();
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (msg == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 2)) {
                EndDialog(hDlg, 0); return TRUE;
            }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            if (msg == WM_COMMAND && LOWORD(wp) == 104) {
                bool pct = IsDlgButtonChecked(hDlg, 104) == BST_CHECKED;
                wchar_t b[16];
                GetDlgItemText(hDlg, 101, b, 16);
                if (pct) {
                    swprintf_s(b, L"100");
                    SetDlgItemText(hDlg, 102, L"100");
                    EnableWindow(GetDlgItem(hDlg, 102), FALSE);
                    EnableWindow(GetDlgItem(hDlg, 103), FALSE);
                    CheckDlgButton(hDlg, 103, BST_UNCHECKED);
                } else {
                    swprintf_s(b, L"%d", t->imgW);
                    SetDlgItemText(hDlg, 102, L"");
                    EnableWindow(GetDlgItem(hDlg, 102), TRUE);
                    EnableWindow(GetDlgItem(hDlg, 103), TRUE);
                    CheckDlgButton(hDlg, 103, BST_CHECKED);
                }
                SetDlgItemText(hDlg, 101, b);
                return TRUE;
            }
            return FALSE;
        }, (LPARAM)t);
}

static void FlattenToBitmap();
static void UpdateStatus();
static void FitToWindow();
static void RebuildToolbar();
static void UpdateTabControl();
static void SwitchToTab(int idx);
static void CloseTab(int idx);

static void CropImage(int x, int y, int w, int h) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage || w < 2 || h < 2) return;
    x = max(0, min(x, t->imgW - 1));
    y = max(0, min(y, t->imgH - 1));
    w = min(w, t->imgW - x);
    h = min(h, t->imgH - y);
    if (w < 2 || h < 2) return;

    // Save original tab state
    HBITMAP hOrigBmp = t->hBitmap;
    int origW = t->imgW, origH = t->imgH;
    auto origStrokes = t->strokes;
    auto origRedo = t->redoStack;

    HDC hdcScr = GetDC(nullptr);

    // Create a copy of the original bitmap
    HBITMAP hCopy = CreateCompatibleBitmap(hdcScr, origW, origH);
    {
        HDC hdcTemp = CreateCompatibleDC(hdcScr);
        HGDIOBJ oldTemp = SelectObject(hdcTemp, hCopy);
        HDC hdcOrig = CreateCompatibleDC(hdcScr);
        HGDIOBJ oldOrig = SelectObject(hdcOrig, hOrigBmp);
        BitBlt(hdcTemp, 0, 0, origW, origH, hdcOrig, 0, 0, SRCCOPY);
        SelectObject(hdcOrig, oldOrig);
        DeleteDC(hdcOrig);
        SelectObject(hdcTemp, oldTemp);
        DeleteDC(hdcTemp);
    }

    // Point tab at the copy, restore strokes, and flatten to bake them onto the copy
    t->hBitmap = hCopy;
    t->strokes = origStrokes;
    t->redoStack = origRedo;
    FlattenToBitmap();

    // Crop from the flattened copy
    HBITMAP hCropped = CreateCompatibleBitmap(hdcScr, w, h);
    {
        HDC hdcSrc = CreateCompatibleDC(hdcScr);
        HGDIOBJ oldSrc = SelectObject(hdcSrc, hCopy);
        HDC hdcDst = CreateCompatibleDC(hdcScr);
        HGDIOBJ oldDst = SelectObject(hdcDst, hCropped);
        BitBlt(hdcDst, 0, 0, w, h, hdcSrc, x, y, SRCCOPY);
        SelectObject(hdcDst, oldDst);
        DeleteDC(hdcDst);
        SelectObject(hdcSrc, oldSrc);
        DeleteDC(hdcSrc);
    }
    DeleteObject(hCopy);
    ReleaseDC(nullptr, hdcScr);

    // Restore original tab unchanged
    t->hBitmap = hOrigBmp;
    t->imgW = origW;
    t->imgH = origH;
    t->strokes = origStrokes;
    t->redoStack = origRedo;
    t->selIdx = -1;

    // Add cropped result as a new tab
    AddTab();
    TabData* newTab = ActiveTab();
    newTab->hBitmap = hCropped;
    newTab->imgW = w;
    newTab->imgH = h;
    newTab->hasImage = true;
    FitToWindow();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    UpdateStatus();
    UpdateTabControl();
}

// ---------------------------------------------------------------------------
// Image editing: pixel access, undoable image operations and filters.
// Pixels are 32-bit 0x00RRGGBB, top-down, row-major.
// ---------------------------------------------------------------------------

static bool ReadPixels(HBITMAP hb, int w, int h, std::vector<DWORD>& px) {
    if (!hb || w < 1 || h < 1) return false;
    px.assign((size_t)w * h, 0);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC hdc = GetDC(nullptr);
    int lines = GetDIBits(hdc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    return lines == h;
}

static HBITMAP PixelsToBitmap(const std::vector<DWORD>& px, int w, int h) {
    void* bits = nullptr;
    HBITMAP hb = CreateDib32(w, h, &bits);
    if (!hb || !bits) return nullptr;
    DWORD* d = (DWORD*)bits;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) d[i] = px[i] | 0xFF000000;
    return hb;
}

static HBITMAP CopyBitmapPixels(HBITMAP hb, int w, int h) {
    std::vector<DWORD> px;
    if (!ReadPixels(hb, w, h, px)) return nullptr;
    return PixelsToBitmap(px, w, h);
}

static void FreeSnaps(std::vector<ImgSnap>& v) {
    for (auto& s : v) if (s.bmp) DeleteObject(s.bmp);
    v.clear();
}

static const size_t MAX_IMG_UNDO = 20;

// Saves the current bitmap and annotations so the next image edit can be undone.
static bool PushImageUndo(TabData* t) {
    if (!t || !t->hasImage) return false;
    ImgSnap s;
    s.bmp = CopyBitmapPixels(t->hBitmap, t->imgW, t->imgH);
    if (!s.bmp) return false;
    s.w = t->imgW; s.h = t->imgH;
    s.strokes = t->strokes;
    t->imgUndo.push_back(std::move(s));
    if (t->imgUndo.size() > MAX_IMG_UNDO) {
        DeleteObject(t->imgUndo.front().bmp);
        t->imgUndo.erase(t->imgUndo.begin());
    }
    FreeSnaps(t->imgRedo);
    return true;
}

static bool CanImageRedo(TabData* t) {
    return t && !t->imgRedo.empty() && t->redoStack.empty() &&
           t->strokes.size() == t->imgRedo.back().strokeCount;
}

// Moves the current state onto `to` and restores the last state from `from`.
static void SwapImageState(TabData* t, std::vector<ImgSnap>& from, std::vector<ImgSnap>& to) {
    ImgSnap cur;
    cur.bmp = t->hBitmap; cur.w = t->imgW; cur.h = t->imgH;
    cur.strokes = t->strokes;
    ImgSnap prev = std::move(from.back());
    from.pop_back();
    t->hBitmap = prev.bmp; t->imgW = prev.w; t->imgH = prev.h;
    t->strokes = std::move(prev.strokes);
    t->redoStack.clear();
    t->selIdx = -1;
    cur.strokeCount = t->strokes.size();
    bool sizeChanged = cur.w != t->imgW || cur.h != t->imgH;
    to.push_back(std::move(cur));
    if (sizeChanged) FitToWindow();
    else UpdateCanvasScrollBars();
}

static void ImageUndo(TabData* t) { if (t && !t->imgUndo.empty()) SwapImageState(t, t->imgUndo, t->imgRedo); }
static void ImageRedo(TabData* t) { if (CanImageRedo(t)) SwapImageState(t, t->imgRedo, t->imgUndo); }

static inline int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline DWORD MkPx(int r, int gg, int b) { return ((DWORD)Clamp255(r) << 16) | ((DWORD)Clamp255(gg) << 8) | (DWORD)Clamp255(b); }
#define PX_R(p) ((int)(((p) >> 16) & 0xFF))
#define PX_G(p) ((int)(((p) >> 8) & 0xFF))
#define PX_B(p) ((int)((p) & 0xFF))

static void BoxBlur(std::vector<DWORD>& px, int w, int h, int r) {
    std::vector<DWORD> tmp(px.size());
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<DWORD>& src = pass == 0 ? px : tmp;
        std::vector<DWORD>& dst = pass == 0 ? tmp : px;
        int len = pass == 0 ? w : h, lines = pass == 0 ? h : w;
        for (int l = 0; l < lines; l++) {
            auto at = [&](int i) -> size_t { return pass == 0 ? (size_t)l * w + i : (size_t)i * w + l; };
            int sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int i = 0; i <= r && i < len; i++) { DWORD p = src[at(i)]; sr += PX_R(p); sg += PX_G(p); sb += PX_B(p); cnt++; }
            for (int i = 0; i < len; i++) {
                dst[at(i)] = MkPx(sr / cnt, sg / cnt, sb / cnt);
                int add = i + r + 1, rem = i - r;
                if (add < len) { DWORD p = src[at(add)]; sr += PX_R(p); sg += PX_G(p); sb += PX_B(p); cnt++; }
                if (rem >= 0) { DWORD p = src[at(rem)]; sr -= PX_R(p); sg -= PX_G(p); sb -= PX_B(p); cnt--; }
            }
        }
    }
}

static void ApplyBrightnessContrast(std::vector<DWORD>& px, int bright, int contrast) {
    double c = contrast * 2.55;
    double f = (259.0 * (c + 255.0)) / (255.0 * (259.0 - c));
    double b = bright * 2.55;
    int lut[256];
    for (int i = 0; i < 256; i++) lut[i] = Clamp255((int)lround(f * (i - 128) + 128 + b));
    for (auto& p : px) p = MkPx(lut[PX_R(p)], lut[PX_G(p)], lut[PX_B(p)]);
}

// Runs one image operation on the active tab. Annotations are baked in first
// and everything is undoable with Ctrl+Z.
static void ApplyImageOp(int op) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    if (!PushImageUndo(t)) return;
    FlattenToBitmap();
    t->selIdx = -1;

    int w = t->imgW, h = t->imgH;
    std::vector<DWORD> src;
    if (!ReadPixels(t->hBitmap, w, h, src)) return;
    std::vector<DWORD> dst;
    int nw = w, nh = h;

    switch (op) {
    case IDM_IMAGE_ROT_RIGHT:
        nw = h; nh = w; dst.resize(src.size());
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[(size_t)x * nw + (h - 1 - y)] = src[(size_t)y * w + x];
        break;
    case IDM_IMAGE_ROT_LEFT:
        nw = h; nh = w; dst.resize(src.size());
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[(size_t)(w - 1 - x) * nw + y] = src[(size_t)y * w + x];
        break;
    case IDM_IMAGE_ROT_180:
        dst.assign(src.rbegin(), src.rend());
        break;
    case IDM_IMAGE_FLIP_H:
        dst = src;
        for (int y = 0; y < h; y++) std::reverse(dst.begin() + (size_t)y * w, dst.begin() + (size_t)(y + 1) * w);
        break;
    case IDM_IMAGE_FLIP_V:
        dst.resize(src.size());
        for (int y = 0; y < h; y++)
            std::copy(src.begin() + (size_t)y * w, src.begin() + (size_t)(y + 1) * w, dst.begin() + (size_t)(h - 1 - y) * w);
        break;
    case IDM_IMAGE_GRAYSCALE:
        dst = src;
        for (auto& p : dst) { int l = (PX_R(p) * 299 + PX_G(p) * 587 + PX_B(p) * 114) / 1000; p = MkPx(l, l, l); }
        break;
    case IDM_IMAGE_SEPIA:
        dst = src;
        for (auto& p : dst) {
            int r = PX_R(p), gg = PX_G(p), b = PX_B(p);
            p = MkPx((r * 393 + gg * 769 + b * 189) / 1000,
                     (r * 349 + gg * 686 + b * 168) / 1000,
                     (r * 272 + gg * 534 + b * 131) / 1000);
        }
        break;
    case IDM_IMAGE_INVERT:
        dst = src;
        for (auto& p : dst) p = (~p) & 0x00FFFFFF;
        break;
    case IDM_IMAGE_BLUR:
        dst = src;
        BoxBlur(dst, w, h, 2);
        break;
    case IDM_IMAGE_SHARPEN:
        dst = src;
        for (int y = 1; y < h - 1; y++)
            for (int x = 1; x < w - 1; x++) {
                size_t i = (size_t)y * w + x;
                DWORD c = src[i], l = src[i - 1], r = src[i + 1], u = src[i - w], d = src[i + w];
                dst[i] = MkPx(5 * PX_R(c) - PX_R(l) - PX_R(r) - PX_R(u) - PX_R(d),
                              5 * PX_G(c) - PX_G(l) - PX_G(r) - PX_G(u) - PX_G(d),
                              5 * PX_B(c) - PX_B(l) - PX_B(r) - PX_B(u) - PX_B(d));
            }
        break;
    case IDM_IMAGE_BORDER: {
        int bw = max(1, g.penWidth);
        nw = w + 2 * bw; nh = h + 2 * bw;
        dst.assign((size_t)nw * nh, MkPx(GetRValue(g.penColor), GetGValue(g.penColor), GetBValue(g.penColor)));
        for (int y = 0; y < h; y++)
            std::copy(src.begin() + (size_t)y * w, src.begin() + (size_t)(y + 1) * w,
                      dst.begin() + (size_t)(y + bw) * nw + bw);
        break;
    }
    default:
        return;
    }

    HBITMAP hNew = PixelsToBitmap(dst, nw, nh);
    if (!hNew) return;
    DeleteObject(t->hBitmap);
    t->hBitmap = hNew;
    t->imgW = nw; t->imgH = nh;
    if (nw != w || nh != h) FitToWindow();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

// Pixelates a rectangle of the image (for hiding text, faces, etc.).
static void PixelateRegion(int x, int y, int w, int h) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    int x0 = max(0, x), y0 = max(0, y);
    int x1 = min(t->imgW, x + w), y1 = min(t->imgH, y + h);
    if (x1 - x0 < 2 || y1 - y0 < 2) return;
    if (!PushImageUndo(t)) return;
    FlattenToBitmap();
    std::vector<DWORD> px;
    if (!ReadPixels(t->hBitmap, t->imgW, t->imgH, px)) return;
    int block = max(6, min(20, min(x1 - x0, y1 - y0) / 6));
    for (int by = y0; by < y1; by += block)
        for (int bx = x0; bx < x1; bx += block) {
            int ex = min(bx + block, x1), ey = min(by + block, y1);
            long long sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = by; yy < ey; yy++)
                for (int xx = bx; xx < ex; xx++) {
                    DWORD p = px[(size_t)yy * t->imgW + xx];
                    sr += PX_R(p); sg += PX_G(p); sb += PX_B(p); n++;
                }
            DWORD avg = MkPx((int)(sr / n), (int)(sg / n), (int)(sb / n));
            for (int yy = by; yy < ey; yy++)
                for (int xx = bx; xx < ex; xx++) px[(size_t)yy * t->imgW + xx] = avg;
        }
    HBITMAP hNew = PixelsToBitmap(px, t->imgW, t->imgH);
    if (!hNew) return;
    DeleteObject(t->hBitmap);
    t->hBitmap = hNew;
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

// Brightness / contrast dialog with live preview.
struct BCState {
    TabData* t;
    std::vector<DWORD> orig;
};

static void BC_Preview(HWND hDlg, BCState* st) {
    int b = (int)SendDlgItemMessage(hDlg, IDC_BRIGHT_SLIDER, TBM_GETPOS, 0, 0);
    int c = (int)SendDlgItemMessage(hDlg, IDC_CONTRAST_SLIDER, TBM_GETPOS, 0, 0);
    wchar_t buf[16];
    swprintf_s(buf, L"%d", b); SetDlgItemText(hDlg, IDC_BRIGHT_VAL, buf);
    swprintf_s(buf, L"%d", c); SetDlgItemText(hDlg, IDC_CONTRAST_VAL, buf);
    std::vector<DWORD> px = st->orig;
    if (b != 0 || c != 0) ApplyBrightnessContrast(px, b, c);
    HBITMAP hNew = PixelsToBitmap(px, st->t->imgW, st->t->imgH);
    if (!hNew) return;
    DeleteObject(st->t->hBitmap);
    st->t->hBitmap = hNew;
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    UpdateWindow(g.hwndCanvas);
}

static void ShowBrightnessDlg(HWND parent) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    if (!PushImageUndo(t)) return;
    FlattenToBitmap();
    t->selIdx = -1;
    BCState st{ t, {} };
    if (!ReadPixels(t->hBitmap, t->imgW, t->imgH, st.orig)) { ImageUndo(t); FreeSnaps(t->imgRedo); return; }
    INT_PTR res = DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_BRIGHTNESS), parent,
        [](HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            BCState* st = (BCState*)GetWindowLongPtr(hDlg, DWLP_USER);
            switch (msg) {
            case WM_INITDIALOG:
                SetWindowLongPtr(hDlg, DWLP_USER, lp);
                for (int id : { IDC_BRIGHT_SLIDER, IDC_CONTRAST_SLIDER }) {
                    SendDlgItemMessage(hDlg, id, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));
                    SendDlgItemMessage(hDlg, id, TBM_SETPAGESIZE, 0, 10);
                    SendDlgItemMessage(hDlg, id, TBM_SETPOS, TRUE, 0);
                }
                return TRUE;
            case WM_HSCROLL:
                if (st) BC_Preview(hDlg, st);
                return TRUE;
            case WM_COMMAND:
                if (LOWORD(wp) == IDC_BC_RESET && st) {
                    SendDlgItemMessage(hDlg, IDC_BRIGHT_SLIDER, TBM_SETPOS, TRUE, 0);
                    SendDlgItemMessage(hDlg, IDC_CONTRAST_SLIDER, TBM_SETPOS, TRUE, 0);
                    BC_Preview(hDlg, st);
                    return TRUE;
                }
                if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                    EndDialog(hDlg, LOWORD(wp));
                    return TRUE;
                }
                break;
            case WM_CLOSE:
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }, (LPARAM)&st);
    if (res != IDOK) {
        // Throw away the preview and restore the untouched image and annotations.
        ImageUndo(t);
        FreeSnaps(t->imgRedo);
    }
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

static void ShowImageMenu(HWND hwnd, int x, int y) {
    HMENU hm = CreatePopupMenu();
    AppendMenu(hm, MF_STRING, IDM_IMAGE_ROT_LEFT,  L"Rotate Left\tCtrl+L");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_ROT_RIGHT, L"Rotate Right\tCtrl+R");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_ROT_180,   L"Rotate 180°");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_FLIP_H,    L"Flip Horizontal");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_FLIP_V,    L"Flip Vertical");
    AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hm, MF_STRING, IDM_IMAGE_BRIGHTNESS, L"Brightness / Contrast...");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_GRAYSCALE, L"Grayscale");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_SEPIA,     L"Sepia");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_INVERT,    L"Invert Colors");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_BLUR,      L"Blur");
    AppendMenu(hm, MF_STRING, IDM_IMAGE_SHARPEN,   L"Sharpen");
    AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
    wchar_t border[64];
    swprintf_s(border, L"Add Border (%d px, pen color)", max(1, g.penWidth));
    AppendMenu(hm, MF_STRING, IDM_IMAGE_BORDER, border);
    AppendMenu(hm, MF_STRING, IDM_TOOL_PIXELATE,   L"Pixelate Region");
    AppendMenu(hm, MF_STRING, IDM_EDIT_CROP,       L"Crop...");
    AppendMenu(hm, MF_STRING, IDM_EDIT_RESIZE,     L"Resize...");
    TrackPopupMenu(hm, TPM_LEFTALIGN | TPM_TOPALIGN, x, y, 0, hwnd, nullptr);
    DestroyMenu(hm);
}

static void ShowAboutDlg(HWND parent) {
    DialogBox(nullptr, MAKEINTRESOURCE(IDD_ABOUT), parent,
        [](HWND hDlg, UINT msg, WPARAM, LPARAM) -> INT_PTR {
            if (msg == WM_COMMAND) { EndDialog(hDlg, 0); return TRUE; }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            return FALSE;
        });
}

// Index of a text/callout that was just placed and is waiting for its first
// edit in the dialog (-1 if none). Cancel removes it.
static int g_pendingNewAnnot = -1;

static void RebuildToolbar();

// Refreshes toolbar highlight, undo/redo state and the status bar after
// changes made from the canvas or a dialog.
static void RefreshChrome() {
    RebuildToolbar();
    UpdateStatus();
    if (g.hwndMain) InvalidateRect(g.hwndMain, nullptr, FALSE);
    if (g.hwndCanvas) InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

// The text/callout edit dialog is bound to one specific shape (tab id + index)
// for its whole lifetime. It is modal, so clicks on the canvas can't change
// which shape gets edited, and it previews every change live. Cancel restores
// the original (or removes a shape that was just placed).
struct AnnotEdit {
    int tabId = -1;
    int idx = -1;
    Stroke original;
    bool isNew = false;
    bool initializing = false;
};
static AnnotEdit g_annotEdit;

static TabData* FindTabById(int id) {
    for (auto& t : g.tabs)
        if (t.id == id) return &t;
    return nullptr;
}

static Stroke* AnnotEditTarget() {
    TabData* t = FindTabById(g_annotEdit.tabId);
    if (!t || g_annotEdit.idx < 0 || g_annotEdit.idx >= (int)t->strokes.size()) return nullptr;
    Stroke& s = t->strokes[g_annotEdit.idx];
    if (s.tool != Tool::Text && s.tool != Tool::Callout) return nullptr;
    return &s;
}

static std::wstring GetDlgItemString(HWND hDlg, int id) {
    HWND h = GetDlgItem(hDlg, id);
    int len = GetWindowTextLength(h);
    std::wstring s(len + 1, L'\0');
    GetWindowText(h, &s[0], len + 1);
    s.resize(len);
    return s;
}

// Copies the dialog's current values into the shape being edited.
static void ApplyAnnotDialog(HWND hDlg) {
    Stroke* s = AnnotEditTarget();
    if (!s) return;
    s->text = GetDlgItemString(hDlg, 300);
    // The font box is editable: read what is shown, not the list selection
    std::wstring fn = GetDlgItemString(hDlg, 301);
    if (!fn.empty()) s->fontName = fn;
    int sz = _wtoi(GetDlgItemString(hDlg, 302).c_str());
    if (sz >= 6) s->fontSize = min(200, sz);
}

static void RemoveAnnotEditTarget() {
    TabData* t = FindTabById(g_annotEdit.tabId);
    if (!t || g_annotEdit.idx < 0 || g_annotEdit.idx >= (int)t->strokes.size()) return;
    t->strokes.erase(t->strokes.begin() + g_annotEdit.idx);
    if (t->selIdx == g_annotEdit.idx) t->selIdx = -1;
    else if (t->selIdx > g_annotEdit.idx) t->selIdx--;
}

static void RestoreAnnotOriginal() {
    if (Stroke* s = AnnotEditTarget()) *s = g_annotEdit.original;
}

static void RedrawCanvasNow() {
    if (!g.hwndCanvas) return;
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    UpdateWindow(g.hwndCanvas);
}

// Puts the dialog beside the shape (right of it if there is room, else left),
// kept inside the monitor's work area.
static void PositionAnnotDialog(HWND hDlg) {
    Stroke* s = AnnotEditTarget();
    TabData* t = FindTabById(g_annotEdit.tabId);
    if (!s || !t || !g.hwndCanvas) return;
    POINT tl, br;
    GetBBox(*s, tl, br);
    if (s->tool == Tool::Callout) {
        // The bubble grows around its centre as text is typed; leave room
        // for its widest possible size so the dialog doesn't cover it.
        int half = CALLOUT_MAX_TEXT_W / 2 + max(6, s->fontSize / 3) + 4;
        tl.x = min(tl.x, s->endPt.x - half);
        br.x = max(br.x, s->endPt.x + half);
    }
    POINT a = { (LONG)(tl.x * t->zoom + t->offsetX), (LONG)(tl.y * t->zoom + t->offsetY) };
    POINT b = { (LONG)(br.x * t->zoom + t->offsetX), (LONG)(br.y * t->zoom + t->offsetY) };
    ClientToScreen(g.hwndCanvas, &a);
    ClientToScreen(g.hwndCanvas, &b);

    RECT dr;
    GetWindowRect(hDlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(MonitorFromPoint(a, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;

    int x = b.x + 16;
    if (x + dw > wa.right) x = a.x - 16 - dw;
    if (x < wa.left) x = wa.right - dw;   // no room either side: keep on screen
    int y = a.y;
    x = max((int)wa.left, min(x, (int)wa.right - dw));
    y = max((int)wa.top, min(y, (int)wa.bottom - dh));
    SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK FontDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        Stroke* s = AnnotEditTarget();
        if (!s) { EndDialog(hDlg, IDCANCEL); return TRUE; }
        g_annotEdit.initializing = true;
        SetWindowText(hDlg, s->tool == Tool::Callout ? L"Edit Callout" : L"Edit Text");
        {
            // The edit box only shows line breaks written as \r\n
            std::wstring shown;
            for (size_t i = 0; i < s->text.size(); i++) {
                wchar_t ch = s->text[i];
                if (ch == L'\n' && (i == 0 || s->text[i - 1] != L'\r')) shown += L'\r';
                shown += ch;
            }
            SetDlgItemText(hDlg, 300, shown.c_str());
        }
        HWND hCombo = GetDlgItem(hDlg, 301);
        HDC hdc = GetDC(nullptr);
        EnumFontFamilies(hdc, nullptr, [](const LOGFONT* lplf, const TEXTMETRIC*, DWORD, LPARAM lParam) -> int {
            HWND hc = (HWND)lParam;
            if (lplf->lfFaceName[0] != L'@' &&
                SendMessage(hc, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)lplf->lfFaceName) == CB_ERR)
                SendMessage(hc, CB_ADDSTRING, 0, (LPARAM)lplf->lfFaceName);
            return 1;
        }, (LPARAM)hCombo);
        ReleaseDC(nullptr, hdc);
        int idx = (int)SendMessage(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)s->fontName.c_str());
        if (idx != CB_ERR) SendMessage(hCombo, CB_SETCURSEL, idx, 0);
        else SetWindowText(hCombo, s->fontName.c_str());
        SetDlgItemInt(hDlg, 302, s->fontSize, FALSE);
        PositionAnnotDialog(hDlg);
        g_annotEdit.initializing = false;

        // Focus the text with everything selected so typing replaces it
        HWND hEdit = GetDlgItem(hDlg, 300);
        SetFocus(hEdit);
        SendMessage(hEdit, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        // Live preview of every change
        if (!g_annotEdit.initializing &&
            ((id == 300 && code == EN_CHANGE) || (id == 302 && code == EN_CHANGE) ||
             (id == 301 && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)))) {
            if (id == 301 && code == CBN_SELCHANGE) {
                // The edit part isn't updated yet during CBN_SELCHANGE
                HWND hCombo = GetDlgItem(hDlg, 301);
                int sel = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    int n = (int)SendMessage(hCombo, CB_GETLBTEXTLEN, sel, 0);
                    std::wstring fn(n + 1, L'\0');
                    SendMessage(hCombo, CB_GETLBTEXT, sel, (LPARAM)&fn[0]);
                    fn.resize(n);
                    SetWindowText(hCombo, fn.c_str());
                }
            }
            ApplyAnnotDialog(hDlg);
            RedrawCanvasNow();
            return TRUE;
        }
        if (id == IDOK) {
            ApplyAnnotDialog(hDlg);
            Stroke* s = AnnotEditTarget();
            bool blank = !s || s->text.find_first_not_of(L" \t\r\n") == std::wstring::npos;
            if (blank) {
                // Don't keep an empty shape: drop a new one, restore an existing one
                if (g_annotEdit.isNew) RemoveAnnotEditTarget();
                else RestoreAnnotOriginal();
            } else {
                g.fontName = s->fontName;   // defaults for the next text/callout
                g.fontSize = s->fontSize;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            if (g_annotEdit.isNew) RemoveAnnotEditTarget();
            else RestoreAnnotOriginal();
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        SendMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    return FALSE;
}

static void ShowFontDlg() {
    static bool open = false;
    if (open) return;
    TabData* t = ActiveTab();
    if (!t || t->selIdx < 0 || t->selIdx >= (int)t->strokes.size() ||
        (t->strokes[t->selIdx].tool != Tool::Text && t->strokes[t->selIdx].tool != Tool::Callout)) {
        g_pendingNewAnnot = -1;
        return;
    }
    g_annotEdit = AnnotEdit();
    g_annotEdit.tabId = t->id;
    g_annotEdit.idx = t->selIdx;
    g_annotEdit.original = t->strokes[t->selIdx];
    g_annotEdit.isNew = (g_pendingNewAnnot == t->selIdx);
    g_pendingNewAnnot = -1;

    // Stop any drag started by the first click of a double-click
    g.isDraggingSel = false;
    g.isResizingSel = false;
    if (GetCapture() == g.hwndCanvas) ReleaseCapture();

    open = true;
    DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_FONT), g.hwndMain, FontDlgProc, 0);
    open = false;
    g_annotEdit = AnnotEdit();
    RefreshChrome();
}

static void OpenImage(HWND hwnd) {
    wchar_t f[MAX_PATH] = L"";
    OPENFILENAME of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = hwnd;
    of.lpstrFilter = L"Images (*.png;*.jpg;*.bmp;*.gif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tiff\0All\0*.*\0";
    of.lpstrFile = f;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST;
    of.lpstrTitle = L"Open Image";
    if (GetOpenFileName(&of)) {
        int w = 0, h = 0;
        HBITMAP hb = LoadImageFile(f, w, h);
        if (hb) {
            SetImage(hb, w, h);
            TabData* t = ActiveTab();
            if (t) t->filename = f;
            FitToWindow();
            InvalidateRect(g.hwndCanvas, nullptr, FALSE);
        } else {
            MessageBox(hwnd, L"Failed to load image.", L"Error", MB_OK | MB_ICONERROR);
        }
    }
}

static void SaveImage(HWND hwnd) {
    TabData* t = ActiveTab();
    if (!t || !t->hasImage) return;
    wchar_t f[MAX_PATH] = L"image.png";
    OPENFILENAME of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = hwnd;
    of.lpstrFilter = L"PNG\0*.png\0JPEG\0*.jpg\0BMP\0*.bmp\0";
    of.lpstrFile = f;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_OVERWRITEPROMPT;
    of.lpstrDefExt = L"png";
    of.lpstrTitle = L"Save Image";
    if (GetSaveFileName(&of)) {
        FlattenToBitmap();
        if (SaveImageFile(t->hBitmap, f)) {
            t->filename = f;
            UpdateTabControl();
            wchar_t m[256];
            swprintf_s(m, L"Saved: %s", f);
            SendMessage(g.hStatusbar, SB_SETTEXT, 0, (LPARAM)m);
        } else {
            MessageBox(hwnd, L"Save failed.", L"Error", MB_OK | MB_ICONERROR);
        }
    }
}

static void NewCanvas(HWND hwnd) {
    DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_RESIZE), hwnd,
        [](HWND hDlg, UINT msg, WPARAM wp, LPARAM) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                SetDlgItemText(hDlg, 101, L"800");
                SetDlgItemText(hDlg, 102, L"600");
                CheckDlgButton(hDlg, 103, BST_CHECKED);
                SetFocus(GetDlgItem(hDlg, 101));
                return FALSE;
            }
            if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
                wchar_t bW[16], bH[16];
                GetDlgItemText(hDlg, 101, bW, 16);
                GetDlgItemText(hDlg, 102, bH, 16);
                int w = _wtoi(bW), h = _wtoi(bH);
                if (w < 1 || h < 1) {
                    MessageBox(hDlg, L"Invalid", L"Error", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                HDC hdcScr = GetDC(nullptr);
                HBITMAP hb = CreateCompatibleBitmap(hdcScr, w, h);
                ReleaseDC(nullptr, hdcScr);
                HDC hdc = CreateCompatibleDC(nullptr);
                SelectObject(hdc, hb);
                HBRUSH hBr = CreateSolidBrush(RGB(255, 255, 255));
                RECT fr = { 0, 0, w, h };
                FillRect(hdc, &fr, hBr);
                DeleteObject(hBr);
                DeleteDC(hdc);
                SetImage(hb, w, h);
                FitToWindow();
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            if (msg == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 2)) {
                EndDialog(hDlg, 0); return TRUE;
            }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            return FALSE;
        }, 0);
}

static void UpdateStatus() {
    if (!g.hStatusbar) return;
    wchar_t buf[256];
    const wchar_t* names[] = { L"Select", L"Pen", L"Line", L"Rectangle", L"Ellipse", L"Arrow", L"Eraser", L"Fill", L"Crop", L"Text", L"Callout", L"Highlighter", L"Pixelate", L"None" };
    int ti = (int)g.currentTool;
    TabData* t = ActiveTab();
    if (t && t->hasImage) {
        const wchar_t* selInfo = L"";
        if (t->selIdx >= 0 && t->selIdx < (int)t->strokes.size())
            selInfo = L" | Shape selected";
        swprintf_s(buf, L"%dx%d | Zoom: %.0f%% | Tool: %s%s | Strokes: %zu",
            t->imgW, t->imgH, t->zoom * 100.0, names[ti], selInfo, t->strokes.size());
    } else {
        swprintf_s(buf, L"Ready - F11: Fullscreen Capture | F12: Region Capture | Ctrl+N: New | Ctrl+O: Open");
    }
    SendMessage(g.hStatusbar, SB_SETTEXT, 0, (LPARAM)buf);
}

static void PositionTabCloseBtns() {
    if (!g.hwndTab) return;
    for (HWND hb : g.tabCloseBtns) DestroyWindow(hb);
    g.tabCloseBtns.clear();
    int count = TabCtrl_GetItemCount(g.hwndTab);
    int btnW = 18, btnH = 18;
    for (int i = 0; i < count; i++) {
        RECT tabRc = {};
        TabCtrl_GetItemRect(g.hwndTab, i, &tabRc);
        int bx = tabRc.right - btnW - 2;
        int by = tabRc.top + (tabRc.bottom - tabRc.top - btnH) / 2;
        int id = IDC_TAB_CLOSE + i;
        HWND hb = CreateWindowEx(0, L"Button", L"x",
            WS_CHILD | WS_VISIBLE | BS_FLAT | BS_PUSHBUTTON,
            bx, by, btnW, btnH,
            g.hwndTab, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
        SendMessage(hb, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
        g.tabCloseBtns.push_back(hb);
    }
}

static void UpdateTabControl() {
    if (!g.hwndTab) return;
    TCITEM tie = {};
    tie.mask = TCIF_TEXT;
    while (TabCtrl_DeleteItem(g.hwndTab, 0));
    for (size_t i = 0; i < g.tabs.size(); i++) {
        wchar_t label[64];
        if (!g.tabs[i].filename.empty()) {
            const wchar_t* fname = wcsrchr(g.tabs[i].filename.c_str(), L'\\');
            fname = fname ? fname + 1 : g.tabs[i].filename.c_str();
            wcscpy_s(label, fname);
        } else {
            swprintf_s(label, L"Screenshot %d", g.tabs[i].id);
        }
        tie.pszText = label;
        TabCtrl_InsertItem(g.hwndTab, (int)i, &tie);
    }
    TabCtrl_SetCurSel(g.hwndTab, g.activeTab);
    PositionTabCloseBtns();
}

static void SwitchToTab(int idx) {
    if (idx < 0 || idx >= (int)g.tabs.size() || idx == g.activeTab) return;
    g.activeTab = idx;
    TabCtrl_SetCurSel(g.hwndTab, idx);
    TabData* t = ActiveTab();
    if (t && t->hasImage) FitToWindow();
    UpdateCanvasScrollBars();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    InvalidateRect(g.hwndMain, nullptr, FALSE);
    RebuildToolbar();
    UpdateStatus();
    PositionTabCloseBtns();
}
static void CloseTab(int idx) {
    if (idx < 0 || idx >= (int)g.tabs.size()) return;
    if (g.tabs[idx].hBitmap) DeleteObject(g.tabs[idx].hBitmap);
    FreeSnaps(g.tabs[idx].imgUndo);
    FreeSnaps(g.tabs[idx].imgRedo);
    g.tabs.erase(g.tabs.begin() + idx);
    if (g.tabs.empty()) {
        g.activeTab = -1;
        InvalidateRect(g.hwndCanvas, nullptr, TRUE);
    } else {
        if (g.activeTab >= (int)g.tabs.size()) g.activeTab = (int)g.tabs.size() - 1;
TabData* t = ActiveTab();
        if (t && t->hasImage) FitToWindow();
    }
    UpdateTabControl();
    UpdateCanvasScrollBars();
    RebuildToolbar();
    UpdateStatus();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

static void DrawFontIcon(HDC hdc, int cx, int cy, wchar_t glyph, COLORREF c, bool mirror = false) {
    int oldMode = 0;
    XFORM oldXf = {};
    if (mirror) {
        oldMode = SetGraphicsMode(hdc, GM_ADVANCED);
        GetWorldTransform(hdc, &oldXf);
        XFORM xf = { -1.0f, 0.0f, 0.0f, 1.0f, (FLOAT)(2 * cx), 0.0f };
        SetWorldTransform(hdc, &xf);
    }
    HFONT hf = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    HFONT old = (HFONT)SelectObject(hdc, hf);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c);
    wchar_t str[2] = { glyph, 0 };
    SIZE sz;
    GetTextExtentPoint32W(hdc, str, 1, &sz);
    TextOutW(hdc, cx - sz.cx / 2, cy - sz.cy / 2, str, 1);
    SelectObject(hdc, old);
    DeleteObject(hf);
    if (mirror) {
        SetWorldTransform(hdc, &oldXf);
        SetGraphicsMode(hdc, oldMode);
    }
}

struct IconBtn { int id; wchar_t glyph; };
static const IconBtn ICON_BUTTONS[] = {
    { IDM_FILE_NEW,           0xE7C3 },
    { IDM_FILE_OPEN,          0xE838 },
    { IDM_FILE_SAVE,          0xE74E },
    { 0, 0 },
    { IDM_EDIT_UNDO,          0xE7A7 },
    { IDM_EDIT_REDO,          0xE7A6 },
    { 0, 0 },
    { IDM_EDIT_COPY,          0xE8C8 },
    { IDM_EDIT_PASTE,         0xE77F },
    { 0, 0 },
    { IDM_CAPTURE_FULLSCREEN, 0xE740 },
    { IDM_CAPTURE_REGION,     0xE8B3 },
    { IDM_CAPTURE_SCROLLING,  0xEC8F },
    { 0, 0 },
    { IDM_TOOL_SELECT,        0xE7C9 },
    { IDM_TOOL_PEN,           0xE76D },
    { IDM_TOOL_LINE,          0xEC87 },
    { IDM_TOOL_RECT,          0xE739 },
    { IDM_TOOL_ELLIPSE,       0xEA3A },
    { IDM_TOOL_ARROW,         0xE72A },
    { IDM_TOOL_ERASER,        0xE75C },
    { IDM_TOOL_FILL,          0xE790 },
    { IDM_TOOL_TEXT,          0xE8D2 },
    { IDM_TOOL_CALLOUT,      0xE90A },
    { IDM_TOOL_HIGHLIGHTER,  0xE891 },
    { IDM_TOOL_PIXELATE,     0xE80A },
    { 0, 0 },
    { IDM_IMAGE_ROT_LEFT,    0xE7AD },
    { IDM_IMAGE_ROT_RIGHT,   0xE7AD },
    { IDM_IMAGE_FLIP_H,      0xE8AB },
    { IDM_IMAGE_MENU,        0xE9E9 },
    { 0, 0 },
    { IDM_RECORD_TOGGLE,     0xE768 },
    { 0, 0 },
    { IDC_COLOR_BTN,          0xE790 },
    { IDC_PEN_WIDTH,          0xE8E3 },
    { IDM_EDIT_CROP,          0xE7A8 },
    { IDM_EDIT_RESIZE,        0xE744 },
};

static const wchar_t* GetTooltipForId(int id) {
    switch (id) {
    case IDM_FILE_NEW:           return L"New Canvas (Ctrl+N)";
    case IDM_FILE_OPEN:          return L"Open Image (Ctrl+O)";
    case IDM_FILE_SAVE:          return L"Save Image (Ctrl+S)";
    case IDM_EDIT_UNDO:          return L"Undo (Ctrl+Z)";
    case IDM_EDIT_REDO:          return L"Redo (Ctrl+Y)";
    case IDM_EDIT_COPY:          return L"Copy (Ctrl+C)";
    case IDM_EDIT_PASTE:         return L"Paste (Ctrl+V)";
    case IDM_CAPTURE_FULLSCREEN: return L"Capture Fullscreen (F11)";
    case IDM_CAPTURE_REGION:     return L"Capture Region (F12)";
    case IDM_CAPTURE_SCROLLING:  return L"Scrolling Capture (F10)";
    case IDM_TOOL_SELECT:        return L"Select / Move Shape (V)";
    case IDM_TOOL_PEN:           return L"Pen Tool (Draw freehand)";
    case IDM_TOOL_LINE:          return L"Line Tool";
    case IDM_TOOL_RECT:          return L"Rectangle Tool";
    case IDM_TOOL_ELLIPSE:       return L"Ellipse Tool";
    case IDM_TOOL_ARROW:         return L"Arrow Tool";
    case IDM_TOOL_ERASER:        return L"Eraser Tool";
    case IDM_TOOL_FILL:          return L"Fill Tool";
    case IDM_TOOL_TEXT:          return L"Text Tool (Click to place text)";
    case IDM_TOOL_CALLOUT:      return L"Callout Tool (Drag to place arrow + text bubble)";
    case IDM_TOOL_HIGHLIGHTER:  return L"Highlighter Tool (Semi-transparent pen)";
    case IDC_COLOR_BTN:          return L"Pen Color";
    case IDC_PEN_WIDTH:          return L"Pen Width";
    case IDM_EDIT_CROP:          return L"Crop Image";
    case IDM_EDIT_RESIZE:        return L"Resize Image";
    case IDM_RECORD_TOGGLE:      return L"Start / Stop Screen Recording (2 fps)";
    case IDM_TOOL_PIXELATE:      return L"Pixelate Tool (Drag to blur out a region)";
    case IDM_IMAGE_ROT_LEFT:     return L"Rotate Left (Ctrl+L)";
    case IDM_IMAGE_ROT_RIGHT:    return L"Rotate Right (Ctrl+R)";
    case IDM_IMAGE_FLIP_H:       return L"Flip Horizontal";
    case IDM_IMAGE_MENU:         return L"Image Adjustments (flip, grayscale, brightness, blur...)";
    default: return L"";
    }
}

static bool GetJpegClsid(CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, L"image/jpeg") == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

static void RecordFrame() {
    if (!g.isRecording || !g.aviStream) return;

    int vw = g.recFrameW;
    int vh = g.recFrameH;
    int vx = g.recMonX;
    int vy = g.recMonY;

    HDC hScr = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hScr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (hBmp) {
        HGDIOBJ old = SelectObject(hdc, hBmp);
        BitBlt(hdc, 0, 0, vw, vh, hScr, vx, vy, SRCCOPY);
        SelectObject(hdc, old);
        int row32 = vw * 4;
        g.recBuf24.resize(g.recRow24 * vh);
        BYTE* src = (BYTE*)pBits;
        BYTE* dst = g.recBuf24.data();
        for (int y = 0; y < vh; y++)
            for (int x = 0; x < vw; x++) {
                dst[y * g.recRow24 + x * 3 + 0] = src[y * row32 + x * 4 + 0];
                dst[y * g.recRow24 + x * 3 + 1] = src[y * row32 + x * 4 + 1];
                dst[y * g.recRow24 + x * 3 + 2] = src[y * row32 + x * 4 + 2];
            }
        BITMAPINFO bmi24 = {};
        bmi24.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi24.bmiHeader.biWidth = vw;
        bmi24.bmiHeader.biHeight = -vh;
        bmi24.bmiHeader.biPlanes = 1;
        bmi24.bmiHeader.biBitCount = 24;
        bmi24.bmiHeader.biCompression = BI_RGB;
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromBITMAPINFO(&bmi24, dst);
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            IStream* stm = nullptr;
            if (CreateStreamOnHGlobal(nullptr, TRUE, &stm) == S_OK) {
                Gdiplus::EncoderParameters eps = {};
                eps.Count = 1;
                eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
                eps.Parameter[0].Type = EncoderParameterValueTypeLong;
                eps.Parameter[0].NumberOfValues = 1;
                ULONG quality = 85;
                eps.Parameter[0].Value = &quality;
                if (bmp->Save(stm, &g.recJpegClsid, &eps) == Gdiplus::Ok) {
                    HGLOBAL hg = nullptr;
                    if (GetHGlobalFromStream(stm, &hg) == S_OK) {
                        void* jp = GlobalLock(hg);
                        SIZE_T js = GlobalSize(hg);
                        g.recJpegBuf.resize(js);
                        memcpy(g.recJpegBuf.data(), jp, js);
                        GlobalUnlock(hg);
                        AVIStreamWrite(g.aviStream, g.recFrameCount++, 1, g.recJpegBuf.data(), (LONG)js, AVIIF_KEYFRAME, nullptr, nullptr);
                    }
                }
                stm->Release();
            }
        }
        delete bmp;
        DeleteObject(hBmp);
    }
    DeleteDC(hdc);
    ReleaseDC(nullptr, hScr);
}

static void StopRecording(HWND hwnd) {
    if (g.recTimer) { KillTimer(hwnd, g.recTimer); g.recTimer = 0; }
    g.recJpegBuf.clear();
    if (g.aviStream) { AVIStreamRelease(g.aviStream); g.aviStream = nullptr; }
    if (g.aviFile) { AVIFileRelease(g.aviFile); g.aviFile = nullptr; }
    AVIFileExit();
    g.isRecording = false;
    g.recFrameCount = 0;
    RebuildToolbar();
    InvalidateRect(hwnd, nullptr, FALSE);
    wchar_t msg[64];
    swprintf_s(msg, L"Recording stopped");
    SendMessage(g.hStatusbar, SB_SETTEXT, 0, (LPARAM)msg);
}

extern std::vector<RECT> g_monitorRects;
extern BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT, LPARAM);

static void StartRecording(HWND hwnd) {
    g_monitorRects.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
    if (g_monitorRects.empty()) return;

    int monCount = (int)g_monitorRects.size();
    int selMon = 0;

    if (monCount > 1) {
        POINT pt = { 0, 0 };
        ClientToScreen(hwnd, &pt);
        RECT tbRc;
        GetWindowRect(hwnd, &tbRc);
        pt.x = tbRc.left + 200;
        pt.y = tbRc.top + g.toolbarH + 2;

        HMENU hMenu = CreatePopupMenu();
        for (int i = 0; i < monCount && i < 8; i++) {
            const RECT& mr = g_monitorRects[i];
            wchar_t label[64];
            wsprintfW(label, L"Monitor %d  (%dx%d)", i + 1,
                mr.right - mr.left, mr.bottom - mr.top);
            AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_MON1 + i, label);
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_MON_ALL, L"All Monitors");
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
            pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (cmd == 0) return;
        if (cmd >= IDM_CAPTURE_MON1 && cmd <= IDM_CAPTURE_MON8) {
            selMon = cmd - IDM_CAPTURE_MON1;
            if (selMon >= monCount) return;
        } else {
            selMon = -1;
        }
    }

    wchar_t f[MAX_PATH] = L"recording.avi";
    OPENFILENAME of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = hwnd;
    of.lpstrFilter = L"AVI Video (*.avi)\0*.avi\0All Files (*.*)\0*.*\0";
    of.lpstrFile = f;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_OVERWRITEPROMPT;
    of.lpstrDefExt = L"avi";
    if (!GetSaveFileName(&of)) return;

    if (selMon < 0) {
        g.recMonX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g.recMonY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g.recFrameW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g.recFrameH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        const RECT& mr = g_monitorRects[selMon];
        g.recMonX = mr.left;
        g.recMonY = mr.top;
        g.recFrameW = mr.right - mr.left;
        g.recFrameH = mr.bottom - mr.top;
    }

    AVIFileInit();
    HRESULT hr = AVIFileOpen(&g.aviFile, f, OF_WRITE | OF_CREATE, nullptr);
    if (hr != AVIERR_OK) {
        AVIFileExit();
        MessageBox(hwnd, L"Failed to create AVI file.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    AVISTREAMINFO si = {};
    si.fccType = streamtypeVIDEO;
    si.fccHandler = 0;
    si.dwScale = 1;
    si.dwRate = 2;
    si.dwSuggestedBufferSize = g.recFrameW * g.recFrameH * 4;
    SetRect(&si.rcFrame, 0, 0, g.recFrameW, g.recFrameH);
    hr = AVIFileCreateStream(g.aviFile, &g.aviStream, &si);
    if (hr != AVIERR_OK) {
        AVIFileRelease(g.aviFile); g.aviFile = nullptr;
        AVIFileExit();
        MessageBox(hwnd, L"Failed to create video stream.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    g.recRow24 = ((g.recFrameW * 24 + 31) / 32) * 4;
    GetJpegClsid(&g.recJpegClsid);

    BITMAPINFOHEADER bmih = {};
    bmih.biSize = sizeof(BITMAPINFOHEADER);
    bmih.biWidth = g.recFrameW;
    bmih.biHeight = g.recFrameH;
    bmih.biPlanes = 1;
    bmih.biBitCount = 24;
    bmih.biCompression = mmioFOURCC('M','J','P','G');
    bmih.biSizeImage = 0;

    hr = AVIStreamSetFormat(g.aviStream, 0, &bmih, sizeof(BITMAPINFOHEADER));
    if (hr != AVIERR_OK) {
        AVIStreamRelease(g.aviStream); g.aviStream = nullptr;
        AVIFileRelease(g.aviFile); g.aviFile = nullptr;
        AVIFileExit();
        MessageBox(hwnd, L"Failed to set video format.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    g.isRecording = true;
    g.recFrameCount = 0;
    g.recTimer = SetTimer(hwnd, 3001, 500, nullptr);
    RecordFrame();
    RebuildToolbar();
    InvalidateRect(hwnd, nullptr, FALSE);
    wchar_t msg[64];
    swprintf_s(msg, L"Recording started (2 fps) — %dx%d", g.recFrameW, g.recFrameH);
    SendMessage(g.hStatusbar, SB_SETTEXT, 0, (LPARAM)msg);
}

static void RebuildToolbar() {
    g.tbBtns.clear();
    TabData* t = ActiveTab();
    int x = 4;
    int bh = 32;
    int by = (g.toolbarH - bh) / 2;
    int btnW = 32;

    for (int i = 0; i < _countof(ICON_BUTTONS); i++) {
        if (ICON_BUTTONS[i].glyph == 0) {
            g.tbBtns.push_back({ 0, x, by, 7, bh, true, true });
            x += 8;
        } else {
            bool enabled = true;
            if (ICON_BUTTONS[i].id == IDM_EDIT_UNDO) enabled = t && (!t->strokes.empty() || !t->imgUndo.empty());
            if (ICON_BUTTONS[i].id == IDM_EDIT_REDO) enabled = t && (!t->redoStack.empty() || CanImageRedo(t));
            if (ICON_BUTTONS[i].id >= IDM_IMAGE_MENU && ICON_BUTTONS[i].id <= IDM_IMAGE_BORDER)
                enabled = t && t->hasImage;
            g.tbBtns.push_back({ ICON_BUTTONS[i].id, x, by, btnW, bh, false, enabled });
            x += btnW + 1;
        }
    }

}

// ---------------------------------------------------------------------------
// Toolbar tooltips. The toolbar is custom-drawn, so instead of relying on the
// tooltip control's own hover detection (which never fired reliably here) we
// track the hovered button ourselves and show one "tracking" tooltip under it.
// ---------------------------------------------------------------------------
static const UINT_PTR TIP_TIMER_ID = 3002;
static const UINT TIP_DELAY_MS = 400;
static bool g_tipShown = false;
static bool g_trackingLeave = false;

static TOOLINFO TipToolInfo() {
    TOOLINFO ti = {};
    // The V2 size is accepted by every comctl32 version. sizeof(TOOLINFO) is
    // the V3 size, which the old (v5) library rejects, so when the exe ran
    // without its manifest no tooltip was ever registered.
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.hwnd = g.hwndMain;
    ti.uId = 1;
    return ti;
}

static void CreateToolbarTooltip(HWND hwnd) {
    g.hwndTooltip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(g.hwndTooltip, TTM_SETMAXTIPWIDTH, 0, 400);
    TOOLINFO ti = TipToolInfo();
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.lpszText = (LPWSTR)L"";
    SendMessage(g.hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
}

static void HideToolbarTip() {
    if (g.hwndMain) KillTimer(g.hwndMain, TIP_TIMER_ID);
    if (g_tipShown && g.hwndTooltip) {
        TOOLINFO ti = TipToolInfo();
        SendMessage(g.hwndTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    }
    g_tipShown = false;
}

static void ShowToolbarTip(int btnIdx) {
    if (!g.hwndTooltip || btnIdx < 0 || btnIdx >= (int)g.tbBtns.size()) return;
    const auto& b = g.tbBtns[btnIdx];
    const wchar_t* text = GetTooltipForId(b.id);
    if (b.isSep || !text || !*text) { HideToolbarTip(); return; }
    TOOLINFO ti = TipToolInfo();
    ti.lpszText = (LPWSTR)text;
    SendMessage(g.hwndTooltip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
    POINT pt = { b.x, b.y + b.h + 4 };
    ClientToScreen(g.hwndMain, &pt);
    SendMessage(g.hwndTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM((WORD)(short)pt.x, (WORD)(short)pt.y));
    SendMessage(g.hwndTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    g_tipShown = true;
}

// Called whenever the hovered toolbar button changes (-1 = none).
static void OnToolbarHoverChanged(int newIdx) {
    if (newIdx < 0) { HideToolbarTip(); return; }
    if (g_tipShown) ShowToolbarTip(newIdx);          // already showing: follow at once
    else SetTimer(g.hwndMain, TIP_TIMER_ID, TIP_DELAY_MS, nullptr);
}

static void RenderToolbar(HDC hdc, RECT& rc) {
    HBRUSH hBr = CreateSolidBrush(RGB(37, 37, 38));
    FillRect(hdc, &rc, hBr);
    DeleteObject(hBr);

    HPEN hDiv = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
    HPEN hold = (HPEN)SelectObject(hdc, hDiv);
    MoveToEx(hdc, 0, rc.bottom - 1, nullptr);
    LineTo(hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, hold);
    DeleteObject(hDiv);

    for (size_t i = 0; i < g.tbBtns.size(); i++) {
        const auto& b = g.tbBtns[i];
        if (b.isSep) {
            HPEN sep = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
            HPEN oldSep = (HPEN)SelectObject(hdc, sep);
            MoveToEx(hdc, b.x + 3, b.y + 4, nullptr);
            LineTo(hdc, b.x + 3, b.y + b.h - 4);
            SelectObject(hdc, oldSep);
            DeleteObject(sep);
            continue;
        }

        RECT br = { b.x, b.y, b.x + b.w, b.y + b.h };
        bool hover = ((int)i == g.hoverBtn);
        bool selected = false;
        if (b.id == IDM_TOOL_SELECT) selected = (g.currentTool == Tool::Select);
        else if (b.id == IDM_TOOL_PEN) selected = (g.currentTool == Tool::Pen);
        else if (b.id == IDM_TOOL_LINE) selected = (g.currentTool == Tool::Line);
        else if (b.id == IDM_TOOL_RECT) selected = (g.currentTool == Tool::Rectangle);
        else if (b.id == IDM_TOOL_ELLIPSE) selected = (g.currentTool == Tool::Ellipse);
        else if (b.id == IDM_TOOL_ERASER) selected = (g.currentTool == Tool::Eraser);
        else if (b.id == IDM_TOOL_TEXT) selected = (g.currentTool == Tool::Text);
        else if (b.id == IDM_TOOL_ARROW) selected = (g.currentTool == Tool::Arrow);
        else if (b.id == IDM_TOOL_FILL) selected = (g.currentTool == Tool::Fill);
        else if (b.id == IDM_TOOL_CALLOUT) selected = (g.currentTool == Tool::Callout);
        else if (b.id == IDM_TOOL_HIGHLIGHTER) selected = (g.currentTool == Tool::Highlighter);
        else if (b.id == IDM_TOOL_PIXELATE) selected = (g.currentTool == Tool::Pixelate);
        else if (b.id == IDM_EDIT_CROP) selected = (g.currentTool == Tool::Crop);

        if (selected) {
            HBRUSH hbs = CreateSolidBrush(RGB(0, 122, 204));
            RoundRect(hdc, br.left, br.top, br.right, br.bottom, 4, 4);
            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, hbs);
            HPEN noPen = (HPEN)SelectObject(hdc, (HPEN)GetStockObject(NULL_PEN));
            RoundRect(hdc, br.left + 1, br.top + 1, br.right - 1, br.bottom - 1, 3, 3);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, noPen);
            DeleteObject(hbs);
        } else if (hover) {
            HBRUSH hbh = CreateSolidBrush(RGB(62, 62, 66));
            HPEN noPen = (HPEN)SelectObject(hdc, (HPEN)GetStockObject(NULL_PEN));
            HBRUSH oldBr = (HBRUSH)SelectObject(hdc, hbh);
            RoundRect(hdc, br.left, br.top, br.right, br.bottom, 4, 4);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, noPen);
            DeleteObject(hbh);
        }

        COLORREF iconColor = !b.enabled ? RGB(80, 80, 80) :
                             selected ? RGB(255, 255, 255) :
                             hover ? RGB(240, 240, 240) : RGB(180, 180, 180);

        for (int fi = 0; fi < _countof(ICON_BUTTONS); fi++) {
            if (ICON_BUTTONS[fi].id == b.id && ICON_BUTTONS[fi].glyph) {
                if (b.id == IDC_COLOR_BTN) {
                    HBRUSH br = CreateSolidBrush(g.penColor);
                    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
                    HPEN op = (HPEN)SelectObject(hdc, pen);
                    HGDIOBJ ob = SelectObject(hdc, br);
                    RoundRect(hdc, b.x + b.w / 2 - 7, b.y + b.h / 2 - 7, b.x + b.w / 2 + 7, b.y + b.h / 2 + 7, 4, 4);
                    SelectObject(hdc, op); SelectObject(hdc, ob);
                    DeleteObject(pen); DeleteObject(br);
                } else if (b.id == IDC_PEN_WIDTH) {
                    COLORREF fc = iconColor;
                    HPEN pen;
                    HPEN op2;
                    pen = CreatePen(PS_SOLID, 1, fc); op2 = (HPEN)SelectObject(hdc, pen);
                    MoveToEx(hdc, b.x + b.w / 2 - 8, b.y + b.h / 2 - 5, nullptr); LineTo(hdc, b.x + b.w / 2 + 8, b.y + b.h / 2 - 5);
                    SelectObject(hdc, op2); DeleteObject(pen);
                    pen = CreatePen(PS_SOLID, 2, fc); op2 = (HPEN)SelectObject(hdc, pen);
                    MoveToEx(hdc, b.x + b.w / 2 - 8, b.y + b.h / 2, nullptr); LineTo(hdc, b.x + b.w / 2 + 8, b.y + b.h / 2);
                    SelectObject(hdc, op2); DeleteObject(pen);
                    pen = CreatePen(PS_SOLID, 3, fc); op2 = (HPEN)SelectObject(hdc, pen);
                    MoveToEx(hdc, b.x + b.w / 2 - 8, b.y + b.h / 2 + 5, nullptr); LineTo(hdc, b.x + b.w / 2 + 8, b.y + b.h / 2 + 5);
                    SelectObject(hdc, op2); DeleteObject(pen);
                } else {
                    wchar_t glyph = ICON_BUTTONS[fi].glyph;
                    if (b.id == IDM_RECORD_TOGGLE && g.isRecording) glyph = 0xE71A;
                    DrawFontIcon(hdc, b.x + b.w / 2, b.y + b.h / 2, glyph, iconColor,
                                 b.id == IDM_IMAGE_ROT_LEFT);
                }
                break;
            }
        }
    }
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hMemBmp = CreateCompatibleBitmap(hdcScreen, rc.right, rc.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hMemBmp);

        HBRUSH hb = CreateSolidBrush(RGB(45, 45, 48));
        FillRect(hdcMem, &rc, hb);
        DeleteObject(hb);

        if (!T || !T->hasImage) {
            SetBkMode(hdcMem, TRANSPARENT);
            SetTextColor(hdcMem, RGB(150, 150, 150));
            HFONT hf = CreateFont(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT hof = (HFONT)SelectObject(hdcMem, hf);
            const wchar_t* m1 = L"No image loaded";
            const wchar_t* m2 = L"Ctrl+N: New Canvas  |  Ctrl+O: Open Image  |  F11: Capture Fullscreen  |  F12: Capture Region";
            SIZE s1, s2;
            GetTextExtentPoint32(hdcMem, m1, (int)wcslen(m1), &s1);
            GetTextExtentPoint32(hdcMem, m2, (int)wcslen(m2), &s2);
            TextOut(hdcMem, (rc.right - s1.cx) / 2, rc.bottom / 2 - 30, m1, (int)wcslen(m1));
            SetTextColor(hdcMem, RGB(110, 110, 110));
            HFONT hf2 = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SelectObject(hdcMem, hf2);
            TextOut(hdcMem, (rc.right - s2.cx) / 2, rc.bottom / 2 + 10, m2, (int)wcslen(m2));
            SelectObject(hdcMem, hof);
            DeleteObject(hf);
            DeleteObject(hf2);
        } else {
            HDC hdcSrc = CreateCompatibleDC(hdcMem);
            SelectObject(hdcSrc, T->hBitmap);
            SetStretchBltMode(hdcMem, HALFTONE);
            int dx = (int)T->offsetX, dy = (int)T->offsetY;
            int dw = (int)(T->imgW * T->zoom), dh = (int)(T->imgH * T->zoom);
            StretchBlt(hdcMem, dx, dy, dw, dh, hdcSrc, 0, 0, T->imgW, T->imgH, SRCCOPY);

            HPEN hPen = nullptr;
            HPEN ho = nullptr;
            double zx = T->zoom, ox = T->offsetX, oy = T->offsetY;

            for (size_t si = 0; si < T->strokes.size(); si++) {
                const auto& s = T->strokes[si];

                if (s.tool == Tool::Highlighter) {
                    if (s.points.size() >= 2) {
                        Gdiplus::Graphics gpGraphics(hdcMem);
                        gpGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        BYTE a = 80, r = GetRValue(s.color), gr = GetGValue(s.color), b = GetBValue(s.color);
                        Gdiplus::Color hc(a, r, gr, b);
                        int hw = max(2, (int)(s.penWidth * T->zoom));
                        Gdiplus::Pen gpPen(hc, (Gdiplus::REAL)hw);
                        gpPen.SetStartCap(Gdiplus::LineCapRound);
                        gpPen.SetEndCap(Gdiplus::LineCapRound);
                        gpPen.SetLineJoin(Gdiplus::LineJoinRound);
                        Gdiplus::Point* pts = new Gdiplus::Point[(int)s.points.size()];
                        for (size_t i = 0; i < s.points.size(); i++) {
                            pts[i].X = (int)(s.points[i].x * zx + ox);
                            pts[i].Y = (int)(s.points[i].y * zx + oy);
                        }
                        gpGraphics.DrawLines(&gpPen, pts, (int)s.points.size());
                        delete[] pts;
                    }
                    continue;
                }

                COLORREF c = s.tool == Tool::Eraser ? RGB(255, 255, 255) : s.color;
                int pw = max(1, (int)(s.penWidth * T->zoom));
                hPen = CreatePen(PS_SOLID, pw, c);
                ho = (HPEN)SelectObject(hdcMem, hPen);

                switch (s.tool) {
                case Tool::Pen:
                case Tool::Eraser:
                    if (s.points.size() >= 2) {
                        MoveToEx(hdcMem, (int)(s.points[0].x * zx + ox), (int)(s.points[0].y * zx + oy), nullptr);
                        for (size_t i = 1; i < s.points.size(); i++)
                            LineTo(hdcMem, (int)(s.points[i].x * zx + ox), (int)(s.points[i].y * zx + oy));
                    }
                    break;
                case Tool::Line:
                    MoveToEx(hdcMem, (int)(s.startPt.x * zx + ox), (int)(s.startPt.y * zx + oy), nullptr);
                    LineTo(hdcMem, (int)(s.endPt.x * zx + ox), (int)(s.endPt.y * zx + oy));
                    break;
                case Tool::Arrow: {
                    double sx2 = s.startPt.x * zx + ox, sy2 = s.startPt.y * zx + oy;
                    double ex2 = s.endPt.x * zx + ox, ey2 = s.endPt.y * zx + oy;
                    MoveToEx(hdcMem, (int)sx2, (int)sy2, nullptr);
                    LineTo(hdcMem, (int)ex2, (int)ey2);
                    double a = atan2(ey2 - sy2, ex2 - sx2);
                    double hl = max(10.0, 15.0 * zx);
                    MoveToEx(hdcMem, (int)ex2, (int)ey2, nullptr);
                    LineTo(hdcMem, (int)(ex2 - hl * cos(a - 0.4)), (int)(ey2 - hl * sin(a - 0.4)));
                    MoveToEx(hdcMem, (int)ex2, (int)ey2, nullptr);
                    LineTo(hdcMem, (int)(ex2 - hl * cos(a + 0.4)), (int)(ey2 - hl * sin(a + 0.4)));
                    break;
                }
                case Tool::Rectangle: {
                    HBRUSH hoBr = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                    Rectangle(hdcMem,
                        (int)(min(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(min(s.startPt.y, s.endPt.y) * zx + oy),
                        (int)(max(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(max(s.startPt.y, s.endPt.y) * zx + oy));
                    SelectObject(hdcMem, hoBr);
                    break;
                }
                case Tool::Ellipse: {
                    HBRUSH hoBr = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                    Ellipse(hdcMem,
                        (int)(min(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(min(s.startPt.y, s.endPt.y) * zx + oy),
                        (int)(max(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(max(s.startPt.y, s.endPt.y) * zx + oy));
                    SelectObject(hdcMem, hoBr);
                    break;
                }
                case Tool::Fill: {
                    SelectObject(hdcMem, ho);
                    DeleteObject(hPen);
                    HBRUSH fb = CreateSolidBrush(s.color);
                    SelectObject(hdcMem, fb);
                    RECT fr = {
                        (int)(min(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(min(s.startPt.y, s.endPt.y) * zx + ox),
                        (int)(max(s.startPt.x, s.endPt.x) * zx + ox),
                        (int)(max(s.startPt.y, s.endPt.y) * zx + ox)
                    };
                    FillRect(hdcMem, &fr, fb);
                    DeleteObject(fb);
                    hPen = nullptr;
                    break;
                }
                case Tool::Text: {
                    if (hPen) { SelectObject(hdcMem, ho); DeleteObject(hPen); hPen = nullptr; }
                    double zz = T->zoom;
                    int fs = max(8, (int)(s.fontSize * zz));
                    HFONT hf = CreateFont(-fs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
                    HFONT hof = (HFONT)SelectObject(hdcMem, hf);
                    SetBkMode(hdcMem, TRANSPARENT);
                    SetTextColor(hdcMem, s.color);
                    int tx = (int)(s.startPt.x * zz + T->offsetX);
                    int ty = (int)(s.startPt.y * zz + T->offsetY);
                    TextOut(hdcMem, tx, ty - fs, s.text.c_str(), (int)s.text.size());
                    SelectObject(hdcMem, hof);
                    DeleteObject(hf);
                    break;
                }
                case Tool::Callout: {
                    if (hPen) { SelectObject(hdcMem, ho); DeleteObject(hPen); hPen = nullptr; }
                    DrawCallout(hdcMem, s, T->zoom, T->offsetX, T->offsetY);
                    break;
                }
                default: break;
                }
                if (hPen) { SelectObject(hdcMem, ho); DeleteObject(hPen); hPen = nullptr; }

                if ((int)si == T->selIdx && T->selIdx >= 0 && T->selIdx < (int)T->strokes.size()) {
                    POINT tl, br;
                    GetBBox(s, tl, br);
                    int sL = (int)(tl.x * zx + ox) - 2;
                    int sT = (int)(tl.y * zx + oy) - 2;
                    int sR = (int)(br.x * zx + ox) + 2;
                    int sB = (int)(br.y * zx + oy) + 2;

                    HPEN selPen = CreatePen(PS_DOT, 1, RGB(0, 120, 215));
                    HBRUSH selBr = (HBRUSH)GetStockObject(NULL_BRUSH);
                    HPEN prevPen = (HPEN)SelectObject(hdcMem, selPen);
                    HBRUSH prevBr = (HBRUSH)SelectObject(hdcMem, selBr);
                    Rectangle(hdcMem, sL, sT, sR, sB);
                    SelectObject(hdcMem, prevPen);
                    SelectObject(hdcMem, prevBr);
                    DeleteObject(selPen);

                    POINT handles[8];
                    handles[0] = { sL, sT };
                    handles[1] = { (sL + sR) / 2, sT };
                    handles[2] = { sR, sT };
                    handles[3] = { sL, (sT + sB) / 2 };
                    handles[4] = { sR, (sT + sB) / 2 };
                    handles[5] = { sL, sB };
                    handles[6] = { (sL + sR) / 2, sB };
                    handles[7] = { sR, sB };

                    int hs = HANDLE_SIZE;
                    for (int hi = 0; hi < 8; hi++) {
                        HBRUSH hHFill = CreateSolidBrush(RGB(255, 255, 255));
                        HPEN hHBorder = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
                        HPEN oldHP = (HPEN)SelectObject(hdcMem, hHBorder);
                        HBRUSH oldHB = (HBRUSH)SelectObject(hdcMem, hHFill);
                        Rectangle(hdcMem, handles[hi].x - hs, handles[hi].y - hs, handles[hi].x + hs, handles[hi].y + hs);
                        SelectObject(hdcMem, oldHP);
                        SelectObject(hdcMem, oldHB);
                        DeleteObject(hHFill);
                        DeleteObject(hHBorder);
                    }
                }
            }
            DeleteDC(hdcSrc);

            if (g.isDrawing && g.currentTool != Tool::Pen && g.currentTool != Tool::Eraser && g.currentTool != Tool::Fill) {
                HPEN trackPen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
                HPEN prevPen2 = (HPEN)SelectObject(hdcMem, trackPen);
                HBRUSH prevBr2 = (HBRUSH)SelectObject(hdcMem, (HBRUSH)GetStockObject(NULL_BRUSH));
                SetBkColor(hdcMem, RGB(45, 45, 48));

                int sx = (int)(g.drawStart.x * zx + ox);
                int sy = (int)(g.drawStart.y * zx + oy);
                int ex = (int)(g.drawEnd.x * zx + ox);
                int ey = (int)(g.drawEnd.y * zx + oy);

                switch (g.currentTool) {
                case Tool::Line:
                    MoveToEx(hdcMem, sx, sy, nullptr);
                    LineTo(hdcMem, ex, ey);
                    break;
                case Tool::Arrow: {
                    MoveToEx(hdcMem, sx, sy, nullptr);
                    LineTo(hdcMem, ex, ey);
                    double a = atan2((double)(ey - sy), (double)(ex - sx));
                    double hl = max(10.0, 15.0 * zx);
                    MoveToEx(hdcMem, ex, ey, nullptr);
                    LineTo(hdcMem, (int)(ex - hl * cos(a - 0.4)), (int)(ey - hl * sin(a - 0.4)));
                    MoveToEx(hdcMem, ex, ey, nullptr);
                    LineTo(hdcMem, (int)(ex - hl * cos(a + 0.4)), (int)(ey - hl * sin(a + 0.4)));
                    break;
                }
                case Tool::Rectangle:
                    Rectangle(hdcMem, min(sx, ex), min(sy, ey), max(sx, ex), max(sy, ey));
                    break;
                case Tool::Ellipse:
                    Ellipse(hdcMem, min(sx, ex), min(sy, ey), max(sx, ex), max(sy, ey));
                    break;
                case Tool::Callout: {
                    // Live preview = exactly what will be placed
                    Stroke pv;
                    pv.tool = Tool::Callout;
                    pv.color = g.penColor;
                    pv.penWidth = g.penWidth;
                    pv.startPt = g.drawStart;
                    pv.endPt = g.drawEnd;
                    pv.text = L"Text";
                    pv.fontSize = g.fontSize;
                    pv.fontName = g.fontName;
                    DrawCallout(hdcMem, pv, zx, ox, oy);
                    break;
                }
                default: break;
                }

                SelectObject(hdcMem, prevPen2);
                SelectObject(hdcMem, prevBr2);
                DeleteObject(trackPen);
            }

            if (g.isCropping) {
                int csx = (int)(g.cropStart.x * zx + ox);
                int csy = (int)(g.cropStart.y * zx + oy);
                int cex = (int)(g.cropEnd.x * zx + ox);
                int cey = (int)(g.cropEnd.y * zx + oy);
                int cl = min(csx, cex), ct = min(csy, cey);
                int cr = max(csx, cex), cb = max(csy, cey);

                HPEN cropPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
                HBRUSH cropBr = CreateSolidBrush(RGB(0, 120, 215));
                HPEN prevPenC = (HPEN)SelectObject(hdcMem, cropPen);
                HBRUSH prevBrC = (HBRUSH)SelectObject(hdcMem, cropBr);
                SetROP2(hdcMem, R2_MASKPEN);
                Rectangle(hdcMem, cl, ct, cr, cb);
                SetROP2(hdcMem, R2_COPYPEN);
                SelectObject(hdcMem, prevPenC);
                SelectObject(hdcMem, prevBrC);
                DeleteObject(cropPen);
                DeleteObject(cropBr);

                HPEN cropBorder = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
                SelectObject(hdcMem, cropBorder);
                SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                SetBkColor(hdcMem, RGB(45, 45, 48));
                Rectangle(hdcMem, cl, ct, cr, cb);
                SelectObject(hdcMem, prevPenC);
                DeleteObject(cropBorder);
            }
        }

        BitBlt(hdcScreen, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    case WM_LBUTTONDOWN: {
        if (!T || !T->hasImage) break;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT ip = CanvasToImage(pt);

        if (wp & MK_MBUTTON || GetAsyncKeyState(VK_SPACE) & 0x8000) {
            g.isPanning = true;
            g.panLast = pt;
            SetCapture(hwnd);
            return 0;
        }

        if (g.currentTool == Tool::Select) {
            int hit = HitTestShape(ip);

            if (hit >= 0 && hit != T->selIdx) {
                T->selIdx = hit;
                g.isDraggingSel = true;
                g.dragStartImg = ip;
                T->selBackup = T->strokes[hit];
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateStatus();
                return 0;
            }

            int hh = HitTestHandle(pt);
            if (hh >= 0 && T->selIdx >= 0) {
                g.isResizingSel = true;
                g.resizeHandle = hh;
                g.dragStartImg = ip;
                T->selBackup = T->strokes[T->selIdx];
                SetCapture(hwnd);
                return 0;
            }

            if (hit >= 0) {
                T->selIdx = hit;
                g.isDraggingSel = true;
                g.dragStartImg = ip;
                T->selBackup = T->strokes[hit];
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateStatus();
                return 0;
            }

            T->selIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateStatus();
            return 0;
        }

        if (g.currentTool == Tool::Fill) {
            if (ip.x >= 0 && ip.x < T->imgW && ip.y >= 0 && ip.y < T->imgH) {
                HDC hdc = CreateCompatibleDC(nullptr);
                SelectObject(hdc, T->hBitmap);
                HBRUSH fb = CreateSolidBrush(g.penColor);
                RECT fr = { ip.x - 1, ip.y - 1, ip.x + 2, ip.y + 2 };
                FillRect(hdc, &fr, fb);
                DeleteObject(fb);
                DeleteDC(hdc);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (g.currentTool == Tool::Text) {
            Stroke s;
            s.tool = Tool::Text;
            s.color = g.penColor;
            s.penWidth = g.penWidth;
            s.startPt = ip;
            s.endPt = ip;
            s.fontSize = g.fontSize;
            s.fontName = g.fontName;
            s.text = L"Text";
            T->strokes.push_back(s);
            T->redoStack.clear();
            T->selIdx = (int)T->strokes.size() - 1;
            g_pendingNewAnnot = T->selIdx;
            g.currentTool = Tool::Select;
            PostMessage(hwnd, WM_DEFER_FONTDLG, 0, 0);
            RefreshChrome();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g.currentTool == Tool::Crop || g.currentTool == Tool::Pixelate) {
            g.isCropping = true;
            g.cropStart = ip;
            g.cropEnd = ip;
            SetCapture(hwnd);
            return 0;
        }

        g.isDrawing = true;
        g.drawStart = ip;
        g.drawEnd = ip;
        SetCapture(hwnd);   // keep receiving the mouse until the button is released

        if (g.currentTool == Tool::Pen || g.currentTool == Tool::Eraser || g.currentTool == Tool::Highlighter) {
            Stroke s;
            s.tool = g.currentTool;
            s.color = g.currentTool == Tool::Eraser ? RGB(255, 255, 255) : g.penColor;
            s.penWidth = g.currentTool == Tool::Eraser ? g.penWidth * 3 :
                         g.currentTool == Tool::Highlighter ? g.penWidth * 3 : g.penWidth;
            s.points.push_back(ip);
            T->strokes.push_back(s);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        if (T && g.currentTool == Tool::Select && T->selIdx >= 0 && T->selIdx < (int)T->strokes.size() &&
            (T->strokes[T->selIdx].tool == Tool::Text || T->strokes[T->selIdx].tool == Tool::Callout)) {
            PostMessage(hwnd, WM_DEFER_FONTDLG, 0, 0);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.isPanning) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            ScrollCanvasBy(g.panLast.x - pt.x, g.panLast.y - pt.y);
            g.panLast = pt;
            return 0;
        }
        if (g.isDraggingSel && T->selIdx >= 0) {
            POINT ip = CanvasToImage(pt);
            int dx = ip.x - g.dragStartImg.x;
            int dy = ip.y - g.dragStartImg.y;
            auto& s = T->strokes[T->selIdx];
            if (s.tool == Tool::Pen || s.tool == Tool::Eraser || s.tool == Tool::Highlighter) {
                for (size_t i = 0; i < s.points.size(); i++) {
                    s.points[i].x = T->selBackup.points[i].x + dx;
                    s.points[i].y = T->selBackup.points[i].y + dy;
                }
            } else {
                s.startPt.x = T->selBackup.startPt.x + dx;
                s.startPt.y = T->selBackup.startPt.y + dy;
                s.endPt.x = T->selBackup.endPt.x + dx;
                s.endPt.y = T->selBackup.endPt.y + dy;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isResizingSel && T->selIdx >= 0) {
            POINT ip = CanvasToImage(pt);
            ResizeStroke(T->selIdx, g.resizeHandle, ip);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isDrawing) {
            POINT ip = CanvasToImage(pt);
            g.drawEnd = ip;
            if ((g.currentTool == Tool::Pen || g.currentTool == Tool::Eraser || g.currentTool == Tool::Highlighter) && !T->strokes.empty()) {
                T->strokes.back().points.push_back(ip);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g.currentTool == Tool::Select) {
            int hh = HitTestHandle(pt);
            if (hh >= 0) {
                const wchar_t* cursors[] = { IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE, IDC_SIZEWE, IDC_SIZENESW, IDC_SIZENS, IDC_SIZENWSE };
                SetCursor(LoadCursor(nullptr, cursors[hh]));
            } else {
                int hit = HitTestShape(CanvasToImage(pt));
                SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_SIZEALL : IDC_ARROW));
            }
        }

        if (g.isCropping) {
            g.cropEnd = CanvasToImage(pt);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT ip = CanvasToImage(pt);
        // Whatever this button-up finishes, give the mouse back afterwards.
        // (Resizing used to keep the capture, so later clicks on the toolbar
        // were swallowed by the canvas.)
        struct ReleaseOnExit {
            HWND h;
            ~ReleaseOnExit() { if (GetCapture() == h) ReleaseCapture(); }
        } releaseOnExit{ hwnd };

        if (g.isPanning) {
            g.isPanning = false;
            ReleaseCapture();
            UpdateCanvasScrollBars();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isDraggingSel) {
            g.isDraggingSel = false;
            T->redoStack.clear();
            UpdateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isResizingSel) {
            g.isResizingSel = false;
            T->redoStack.clear();
            UpdateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isCropping) {
            g.isCropping = false;
            ReleaseCapture();
            int x = min(g.cropStart.x, g.cropEnd.x);
            int y = min(g.cropStart.y, g.cropEnd.y);
            int w = abs(g.cropEnd.x - g.cropStart.x);
            int h = abs(g.cropEnd.y - g.cropStart.y);
            if (g.currentTool == Tool::Pixelate) {
                // Stay in pixelate mode so several areas can be hidden in a row.
                if (w > 2 && h > 2) PixelateRegion(x, y, w, h);
                RefreshChrome();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (w > 2 && h > 2) {
                CropImage(x, y, w, h);
            }
            g.currentTool = Tool::Select;
            RebuildToolbar();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g.isDrawing) {
            g.drawEnd = ip;
            g.isDrawing = false;
            if (g.currentTool != Tool::Pen && g.currentTool != Tool::Eraser && g.currentTool != Tool::Highlighter) {
                Stroke s;
                s.tool = g.currentTool;
                s.color = g.penColor;
                s.penWidth = g.penWidth;
                s.startPt = g.drawStart;
                s.endPt = g.drawEnd;
                if (g.currentTool == Tool::Callout) {
                    s.text = L"Text";
                    s.fontSize = g.fontSize;
                    s.fontName = g.fontName;
                    // A plain click (no drag): put the bubble up and to the
                    // right of the clicked spot so the arrow points at it.
                    if (abs(s.endPt.x - s.startPt.x) < 6 && abs(s.endPt.y - s.startPt.y) < 6) {
                        int off = max(60, g.fontSize * 3);
                        s.endPt = { s.startPt.x + off, s.startPt.y - off };
                    }
                }
                T->strokes.push_back(s);
                T->redoStack.clear();
                if (g.currentTool == Tool::Callout) {
                    T->selIdx = (int)T->strokes.size() - 1;
                    g_pendingNewAnnot = T->selIdx;
                    g.currentTool = Tool::Select;
                    RefreshChrome();
                    PostMessage(hwnd, WM_DEFER_FONTDLG, 0, 0);
                }
            } else {
                T->redoStack.clear();
            }
            UpdateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // Lost the mouse in the middle of an operation (another window, a
        // dialog, Alt+Tab): end it cleanly instead of leaving it stuck.
        if ((HWND)lp != hwnd) {
            g.isDraggingSel = false;
            g.isResizingSel = false;
            g.isPanning = false;
            if (g.isCropping) g.isCropping = false;
            if (g.isDrawing) {
                g.isDrawing = false;   // freehand strokes keep what was drawn
                if (T) T->redoStack.clear();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        g.isPanning = true;
        g.panLast = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(hwnd);
        return 0;

    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (g.isPanning) { g.isPanning = false; ReleaseCapture(); }
        return 0;

    case WM_MOUSEWHEEL: {
        // Wheel scrolls, Shift+wheel scrolls sideways, Ctrl+wheel zooms at the cursor.
        if (!T || !T->hasImage) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        WORD keys = GET_KEYSTATE_WPARAM(wp);
        if (keys & MK_CONTROL) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            ZoomAt(delta > 0 ? T->zoom * 1.1 : T->zoom / 1.1, pt.x, pt.y);
        } else {
            int step = -delta * 120 / WHEEL_DELTA;   // 120 px per notch
            if (keys & MK_SHIFT) ScrollCanvasBy(step, 0);
            else ScrollCanvasBy(0, step);
        }
        UpdateStatus();
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        if (!T || !T->hasImage) return 0;
        ScrollCanvasBy(GET_WHEEL_DELTA_WPARAM(wp) * 120 / WHEEL_DELTA, 0);
        return 0;
    }
    case WM_HSCROLL: {
        TabData* t = ActiveTab();
        if (!t || !t->hasImage) return 0;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_HORZ, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT:   pos -= 30; break;
        case SB_LINERIGHT:  pos += 30; break;
        case SB_PAGELEFT:   pos -= (int)si.nPage; break;
        case SB_PAGERIGHT:  pos += (int)si.nPage; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        case SB_LEFT:       pos = si.nMin; break;
        case SB_RIGHT:      pos = si.nMax; break;
        }
        t->offsetX = -pos;              // clamped by UpdateCanvasScrollBars
        UpdateCanvasScrollBars();
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateStatus();
        return 0;
    }
    case WM_VSCROLL: {
        TabData* t = ActiveTab();
        if (!t || !t->hasImage) return 0;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:     pos -= 30; break;
        case SB_LINEDOWN:   pos += 30; break;
        case SB_PAGEUP:     pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:   pos += (int)si.nPage; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        case SB_TOP:        pos = si.nMin; break;
        case SB_BOTTOM:     pos = si.nMax; break;
        }
        t->offsetY = -pos;              // clamped by UpdateCanvasScrollBars
        UpdateCanvasScrollBars();
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateStatus();
        return 0;
    }
    case WM_DEFER_FONTDLG: {
        ShowFontDlg();
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static HWND g_hwndOverlay = nullptr;
static HBITMAP g_overlayScreenBmp = nullptr;
static int g_overlayScreenW = 0, g_overlayScreenH = 0;
static int g_overlayVirtX = 0, g_overlayVirtY = 0;
static POINT g_overlayStart = {};
static POINT g_overlayCur = {};
static bool g_overlayDragging = false;
std::vector<RECT> g_monitorRects;

static void Overlay_Finish(HWND hwndMain, bool ok) {
    if (g_overlayScreenBmp) { DeleteObject(g_overlayScreenBmp); g_overlayScreenBmp = nullptr; }
    if (g_hwndOverlay) { DestroyWindow(g_hwndOverlay); g_hwndOverlay = nullptr; }
    g_overlayDragging = false;
    g_monitorRects.clear();
    if (ok) {
        int x = min(g_overlayStart.x, g_overlayCur.x) + g_overlayVirtX;
        int y = min(g_overlayStart.y, g_overlayCur.y) + g_overlayVirtY;
        int w = abs(g_overlayCur.x - g_overlayStart.x);
        int h = abs(g_overlayCur.y - g_overlayStart.y);
        if (w > 2 && h > 2) {
            HBITMAP hb = CaptureRegion(x, y, w, h);
            ShowWindow(hwndMain, SW_SHOW);
            if (hb) {
                FlattenToBitmap();
                SetImage(hb, w, h);
                FitToWindow();
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                UpdateStatus();
            }
        } else {
            ShowWindow(hwndMain, SW_SHOW);
        }
    } else {
        ShowWindow(hwndMain, SW_SHOW);
    }
    InvalidateRect(hwndMain, nullptr, FALSE);
}

BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC hdcMon, LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMon; (void)hdcMon; (void)dwData;
    RECT rc = *lprcMonitor;
    g_monitorRects.push_back(rc);
    return TRUE;
}

static void Overlay_UpdateRegion() {
    if (!g_hwndOverlay || !g_overlayScreenBmp) return;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmComp = CreateCompatibleBitmap(hdcScreen, g_overlayScreenW, g_overlayScreenH);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmComp);

    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmSrcOld = (HBITMAP)SelectObject(hdcSrc, g_overlayScreenBmp);

    BitBlt(hdcMem, 0, 0, g_overlayScreenW, g_overlayScreenH, hdcSrc, 0, 0, SRCCOPY);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 140, 0 };
    RECT rcFull = { 0, 0, g_overlayScreenW, g_overlayScreenH };
    HBRUSH hbrDim = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdcMem, &rcFull, hbrDim);
    DeleteObject(hbrDim);

    if (g_overlayDragging) {
        int x = min(g_overlayStart.x, g_overlayCur.x);
        int y = min(g_overlayStart.y, g_overlayCur.y);
        int w = abs(g_overlayCur.x - g_overlayStart.x);
        int h = abs(g_overlayCur.y - g_overlayStart.y);
        if (w > 0 && h > 0) {
            BitBlt(hdcMem, x, y, w, h, hdcSrc, x, y, SRCCOPY);
            HPEN hp = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
            HBRUSH hbr2 = (HBRUSH)GetStockObject(NULL_BRUSH);
            HPEN ho = (HPEN)SelectObject(hdcMem, hp);
            HBRUSH hbo2 = (HBRUSH)SelectObject(hdcMem, hbr2);
            Rectangle(hdcMem, x, y, x + w, y + h);
            SelectObject(hdcMem, ho);
            SelectObject(hdcMem, hbo2);
            DeleteObject(hp);
        }
    }

    SelectObject(hdcSrc, hbmSrcOld);
    DeleteDC(hdcSrc);

    POINT ptWnd = { 0, 0 };
    SIZE szWnd = { g_overlayScreenW, g_overlayScreenH };
    POINT ptSrc = { 0, 0 };
    UpdateLayeredWindow(g_hwndOverlay, hdcScreen, nullptr, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    DeleteObject(hbmComp);
    ReleaseDC(nullptr, hdcScreen);
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Overlay_Finish(g.hwndMain, false);
            return 0;
        }
        return 0;

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        g_overlayDragging = true;
        g_overlayStart = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_overlayCur = g_overlayStart;
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_overlayDragging) {
            g_overlayCur = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            Overlay_UpdateRegion();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_overlayDragging) {
            ReleaseCapture();
            g_overlayCur = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_overlayDragging = false;
            Overlay_Finish(g.hwndMain, true);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        Overlay_Finish(g.hwndMain, false);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static WNDPROC g_prevTabProc = nullptr;
static LRESULT CALLBACK TabBtnForwardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        SendMessage(g.hwndMain, msg, wp, lp);
        return 0;
    }
    return CallWindowProc(g_prevTabProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {

    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc);

        g.hStatusbar = CreateWindowEx(0, STATUSCLASSNAME, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, nullptr, nullptr);

        g.hwndTab = CreateWindowEx(0, WC_TABCONTROL, nullptr,
            WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH | TCS_FORCELABELLEFT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TAB, nullptr, nullptr);
        SendMessage(g.hwndTab, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
        RECT tabRc = {};
        SendMessage(g.hwndTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&tabRc);
        int tabH = 24;
        SendMessage(g.hwndTab, TCM_SETITEMSIZE, 0, MAKELPARAM(120, tabH));
        g_prevTabProc = (WNDPROC)SetWindowLongPtr(g.hwndTab, GWLP_WNDPROC, (LONG_PTR)TabBtnForwardProc);

g.hwndCanvas = CreateWindowEx(0, L"Static", nullptr,
            WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | SS_NOTIFY,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CANVAS, nullptr, nullptr);
        SetWindowLongPtr(g.hwndCanvas, GWLP_WNDPROC, (LONG_PTR)CanvasProc);
        // Dark scroll bars to match the canvas (ignored on older Windows).
        SetWindowTheme(g.hwndCanvas, L"DarkMode_Explorer", nullptr);
        UpdateCanvasScrollBars();

        g.hwndMain = hwnd;

        CreateToolbarTooltip(hwnd);

        RebuildToolbar();

        WNDCLASSEX wcOverlay = { sizeof(wcOverlay) };
        wcOverlay.lpfnWndProc = OverlayProc;
        wcOverlay.hInstance = GetModuleHandle(nullptr);
        wcOverlay.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wcOverlay.lpszClassName = L"ScreenCapOverlay";
        RegisterClassEx(&wcOverlay);

        return 0;
    }

case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int stH = 22;
        int tabY = g.toolbarH;
        int tabH = g.tabH;
        MoveWindow(g.hwndTab, 0, tabY, rc.right, tabH, TRUE);
        MoveWindow(g.hwndCanvas, 0, tabY + tabH, rc.right, rc.bottom - tabY - tabH - stH, TRUE);
        SendMessage(g.hStatusbar, WM_SIZE, 0, 0);
        TabData* t = ActiveTab();
        // Re-fit only if the user hasn't zoomed in/out themselves.
        if (t && t->hasImage && t->fitMode) FitToWindow();
        UpdateCanvasScrollBars();
        InvalidateRect(hwnd, nullptr, FALSE);
        InvalidateRect(g.hwndCanvas, nullptr, FALSE);
        UpdateStatus();
        PositionTabCloseBtns();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hMemBmp = CreateCompatibleBitmap(hdcScreen, rc.right, rc.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hMemBmp);
        RECT tbRc = { 0, 0, rc.right, g.toolbarH };
        RenderToolbar(hdcMem, tbRc);
        BitBlt(hdcScreen, 0, 0, rc.right, g.toolbarH, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: return 1;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int newH = -1;
        for (size_t i = 0; i < g.tbBtns.size(); i++) {
            const auto& b = g.tbBtns[i];
            if (!b.isSep && pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h) {
                newH = (int)i;
                break;
            }
        }
        if (!g_trackingLeave) {
            // Get WM_MOUSELEAVE when the cursor leaves the window or moves onto
            // a child (canvas, tabs), so the hover and tooltip are cleared.
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_trackingLeave = TrackMouseEvent(&tme) != FALSE;
        }
        if (newH != g.hoverBtn) {
            g.hoverBtn = newH;
            OnToolbarHoverChanged(newH);
            RECT tr = { 0, 0, 0, g.toolbarH };
            GetClientRect(hwnd, &tr);
            tr.bottom = g.toolbarH;
            InvalidateRect(hwnd, &tr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_trackingLeave = false;
        // Windows sometimes reports a leave while the cursor is still over
        // the toolbar (e.g. when a popup like the tooltip appears). Ignore it.
        POINT cur;
        GetCursorPos(&cur);
        if (WindowFromPoint(cur) == hwnd) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_trackingLeave = TrackMouseEvent(&tme) != FALSE;
            return 0;
        }
        HideToolbarTip();
        if (g.hoverBtn != -1) {
            g.hoverBtn = -1;
            RECT tr;
            GetClientRect(hwnd, &tr);
            tr.bottom = g.toolbarH;
            InvalidateRect(hwnd, &tr, FALSE);
        }
        return 0;
    }

    case WM_ACTIVATEAPP:
        if (!wp) HideToolbarTip();
        break;

    case WM_LBUTTONDOWN: {
        HideToolbarTip();
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (size_t i = 0; i < g.tbBtns.size(); i++) {
            const auto& b = g.tbBtns[i];
            if (!b.isSep && pt.x >= b.x && pt.x < b.x + b.w && pt.y >= b.y && pt.y < b.y + b.h) {
                SendMessage(hwnd, WM_COMMAND, b.id, 0);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDM_FILE_NEW:      NewCanvas(hwnd); break;
        case IDM_FILE_OPEN:     OpenImage(hwnd); break;
        case IDM_FILE_SAVE:
        case IDM_FILE_SAVEAS:   SaveImage(hwnd); break;
        case IDM_FILE_CLOSE_TAB: CloseTab(g.activeTab); break;
        default:
            if (id >= IDC_TAB_CLOSE && id < IDC_TAB_CLOSE + (int)g.tabs.size()) {
                CloseTab(id - IDC_TAB_CLOSE);
            }
            break;
        case IDM_FILE_EXIT:
            if (g.isRecording) StopRecording(hwnd);
            PostQuitMessage(0); break;

        case IDM_EDIT_UNDO:
            if (T && !T->strokes.empty()) {
                T->redoStack.push_back(T->strokes.back());
                T->strokes.pop_back();
                T->selIdx = -1;
            } else if (T) {
                ImageUndo(T);
            }
            break;
        case IDM_EDIT_REDO:
            if (T && !T->redoStack.empty()) {
                T->strokes.push_back(T->redoStack.back());
                T->redoStack.pop_back();
                T->selIdx = -1;
            } else if (T) {
                ImageRedo(T);
            }
            break;

        case IDM_IMAGE_MENU: {
            if (!T || !T->hasImage) break;
            POINT pt = { 0, g.toolbarH };
            for (auto& b : g.tbBtns) if (b.id == IDM_IMAGE_MENU) { pt.x = b.x; pt.y = b.y + b.h; }
            ClientToScreen(hwnd, &pt);
            ShowImageMenu(hwnd, pt.x, pt.y);
            break;
        }
        case IDM_IMAGE_ROT_LEFT:
        case IDM_IMAGE_ROT_RIGHT:
        case IDM_IMAGE_ROT_180:
        case IDM_IMAGE_FLIP_H:
        case IDM_IMAGE_FLIP_V:
        case IDM_IMAGE_GRAYSCALE:
        case IDM_IMAGE_SEPIA:
        case IDM_IMAGE_INVERT:
        case IDM_IMAGE_BLUR:
        case IDM_IMAGE_SHARPEN:
        case IDM_IMAGE_BORDER:
            ApplyImageOp(id);
            break;
        case IDM_IMAGE_BRIGHTNESS:
            ShowBrightnessDlg(hwnd);
            break;
        case IDM_TOOL_PIXELATE:
            if (T && T->hasImage) { g.currentTool = Tool::Pixelate; g.isCropping = false; }
            break;
        case IDM_EDIT_CLEAR:
            if (T) { T->strokes.clear(); T->redoStack.clear(); T->selIdx = -1; }
            break;
        case IDM_EDIT_COPY: {
            TabData* t = ActiveTab();
            if (t && t->hasImage) {
                FlattenToBitmap();
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    HBITMAP hBmp = (HBITMAP)CopyImage(t->hBitmap, IMAGE_BITMAP, 0, 0, LR_DEFAULTSIZE);
                    SetClipboardData(CF_BITMAP, hBmp);
                    CloseClipboard();
                    SendMessage(g.hStatusbar, SB_SETTEXT, 0, (LPARAM)L"Copied to clipboard");
                }
            }
            break;
        }
        case IDM_EDIT_PASTE: {
            if (OpenClipboard(hwnd)) {
                HBITMAP hBmp = (HBITMAP)GetClipboardData(CF_BITMAP);
                if (hBmp) {
                    BITMAP bm;
                    GetObject(hBmp, sizeof(bm), &bm);
                    HBITMAP hCopy = (HBITMAP)CopyImage(hBmp, IMAGE_BITMAP, bm.bmWidth, bm.bmHeight, LR_DEFAULTSIZE);
                    SetImage(hCopy, bm.bmWidth, bm.bmHeight);
                    FitToWindow();
                    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                }
                CloseClipboard();
            }
            break;
        }
        case IDM_EDIT_DELETE:
            if (T && T->selIdx >= 0 && T->selIdx < (int)T->strokes.size()) {
                T->redoStack.push_back(T->strokes[T->selIdx]);
                T->strokes.erase(T->strokes.begin() + T->selIdx);
                T->selIdx = -1;
            }
            break;
        case IDM_EDIT_RESIZE:
            if (T && T->hasImage) ShowResizeDlg(hwnd);
            break;
        case IDM_EDIT_CROP:
            if (T && T->hasImage) {
                g.currentTool = Tool::Crop;
                g.isCropping = false;
                RebuildToolbar();
            }
            break;

        case IDM_CAPTURE_FULLSCREEN: {
            FlattenToBitmap();
            g_monitorRects.clear();
            EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
            if ((int)g_monitorRects.size() > 1) {
                POINT pt = { 0, 0 };
                ClientToScreen(hwnd, &pt);
                pt.x += 200;
                pt.y += g.toolbarH + 2;
                HMENU hMenu = CreatePopupMenu();
                for (int i = 0; i < (int)g_monitorRects.size() && i < 8; i++) {
                    const RECT& mr = g_monitorRects[i];
                    wchar_t label[64];
                    wsprintfW(label, L"Monitor %d  (%dx%d)", i + 1,
                        mr.right - mr.left, mr.bottom - mr.top);
                    AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_MON1 + i, label);
                }
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_CAPTURE_MON_ALL, L"All Monitors");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                    pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == IDM_CAPTURE_MON_ALL) {
                    HideAndCapture(hwnd);
                    HBITMAP hb = CaptureScreen();
                    int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    int ch = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                    ShowWindow(hwnd, SW_SHOW);
                    if (hb) {
                        SetImage(hb, cw, ch);
                        FitToWindow();
                        InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                    }
                } else if (cmd >= IDM_CAPTURE_MON1 && cmd <= IDM_CAPTURE_MON8) {
                    int idx = cmd - IDM_CAPTURE_MON1;
                    if (idx < (int)g_monitorRects.size()) {
                        const RECT& mr = g_monitorRects[idx];
                        int mx = mr.left, my = mr.top;
                        int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
                        HideAndCapture(hwnd);
                        HBITMAP hb = CaptureRegion(mx, my, mw, mh);
                        ShowWindow(hwnd, SW_SHOW);
                        if (hb) {
                            SetImage(hb, mw, mh);
                            FitToWindow();
                            InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                        }
                    }
                }
                g_monitorRects.clear();
            } else {
                HideAndCapture(hwnd);
                HBITMAP hb = CaptureScreen();
                int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int ch = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                ShowWindow(hwnd, SW_SHOW);
                if (hb) {
                    SetImage(hb, cw, ch);
                    FitToWindow();
                    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                }
            }
            break;
        }
        case IDM_CAPTURE_REGION: {
            FlattenToBitmap();
            HideAndCapture(hwnd);
            g_overlayVirtX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            g_overlayVirtY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            g_overlayScreenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            g_overlayScreenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_overlayScreenBmp = CaptureScreen();
            g_overlayDragging = false;
            g_hwndOverlay = CreateWindowEx(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                L"ScreenCapOverlay", nullptr,
                WS_POPUP | WS_VISIBLE,
                g_overlayVirtX, g_overlayVirtY, g_overlayScreenW, g_overlayScreenH,
                nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
            ShowWindow(g_hwndOverlay, SW_SHOW);
            Overlay_UpdateRegion();
            break;
        }
        case IDM_CAPTURE_SCROLLING:
            ScrollCaptureWindow(hwnd);
            break;


        case IDM_TOOL_SELECT: g.currentTool = Tool::Select; break;
        case IDM_TOOL_PEN:     g.currentTool = Tool::Pen; break;
        case IDM_TOOL_LINE:    g.currentTool = Tool::Line; break;
        case IDM_TOOL_RECT:    g.currentTool = Tool::Rectangle; break;
        case IDM_TOOL_ELLIPSE: g.currentTool = Tool::Ellipse; break;
        case IDM_TOOL_ARROW:   g.currentTool = Tool::Arrow; break;
        case IDM_TOOL_ERASER:  g.currentTool = Tool::Eraser; break;
        case IDM_TOOL_FILL:    g.currentTool = Tool::Fill; break;
        case IDM_TOOL_TEXT:    g.currentTool = Tool::Text; break;
        case IDM_TOOL_CALLOUT: g.currentTool = Tool::Callout; break;
        case IDM_TOOL_HIGHLIGHTER: g.currentTool = Tool::Highlighter; break;

        case IDM_COLOR_BLACK:   g.penColor = RGB(0, 0, 0); break;
        case IDM_COLOR_RED:     g.penColor = RGB(220, 20, 20); break;
        case IDM_COLOR_GREEN:   g.penColor = RGB(0, 160, 0); break;
        case IDM_COLOR_BLUE:    g.penColor = RGB(0, 80, 255); break;
        case IDM_COLOR_YELLOW:  g.penColor = RGB(240, 200, 0); break;
        case IDM_COLOR_CUSTOM:  ShowColorDlg(hwnd); break;
        case IDC_COLOR_BTN:     ShowColorDlg(hwnd); break;
        case IDC_PEN_WIDTH:     ShowPenWidthDlg(hwnd); break;

case IDM_VIEW_ZOOM_IN:
            if (T) ZoomAt(T->zoom * 1.25);
            break;
        case IDM_VIEW_ZOOM_OUT:
            if (T) ZoomAt(T->zoom / 1.25);
            break;
        case IDM_VIEW_ZOOM_100:
            if (T) ZoomAt(1.0);
            break;
        case IDM_VIEW_FIT:
            if (T && T->hasImage) { FitToWindow(); InvalidateRect(g.hwndCanvas, nullptr, FALSE); }
            break;

        case IDM_HELP_ABOUT: ShowAboutDlg(hwnd); break;

        case IDM_RECORD_TOGGLE:
            if (g.isRecording) StopRecording(hwnd);
            else StartRecording(hwnd);
            break;
        }

        if (id >= IDM_COLOR_BLACK && id <= IDM_COLOR_CUSTOM && T && T->selIdx >= 0 && T->selIdx < (int)T->strokes.size()) {
            T->strokes[T->selIdx].color = g.penColor;
        }

        RebuildToolbar();
        UpdateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        InvalidateRect(g.hwndCanvas, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
        if (ctrl && wp == 'Z') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_UNDO, 0); return 0; }
        if (ctrl && wp == 'Y') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_REDO, 0); return 0; }
        if (ctrl && wp == 'N') { SendMessage(hwnd, WM_COMMAND, IDM_FILE_NEW, 0); return 0; }
        if (ctrl && wp == 'O') { SendMessage(hwnd, WM_COMMAND, IDM_FILE_OPEN, 0); return 0; }
        if (ctrl && wp == 'S') { SendMessage(hwnd, WM_COMMAND, IDM_FILE_SAVE, 0); return 0; }
        if (ctrl && wp == 'W') { SendMessage(hwnd, WM_COMMAND, IDM_FILE_CLOSE_TAB, 0); return 0; }
        if (ctrl && wp == 'C') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_COPY, 0); return 0; }
        if (ctrl && wp == 'V') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_PASTE, 0); return 0; }
        if (ctrl && wp == 'R') { SendMessage(hwnd, WM_COMMAND, IDM_IMAGE_ROT_RIGHT, 0); return 0; }
        if (ctrl && wp == 'L') { SendMessage(hwnd, WM_COMMAND, IDM_IMAGE_ROT_LEFT, 0); return 0; }
        if (wp == VK_F11) { SendMessage(hwnd, WM_COMMAND, IDM_CAPTURE_FULLSCREEN, 0); return 0; }
        if (wp == VK_F12) { SendMessage(hwnd, WM_COMMAND, IDM_CAPTURE_REGION, 0); return 0; }
        if (wp == VK_F10) { SendMessage(hwnd, WM_COMMAND, IDM_CAPTURE_SCROLLING, 0); return 0; }
        if (ctrl && (wp == VK_OEM_PLUS || wp == '+')) { SendMessage(hwnd, WM_COMMAND, IDM_VIEW_ZOOM_IN, 0); return 0; }
        if (ctrl && (wp == VK_OEM_MINUS || wp == '-')) { SendMessage(hwnd, WM_COMMAND, IDM_VIEW_ZOOM_OUT, 0); return 0; }
        if (ctrl && wp == '0') { SendMessage(hwnd, WM_COMMAND, IDM_VIEW_ZOOM_100, 0); return 0; }
        if (wp == VK_DELETE) { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_DELETE, 0); return 0; }
        if (wp == VK_ESCAPE) { if (T) T->selIdx = -1; InvalidateRect(g.hwndCanvas, nullptr, FALSE); UpdateStatus(); return 0; }
        if (!ctrl && wp == 'V') { SendMessage(hwnd, WM_COMMAND, IDM_TOOL_SELECT, 0); return 0; }
        if (ctrl && wp == 'A') { g.currentTool = Tool::Select; SendMessage(hwnd, WM_COMMAND, IDM_TOOL_SELECT, 0); return 0; }
        break;
    }

    case WM_CONTEXTMENU: {
        HMENU hm = CreatePopupMenu();
        HMENU hTools = CreatePopupMenu();
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Select ? MF_CHECKED : 0), IDM_TOOL_SELECT, L"Select\tV");
        AppendMenu(hTools, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Pen ? MF_CHECKED : 0), IDM_TOOL_PEN, L"Pen");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Line ? MF_CHECKED : 0), IDM_TOOL_LINE, L"Line");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Rectangle ? MF_CHECKED : 0), IDM_TOOL_RECT, L"Rectangle");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Ellipse ? MF_CHECKED : 0), IDM_TOOL_ELLIPSE, L"Ellipse");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Arrow ? MF_CHECKED : 0), IDM_TOOL_ARROW, L"Arrow");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Eraser ? MF_CHECKED : 0), IDM_TOOL_ERASER, L"Eraser");
        AppendMenu(hTools, MF_STRING | (g.currentTool == Tool::Fill ? MF_CHECKED : 0), IDM_TOOL_FILL, L"Fill");
        AppendMenu(hm, MF_POPUP, (UINT_PTR)hTools, L"Tools");
        AppendMenu(hm, MF_SEPARATOR, 0, nullptr);

        if (T->selIdx >= 0 && T->selIdx < (int)T->strokes.size()) {
            HMENU hColor = CreatePopupMenu();
            AppendMenu(hColor, MF_STRING, IDM_COLOR_BLACK, L"Black");
            AppendMenu(hColor, MF_STRING, IDM_COLOR_RED, L"Red");
            AppendMenu(hColor, MF_STRING, IDM_COLOR_GREEN, L"Green");
            AppendMenu(hColor, MF_STRING, IDM_COLOR_BLUE, L"Blue");
            AppendMenu(hColor, MF_STRING, IDM_COLOR_YELLOW, L"Yellow");
            AppendMenu(hColor, MF_SEPARATOR, 0, nullptr);
            AppendMenu(hColor, MF_STRING, IDM_COLOR_CUSTOM, L"Custom...");
            AppendMenu(hm, MF_POPUP, (UINT_PTR)hColor, L"Change Selected Color");
            AppendMenu(hm, MF_STRING, IDC_PEN_WIDTH, L"Change Selected Width...");
            AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
            AppendMenu(hm, MF_STRING, IDM_EDIT_DELETE, L"Delete Selected\tDel");
            AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
        }

        HMENU hColors = CreatePopupMenu();
        AppendMenu(hColors, MF_STRING | (g.penColor == RGB(0, 0, 0) ? MF_CHECKED : 0), IDM_COLOR_BLACK, L"Black");
        AppendMenu(hColors, MF_STRING | (g.penColor == RGB(220, 20, 20) ? MF_CHECKED : 0), IDM_COLOR_RED, L"Red");
        AppendMenu(hColors, MF_STRING | (g.penColor == RGB(0, 160, 0) ? MF_CHECKED : 0), IDM_COLOR_GREEN, L"Green");
        AppendMenu(hColors, MF_STRING | (g.penColor == RGB(0, 80, 255) ? MF_CHECKED : 0), IDM_COLOR_BLUE, L"Blue");
        AppendMenu(hColors, MF_STRING | (g.penColor == RGB(240, 200, 0) ? MF_CHECKED : 0), IDM_COLOR_YELLOW, L"Yellow");
        AppendMenu(hColors, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hColors, MF_STRING, IDM_COLOR_CUSTOM, L"Custom...");
        AppendMenu(hm, MF_POPUP, (UINT_PTR)hColors, L"Pen Color");
        AppendMenu(hm, MF_STRING, IDC_PEN_WIDTH, L"Pen Width...");
        AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hm, MF_STRING, IDM_EDIT_UNDO, L"Undo\tCtrl+Z");
        AppendMenu(hm, MF_STRING, IDM_EDIT_REDO, L"Redo\tCtrl+Y");
        AppendMenu(hm, MF_STRING, IDM_EDIT_CLEAR, L"Clear All Strokes");
        AppendMenu(hm, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hm, MF_STRING, IDM_VIEW_FIT, L"Fit to Window");

        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        TrackPopupMenu(hm, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(hm);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            int idx = TabCtrl_GetCurSel(g.hwndTab);
            SwitchToTab(idx);
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == 3001) { RecordFrame(); return 0; }
        if (wp == TIP_TIMER_ID) {
            KillTimer(hwnd, TIP_TIMER_ID);
            if (g.hoverBtn >= 0) ShowToolbarTip(g.hoverBtn);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g.isRecording) {
            if (g.recTimer) KillTimer(hwnd, g.recTimer);
            if (g.aviStream) AVIStreamRelease(g.aviStream);
            if (g.aviFile) AVIFileRelease(g.aviFile);
            AVIFileExit();
            g.isRecording = false;
        }
        for (auto& tab : g.tabs)
            { if (tab.hBitmap) DeleteObject(tab.hBitmap); FreeSnaps(tab.imgUndo); FreeSnaps(tab.imgRedo); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    PFN_SetProcessDpiAwarenessContext pSetDpiCtx =
        (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (pSetDpiCtx) {
        pSetDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }

    GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    RegisterClassEx(&wc);

    RECT desk;
    GetWindowRect(GetDesktopWindow(), &desk);
    int ww = min(1280, desk.right - desk.left - 100);
    int wh = min(860, desk.bottom - desk.top - 80);
    int wx = (desk.right - ww) / 2;
    int wy = (desk.bottom - wh) / 2;

    HWND hwnd = CreateWindowEx(
        WS_EX_ACCEPTFILES,
        APP_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        GdiplusShutdown(gdiToken);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        // Windows delivers F10 as a system key (it would open the window menu),
        // so the F10 shortcut never reached WM_KEYDOWN.
        if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_F10 &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0 &&
            (msg.hwnd == g.hwndMain || IsChild(g.hwndMain, msg.hwnd))) {
            PostMessage(g.hwndMain, WM_COMMAND, IDM_CAPTURE_SCROLLING, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiToken);
    return (int)msg.wParam;
}
