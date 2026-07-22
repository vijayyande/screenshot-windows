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
#include <gdiplus.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t* APP_CLASS = L"ScreenshotEditorClass";
static const wchar_t* APP_TITLE = L"Screenshot Editor";

enum class Tool { Select, Pen, Line, Rectangle, Ellipse, Arrow, Eraser, Fill, Crop, Text, Callout, Highlighter, None };
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

struct TabData {
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

    int toolbarH = 44;
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

static void GetBBox(const Stroke& s, POINT& tl, POINT& br) {
    if (s.tool == Tool::Pen || s.tool == Tool::Eraser || s.tool == Tool::Highlighter) {
        if (s.points.empty()) { tl = br = { 0,0 }; return; }
        tl = br = s.points[0];
        for (size_t i = 1; i < s.points.size(); i++) {
            tl.x = min(tl.x, s.points[i].x); tl.y = min(tl.y, s.points[i].y);
            br.x = max(br.x, s.points[i].x); br.y = max(br.y, s.points[i].y);
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
            HDC hdc = CreateCompatibleDC(nullptr);
            HFONT hf = CreateFont(-s.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
            HFONT old = (HFONT)SelectObject(hdc, hf);
            SIZE sz;
            GetTextExtentPoint32(hdc, s.text.c_str(), (int)s.text.size(), &sz);
            SelectObject(hdc, old);
            DeleteObject(hf);
            DeleteDC(hdc);
            RECT r = { s.startPt.x - 2, s.startPt.y - sz.cy - 2, s.startPt.x + sz.cx + 2, s.startPt.y + 2 };
            if (imgPt.x >= r.left && imgPt.x <= r.right && imgPt.y >= r.top && imgPt.y <= r.bottom)
                return i;
            break;
        }
        case Tool::Callout: {
            double dSq = PtSegDistSq(imgPt, s.startPt, s.endPt);
            double penTol = max(tol, s.penWidth / 2.0 + tol);
            if (dSq <= penTol * penTol) return i;
            HDC hdc = CreateCompatibleDC(nullptr);
            HFONT hf = CreateFont(-s.fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
            HFONT old = (HFONT)SelectObject(hdc, hf);
            SIZE tsz;
            GetTextExtentPoint32(hdc, s.text.c_str(), (int)s.text.size(), &tsz);
            SelectObject(hdc, old);
            DeleteObject(hf);
            DeleteDC(hdc);
            int pad = 6;
            int bw = tsz.cx + pad * 2;
            int bh = tsz.cy + pad * 2;
            RECT br2 = { s.endPt.x - bw / 2 - 2, s.endPt.y - bh / 2 - 2,
                         s.endPt.x + bw / 2 + 2, s.endPt.y + bh / 2 + 2 };
            if (imgPt.x >= br2.left && imgPt.x <= br2.right && imgPt.y >= br2.top && imgPt.y <= br2.bottom)
                return i;
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
    POINT tl, br;
    GetBBox(s, tl, br);

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
    HBITMAP hBmp = CreateCompatibleBitmap(hScr, vw, vh);
    SelectObject(hdc, hBmp);
    BitBlt(hdc, 0, 0, vw, vh, hScr, vx, vy, SRCCOPY);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hScr);
    return hBmp;
}

static void HideAndCapture(HWND hwnd) {
    ShowWindow(hwnd, SW_MINIMIZE);
    RedrawWindow(nullptr, nullptr, nullptr,
        RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_ERASE);
    DwmFlush();
    Sleep(50);
}

static HBITMAP CaptureRegion(int x, int y, int w, int h) {
    HDC hScr = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hScr);
    HBITMAP hBmp = CreateCompatibleBitmap(hScr, w, h);
    SelectObject(hdc, hBmp);
    BitBlt(hdc, 0, 0, w, h, hScr, x, y, SRCCOPY);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hScr);
    return hBmp;
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

    FlattenToBitmap();
    HDC hdcScr = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(hdcScr);
    HDC hdcDst = CreateCompatibleDC(hdcScr);
    HBITMAP hNew = CreateCompatibleBitmap(hdcScr, w, h);
    SelectObject(hdcSrc, t->hBitmap);
    SelectObject(hdcDst, hNew);
    BitBlt(hdcDst, 0, 0, w, h, hdcSrc, x, y, SRCCOPY);
    DeleteDC(hdcSrc);
    DeleteDC(hdcDst);
    ReleaseDC(nullptr, hdcScr);
    DeleteObject(t->hBitmap);
    t->hBitmap = hNew;
    t->imgW = w;
    t->imgH = h;
    t->selIdx = -1;
    FitToWindow();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    UpdateStatus();
}

static void ShowAboutDlg(HWND parent) {
    DialogBox(nullptr, MAKEINTRESOURCE(IDD_ABOUT), parent,
        [](HWND hDlg, UINT msg, WPARAM, LPARAM) -> INT_PTR {
            if (msg == WM_COMMAND) { EndDialog(hDlg, 0); return TRUE; }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            return FALSE;
        });
}

static void ShowFontDlg() {
    TabData* t = ActiveTab();
    if (!t || t->selIdx < 0 || t->selIdx >= (int)t->strokes.size() ||
        (t->strokes[t->selIdx].tool != Tool::Text && t->strokes[t->selIdx].tool != Tool::Callout)) return;
    DialogBoxParam(nullptr, MAKEINTRESOURCE(IDD_FONT), g.hwndMain,
        [](HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            if (msg == WM_INITDIALOG) {
                TabData* t = (TabData*)lp;
                SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)lp);
                SetDlgItemText(hDlg, 300, t->strokes[t->selIdx].text.c_str());
                HWND hCombo = GetDlgItem(hDlg, 301);
                HDC hdc = GetDC(nullptr);
                EnumFontFamilies(hdc, nullptr, [](const LOGFONT* lplf, const TEXTMETRIC*, DWORD, LPARAM lParam) -> int {
                    SendMessage((HWND)lParam, CB_ADDSTRING, 0, (LPARAM)lplf->lfFaceName);
                    return 1;
                }, (LPARAM)hCombo);
                ReleaseDC(nullptr, hdc);
                int idx = (int)SendMessage(hCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)t->strokes[t->selIdx].fontName.c_str());
                if (idx == CB_ERR) idx = 0;
                SendMessage(hCombo, CB_SETCURSEL, idx, 0);
                wchar_t buf[16];
                swprintf_s(buf, L"%d", t->strokes[t->selIdx].fontSize);
                SetDlgItemText(hDlg, 302, buf);
                return TRUE;
            }
            TabData* t = (TabData*)GetWindowLongPtr(hDlg, DWLP_USER);
            if (!t) return FALSE;
            if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
                wchar_t txt[256];
                GetDlgItemText(hDlg, 300, txt, 256);
                HWND hCombo = GetDlgItem(hDlg, 301);
                int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                wchar_t fn[LF_FACESIZE];
                SendMessage(hCombo, CB_GETLBTEXT, idx, (LPARAM)fn);
                wchar_t bf[16];
                GetDlgItemText(hDlg, 302, bf, 16);
                int sz = max(6, min(200, _wtoi(bf)));
                auto& s = t->strokes[t->selIdx];
                s.text = txt;
                s.fontName = fn;
                s.fontSize = sz;
                g.fontName = fn;
                g.fontSize = sz;
                EndDialog(hDlg, IDOK);
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                return TRUE;
            }
            if (msg == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 2)) {
                EndDialog(hDlg, 0); return TRUE;
            }
            if (msg == WM_CLOSE) { EndDialog(hDlg, 0); return TRUE; }
            return FALSE;
        }, (LPARAM)t);
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
    const wchar_t* names[] = { L"Select", L"Pen", L"Line", L"Rectangle", L"Ellipse", L"Arrow", L"Eraser", L"Fill", L"Crop", L"Text", L"Callout", L"Highlighter", L"None" };
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

static void UpdateTabControl() {
    if (!g.hwndTab) return;
    TCITEM tie = {};
    tie.mask = TCIF_TEXT;
    while (TabCtrl_DeleteItem(g.hwndTab, 0));
    for (size_t i = 0; i < g.tabs.size(); i++) {
        wchar_t label[32];
        swprintf_s(label, L"Screenshot %d", g.tabs[i].id);
        tie.pszText = label;
        TabCtrl_InsertItem(g.hwndTab, (int)i, &tie);
    }
    TabCtrl_SetCurSel(g.hwndTab, g.activeTab);
}

static void SwitchToTab(int idx) {
    if (idx < 0 || idx >= (int)g.tabs.size() || idx == g.activeTab) return;
    g.activeTab = idx;
    TabCtrl_SetCurSel(g.hwndTab, idx);
    TabData* t = ActiveTab();
    if (t && t->hasImage) FitToWindow();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
    InvalidateRect(g.hwndMain, nullptr, FALSE);
    RebuildToolbar();
    UpdateStatus();
}

static void CloseTab(int idx) {
    if (idx < 0 || idx >= (int)g.tabs.size()) return;
    if (g.tabs[idx].hBitmap) DeleteObject(g.tabs[idx].hBitmap);
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
    RebuildToolbar();
    UpdateStatus();
    InvalidateRect(g.hwndCanvas, nullptr, FALSE);
}

static void DrawIcon_New(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(46, 204, 113);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 6, cy - 7, cx + 2, cy + 7);
    MoveToEx(hdc, cx + 2, cy - 7, nullptr);
    LineTo(hdc, cx + 2, cy - 3);
    LineTo(hdc, cx + 6, cy - 7);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Open(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(52, 152, 219);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(RGB(52, 152, 219));
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 7, cy - 2, cx + 7, cy + 7);
    SelectObject(hdc, ob); DeleteObject(br);
    br = (HBRUSH)GetStockObject(NULL_BRUSH);
    ob = SelectObject(hdc, br);
    MoveToEx(hdc, cx - 7, cy - 2, nullptr);
    LineTo(hdc, cx - 5, cy - 6);
    LineTo(hdc, cx + 1, cy - 6);
    LineTo(hdc, cx + 3, cy - 2);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Save(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(52, 152, 219);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
    Rectangle(hdc, cx - 4, cy - 2, cx + 4, cy + 5);
    MoveToEx(hdc, cx - 4, cy - 4, nullptr);
    LineTo(hdc, cx - 1, cy - 4);
    LineTo(hdc, cx - 1, cy - 1);
    LineTo(hdc, cx - 4, cy - 1);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Undo(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(230, 126, 34);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Arc(hdc, cx - 6, cy - 6, cx + 4, cy + 4, cx + 3, cy - 5, cx - 6, cy);
    MoveToEx(hdc, cx - 2, cy - 7, nullptr);
    LineTo(hdc, cx - 6, cy - 3);
    LineTo(hdc, cx - 2, cy);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Redo(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(230, 126, 34);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Arc(hdc, cx - 4, cy - 6, cx + 6, cy + 4, cx - 3, cy - 5, cx + 6, cy);
    MoveToEx(hdc, cx + 2, cy - 7, nullptr);
    LineTo(hdc, cx + 6, cy - 3);
    LineTo(hdc, cx + 2, cy);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Copy(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(26, 188, 156);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 5, cy - 6, cx + 5, cy + 1);
    Rectangle(hdc, cx - 3, cy - 3, cx + 7, cy + 7);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Paste(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(26, 188, 156);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(fc);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 5, cy - 1, cx + 5, cy + 7);
    SelectObject(hdc, ob); DeleteObject(br);
    br = (HBRUSH)GetStockObject(NULL_BRUSH);
    ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 4, cy - 6, cx + 4, cy + 1);
    SelectObject(hdc, ob);
    MoveToEx(hdc, cx, cy - 8, nullptr);
    LineTo(hdc, cx, cy - 3);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Fullscreen(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(155, 89, 182);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
    MoveToEx(hdc, cx - 3, cy - 6, nullptr);
    LineTo(hdc, cx - 3, cy - 3);
    LineTo(hdc, cx - 6, cy - 3);
    MoveToEx(hdc, cx + 3, cy + 6, nullptr);
    LineTo(hdc, cx + 3, cy + 3);
    LineTo(hdc, cx + 6, cy + 3);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Region(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(155, 89, 182);
    HPEN pen = CreatePen(PS_DOT, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Select(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(236, 240, 241);
    POINT pts[] = { { cx - 3, cy - 6 }, { cx - 3, cy + 5 }, { cx + 0, cy + 2 },
                    { cx + 2, cy + 5 }, { cx + 4, cy + 3 }, { cx + 1, cy + 1 },
                    { cx + 4, cy - 2 } };
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(fc);
    HGDIOBJ ob = SelectObject(hdc, br);
    Polygon(hdc, pts, 7);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen); DeleteObject(br);
}
static void DrawIcon_Pen(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(231, 76, 60);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy + 5, nullptr);
    LineTo(hdc, cx + 4, cy - 6);
    MoveToEx(hdc, cx - 5, cy + 5, nullptr);
    LineTo(hdc, cx - 1, cy + 5);
    MoveToEx(hdc, cx - 5, cy + 5, nullptr);
    LineTo(hdc, cx - 5, cy + 1);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Line(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(52, 152, 219);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy + 5, nullptr);
    LineTo(hdc, cx + 5, cy - 5);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Rect(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(243, 156, 18);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 6, cy - 4, cx + 6, cy + 4);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Ellipse(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(155, 89, 182);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Ellipse(hdc, cx - 6, cy - 4, cx + 6, cy + 4);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Text(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(26, 188, 156);
    HFONT hf = CreateFont(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, hf);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fc);
    TextOut(hdc, cx - 5, cy - 7, L"Ab", 2);
    SelectObject(hdc, old);
    DeleteObject(hf);
}
static void DrawIcon_Callout(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(243, 156, 18);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(RGB(243, 156, 18));
    HBRUSH obr = (HBRUSH)SelectObject(hdc, br);
    RoundRect(hdc, cx - 7, cy - 6, cx + 6, cy + 2, 4, 4);
    SelectObject(hdc, obr);
    DeleteObject(br);
    MoveToEx(hdc, cx - 1, cy + 2, nullptr);
    LineTo(hdc, cx - 5, cy + 7);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Highlighter(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(241, 196, 15);
    HPEN pen = CreatePen(PS_SOLID, 5, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 6, cy + 3, nullptr);
    LineTo(hdc, cx + 6, cy - 5);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Arrow(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(231, 76, 60);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy + 4, nullptr);
    LineTo(hdc, cx + 3, cy - 5);
    MoveToEx(hdc, cx + 3, cy - 5, nullptr);
    LineTo(hdc, cx - 1, cy - 3);
    MoveToEx(hdc, cx + 3, cy - 5, nullptr);
    LineTo(hdc, cx + 1, cy - 1);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Eraser(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(241, 196, 15);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(RGB(241, 196, 15));
    HGDIOBJ ob = SelectObject(hdc, br);
    POINT pts[] = { { cx - 5, cy - 3 }, { cx + 1, cy - 3 }, { cx + 5, cy + 1 }, { cx - 1, cy + 5 }, { cx - 5, cy + 3 } };
    Polygon(hdc, pts, 5);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen); DeleteObject(br);
}
static void DrawIcon_Fill(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(52, 152, 219);
    HPEN pen = CreatePen(PS_SOLID, 1, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(RGB(52, 152, 219));
    HGDIOBJ ob = SelectObject(hdc, br);
    MoveToEx(hdc, cx - 4, cy - 4, nullptr);
    LineTo(hdc, cx - 1, cy - 1);
    LineTo(hdc, cx + 1, cy - 1);
    LineTo(hdc, cx + 4, cy + 2);
    LineTo(hdc, cx + 2, cy + 5);
    LineTo(hdc, cx - 1, cy + 2);
    LineTo(hdc, cx + 1, cy - 1);
    SelectObject(hdc, ob); DeleteObject(br);
    SelectObject(hdc, op);
    DeleteObject(pen);
}
static void DrawIcon_Crop(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(26, 188, 156);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 5, cy - 5, cx + 5, cy + 5);
    MoveToEx(hdc, cx - 8, cy - 2, nullptr); LineTo(hdc, cx + 2, cy - 2);
    MoveToEx(hdc, cx - 2, cy - 8, nullptr); LineTo(hdc, cx - 2, cy + 2);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Resize(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(149, 165, 166);
    HPEN pen = CreatePen(PS_SOLID, 2, fc);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH br = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ ob = SelectObject(hdc, br);
    Rectangle(hdc, cx - 5, cy - 5, cx + 5, cy + 5);
    MoveToEx(hdc, cx - 8, cy, nullptr); LineTo(hdc, cx + 8, cy);
    MoveToEx(hdc, cx, cy - 8, nullptr); LineTo(hdc, cx, cy + 8);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);
}
static void DrawIcon_Color(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    HBRUSH br = CreateSolidBrush(g.penColor);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, br);
    RoundRect(hdc, cx - 5, cy - 5, cx + 5, cy + 5, 3, 3);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen); DeleteObject(br);
}
static void DrawIcon_Width(HDC hdc, int cx, int cy, COLORREF c) {
    (void)c;
    COLORREF fc = RGB(149, 165, 166);
    HPEN pen;
    HPEN op;
    pen = CreatePen(PS_SOLID, 1, fc); op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy - 3, nullptr); LineTo(hdc, cx + 5, cy - 3);
    SelectObject(hdc, op); DeleteObject(pen);
    pen = CreatePen(PS_SOLID, 2, fc); op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy, nullptr); LineTo(hdc, cx + 5, cy);
    SelectObject(hdc, op); DeleteObject(pen);
    pen = CreatePen(PS_SOLID, 3, fc); op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, cx - 5, cy + 3, nullptr); LineTo(hdc, cx + 5, cy + 3);
    SelectObject(hdc, op); DeleteObject(pen);
}

typedef void (*IconDrawFn)(HDC, int, int, COLORREF);
struct IconBtn { int id; IconDrawFn fn; };
static const IconBtn ICON_BUTTONS[] = {
    { IDM_FILE_NEW,           DrawIcon_New },
    { IDM_FILE_OPEN,          DrawIcon_Open },
    { IDM_FILE_SAVE,          DrawIcon_Save },
    { 0, nullptr },
    { IDM_EDIT_UNDO,          DrawIcon_Undo },
    { IDM_EDIT_REDO,          DrawIcon_Redo },
    { 0, nullptr },
    { IDM_EDIT_COPY,          DrawIcon_Copy },
    { IDM_EDIT_PASTE,         DrawIcon_Paste },
    { 0, nullptr },
    { IDM_CAPTURE_FULLSCREEN, DrawIcon_Fullscreen },
    { IDM_CAPTURE_REGION,     DrawIcon_Region },
    { 0, nullptr },
    { IDM_TOOL_SELECT,        DrawIcon_Select },
    { IDM_TOOL_PEN,           DrawIcon_Pen },
    { IDM_TOOL_LINE,          DrawIcon_Line },
    { IDM_TOOL_RECT,          DrawIcon_Rect },
    { IDM_TOOL_ELLIPSE,       DrawIcon_Ellipse },
    { IDM_TOOL_ARROW,         DrawIcon_Arrow },
    { IDM_TOOL_ERASER,        DrawIcon_Eraser },
    { IDM_TOOL_FILL,          DrawIcon_Fill },
    { IDM_TOOL_TEXT,          DrawIcon_Text },
    { IDM_TOOL_CALLOUT,      DrawIcon_Callout },
    { IDM_TOOL_HIGHLIGHTER,  DrawIcon_Highlighter },
    { 0, nullptr },
    { IDC_COLOR_BTN,          DrawIcon_Color },
    { IDC_PEN_WIDTH,          DrawIcon_Width },
    { IDM_EDIT_CROP,          DrawIcon_Crop },
    { IDM_EDIT_RESIZE,        DrawIcon_Resize },
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
    default: return L"";
    }
}

static void RebuildToolbar() {
    g.tbBtns.clear();
    TabData* t = ActiveTab();
    int x = 4;
    int bh = 32;
    int by = (g.toolbarH - bh) / 2;
    int btnW = 30;

    for (int i = 0; i < _countof(ICON_BUTTONS); i++) {
        if (ICON_BUTTONS[i].fn == nullptr) {
            g.tbBtns.push_back({ 0, x, by, 8, bh, true, true });
            x += 10;
        } else {
            bool enabled = true;
            if (ICON_BUTTONS[i].id == IDM_EDIT_UNDO) enabled = t && !t->strokes.empty();
            if (ICON_BUTTONS[i].id == IDM_EDIT_REDO) enabled = t && !t->redoStack.empty();
            g.tbBtns.push_back({ ICON_BUTTONS[i].id, x, by, btnW, bh, false, enabled });
            x += btnW + 1;
        }
    }

    if (g.hwndTooltip) {
        SendMessage(g.hwndTooltip, TTM_DELTOOL, 0, (LPARAM)"Toolbar");
        for (auto& b : g.tbBtns) {
            if (b.isSep) continue;
            TOOLINFO ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = g.hwndMain;
            ti.lpszText = (LPWSTR)GetTooltipForId(b.id);
            ti.rect = { b.x, b.y, b.x + b.w, b.y + b.h };
            SendMessage(g.hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
        }
    }
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
        else if (b.id >= IDM_TOOL_PEN && b.id <= IDM_TOOL_FILL) {
            Tool tmap[] = { Tool::Pen, Tool::Line, Tool::Rectangle, Tool::Ellipse, Tool::Arrow, Tool::Eraser, Tool::Fill };
            int tidx = b.id - IDM_TOOL_PEN;
            if (tidx >= 0 && tidx < 7) selected = (g.currentTool == tmap[tidx]);
        }

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
            if (ICON_BUTTONS[fi].id == b.id && ICON_BUTTONS[fi].fn) {
                ICON_BUTTONS[fi].fn(hdc, b.x + b.w / 2, b.y + b.h / 2, iconColor);
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
                    double zz = T->zoom;
                    int fs = max(8, (int)(s.fontSize * zz));
                    HFONT hf = CreateFont(-fs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, s.fontName.c_str());
                    HFONT hof2 = (HFONT)SelectObject(hdcMem, hf);
                    SetBkMode(hdcMem, TRANSPARENT);
                    SetTextColor(hdcMem, s.color);
                    SIZE tsz = { 0, 0 };
                    if (!s.text.empty())
                        GetTextExtentPoint32(hdcMem, s.text.c_str(), (int)s.text.size(), &tsz);
                    int pad = (int)(6 * zz);
                    int bw = tsz.cx + pad * 2;
                    int bh = tsz.cy + pad * 2;
                    int bx = (int)(s.endPt.x * zz + T->offsetX) - bw / 2;
                    int by = (int)(s.endPt.y * zz + T->offsetY) - bh / 2;
                    HBRUSH bgBr = CreateSolidBrush(RGB(255, 255, 255));
                    HPEN borderPen = CreatePen(PS_SOLID, max(1, (int)(s.penWidth * zz)), s.color);
                    HBRUSH ob2 = (HBRUSH)SelectObject(hdcMem, bgBr);
                    HPEN op2 = (HPEN)SelectObject(hdcMem, borderPen);
                    RoundRect(hdcMem, bx, by, bx + bw, by + bh, (int)(8 * zz), (int)(8 * zz));
                    SelectObject(hdcMem, ob2);
                    SelectObject(hdcMem, op2);
                    DeleteObject(bgBr);
                    DeleteObject(borderPen);
                    TextOut(hdcMem, bx + pad, by + pad, s.text.c_str(), (int)s.text.size());
                    double acx = s.endPt.x * zz + T->offsetX;
                    double acy = s.endPt.y * zz + T->offsetY;
                    double atx = s.startPt.x * zz + T->offsetX;
                    double aty = s.startPt.y * zz + T->offsetY;
                    double ang = atan2(aty - acy, atx - acx);
                    double bx1 = bx, by1 = by, bx2 = bx + bw, by2 = by + bh;
                    double ix = acx, iy = acy;
                    double adx = cos(ang), ady = sin(ang);
                    if (adx > 0) ix = bx2; else if (adx < 0) ix = bx1;
                    if (ady > 0) iy = by2; else if (ady < 0) iy = by1;
                    if (adx != 0) { double t = (ix - acx) / adx; iy = acy + t * ady; }
                    if (iy < by1 || iy > by2) {
                        if (ady > 0) iy = by2; else iy = by1;
                        double t = (iy - acy) / ady;
                        ix = acx + t * adx;
                    }
                    HPEN arrPen = CreatePen(PS_SOLID, max(1, (int)(s.penWidth * zz)), s.color);
                    HPEN op3 = (HPEN)SelectObject(hdcMem, arrPen);
                    MoveToEx(hdcMem, (int)ix, (int)iy, nullptr);
                    LineTo(hdcMem, (int)atx, (int)aty);
                    double hl = max(8.0, 12.0 * zz);
                    MoveToEx(hdcMem, (int)atx, (int)aty, nullptr);
                    LineTo(hdcMem, (int)(atx - hl * cos(ang - 0.4)), (int)(aty - hl * sin(ang - 0.4)));
                    MoveToEx(hdcMem, (int)atx, (int)aty, nullptr);
                    LineTo(hdcMem, (int)(atx - hl * cos(ang + 0.4)), (int)(aty - hl * sin(ang + 0.4)));
                    SelectObject(hdcMem, op3);
                    DeleteObject(arrPen);
                    SelectObject(hdcMem, hof2);
                    DeleteObject(hf);
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
                    int bw = 80, bh = 30;
                    int cbx = ex - bw / 2, cby = ey - bh / 2;
                    HBRUSH bgBr = CreateSolidBrush(RGB(255, 255, 255));
                    HBRUSH ob2 = (HBRUSH)SelectObject(hdcMem, bgBr);
                    RoundRect(hdcMem, cbx, cby, cbx + bw, cby + bh, 8, 8);
                    SelectObject(hdcMem, ob2);
                    DeleteObject(bgBr);
                    MoveToEx(hdcMem, ex, ey, nullptr);
                    LineTo(hdcMem, sx, sy);
                    double a = atan2((double)(ey - sy), (double)(ex - sx));
                    double hl = max(8.0, 12.0 * zx);
                    MoveToEx(hdcMem, sx, sy, nullptr);
                    LineTo(hdcMem, (int)(sx - hl * cos(a - 0.4)), (int)(sy - hl * sin(a - 0.4)));
                    MoveToEx(hdcMem, sx, sy, nullptr);
                    LineTo(hdcMem, (int)(sx - hl * cos(a + 0.4)), (int)(sy - hl * sin(a + 0.4)));
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
            int hh = HitTestHandle(pt);
            if (hh >= 0 && T->selIdx >= 0) {
                g.isResizingSel = true;
                g.resizeHandle = hh;
                g.dragStartImg = ip;
                T->selBackup = T->strokes[T->selIdx];
                SetCapture(hwnd);
                return 0;
            }
            int hit = HitTestShape(ip);
            if (hit >= 0) {
                T->selIdx = hit;
                g.isDraggingSel = true;
                g.dragStartImg = ip;
                T->selBackup = T->strokes[hit];
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
            T->selIdx = (int)T->strokes.size() - 1;
            ShowFontDlg();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g.currentTool == Tool::Crop) {
            g.isCropping = true;
            g.cropStart = ip;
            g.cropEnd = ip;
            SetCapture(hwnd);
            return 0;
        }

        g.isDrawing = true;
        g.drawStart = ip;
        g.drawEnd = ip;

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
        if (g.currentTool == Tool::Select && T->selIdx >= 0 &&
            (T->strokes[T->selIdx].tool == Tool::Text || T->strokes[T->selIdx].tool == Tool::Callout)) {
            ShowFontDlg();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g.isPanning) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            T->offsetX += pt.x - g.panLast.x;
            T->offsetY += pt.y - g.panLast.y;
            g.panLast = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
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

        if (g.isPanning) {
            g.isPanning = false;
            ReleaseCapture();
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
                }
                T->strokes.push_back(s);
                T->redoStack.clear();
                if (g.currentTool == Tool::Callout) {
                    T->selIdx = (int)T->strokes.size() - 1;
                    ShowFontDlg();
                }
            } else {
                T->redoStack.clear();
            }
            UpdateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

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
        if (!T) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        double oldZ = T->zoom;
        if (delta > 0) T->zoom *= 1.1;
        else T->zoom /= 1.1;
        T->zoom = max(0.02, min(T->zoom, 30.0));
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        double mx = (pt.x - T->offsetX) / oldZ;
        double my = (pt.y - T->offsetY) / oldZ;
        T->offsetX = pt.x - mx * T->zoom;
        T->offsetY = pt.y - my * T->zoom;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateStatus();
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

static void Overlay_Finish(HWND hwndMain, bool ok) {
    if (g_overlayScreenBmp) { DeleteObject(g_overlayScreenBmp); g_overlayScreenBmp = nullptr; }
    if (g_hwndOverlay) { DestroyWindow(g_hwndOverlay); g_hwndOverlay = nullptr; }
    g_overlayDragging = false;
    if (ok) {
        int x = min(g_overlayStart.x, g_overlayCur.x) + g_overlayVirtX;
        int y = min(g_overlayStart.y, g_overlayCur.y) + g_overlayVirtY;
        int w = abs(g_overlayCur.x - g_overlayStart.x);
        int h = abs(g_overlayCur.y - g_overlayStart.y);
        ShowWindow(hwndMain, SW_RESTORE);
        if (w > 2 && h > 2) {
            HBITMAP hb = CaptureRegion(x, y, w, h);
            if (hb) {
                FlattenToBitmap();
                SetImage(hb, w, h);
                FitToWindow();
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
                UpdateStatus();
            }
        }
    } else {
        ShowWindow(hwndMain, SW_RESTORE);
    }
    InvalidateRect(hwndMain, nullptr, FALSE);
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
        if (g_overlayDragging) {
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            return TRUE;
        }
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
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

        g.hwndCanvas = CreateWindowEx(0, L"Static", nullptr,
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CANVAS, nullptr, nullptr);
        SetWindowLongPtr(g.hwndCanvas, GWLP_WNDPROC, (LONG_PTR)CanvasProc);

        g.hwndMain = hwnd;

        g.hwndTooltip = CreateWindowEx(0, TOOLTIPS_CLASS, nullptr,
            WS_POPUP | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessage(g.hwndTooltip, TTM_SETMAXTIPWIDTH, 0, 300);

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
        if (t && t->hasImage) FitToWindow();
        InvalidateRect(hwnd, nullptr, FALSE);
        InvalidateRect(g.hwndCanvas, nullptr, FALSE);
        UpdateStatus();
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
        if (newH != g.hoverBtn) {
            g.hoverBtn = newH;
            RECT tr = { 0, 0, 0, g.toolbarH };
            GetClientRect(hwnd, &tr);
            tr.bottom = g.toolbarH;
            InvalidateRect(hwnd, &tr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
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
        case IDM_FILE_EXIT:     PostQuitMessage(0); break;

        case IDM_EDIT_UNDO:
            if (T && !T->strokes.empty()) {
                T->redoStack.push_back(T->strokes.back());
                T->strokes.pop_back();
                T->selIdx = -1;
            }
            break;
        case IDM_EDIT_REDO:
            if (T && !T->redoStack.empty()) {
                T->strokes.push_back(T->redoStack.back());
                T->redoStack.pop_back();
                T->selIdx = -1;
            }
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
            HideAndCapture(hwnd);
            HBITMAP hb = CaptureScreen();
            int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int ch = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            ShowWindow(hwnd, SW_RESTORE);
            if (hb) {
                SetImage(hb, cw, ch);
                FitToWindow();
                InvalidateRect(g.hwndCanvas, nullptr, FALSE);
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
            if (T) T->zoom = min(T->zoom * 1.25, 30.0);
            InvalidateRect(g.hwndCanvas, nullptr, FALSE);
            break;
        case IDM_VIEW_ZOOM_OUT:
            if (T) T->zoom = max(T->zoom / 1.25, 0.02);
            InvalidateRect(g.hwndCanvas, nullptr, FALSE);
            break;
        case IDM_VIEW_ZOOM_100:
            if (T) { T->zoom = 1.0; T->offsetX = 0; T->offsetY = 0; }
            InvalidateRect(g.hwndCanvas, nullptr, FALSE);
            break;
        case IDM_VIEW_FIT:
            if (T && T->hasImage) { FitToWindow(); InvalidateRect(g.hwndCanvas, nullptr, FALSE); }
            break;

        case IDM_HELP_ABOUT: ShowAboutDlg(hwnd); break;
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
        if (ctrl && wp == 'C') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_COPY, 0); return 0; }
        if (ctrl && wp == 'V') { SendMessage(hwnd, WM_COMMAND, IDM_EDIT_PASTE, 0); return 0; }
        if (wp == VK_F11) { SendMessage(hwnd, WM_COMMAND, IDM_CAPTURE_FULLSCREEN, 0); return 0; }
        if (wp == VK_F12) { SendMessage(hwnd, WM_COMMAND, IDM_CAPTURE_REGION, 0); return 0; }
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

    case WM_DESTROY:
        for (auto& tab : g.tabs)
            if (tab.hBitmap) DeleteObject(tab.hBitmap);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDPIAware();

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
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiToken);
    return (int)msg.wParam;
}
