#include "stdafx.h"
#include "Client.h"
#include "DisplayDlg.h"
#include "FullscreenBar.h"
IMPLEMENT_DYNAMIC(CDisplayDlg, CDialog)
CDisplayDlg::CDisplayDlg(CWnd* parent) : CDialog(IDD, parent) {}
CDisplayDlg::~CDisplayDlg() {}
void CDisplayDlg::DoDataExchange(CDataExchange* dx) {
    CDialog::DoDataExchange(dx);
    DDX_Control(dx, IDC_STATIC_DISPLAY, canvas_);
}
BEGIN_MESSAGE_MAP(CDisplayDlg, CDialog)
    ON_WM_SIZE()
END_MESSAGE_MAP()
BOOL CDisplayDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    SetDlgCtrlID(IDC_REMOTE_VIEW);
    ModifyStyle(WS_POPUP | WS_BORDER | WS_HSCROLL | WS_VSCROLL,
                WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, SWP_FRAMECHANGED);
    canvas_.ModifyStyle(SS_TYPEMASK, SS_NOTIFY | WS_TABSTOP);
    return TRUE;
}
bool CDisplayDlg::SetSize(int x, int y, int cx, int cy) {
    if (!GetSafeHwnd() || cx <= 0 || cy <= 0) return false;
    SetWindowPos(NULL,x,y,cx,cy,SWP_NOZORDER | SWP_NOACTIVATE);
    CRect area; GetClientRect(&area);
    LayoutSurface(area.Width(),area.Height());
    return true;
}
void CDisplayDlg::OnSize(UINT type, int cx, int cy) {
    CDialog::OnSize(type,cx,cy);
    if (type != SIZE_MINIMIZED) LayoutSurface(cx,cy);
}
void CDisplayDlg::LayoutSurface(int cx, int cy) {
    if (!canvas_.GetSafeHwnd() || cx <= 0 || cy <= 0) return;
    canvas_.SetWindowPos(NULL,0,0,cx,cy,SWP_NOZORDER | SWP_NOACTIVATE);
    if (display_) display_->SetSize(cx,cy);
    canvas_.Invalidate(FALSE);
}
HWND CDisplayDlg::GetDisplayHwnd() { return canvas_.GetSafeHwnd(); }
bool CDisplayDlg::Init(int, int) { return GetSafeHwnd() != NULL; }
void CDisplayDlg::SetDisplayer(ScreenDisplay* display) {
    display_ = display; canvas_.SetDisplayer(display);
    CRect area; GetClientRect(&area); LayoutSurface(area.Width(),area.Height());
}
void CDisplayDlg::FocusSurface() { if (canvas_.GetSafeHwnd()) canvas_.SetFocus(); }
