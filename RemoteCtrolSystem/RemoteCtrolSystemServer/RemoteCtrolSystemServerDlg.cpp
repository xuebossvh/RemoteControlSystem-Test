// RemoteCtrolSystemServerDlg.cpp : 实现文件
//

#include "stdafx.h"
#include "RemoteCtrolSystemServer.h"
#include "RemoteCtrolSystemServerDlg.h"
#include "WndClassList.h"
#include "IniFile.h"
#include "Config.h"
#include <algorithm>

#define WM_SHOWTASK (WM_USER + 101)
#define WM_TAB_LAYOUT_CHANGED (WM_APP + 205)

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// CRemoteCtrolSystemServerDlg 对话框


CRemoteCtrolSystemServerDlg::CRemoteCtrolSystemServerDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CRemoteCtrolSystemServerDlg::IDD, pParent),m_bIsServerStarted(FALSE)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (iResult != NO_ERROR)
	{
		MessageBox(_T("Error at WSAStartup()\n"));
	}
}

void CRemoteCtrolSystemServerDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_TAB1, m_tabCtrl);
	DDX_Control(pDX, IDC_BTN_STARTSERVER, m_btnStartServer);
}

BEGIN_MESSAGE_MAP(CRemoteCtrolSystemServerDlg, CDialog)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	//}}AFX_MSG_MAP
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB1, &CRemoteCtrolSystemServerDlg::OnTcnSelchangeTab1)
	ON_BN_CLICKED(IDC_BTN_OK, &CRemoteCtrolSystemServerDlg::OnBnClickedBtnOk)
	ON_BN_CLICKED(IDC_BTN_APPLY, &CRemoteCtrolSystemServerDlg::OnBnClickedBtnApply)
	ON_MESSAGE(WM_SHOWTASK,OnShowTask)
	ON_BN_CLICKED(IDC_BTN_STARTSERVER, &CRemoteCtrolSystemServerDlg::OnBnClickedBtnStartserver)
	ON_WM_KEYDOWN()
	ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_MESSAGE(WM_TAB_LAYOUT_CHANGED, &CRemoteCtrolSystemServerDlg::OnTabLayoutChanged)
END_MESSAGE_MAP()


// CRemoteCtrolSystemServerDlg 消息处理程序

BOOL CRemoteCtrolSystemServerDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != NULL)
	{
		CString strAboutMenu;
		strAboutMenu.LoadString(IDS_ABOUTBOX);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// TODO: 在此添加额外的初始化代码

	TCITEM item;
	item.mask = TCIF_TEXT;
	item.pszText = L"服务";
	m_tabCtrl.InsertItem(0,&item);
	item.pszText = L"连接";
	m_tabCtrl.InsertItem(1,&item);
	item.pszText = L"账号管理";
	m_tabCtrl.InsertItem(2,&item);
	item.pszText = L"关于";
	m_tabCtrl.InsertItem(3,&item);

	//设置选项卡的内容的布局的大小
	m_tsdServer.Create(IDD_TAB_SERVER,&m_tabCtrl);
	m_dlgConnection.Create(IDD_TAB_CONNECTION,&m_tabCtrl);
	m_dlgAcount.Create(IDD_TAB_ACCOUNT,&m_tabCtrl);
	m_dlgAbout.Create(IDD_TAB_ABOUT,&m_tabCtrl);

	InitializeTabLayout();

	m_tsdServer.ShowWindow(SW_SHOW);
	m_dlgAcount.ShowWindow(SW_HIDE);
	m_dlgConnection.ShowWindow(SW_HIDE);
	m_dlgAbout.ShowWindow(SW_HIDE);

	WndClassList::m_connectDlg = &m_dlgConnection;
	//m_tsdServer.SetWindowPos(NULL,10,30,440,240,SWP_SHOWWINDOW);
	//m_tsdServer.ShowWindow(SW_SHOW);
	UpdateWindowControl();
	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

void CRemoteCtrolSystemServerDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		/*CAboutDlg dlgAbout;
		dlgAbout.DoModal();*/
	} else if(nID == SC_MINIMIZE)
	{
		CIniFile fileOp(_T("RemoteConfig.ini"));
		if (fileOp.GetProfileInt(_T("sets"),_T("showTask")) == 1)
		{
			AddToTray();
		}
		else
		{
			CDialog::OnSysCommand(nID,lParam);
		}
	}
	else
	{
		CDialog::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CRemoteCtrolSystemServerDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR CRemoteCtrolSystemServerDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

//选项卡切换处理
void CRemoteCtrolSystemServerDlg::OnTcnSelchangeTab1(NMHDR *pNMHDR, LRESULT *pResult)
{
	// TODO: 在此添加控件通知处理程序代码
    LayoutTabPages();
	switch(m_tabCtrl.GetCurSel())
	{
	case 0:
		{
			m_tsdServer.ShowWindow(SW_SHOW);
			m_dlgConnection.ShowWindow(SW_HIDE);
			m_dlgAcount.ShowWindow(SW_HIDE);
			m_dlgAbout.ShowWindow(SW_HIDE);
			break;
		}
	case 1:
		{
			m_tsdServer.ShowWindow(SW_HIDE);
			m_dlgConnection.ShowWindow(SW_SHOW);
			m_dlgAcount.ShowWindow(SW_HIDE);
			m_dlgAbout.ShowWindow(SW_HIDE);
			break;
		}
	case 2:
		{
			m_tsdServer.ShowWindow(SW_HIDE);
			m_dlgConnection.ShowWindow(SW_HIDE);
			m_dlgAcount.ShowWindow(SW_SHOW);
			m_dlgAbout.ShowWindow(SW_HIDE);
			break;
		}
	case 3:
		m_tsdServer.ShowWindow(SW_HIDE);
		m_dlgConnection.ShowWindow(SW_HIDE);
		m_dlgAcount.ShowWindow(SW_HIDE);
		m_dlgAbout.ShowWindow(SW_SHOW);
		{
			break;
		}
	default:
		break;
	}
	*pResult = 0;
}

void CRemoteCtrolSystemServerDlg::OnBnClickedBtnOk()
{
	// TODO: 在此添加控件通知处理程序代码
	// 在这里保存配置信息
	m_tsdServer.UpdateConfig();
}

void CRemoteCtrolSystemServerDlg::SetAutoRun(BOOL bAutoRun)
{
	HKEY hKey = NULL;
	//注册表路径
	CString strRegPath = _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run");
	TCHAR* lpszAppKeyName = _T("RemoteControlServer");
	//获取工程文件完整文件名
	TCHAR m_szAppPath[MAX_PATH];
	DWORD cch = GetModuleFileName(NULL,m_szAppPath,MAX_PATH);
	ASSERT(cch!=0);

	//打开注册表
	if (ERROR_SUCCESS ==RegOpenKeyEx(HKEY_CURRENT_USER,strRegPath,0,KEY_SET_VALUE,&hKey))
	{
		if (bAutoRun) //根据条件添加或删除注册表项
		{
			CString quoted; quoted.Format(L"\"%s\"", m_szAppPath);
			RegSetValueEx(hKey,lpszAppKeyName,0,REG_SZ,reinterpret_cast<const BYTE*>(quoted.GetString()), (quoted.GetLength()+1)*sizeof(wchar_t));
		}
		else
		{
			RegDeleteValue(hKey,lpszAppKeyName);
		}
	}
	if (hKey) RegCloseKey(hKey);


}

void CRemoteCtrolSystemServerDlg::OnBnClickedBtnApply()
{
	// TODO: 在此添加控件通知处理程序代码
	m_tsdServer.UpdateConfig();
	this->UpdateWindowControl();
}

LRESULT CRemoteCtrolSystemServerDlg::OnShowTask( WPARAM wParam, LPARAM lParam )
{
	if(wParam != IDR_MAINFRAME)
		return 1;
	switch(lParam)
	{
	case WM_RBUTTONUP:                                        // 右键起来时弹出菜单
		{

			LPPOINT lpoint = new tagPOINT;
			::GetCursorPos(lpoint);                    // 得到鼠标位置
			CMenu menu;
			menu.CreatePopupMenu();                    // 声明一个弹出式菜单
			menu.AppendMenu(MF_STRING, WM_DESTROY, _T("关闭"));
			menu.TrackPopupMenu(TPM_LEFTALIGN, lpoint->x ,lpoint->y, this);
			HMENU hmenu = menu.Detach();
			menu.DestroyMenu();
			delete lpoint;
		}
		break;
	case WM_LBUTTONDBLCLK:                                 // 双击左键的处理
		{
			this->ShowWindow(SW_SHOWNORMAL);         // 显示主窗口
			DeleteFromTray();
		}
		break;
	}
	return 0;
}

void CRemoteCtrolSystemServerDlg::AddToTray()
{
	NOTIFYICONDATA nid;
	nid.cbSize = (DWORD)sizeof(NOTIFYICONDATA);
	nid.hWnd = this->m_hWnd;
	nid.uID = IDR_MAINFRAME;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_SHOWTASK;
	nid.hIcon = LoadIcon(AfxGetInstanceHandle(),MAKEINTRESOURCE(IDR_MAINFRAME));
	lstrcpy(nid.szTip,_T("计算机远程控制——服务端"));
	Shell_NotifyIcon(NIM_ADD,&nid);
	ShowWindow(SW_HIDE);
}

void CRemoteCtrolSystemServerDlg::DeleteFromTray()
{
	NOTIFYICONDATA nid;
	nid.cbSize = (DWORD)sizeof(NOTIFYICONDATA);
	nid.hWnd = this->m_hWnd;
	nid.uID = IDR_MAINFRAME;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.hIcon = LoadIcon(AfxGetInstanceHandle(),MAKEINTRESOURCE(IDR_MAINFRAME));
	Shell_NotifyIcon(NIM_DELETE,&nid);
}

void CRemoteCtrolSystemServerDlg::OnBnClickedBtnStartserver()
{
    if (m_bIsServerStarted) {
        m_dlgConnection.LoginOutAllUser();
        m_threadServer.Stop();
        m_bIsServerStarted = FALSE;
        m_btnStartServer.SetWindowText(L"Start server");
    } else {
        Config config;
        const int port = config.GetConnectPort();
        if (port < 1 || port > 65535) { MessageBox(L"Port must be between 1 and 65535."); return; }
        try {
            m_threadServer.Start(servicFactory, static_cast<unsigned short>(port));
            m_bIsServerStarted = TRUE;
            m_btnStartServer.SetWindowText(L"Stop server");
            SetWindowText(L"Remote Control Server - running (operation monitor unavailable)");
        } catch (...) {
            MessageBox(L"Could not listen on this port. Check the port and network configuration.");
        }
    }
}

void CRemoteCtrolSystemServerDlg::OnDestroy()
{
    if (m_bIsServerStarted) {
        m_dlgConnection.LoginOutAllUser();
        m_threadServer.Stop();
        m_bIsServerStarted = FALSE;
    }
    WndClassList::m_connectDlg = NULL;
    DeleteFromTray();
    CDialog::OnDestroy();
}

void CRemoteCtrolSystemServerDlg::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	// TODO: 在此添加消息处理程序代码和/或调用默认值
	//AfxMessageBox(_T("aaaaaaaaaaa"));
	CDialog::OnKeyDown(nChar, nRepCnt, nFlags);
}

void CRemoteCtrolSystemServerDlg::UpdateWindowControl()
{
	Config config;
	if (config.GetIsAllowControl())
	{
		((CButton* )GetDlgItem(IDC_BTN_STARTSERVER))->EnableWindow(TRUE);
	}
	else
	{
        ((CButton* )GetDlgItem(IDC_BTN_STARTSERVER))->EnableWindow(FALSE);
	}

	//判断是否允许开机启动
	if (config.GetIsAutoRun())
	{
		SetAutoRun(TRUE);
	}
	else
	{
		SetAutoRun(FALSE);
	}

	// Account persistence is handled by CUserFile using DPAPI.
}

LRESULT CServerTabCtrl::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
    const LRESULT result = CTabCtrl::WindowProc(message, wParam, lParam);
    switch (message) {
    case WM_SIZE:
    case WM_SETFONT:
    case WM_FONTCHANGE:
    case WM_THEMECHANGED:
    case WM_STYLECHANGED:
    case 0x02E3: // WM_DPICHANGED_AFTERPARENT (not exposed by the legacy targetver.h)
        if (GetParent()) GetParent()->PostMessage(WM_TAB_LAYOUT_CHANGED);
        break;
    default:
        break;
    }
    return result;
}

void CRemoteCtrolSystemServerDlg::InitializeTabLayout()
{
    // Preserve the resource's dialog-unit layout, measured at the actual DPI.
    // Pages use their own dialog font; their initial size is the minimum space
    // needed by the controls in those templates.
    ModifyStyle(0, WS_CLIPCHILDREN);
    m_tabCtrl.ModifyStyle(0, WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    CWnd* pages[] = { &m_tsdServer, &m_dlgConnection, &m_dlgAcount, &m_dlgAbout };
    m_minimumPageSize = CSize(0, 0);
    for (CWnd* page : pages) {
        page->ModifyStyle(WS_SYSMENU, WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
        CRect bounds;
        page->GetClientRect(&bounds);
        m_minimumPageSize.cx = (std::max)(m_minimumPageSize.cx, static_cast<LONG>(bounds.Width()));
        m_minimumPageSize.cy = (std::max)(m_minimumPageSize.cy, static_cast<LONG>(bounds.Height()));
    }
    CRect client;
    GetClientRect(&client);
    m_initialClientSize = client.Size();
    m_tabCtrl.GetWindowRect(&m_initialTabRect);
    ScreenToClient(&m_initialTabRect);
    const int buttonIds[] = { IDC_BTN_STARTSERVER, IDC_BTN_APPLY };
    LONG firstButtonTop = client.bottom;
    for (int index = 0; index < 2; ++index) {
        GetDlgItem(buttonIds[index])->GetWindowRect(&m_initialButtonRects[index]);
        ScreenToClient(&m_initialButtonRects[index]);
        firstButtonTop = (std::min)(firstButtonTop, m_initialButtonRects[index].top);
    }
    CRect gap(0, 0, 0, 4);
    MapDialogRect(&gap);
    m_initialTabRect.bottom = (std::min)(m_initialTabRect.bottom, firstButtonTop - gap.bottom);
    m_layoutReady = true;
    LayoutMainControls();
    EnsureTabPageSpace();
    LayoutTabPages();
}

void CRemoteCtrolSystemServerDlg::LayoutMainControls()
{
    if (!m_layoutReady || !::IsWindow(m_tabCtrl.GetSafeHwnd())) return;
    CRect client;
    GetClientRect(&client);
    const int widthChange = client.Width() - m_initialClientSize.cx;
    const int heightChange = client.Height() - m_initialClientSize.cy;
    CRect tab = m_initialTabRect;
    tab.right += widthChange;
    tab.bottom += heightChange;
    m_tabCtrl.SetWindowPos(NULL, tab.left, tab.top,
        (std::max)(1, tab.Width()), (std::max)(1, tab.Height()), SWP_NOZORDER | SWP_NOACTIVATE);
    const int buttonIds[] = { IDC_BTN_STARTSERVER, IDC_BTN_APPLY };
    for (int index = 0; index < 2; ++index) {
        CRect button = m_initialButtonRects[index];
        button.OffsetRect(widthChange, heightChange);
        GetDlgItem(buttonIds[index])->SetWindowPos(NULL, button.left, button.top,
            button.Width(), button.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LayoutTabPages();
}

void CRemoteCtrolSystemServerDlg::EnsureTabPageSpace()
{
    if (!m_layoutReady || IsIconic()) return;
    CRect display;
    m_tabCtrl.GetClientRect(&display);
    m_tabCtrl.AdjustRect(FALSE, &display);
    const int addWidth = (std::max)(0L, m_minimumPageSize.cx - display.Width());
    const int addHeight = (std::max)(0L, m_minimumPageSize.cy - display.Height());
    CRect window;
    GetWindowRect(&window);
    m_minimumWindowSize = CSize(window.Width() + m_minimumPageSize.cx - display.Width(),
        window.Height() + m_minimumPageSize.cy - display.Height());
    if (addWidth || addHeight) {
        SetWindowPos(NULL, 0, 0, window.Width() + addWidth, window.Height() + addHeight,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void CRemoteCtrolSystemServerDlg::LayoutTabPages()
{
    if (!m_layoutReady || !::IsWindow(m_tabCtrl.GetSafeHwnd())) return;
    CRect display;
    m_tabCtrl.GetClientRect(&display);
    // AdjustRect returns the tab's display area in tab-client coordinates.
    // This is also the pages' parent coordinate system: never add screen or
    // main-dialog offsets, and never assume a fixed tab-header pixel height.
    m_tabCtrl.AdjustRect(FALSE, &display);
    if (display.Width() <= 0 || display.Height() <= 0) return;
    CWnd* pages[] = { &m_tsdServer, &m_dlgConnection, &m_dlgAcount, &m_dlgAbout };
    for (CWnd* page : pages) {
        if (::IsWindow(page->GetSafeHwnd())) page->SetWindowPos(NULL,
            display.left, display.top, display.Width(), display.Height(),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_tabCtrl.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void CRemoteCtrolSystemServerDlg::OnSize(UINT type, int width, int height)
{
    CDialog::OnSize(type, width, height);
    if (type != SIZE_MINIMIZED) LayoutMainControls();
}

void CRemoteCtrolSystemServerDlg::OnGetMinMaxInfo(MINMAXINFO* info)
{
    CDialog::OnGetMinMaxInfo(info);
    if (m_layoutReady) {
        info->ptMinTrackSize.x = (std::max)(info->ptMinTrackSize.x, m_minimumWindowSize.cx);
        info->ptMinTrackSize.y = (std::max)(info->ptMinTrackSize.y, m_minimumWindowSize.cy);
    }
}

LRESULT CRemoteCtrolSystemServerDlg::OnTabLayoutChanged(WPARAM, LPARAM)
{
    EnsureTabPageSpace();
    LayoutTabPages();
    return 0;
}
