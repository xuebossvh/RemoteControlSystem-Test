#pragma once
#include <afxwin.h>

enum { IDC_REMOTE_VIEW = 4100, IDC_REMOTE_BAR = 4101, IDC_REMOTE_FULLSCREEN = 4102,
       IDC_REMOTE_FIT = 4103, IDC_REMOTE_DISCONNECT = 4104 };

// A child overlay: it inherits its Client owner's capture exclusion and cannot
// remain floating over other applications when the owner is minimized.
class CFullscreenBar : public CWnd {
public:
    BOOL CreateBar(CWnd* parent);
    void Layout(bool fullscreen, int normalTop, bool reveal = false);
    void TrackPointer();
    void SetControlMode(bool control, bool connected = true, bool local = false) {
        if (modeLabel_.GetSafeHwnd()) modeLabel_.SetWindowText(!connected ? L"已断开" :
            (control ? (local ? L"同机控制" : L"可控制") : L"仅观看"));
    }
protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC*);
    virtual BOOL OnCommand(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()
private:
    void Position();
    CButton fullscreenButton_, fitButton_, disconnectButton_;
    CStatic modeLabel_;
    CFont font_;
    bool fullscreen_ = false, expanded_ = true;
    int width_ = 0, height_ = 0, handle_ = 0, normalTop_ = 0;
    DWORD lastHover_ = 0;
};
