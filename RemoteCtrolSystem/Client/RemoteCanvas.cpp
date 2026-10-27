#include "stdafx.h"
#include "RemoteCanvas.h"
#include <shellapi.h>
#include <cwchar>

// Explicit local test mode only. No cross-process pointers/DCs or saved frames.
// WDA deliberately blocks external screen/DC reads, so GUI regression tests
// ask this process to render its synthetic fixture and read scalar samples.
CRemoteCanvas::CRemoteCanvas() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments) {
        for (int i = 1; i < count; ++i)
            if (std::wcscmp(arguments[i], L"--ui-paint-self-test") == 0) diagnosticsEnabled_ = true;
        LocalFree(arguments);
    }
}
CRemoteCanvas::~CRemoteCanvas() { ReleaseProbe(); }

BEGIN_MESSAGE_MAP(CRemoteCanvas, CStatic)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_MESSAGE(WM_PRINTCLIENT, &CRemoteCanvas::OnPrintClient)
    ON_MESSAGE(WM_PRINT, &CRemoteCanvas::OnPrintClient)
    ON_WM_LBUTTONDOWN()
    ON_WM_GETDLGCODE()
    ON_WM_DESTROY()
    ON_WM_CAPTURECHANGED()
    ON_WM_KILLFOCUS()
    ON_MESSAGE(WM_MOUSELEAVE, &CRemoteCanvas::OnMouseLeave)
    ON_MESSAGE(WM_APP + 0x340, &CRemoteCanvas::OnProbeBegin)
    ON_MESSAGE(WM_APP + 0x341, &CRemoteCanvas::OnProbePixel)
    ON_MESSAGE(WM_APP + 0x342, &CRemoteCanvas::OnProbeEnd)
END_MESSAGE_MAP()

void CRemoteCanvas::OnPaint() {
    CPaintDC dc(this);
    if (display_ && display_->Render(dc.GetSafeHdc())) {
        // Read-only diagnostic: count real WM_PAINT draws, never offscreen prints.
        SetPropW(GetSafeHwnd(), L"RCS.RenderPaintCount", reinterpret_cast<HANDLE>(++paintCount_));
    }
}
BOOL CRemoteCanvas::OnEraseBkgnd(CDC*) { return TRUE; }
LRESULT CRemoteCanvas::OnPrintClient(WPARAM dc, LPARAM) {
    return display_ && display_->Render(reinterpret_cast<HDC>(dc)) ? 1 : 0;
}
void CRemoteCanvas::OnLButtonDown(UINT flags, CPoint point) {
    SetFocus();
    CStatic::OnLButtonDown(flags, point);
}
UINT CRemoteCanvas::OnGetDlgCode() { return DLGC_WANTALLKEYS | DLGC_WANTARROWS; }
void CRemoteCanvas::OnCaptureChanged(CWnd* window) {
    // Synchronous: a delayed notification could release a later, new drag.
    if (!window || window->GetSafeHwnd() != GetSafeHwnd())
        ::SendMessage(::GetAncestor(m_hWnd, GA_ROOT), WM_APP + 0x343, WM_CAPTURECHANGED, 0);
    CStatic::OnCaptureChanged(window);
}
void CRemoteCanvas::OnKillFocus(CWnd* window) {
    ::SendMessage(::GetAncestor(m_hWnd, GA_ROOT), WM_APP + 0x343, WM_KILLFOCUS, 0);
    CStatic::OnKillFocus(window);
}
LRESULT CRemoteCanvas::OnMouseLeave(WPARAM, LPARAM) {
    ::SendMessage(::GetAncestor(m_hWnd, GA_ROOT), WM_APP + 0x343, WM_MOUSELEAVE, 0);
    return 0;
}
void CRemoteCanvas::OnDestroy() {
    ReleaseProbe();
    RemovePropW(GetSafeHwnd(), L"RCS.RenderPaintCount");
    display_ = nullptr;
    CStatic::OnDestroy();
}

void CRemoteCanvas::ReleaseProbe() {
    if (probeDC_ && probeOriginal_) SelectObject(probeDC_, probeOriginal_);
    if (probeBitmap_) DeleteObject(probeBitmap_);
    if (probeDC_) DeleteDC(probeDC_);
    probeDC_ = nullptr; probeBitmap_ = nullptr; probeOriginal_ = nullptr;
    probeWidth_ = probeHeight_ = 0;
}
LRESULT CRemoteCanvas::OnProbeBegin(WPARAM, LPARAM) {
    if (!diagnosticsEnabled_ || !display_ || ::IsIconic(::GetAncestor(m_hWnd, GA_ROOT))) return 0;
    ReleaseProbe();
    CRect area; GetClientRect(&area);
    if (area.Width() <= 0 || area.Height() <= 0 || area.Width() > 8192 || area.Height() > 8192) return 0;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = area.Width(); info.bmiHeader.biHeight = -area.Height();
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    probeDC_ = CreateCompatibleDC(NULL);
    probeBitmap_ = CreateDIBSection(probeDC_, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!probeDC_ || !probeBitmap_) { ReleaseProbe(); return 0; }
    probeOriginal_ = SelectObject(probeDC_, probeBitmap_);
    if (!probeOriginal_ || probeOriginal_ == HGDI_ERROR || !display_->Render(probeDC_)) { ReleaseProbe(); return 0; }
    probeWidth_ = area.Width(); probeHeight_ = area.Height();
    return 1;
}
LRESULT CRemoteCanvas::OnProbePixel(WPARAM x, LPARAM y) {
    if (!diagnosticsEnabled_ || !probeDC_ || x >= static_cast<WPARAM>(probeWidth_) || y < 0 || y >= probeHeight_)
        return static_cast<LRESULT>(CLR_INVALID);
    return static_cast<LRESULT>(GetPixel(probeDC_, static_cast<int>(x), static_cast<int>(y)));
}
LRESULT CRemoteCanvas::OnProbeEnd(WPARAM, LPARAM) {
    if (!diagnosticsEnabled_) return 0;
    ReleaseProbe(); return 1;
}
