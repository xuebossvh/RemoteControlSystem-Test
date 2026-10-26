#include "stdafx.h"
#include "FullscreenBar.h"

BEGIN_MESSAGE_MAP(CFullscreenBar, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

BOOL CFullscreenBar::CreateBar(CWnd* parent) {
    const CString name = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
        ::LoadCursor(NULL, IDC_ARROW), NULL, NULL);
    if (!CreateEx(0, name, L"远程桌面控制", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                  CRect(0,0,1,1), parent, IDC_REMOTE_BAR)) return FALSE;
    CClientDC dc(this);
    const int dpi = dc.GetDeviceCaps(LOGPIXELSX);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    width_ = px(300); height_ = px(42); handle_ = px(7);
    font_.CreatePointFont(90, L"Segoe UI");
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    modeLabel_.Create(L"仅观看", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                      CRect(px(8),px(5),px(70),px(34)), this, 4105);
    fullscreenButton_.Create(L"全屏", style, CRect(px(74),px(5),px(164),px(34)), this, IDC_REMOTE_FULLSCREEN);
    fitButton_.Create(L"适应窗口", style, CRect(px(168),px(5),px(242),px(34)), this, IDC_REMOTE_FIT);
    disconnectButton_.Create(L"断开", style, CRect(px(246),px(5),px(292),px(34)), this, IDC_REMOTE_DISCONNECT);
    fullscreenButton_.SetFont(&font_); fitButton_.SetFont(&font_); disconnectButton_.SetFont(&font_);
    modeLabel_.SetFont(&font_);
    lastHover_ = GetTickCount();
    return TRUE;
}
void CFullscreenBar::Layout(bool fullscreen, int normalTop, bool reveal) {
    if (!GetSafeHwnd()) return;
    const bool changed = fullscreen_ != fullscreen;
    fullscreen_ = fullscreen; normalTop_ = normalTop;
    if (changed || reveal || !fullscreen) { expanded_ = true; lastHover_ = GetTickCount(); }
    fullscreenButton_.SetWindowText(fullscreen ? L"退出全屏  Esc" : L"全屏");
    Position();
}
void CFullscreenBar::Position() {
    CRect parent; GetParent()->GetClientRect(&parent);
    const int top = fullscreen_ ? (expanded_ ? 0 : handle_ - height_) : normalTop_;
    SetWindowPos(&wndTop, max(0, (parent.Width()-width_)/2), top, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void CFullscreenBar::TrackPointer() {
    if (!GetSafeHwnd() || !fullscreen_ || GetParent()->IsIconic()) return;
    POINT pointer; GetCursorPos(&pointer); GetParent()->ScreenToClient(&pointer);
    CRect parent; GetParent()->GetClientRect(&parent);
    const int left = max(0, (parent.Width()-width_)/2);
    const bool over = pointer.x >= left && pointer.x < left+width_ && pointer.y >= 0 &&
                      pointer.y < (expanded_ ? height_ : handle_+4);
    if (over) {
        lastHover_ = GetTickCount();
        if (!expanded_) { expanded_ = true; Position(); }
    } else if (expanded_ && GetTickCount()-lastHover_ > 1800) {
        expanded_ = false; Position();
    }
}
void CFullscreenBar::OnPaint() {
    CPaintDC dc(this); CRect rect; GetClientRect(&rect);
    dc.FillSolidRect(rect, RGB(235,239,245));
    const int gripWidth = width_/8;
    dc.FillSolidRect((width_-gripWidth)/2, height_-handle_+2, gripWidth, max(2,handle_/3), RGB(70,105,155));
}
BOOL CFullscreenBar::OnEraseBkgnd(CDC*) { return TRUE; }
BOOL CFullscreenBar::OnCommand(WPARAM wParam, LPARAM) {
    const UINT id = LOWORD(wParam);
    if (id >= IDC_REMOTE_FULLSCREEN && id <= IDC_REMOTE_DISCONNECT) {
        GetParent()->PostMessage(WM_COMMAND, id); return TRUE;
    }
    return FALSE;
}
