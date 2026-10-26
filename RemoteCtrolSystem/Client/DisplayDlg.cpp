// DisplayDlg.cpp : 实现文件
//

#include "stdafx.h"
#include "Client.h"
#include "DisplayDlg.h"
#include  <math.h>

// CDisplayDlg 对话框

IMPLEMENT_DYNAMIC(CDisplayDlg, CDialog)

CDisplayDlg::CDisplayDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CDisplayDlg::IDD, pParent)
{
	SetScreenSize(1024,768);
	m_IsFullScreen = false;
	m_cx  = 0;
	m_cy  = 0;
	m_ScreenDisplayer = NULL;
}

CDisplayDlg::~CDisplayDlg()
{
}

void CDisplayDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_STATIC_DISPLAY, m_pctDisplay);
}


BEGIN_MESSAGE_MAP(CDisplayDlg, CDialog)
	ON_WM_HSCROLL()
	ON_WM_VSCROLL()
	ON_WM_SIZE()
END_MESSAGE_MAP()


// CDisplayDlg 消息处理程序

void CDisplayDlg::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	int h = 10;
	SCROLLINFO si;
	si.cbSize = sizeof(SCROLLINFO);
	si.fMask = SIF_ALL;
	GetScrollInfo(SB_HORZ, &si);
	int nOldPos = si.nPos;

	switch (nSBCode)
	{
	case SB_LINEDOWN:
		si.nPos = min(si.nPos + h, si.nMax);
		break;
	case SB_PAGEDOWN:
		si.nPos = min(si.nPos + h * 10, si.nMax);
		break;
	case SB_LINEUP:
		si.nPos = max(si.nPos - h, si.nMin);
		break;
	case SB_PAGEUP:
		si.nPos = max(si.nPos - h * 10, si.nMin);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK:
		si.nPos = si.nTrackPos;
		break;
	case SB_TOP:
		si.nPos = si.nMin;
		break;
	case SB_BOTTOM:
		si.nPos = si.nMax;
		break;
	}

	SetScrollInfo(SB_HORZ, &si);
	GetScrollInfo(SB_HORZ, &si); //重新获取新的位置
	ScrollWindowEx(nOldPos - si.nPos,0,NULL, NULL, NULL, NULL, SW_ERASE|SW_SCROLLCHILDREN);
	UpdateWindow();
	//m_ScreenDisplayer->ReflashDC();
	CDialog::OnHScroll(nSBCode, nPos, pScrollBar);
}

void CDisplayDlg::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	int h = 10;
	SCROLLINFO si;
	si.cbSize = sizeof(SCROLLINFO);
	si.fMask = SIF_ALL;
	GetScrollInfo(SB_VERT, &si);
	int nOldPos = si.nPos;

	switch (nSBCode)
	{
	case SB_LINEDOWN:
		si.nPos = min(si.nPos + h, si.nMax);
		break;
	case SB_PAGEDOWN:
		si.nPos = min(si.nPos + h * 10, si.nMax);
		break;
	case SB_LINEUP:
		si.nPos = max(si.nPos - h, si.nMin);
		break;
	case SB_PAGEUP:
		si.nPos = max(si.nPos - h * 10, si.nMin);
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK:
		si.nPos = si.nTrackPos;
		break;
	case SB_TOP:
		si.nPos = si.nMin;
		break;
	case SB_BOTTOM:
		si.nPos = si.nMax;
		break;
	}

	SetScrollInfo(SB_VERT, &si);
	GetScrollInfo(SB_VERT, &si); //重新获取新的位置


	ScrollWindowEx(0, nOldPos - si.nPos,NULL, NULL, NULL, NULL, SW_ERASE|SW_SCROLLCHILDREN);
	UpdateWindow();
	RepositionBars(AFX_IDW_CONTROLBAR_FIRST,AFX_IDW_CONTROLBAR_LAST,0);
	//m_ScreenDisplayer->ReflashDC();
	CDialog::OnVScroll(nSBCode, nPos, pScrollBar);
}

bool CDisplayDlg::SetSize(int x, int y, int cx, int cy)
{
    if (!GetSafeHwnd() || cx <= 0 || cy <= 0) return false;
    m_left = x; m_top = y; m_cx = cx; m_cy = cy;
    ShowScrollBar(SB_BOTH, FALSE);
    MoveWindow(x, y, cx, cy);
    CRect area; GetClientRect(&area);
    LayoutSurface(area.Width(), area.Height());
    return true;
}

void CDisplayDlg::OnSize(UINT type, int cx, int cy)
{
    CDialog::OnSize(type, cx, cy);
    LayoutSurface(cx, cy);
}

void CDisplayDlg::LayoutSurface(int cx, int cy)
{
    if (!m_pctDisplay.GetSafeHwnd() || cx <= 0 || cy <= 0) return;
    m_pctDisplay.MoveWindow(0, 0, cx, cy);
    if (m_ScreenDisplayer) {
        m_ScreenDisplayer->SetSize(cx, cy);
    }
}

HWND CDisplayDlg::GetDisplayHwnd()
{
	return m_pctDisplay.GetSafeHwnd();
}

bool CDisplayDlg::SetScreenSize( int width,int height )
{
	m_ScreenWidth	= width;
	m_ScreenHeight	= height;
	return true;
}

bool CDisplayDlg::Init( int width,int height )
{
	SetScreenSize(width,height);
	m_IsFullScreen	= false;

	return true;
}

void CDisplayDlg::FullScreen()
{

	m_IsFullScreen  = true;
	MoveWindow(0,0,m_ScreenWidth,m_ScreenHeight);
	ScrollWindowEx(GetScrollPos(SB_HORZ),GetScrollPos(SB_VERT),NULL, NULL, NULL, NULL, SW_ERASE|SW_SCROLLCHILDREN);
	SetScrollPos(SB_HORZ,0);
	SetScrollPos(SB_VERT,0);
	ShowScrollBar(SB_HORZ,FALSE);
	ShowScrollBar(SB_VERT,FALSE);
}

bool CDisplayDlg::EndFullScreen()
{
	m_IsFullScreen = false;
	ShowScrollBar(SB_HORZ,FALSE);
	ShowScrollBar(SB_VERT,FALSE);
	return SetSize(m_left,m_top,m_cx,m_cy);
}

bool CDisplayDlg::IsFullScreen()
{
	return m_IsFullScreen;
}



BOOL CDisplayDlg::PreTranslateMessage(MSG* pMsg)
{
	if (WM_KEYDOWN==pMsg->message ||pMsg->wParam == VK_ESCAPE)
	{
		return TRUE;
	}


	return CDialog::PreTranslateMessage(pMsg);
}

void CDisplayDlg::OnOK()
{
	// TODO: 在此添加专用代码和/或调用基类

	//CDialog::OnOK();
}

void CDisplayDlg::SetDisplayer( ScreenDisplay* ScreenDisplayer )
{
	m_ScreenDisplayer = ScreenDisplayer;
}
