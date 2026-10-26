#pragma once
#include "afxwin.h"
#include "RemoteCanvas.h"

class CDisplayDlg : public CDialog {
    DECLARE_DYNAMIC(CDisplayDlg)
public:
    CDisplayDlg(CWnd* parent = NULL);
    virtual ~CDisplayDlg();
    enum { IDD = IDD_DIALOG_DISPLAY };
    HWND GetDisplayHwnd();
    bool SetSize(int x, int y, int cx, int cy);
    bool Init(int width, int height);
    void SetDisplayer(ScreenDisplay* display);
    void FocusSurface();
protected:
    virtual void DoDataExchange(CDataExchange* dx);
    virtual BOOL OnInitDialog();
    virtual void OnOK() {}
    virtual void OnCancel() {}
    afx_msg void OnSize(UINT type, int cx, int cy);
    DECLARE_MESSAGE_MAP()
private:
    void LayoutSurface(int cx, int cy);
    CRemoteCanvas canvas_;
    ScreenDisplay* display_ = nullptr;
};
