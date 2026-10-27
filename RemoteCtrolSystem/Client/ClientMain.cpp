// ClientMain.cpp : 实现文件
//

#include "stdafx.h"
#include "Client.h"
#include "ClientMain.h"
#include "../Graphic/GdiImage.h"
#include "..\Message\KeyboardMsg.h"
#include "..\Message\SetupMsg.h"
#include "..\Message\UserMsg.h"
#include "..\Message\MouseMsg.h"
#include "LocalPeer.h"
#include <windowsx.h>
#include <map>
#include <atlconv.h>

// CClientMain 对话框

IMPLEMENT_DYNAMIC(CClientMain, CDialog)

CClientMain::CClientMain(CWnd* pParent /*=NULL*/)
	: CDialog(CClientMain::IDD, pParent)
{
	m_IsFullScreen = false;
	m_nTimer = 0;
	m_bTcpConect = FALSE;
	m_bCtrlPermission = FALSE;
	m_AltCheck	= FALSE;
	m_WinCheck	= FALSE;
	m_CtrlCheck	= FALSE;
}

CClientMain::~CClientMain()
{
}

void CClientMain::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_STATIC_DISPLAY, m_pctDisplay);
}


BEGIN_MESSAGE_MAP(CClientMain, CDialog)
	ON_WM_CREATE()
	ON_WM_MOVE()
	ON_WM_SIZE()
	ON_COMMAND(ID_BUTTON_SERVER_LIST,&CClientMain::OnBtnServerList)
	ON_COMMAND(ID_BUTTON_SETUP,&CClientMain::OnBtnSetup)
	ON_COMMAND(ID_BUTTON_SHOT_SCREEN,&CClientMain::OnBtnScreenShot)
	ON_COMMAND(ID_BUTTON_FULL_SCREEN,&CClientMain::OnBtnFullScreen)
	ON_COMMAND(IDC_REMOTE_FULLSCREEN,&CClientMain::OnBtnFullScreen)
	ON_COMMAND(IDC_REMOTE_FIT,&CClientMain::OnBtnScaleLocalSize)
	ON_COMMAND(IDC_REMOTE_DISCONNECT,&CClientMain::OnDisconnect)
	ON_COMMAND(ID_BUTTON_ALT,&CClientMain::OnBtnAlt)
	ON_COMMAND(ID_BUTTON_CTRL,&CClientMain::OnBtnCtrl)
	ON_COMMAND(ID_BUTTON_WINKEY,&CClientMain::OnBtnWin)
	ON_COMMAND(ID_BUTTON_SCALE_UP,&CClientMain::OnBtnScaleUp)
	ON_COMMAND(ID_BUTTON_SCALE_DOWN,&CClientMain::OnBtnScaleDown)
	ON_COMMAND(ID_BUTTON_AUTO_SIZE,&CClientMain::OnBtnScaleLocalSize)
	ON_COMMAND(ID_BUTTON_TASKMGR,&CClientMain::OnBtnTackMgr)


	ON_WM_TIMER()
	ON_WM_VSCROLL()
	ON_WM_HSCROLL()
	ON_WM_DESTROY()
	ON_WM_GETMINMAXINFO()
	ON_WM_ACTIVATEAPP()
	ON_MESSAGE(WM_APP + 0x343, &CClientMain::OnInputLost)
//	ON_MESSAGE(WM_ADDIP, OnAddIpMessage)
END_MESSAGE_MAP()


// CClientMain 消息处理程序

int CClientMain::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CDialog::OnCreate(lpCreateStruct) == -1)
		return -1;

	return 0;
}


void CClientMain::OnMove(int x, int y)
{
	CDialog::OnMove(x, y);
	if (IsIconic() || m_changingWindowMode) return;

	if (m_dlgServerList.GetStatus() == SL_DOCK_WINDOW
		&& m_dlgServerList.GetSafeHwnd() != NULL)
	{
		RECT rcClient;
		RECT rcServerList;
		GetWindowRect(&rcClient);
		m_dlgServerList.GetWindowRect(&rcServerList);

		rcServerList.top = rcClient.top;
		rcServerList.bottom = rcClient.bottom;
		rcServerList.right = rcClient.right + rcServerList.right - rcServerList.left;
		rcServerList.left   = rcClient.right;

		m_dlgServerList.MoveWindow(&rcServerList,TRUE);
    }
	LayoutDisplay();
	//m_pctDisplay.SetWindowPos(m_pctDisplay.GetWindow(GW_CHILD),0 ,24,ScreenWidth,ScreenHeight,SWP_NOACTIVATE);

}
void CClientMain::OnSize(UINT nType, int cx, int cy)
{
	CDialog::OnSize(nType, cx, cy);
	if (nType == SIZE_MINIMIZED) ReleaseRemoteInput();
	if (nType == SIZE_MINIMIZED || cx <= 0 || cy <= 0 || m_changingWindowMode) return;

	if (!m_IsFullScreen && m_dlgServerList.GetSafeHwnd() != NULL)
	{
		RECT rcClient;
		RECT rcServerList;
		GetWindowRect(&rcClient);
		m_dlgServerList.GetWindowRect(&rcServerList);

		rcServerList.top = rcClient.top;
		rcServerList.bottom = rcClient.bottom;
		rcServerList.right = rcClient.right + rcServerList.right - rcServerList.left;
		rcServerList.left   = rcClient.right;

		m_dlgServerList.MoveWindow(&rcServerList,TRUE);
	}
	LayoutDisplay();
	//m_pctDisplay.SetWindowPos(m_pctDisplay.GetWindow(GW_CHILD),0 ,24,ScreenWidth,ScreenHeight,SWP_NOACTIVATE);
}

void CClientMain::OnBtnServerList()
{
	if (m_dlgServerList.GetStatus() == SL_HIDE)
	{
		m_dlgServerList.SetStatus(SL_DOCK_WINDOW);
	}
	else if (m_dlgServerList.GetStatus() == SL_DOCK_WINDOW ||
		       m_dlgServerList.GetStatus() == SL_DOCK_SCREEN)
	{
		m_dlgServerList.SetStatus(SL_HIDE);
	}
	else
	{

	}

}

void CClientMain::OnTimer(UINT_PTR nIDEvent)
{

	//m_ScreenShooter.ShotScreenImage(&m_RecvQueue);

    DealMessage();
    m_fullscreenBar.TrackPointer();
	CDialog::OnTimer(nIDEvent);
}

bool CClientMain::SetFrame(int frame)
{
	if (0!=m_nTimer)
	{
		KillTimer(m_nTimer);
	}
	m_nTimer = SetTimer(1,1000/30,0);
	m_Frame = frame;
	if (m_bTcpConect)
	{
	SetupMsg *FrameMsg = new SetupMsg;
	FrameMsg->PacketMsg(RCS_SETUP_MSG_FRAME,frame);
	m_tcpClien.send(*FrameMsg);
	delete FrameMsg;
	}
	return false;
}

void CClientMain::OnBtnSetup()
{
	m_dlgSetup.ShowWindow(SW_SHOW);
}

void CClientMain::OnBtnScreenShot()
{
	OnScreenShot();
}

void CClientMain::OnScreenShot()
{
    GdiImage screenshot = m_ScreenDisplayer.GetDisplayImage();
    screenshot.SetJpegQuality(m_dlgSetup.GetImageQuality());
    CFileDialog save(FALSE, L"png", L"screenshot.png", OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                     L"PNG (*.png)|*.png|JPEG (*.jpg)|*.jpg|Bitmap (*.bmp)|*.bmp||", this);
    if (save.DoModal() != IDOK) return;
    const DWORD formats[] = {GDI_IMAGE_PNG, GDI_IMAGE_JPG, GDI_IMAGE_BMP};
    const wchar_t* extensions[] = {L".png", L".jpg", L".bmp"};
    const unsigned index = save.m_ofn.nFilterIndex - 1;
    if (index >= 3) return;
    CString path = save.GetPathName();
    CString extension = save.GetFileExt();
    if (extension.IsEmpty()) path += extensions[index];
    if (!screenshot.Save(path, formats[index])) MessageBox(L"Could not save the screenshot.");
}

bool CClientMain::SetImageQuality( int quality )
{
	//return m_ScreenShooter.SetQuality(quality);
	if (m_bTcpConect)
	{
	SetupMsg *ImageQualityMsg = new SetupMsg;
	ImageQualityMsg->PacketMsg(RCS_SETUP_MSG_IMAGE_QUALITY,quality);
	m_tcpClien.send(*ImageQualityMsg);
	delete ImageQualityMsg;
	}
	return true;
}

BOOL CClientMain::OnInitDialog()
{
	CDialog::OnInitDialog();
	ModifyStyle(0, WS_CLIPCHILDREN);

	m_bTcpConect  =FALSE;
	m_MainToolBar.CreateEx(this, TBSTYLE_FLAT | TBSTYLE_TOOLTIPS, WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_TOOLTIPS | CBRS_FLYBY);
	m_MainToolBar.LoadToolBar(IDR_TOOLBAR3);
	m_MainToolBar.GetToolBarCtrl().SetBitmapSize(CSize(16,16));
	m_MainToolBar.GetToolBarCtrl().SetButtonSize(CSize(22,22));

	RepositionBars(AFX_IDW_CONTROLBAR_FIRST,AFX_IDW_CONTROLBAR_LAST,0);

	ShowWindow(SW_HIDE);
	Host host;
	if (!m_HostListConfig.Init("ClientConfig.xml")) {
		MessageBox(L"ClientConfig.xml is invalid. Check the application data directory.");
		EndDialog(IDCANCEL); return FALSE;
	}
	m_HostListConfig.ReadHostList(m_vtHostList);
	m_cmbLogin.Init(&m_HostListConfig,&host);
	INT_PTR nResponse =m_cmbLogin.DoModal();
	if (nResponse != IDC_BUTTON_LOGIN)
	{
		EndDialog(IDCANCEL);
		return FALSE;
	}
	else
	{
		m_tcpClien.Start();
		m_bTcpConect = TRUE;
	}

	m_dlgServerList.Create(IDD_DIALOG_SERVER_LIST,this);
	m_dlgServerList.Init(&m_HostListConfig);
	m_dlgSetup.Create(IDD_DIALOG_SETUP,this);
	if (!m_dlgDisplay.Create(IDD_DIALOG_DISPLAY,this)) { m_tcpClien.Close(); EndDialog(IDCANCEL); return FALSE; }

	RECT rcScreen;
	::GetClientRect(::GetDesktopWindow(),&rcScreen);
	int ScreenWidth = rcScreen.right - rcScreen.left;
	int ScreenHeight = rcScreen.bottom - rcScreen.top;
	m_pctDisplay.ShowWindow(SW_HIDE);
	if (!m_fullscreenBar.CreateBar(this)) { m_tcpClien.Close(); EndDialog(IDCANCEL); return FALSE; }
	m_fullscreenBar.SetControlMode(m_bCtrlPermission != FALSE);
	m_dlgDisplay.Init(ScreenWidth,ScreenHeight);
	m_dlgDisplay.ShowWindow(SW_SHOW);
	m_dlgServerList.SetStatus(SL_HIDE);

	if (!m_ScreenDisplayer.Initialize(m_dlgDisplay.GetDisplayHwnd(),ScreenWidth,ScreenHeight,1280,800)) {
        m_tcpClien.Close(); EndDialog(IDCANCEL); return FALSE;
    }


	m_Frame = 10;
	m_dlgSetup.SetFrame(m_Frame);
	SetFrame(m_dlgSetup.GetFrame());

	SetTileNumber(m_dlgSetup.GetHorzTile(),m_dlgSetup.GetVertTile());
	SetImageQuality(m_dlgSetup.GetImageQuality());
	m_dlgDisplay.SetDisplayer(&m_ScreenDisplayer);
    LayoutDisplay();
    SetWindowText(L"Remote Control Client");
    ShowWindow(SW_SHOW);

	return TRUE;  // return TRUE unless you set the focus to a control
	// 异常: OCX 属性页应返回 FALSE
}

void CClientMain::OnBtnFullScreen()
{
    if (m_IsFullScreen) EndFullScreen(); else FullScreen();
}

BOOL CClientMain::PreTranslateMessage(MSG* pMsg)
{
	return CDialog::PreTranslateMessage(pMsg);
}


void CClientMain::OnOK()
{

	//CDialog::OnOK();
}

void CClientMain::OnBtnScaleUp()
{
	m_ScreenDisplayer.ScaleUp();
}

void CClientMain::OnBtnScaleDown()
{
	m_ScreenDisplayer.ScaleDown();
}

void CClientMain::OnDestroy()
{
	if (m_nTimer) KillTimer(m_nTimer);
	ReleaseRemoteInput();
	m_bTcpConect = FALSE;
	m_tcpClien.Close();
	CDialog::OnDestroy();
}

void CClientMain::SendMouseMsg(int x, int y, int status)
{
    if (!m_inputReady || !m_bTcpConect || !m_bCtrlPermission || !m_tcpClien.IsConnected()) return;
    HWND display = m_dlgDisplay.GetDisplayHwnd();
    if (!display) return;
    POINT point = {x, y}; ::ScreenToClient(display, &point);
    int remoteX = 0, remoteY = 0;
    if (!m_ScreenDisplayer.MapWindowPointToServer(point.x, point.y, remoteX, remoteY)) return;
    DispatchMouse(remoteX, remoteY, status);
}

void CClientMain::DispatchMouse(int x, int y, int status)
{
    m_lastRemotePoint = {x, y};
    if (m_localDesktop) {
        const UINT kind = LOWORD(status);
        // Some shell accessibility providers activate Explorer even for Select.
        // Preserve only a gesture that began in our foreground canvas. Do not
        // seize focus after Invoke launches an application, or after an unrelated
        // application becomes foreground while a provider is running.
        const bool protect = m_pressedMouse && ::GetForegroundWindow() == m_hWnd &&
            (status == 0 || kind == WM_LBUTTONDOWN || kind == WM_LBUTTONDBLCLK);
        m_protectLocalGesture = protect;
        m_localGestureInterrupted = false;
        const bool handled = m_localInput.Mouse(x, y, status);
        if (protect && (m_localGestureInterrupted || ::GetForegroundWindow() != m_hWnd)) {
            const HWND foreground = ::GetForegroundWindow();
            const HWND target = m_localInput.TargetWindow();
            const HWND owner = target ? ::GetAncestor(target, GA_ROOTOWNER) : NULL;
            if (IsWindowVisible() && IsWindowEnabled() && !IsIconic() &&
                (foreground == m_hWnd || (owner && ::GetAncestor(foreground, GA_ROOTOWNER) == owner))) {
                SetForegroundWindow();
                SetWindowPos((GetWindowLongPtr(m_hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? &wndTopMost : &wndTop,
                    0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                const HWND surface = m_dlgDisplay.GetDisplayHwnd();
                if (::GetForegroundWindow() == m_hWnd && surface) {
                    ::SetFocus(surface);
                    if (m_pressedMouse) ::SetCapture(surface);
                }
            }
            m_protectLocalGesture = false;
            if (::GetForegroundWindow() != m_hWnd) ReleaseRemoteInput();
        } else m_protectLocalGesture = false;
        const bool down = kind == WM_LBUTTONDOWN || kind == WM_LBUTTONDBLCLK ||
            kind == WM_RBUTTONDOWN || kind == WM_RBUTTONDBLCLK || kind == WM_MBUTTONDOWN || kind == WM_MBUTTONDBLCLK;
        if ((!handled || down) && kind >= WM_LBUTTONDOWN && kind <= WM_MBUTTONDBLCLK)
            SetWindowText(handled ? L"Remote Control Client" :
                L"Remote Control Client - 此窗口不支持同机代理，请直接操作本机窗口或使用两台电脑");
        return;
    }
    MouseMsg message; message.PacketMsg(x, y, status);
    m_tcpClien.send(message);
}

bool CClientMain::HandleRemoteInput(MSG* message)
{
    if (!m_inputReady || !m_bTcpConect || !m_bCtrlPermission || !m_tcpClien.IsConnected()) return false;
    const UINT kind = message->message;
    const bool wheel = kind == WM_MOUSEWHEEL || kind == WM_MOUSEHWHEEL;
    const HWND surface = m_dlgDisplay.GetDisplayHwnd();
    if (!surface) return false;
    if (wheel) {
        // Wheel messages can follow keyboard focus. Route by the actual point,
        // never by a stale focus in the canvas when the user is over local UI.
        POINT point = {GET_X_LPARAM(message->lParam), GET_Y_LPARAM(message->lParam)};
        if (::WindowFromPoint(point) != surface) return false;
    } else if (!IsRemoteInputTarget(message->hwnd)) return false;

    if (kind == WM_KEYDOWN || kind == WM_SYSKEYDOWN || kind == WM_KEYUP || kind == WM_SYSKEYUP) {
        SendKeyboardMsg(static_cast<int>(message->wParam),
            kind == WM_KEYDOWN || kind == WM_SYSKEYDOWN ? RCS_KEY_DOWN : RCS_KEY_UP);
        return true;
    }
    if (!(kind >= WM_MOUSEMOVE && kind <= WM_MBUTTONDBLCLK) && !wheel) return false;
    POINT point = {GET_X_LPARAM(message->lParam), GET_Y_LPARAM(message->lParam)};
    if (wheel) ::ScreenToClient(surface, &point);
    int remoteX = 0, remoteY = 0;
    bool mapped = m_ScreenDisplayer.MapWindowPointToServer(point.x, point.y, remoteX, remoteY);
    // Keep a drag's last up even outside the image, without trapping a hovering
    // pointer. Clamp captured drag coordinates to the visible image boundary.
    if (!mapped && m_pressedMouse) {
        RECT image = m_ScreenDisplayer.GetDisplayRect(), viewport = {};
        ::GetClientRect(surface, &viewport);
        image.left = max(image.left, viewport.left); image.top = max(image.top, viewport.top);
        image.right = min(image.right, viewport.right); image.bottom = min(image.bottom, viewport.bottom);
        if (image.right > image.left && image.bottom > image.top) {
            point.x = max(image.left, min(point.x, image.right - 1));
            point.y = max(image.top, min(point.y, image.bottom - 1));
            mapped = m_ScreenDisplayer.MapWindowPointToServer(point.x, point.y, remoteX, remoteY);
        }
    }
    if (!mapped) return false;
    UINT button = 0; bool down = false, up = false;
    switch (kind) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = true; button = MK_LBUTTON; break;
    case WM_LBUTTONUP: up = true; button = MK_LBUTTON; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = true; button = MK_RBUTTON; break;
    case WM_RBUTTONUP: up = true; button = MK_RBUTTON; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = true; button = MK_MBUTTON; break;
    case WM_MBUTTONUP: up = true; button = MK_MBUTTON; break;
    }
    if (up && !(m_pressedMouse & button)) return true;
    if (down) {
        ::SetFocus(surface);
        m_pressedMouse |= button;
        ::SetCapture(surface);
    }
    const int status = wheel ? static_cast<int>(MAKELONG(kind, HIWORD(message->wParam))) :
        (kind == WM_MOUSEMOVE ? 0 : static_cast<int>(kind));
    if (up) {
        m_pressedMouse &= ~button;
        if (!m_pressedMouse && ::GetCapture() == surface) ::ReleaseCapture();
    }
    DispatchMouse(remoteX, remoteY, status);
    if (kind == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, surface, 0};
        ::TrackMouseEvent(&track);
    }
    return true;
}

void CClientMain::ReleaseRemoteInput()
{
    const UINT buttons = m_pressedMouse;
    m_pressedMouse = 0; // Clear before ReleaseCapture sends a reentrant notification.
    if (!m_localDesktop) {
        if (buttons & MK_LBUTTON) DispatchMouse(m_lastRemotePoint.x, m_lastRemotePoint.y, WM_LBUTTONUP);
        if (buttons & MK_RBUTTON) DispatchMouse(m_lastRemotePoint.x, m_lastRemotePoint.y, WM_RBUTTONUP);
        if (buttons & MK_MBUTTON) DispatchMouse(m_lastRemotePoint.x, m_lastRemotePoint.y, WM_MBUTTONUP);
    }
    for (int key = 0; key < 256; ++key) {
        if (m_pressedKeys[key]) { m_pressedKeys[key] = false; if (!m_localDesktop) DispatchKeyboard(key, RCS_KEY_UP); }
    }
    m_localInput.Release();
    m_AltCheck = m_WinCheck = m_CtrlCheck = FALSE;
    if (m_MainToolBar.GetSafeHwnd()) {
        for (int index = 3; index <= 5; ++index)
            m_MainToolBar.GetToolBarCtrl().CheckButton(m_MainToolBar.GetItemID(index), FALSE);
    }
    const HWND surface = m_dlgDisplay.GetDisplayHwnd();
    if (surface && ::GetCapture() == surface) ::ReleaseCapture();
}

LRESULT CClientMain::OnInputLost(WPARAM reason, LPARAM)
{
    if (m_protectLocalGesture) { m_localGestureInterrupted = true; return 0; }
    // A drag deliberately keeps capture beyond the canvas; ordinary hover does not.
    if (reason == WM_CAPTURECHANGED && !m_pressedMouse) return 0;
    if (reason != WM_MOUSELEAVE || !m_pressedMouse) ReleaseRemoteInput();
    return 0;
}

void CClientMain::OnActivateApp(BOOL active, DWORD thread)
{
    if (!active) {
        if (m_protectLocalGesture) m_localGestureInterrupted = true;
        else ReleaseRemoteInput();
    }
    CDialog::OnActivateApp(active, thread);
}

void CClientMain::SetTileNumber( int nHorz,int nVert )
{
	//测试使用
	//m_ScreenShooter.Init(nHorz,nVert);
	//实际使用
	if (m_bTcpConect)
	{
	SetupMsg* pTileMsg = new SetupMsg;
	pTileMsg->PacketMsg(RCS_SETUP_MSG_TILE,nHorz,nVert);
	m_tcpClien.send(*pTileMsg);
	delete pTileMsg;
	}
}

void CClientMain::OnBtnAlt()
{
	m_AltCheck = !m_AltCheck;
	m_MainToolBar.GetToolBarCtrl().CheckButton(m_MainToolBar.GetItemID(3),m_AltCheck);
	if (m_AltCheck)
	{
		SendKeyboardMsg(VK_MENU,RCS_KEY_DOWN);
	}
	else
	{
		SendKeyboardMsg(VK_MENU,RCS_KEY_UP);
	}
}

void CClientMain::OnBtnCtrl()
{
	m_CtrlCheck = !m_CtrlCheck;
	m_MainToolBar.GetToolBarCtrl().CheckButton(m_MainToolBar.GetItemID(5),m_CtrlCheck);
	if (m_CtrlCheck)
	{
		SendKeyboardMsg(VK_CONTROL,RCS_KEY_DOWN);
	}
	else
	{
		SendKeyboardMsg(VK_CONTROL,RCS_KEY_UP);
	}
}

void CClientMain::OnBtnWin()
{
	m_WinCheck = !m_WinCheck;
	m_MainToolBar.GetToolBarCtrl().CheckButton(m_MainToolBar.GetItemID(4),m_WinCheck);
	if (m_WinCheck)
	{
		SendKeyboardMsg(VK_LWIN,RCS_KEY_DOWN);
	}
	else
	{
		SendKeyboardMsg(VK_LWIN,RCS_KEY_UP);
	}
}

int CClientMain::InitTcpClien(Host host,UserMsg* userMsg)
{
	//AutoLock l(&m_mLocaMutex);
	m_mLocaMutex.Lock();
	m_SendQueue.push(userMsg);
	m_mLocaMutex.Unlock();
	int logresult = m_tcpClien.Init(string(host.ip),host.port,m_SendQueue,m_RecvQueue);
	if (logresult != 0 && logresult != 1)
	{
		m_tcpClien.Close();
	}
	if (logresult == 0)
	{
		m_bCtrlPermission = FALSE;
	}
	else
	{
		m_bCtrlPermission = TRUE;
	}
	return logresult;

}

LRESULT CClientMain::OnAddIpMessage( WPARAM wParam, LPARAM lParam )
{
	if(wParam == 0)
	{
		int nIndex = m_cmbLogin.m_cmbServerList.AddString(m_cmbLogin.m_dlgAddIP.GetAddName().GetString());
		m_cmbLogin.m_cmbServerList.SetCurSel(nIndex);
	}

	return 1;
}

void CClientMain::SendKeyboardMsg( int key,int status )
{
    if (!m_inputReady || !m_bTcpConect || !m_bCtrlPermission || !m_tcpClien.IsConnected()) return;
    if (key >= 0 && key < 256) {
        if (status == RCS_KEY_UP && !m_pressedKeys[key]) return;
        m_pressedKeys[key] = status == RCS_KEY_DOWN;
    }
    DispatchKeyboard(key, status);
}

void CClientMain::DispatchKeyboard(int key, int status)
{
    if (m_localDesktop) {
        if (!m_localInput.Keyboard(key, status) && status == RCS_KEY_DOWN)
            SetWindowText(L"Remote Control Client - 请先点击支持同机键盘操作的控件，或直接操作本机窗口");
        return;
    }
    KeyboardMsg message; message.PacketMsg(key, status);
    m_tcpClien.send(message);
}

void CClientMain::FullScreen()
{
    if (m_IsFullScreen || !m_dlgDisplay.GetSafeHwnd() || IsIconic()) return;
    ReleaseRemoteInput();
    m_savedPlacement.length = sizeof(m_savedPlacement);
    GetWindowPlacement(&m_savedPlacement);
    m_savedStyle = GetWindowLongPtr(m_hWnd,GWL_STYLE);
    m_savedExStyle = GetWindowLongPtr(m_hWnd,GWL_EXSTYLE);
    MONITORINFO monitor = {sizeof(monitor)};
    if (!GetMonitorInfo(MonitorFromWindow(m_hWnd,MONITOR_DEFAULTTONEAREST),&monitor)) return;
    m_changingWindowMode = true; m_IsFullScreen = true;
    if (IsZoomed()) ShowWindow(SW_RESTORE);
    m_MainToolBar.ShowWindow(SW_HIDE);
    m_dlgSetup.ShowWindow(SW_HIDE); m_dlgServerList.SetStatus(SL_HIDE);
    SetWindowLongPtr(m_hWnd,GWL_STYLE,(m_savedStyle & ~(WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_MAXIMIZE|WS_MINIMIZE|DS_MODALFRAME)) | WS_POPUP);
    SetWindowLongPtr(m_hWnd,GWL_EXSTYLE,m_savedExStyle & ~(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
    SetWindowPos(&wndTopMost,monitor.rcMonitor.left,monitor.rcMonitor.top,
                 monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    m_changingWindowMode = false;
    LayoutDisplay(); m_ScreenDisplayer.Scale2LocalSize();
    m_fullscreenBar.Layout(true,0,true); m_dlgDisplay.FocusSurface();
}

bool CClientMain::EndFullScreen()
{
    if (!m_IsFullScreen) return true;
    ReleaseRemoteInput();
    m_changingWindowMode = true; m_IsFullScreen = false;
    SetWindowLongPtr(m_hWnd,GWL_STYLE,m_savedStyle);
    SetWindowLongPtr(m_hWnd,GWL_EXSTYLE,m_savedExStyle);
    SetWindowPos((m_savedExStyle & WS_EX_TOPMOST) ? &wndTopMost : &wndNoTopMost,0,0,0,0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    SetWindowPlacement(&m_savedPlacement);
    m_MainToolBar.ShowWindow(SW_SHOW);
    m_changingWindowMode = false;
    LayoutDisplay(); m_ScreenDisplayer.Scale2LocalSize(); m_dlgDisplay.FocusSurface();
    return true;
}

bool CClientMain::IsFullScreen() { return m_IsFullScreen; }

bool CClientMain::HandleLocalShortcut(MSG* message)
{
    const bool down = message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN;
    const bool up = message->message == WM_KEYUP || message->message == WM_SYSKEYUP;
    if (!down && !up) return false;
    if (message->wParam == VK_ESCAPE && m_suppressEscapeUp) {
        if (up) m_suppressEscapeUp = false;
        return true;
    }
    if (message->wParam == VK_RETURN && m_suppressEnterUp) {
        if (up) m_suppressEnterUp = false;
        return true;
    }
    if (down && message->wParam == VK_ESCAPE && m_IsFullScreen) {
        m_suppressEscapeUp = true; EndFullScreen(); return true;
    }
    if (down && message->wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0) {
        m_suppressEnterUp = true;
        if (!(message->lParam & (1L << 30))) OnBtnFullScreen();
        return true;
    }
    return false;
}

bool CClientMain::IsRemoteInputTarget(HWND target) const
{
    return m_dlgDisplay.GetSafeHwnd() && target == const_cast<CDisplayDlg&>(m_dlgDisplay).GetDisplayHwnd();
}

void CClientMain::DealMessage()
{
    ByteArrayMsg* message = NULL;
    const DWORD started = GetTickCount();
    // Bound both message count and elapsed decode time. Hundreds of JPEG tiles
    // in a full refresh must not delay clicks until the entire batch is decoded.
    for (int i = 0; i < 256 && m_tcpClien.PopReceived(message); ++i) {
        if (message->GetType() == RCS_MSG_IMAGE) {
            m_ScreenDisplayer.RecvImageMsg(message);
        } else if (message->GetType() == RCS_MSG_SETUP) {
            SetupMsg decoded; decoded.SetBuffer(message->GetBufferPtr(), message->GetSize());
            SetupMsg* setup = &decoded;
            if (setup->GetStatusType() == RCS_SETUP_MSG_PIXEL) {
                m_ScreenDisplayer.SetServerSize(setup->GetValue1(),setup->GetValue2());
                m_inputReady = true;
            }
            if (setup->GetStatusType() == RCS_SETUP_MSG_LOCAL_DESKTOP) {
                ReleaseRemoteInput();
                m_localDesktop = IsSameInputDesktop(m_tcpClien,
                    static_cast<DWORD>(setup->GetValue1()), static_cast<DWORD>(setup->GetValue2()));
                m_fullscreenBar.SetControlMode(m_bCtrlPermission != FALSE, m_tcpClien.IsConnected(), m_localDesktop);
            }
            if (setup->GetStatusType() == RCS_SETUP_MSG_PERMISSION &&
                setup->GetValue1() == RCS_PERMISSION_CONTROL) {
                if (setup->GetValue2() != 1) ReleaseRemoteInput();
                m_bCtrlPermission = setup->GetValue2() == 1;
                m_fullscreenBar.SetControlMode(m_bCtrlPermission != FALSE, m_tcpClien.IsConnected(), m_localDesktop);
            }
            delete message;
        } else {
            delete message;
        }
        if (GetTickCount() - started >= 8) break;
    }
    if (!IsIconic()) m_ScreenDisplayer.Display();
    if (m_bTcpConect && !m_tcpClien.IsConnected()) {
        ReleaseRemoteInput();
        m_bTcpConect = FALSE;
        m_bCtrlPermission = FALSE;
        m_fullscreenBar.SetControlMode(false, false);
        SetWindowText(L"Remote Control Client - disconnected");
    }
}

void CClientMain::OnBtnScaleLocalSize()
{
	m_ScreenDisplayer.Scale2LocalSize();
	m_dlgDisplay.FocusSurface();
}

void CClientMain::OnBtnTackMgr()
{
	SendKeyboardMsg(RCS_KEY_TASKMGR,RCS_KEY_DOWN);
}


void CClientMain::LayoutDisplay()
{
    if (m_changingWindowMode || IsIconic() || !m_dlgDisplay.GetSafeHwnd()) return;
    CRect area; GetClientRect(&area);
    if (area.Width() <= 0 || area.Height() <= 0) return;
    int toolbarHeight = 0;
    if (!m_IsFullScreen && m_MainToolBar.GetSafeHwnd()) {
        toolbarHeight = m_MainToolBar.CalcFixedLayout(FALSE,TRUE).cy;
        m_MainToolBar.SetWindowPos(NULL,0,0,area.Width(),toolbarHeight,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    m_dlgDisplay.SetSize(0,toolbarHeight,area.Width(),max(1,area.Height()-toolbarHeight));
    m_dlgDisplay.ShowWindow(SW_SHOW);
    m_fullscreenBar.Layout(m_IsFullScreen,toolbarHeight+6);
    m_ScreenDisplayer.Display();
}

void CClientMain::OnDisconnect() { EndFullScreen(); OnCancel(); }
void CClientMain::OnGetMinMaxInfo(MINMAXINFO* info)
{
    CDialog::OnGetMinMaxInfo(info);
    CClientDC dc(this); const int dpi=dc.GetDeviceCaps(LOGPIXELSX);
    info->ptMinTrackSize.x=max(info->ptMinTrackSize.x,MulDiv(330,dpi,96));
    info->ptMinTrackSize.y=max(info->ptMinTrackSize.y,MulDiv(220,dpi,96));
}
