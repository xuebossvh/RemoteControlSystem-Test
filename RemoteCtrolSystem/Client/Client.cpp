// Client.cpp : 定义应用程序的类行为。
//

#include "stdafx.h"
#include "../Common/RuntimePaths.h"
#include "Client.h"
#include "CaptureExclusion.h"
#include "../Common/RemoteInput.h"
#include "ClientDlg.h"
#include "ClientMain.h"
#include "ServerListDlg.h"
#include "..\Message\KeyboardMsg.h"
#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CClientApp

BEGIN_MESSAGE_MAP(CClientApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()


// CClientApp 构造

CClientApp::CClientApp()
{
	// TODO: 在此处添加构造代码，
	// 将所有重要的初始化放置在 InitInstance 中
}


// 唯一的一个 CClientApp 对象

CClientApp theApp;


// CClientApp 初始化

BOOL CClientApp::InitInstance()
{
	if (!InitializeApplicationDpi()) {
		MessageBoxW(NULL, L"Cannot initialize desktop pixel coordinates.", L"Remote Control Client", MB_OK | MB_ICONERROR);
		return FALSE;
	}
	CaptureExclusion::InitializeForCurrentThread();
	CaptureExclusion::ShowPendingFailure();
	if (!InitializeRuntimeDirectory(L"Client-data")) {
		AfxMessageBox(L"Cannot create or open the application data directory.");
		return FALSE;
	}

	// 如果一个运行在 Windows XP 上的应用程序清单指定要
	// 使用 ComCtl32.dll 版本 6 或更高版本来启用可视化方式，
	//则需要 InitCommonControlsEx()。否则，将无法创建窗口。
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	// 将它设置为包括所有要在应用程序中使用的
	// 公共控件类。
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();

	AfxEnableControlContainer();

	// 标准初始化
	// 如果未使用这些功能并希望减小
	// 最终可执行文件的大小，则应移除下列
	// 不需要的特定初始化例程
	// 更改用于存储设置的注册表项
	// TODO: 应适当修改该字符串，
	// 例如修改为公司或组织名
	SetRegistryKey(_T("应用程序向导生成的本地应用程序"));


	CClientMain dlg;
	m_pMainWnd = &dlg;

	INT_PTR nResponse = dlg.DoModal();
	if (nResponse == IDOK)
	{
		// TODO: 在此放置处理何时用
		//  “确定”来关闭对话框的代码
	}
	else if (nResponse == IDCANCEL)
	{
		// TODO: 在此放置处理何时用
		//  “取消”来关闭对话框的代码
	}


	// 由于对话框已关闭，所以将返回 FALSE 以便退出应用程序，
	//  而不是启动应用程序的消息泵。
	return FALSE;
}

BOOL CClientApp::OnIdle(LONG count)
{
    CaptureExclusion::ShowPendingFailure(m_pMainWnd ? m_pMainWnd->GetSafeHwnd() : NULL);
    return CWinApp::OnIdle(count);
}

BOOL CClientApp::PreTranslateMessage(MSG* message)
{
    if (CaptureExclusion::ProcessThreadMessage(*message)) return TRUE;
    const bool input = (message->message >= WM_MOUSEFIRST && message->message <= WM_MOUSELAST) ||
                       (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST);
    if (input && RemoteInput::IsCurrentMessageInjected())
        return CWinApp::PreTranslateMessage(message); // Do not echo Server input back to itself.
    CClientMain* main = DYNAMIC_DOWNCAST(CClientMain,m_pMainWnd);
    if (!main) return CWinApp::PreTranslateMessage(message);
    if (main->HandleLocalShortcut(message)) return TRUE;
    if (main->HandleRemoteInput(message)) return TRUE;
    return CWinApp::PreTranslateMessage(message);
}

int CClientApp::ExitInstance()
{
	CaptureExclusion::ShutdownForCurrentThread();
	return CWinApp::ExitInstance();
}
