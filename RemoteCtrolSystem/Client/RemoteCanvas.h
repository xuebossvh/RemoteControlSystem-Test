#pragma once
#include <afxwin.h>
#include "../Graphic/ScreenDisplay.h"

class CRemoteCanvas : public CStatic {
public:
    CRemoteCanvas();
    virtual ~CRemoteCanvas();
    void SetDisplayer(ScreenDisplay* display) { display_ = display; Invalidate(FALSE); }
protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC*);
    afx_msg LRESULT OnPrintClient(WPARAM, LPARAM);
    afx_msg void OnLButtonDown(UINT flags, CPoint point);
    afx_msg UINT OnGetDlgCode();
    afx_msg void OnDestroy();
    afx_msg void OnCaptureChanged(CWnd* window);
    afx_msg void OnKillFocus(CWnd* window);
    afx_msg LRESULT OnMouseLeave(WPARAM, LPARAM);
    afx_msg LRESULT OnProbeBegin(WPARAM, LPARAM);
    afx_msg LRESULT OnProbePixel(WPARAM, LPARAM);
    afx_msg LRESULT OnProbeEnd(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()
private:
    ScreenDisplay* display_ = nullptr;
    ULONG_PTR paintCount_ = 0;
    bool diagnosticsEnabled_ = false;
    HDC probeDC_ = nullptr;
    HBITMAP probeBitmap_ = nullptr;
    HGDIOBJ probeOriginal_ = nullptr;
    int probeWidth_ = 0, probeHeight_ = 0;
    void ReleaseProbe();
};
