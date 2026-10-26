// TabConnectionDlg.cpp : 实现文件
//

#include "stdafx.h"
#include "TabConnectionDlg.h"
#include "RemoteCtrolSystemServer.h"


// TabConnectionDlg 对话框

IMPLEMENT_DYNAMIC(TabConnectionDlg, CDialog)

TabConnectionDlg::TabConnectionDlg(CWnd* pParent /*=NULL*/)
	: CDialog(TabConnectionDlg::IDD, pParent)
{

}

TabConnectionDlg::~TabConnectionDlg()
{
}

void TabConnectionDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_CONCETION_LIST, m_listConnection);
}


BEGIN_MESSAGE_MAP(TabConnectionDlg, CDialog)
	ON_MESSAGE(WM_APP + 10, &TabConnectionDlg::OnConnectionsChanged)
	ON_BN_CLICKED(IDC_BTN_BROKEN, &TabConnectionDlg::OnBnClickedBtnBroken)
	ON_BN_CLICKED(IDC_BT_OPERATOR, &TabConnectionDlg::OnBnClickedBtOperator)
END_MESSAGE_MAP()


// TabConnectionDlg 消息处理程序

BOOL TabConnectionDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// TODO:  在此添加额外的初始化

	m_listConnection.SetExtendedStyle(m_listConnection.GetExtendedStyle()| LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES );
	LVCOLUMN lvcol;
	lvcol.mask = LVCF_TEXT | LVCF_FMT | LVCF_SUBITEM | LVCF_WIDTH;
	lvcol.fmt = LVCFMT_CENTER;
	lvcol.pszText = L"登录名";
	lvcol.iSubItem = 1;  //与表列相关的子表项索引号
	lvcol.cx = 120;

	m_listConnection.InsertColumn(0,&lvcol);
	lvcol.pszText = L"IP地址";
	m_listConnection.InsertColumn(1,&lvcol);
	lvcol.cx = 70;
	lvcol.pszText = L"权限";
	m_listConnection.InsertColumn(2,&lvcol);

	return TRUE;  // return TRUE unless you set the focus to a control
	// 异常: OCX 属性页应返回 FALSE
}

void TabConnectionDlg::AddConnection(ConnectionData data) { TryAddConnection(data); }

bool TabConnectionDlg::TryAddConnection(ConnectionData data)
{
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        for (const auto& existing : m_ConnectionData)
            if (existing.username == data.username) return false;
        m_ConnectionData.push_back(data);
    }
    UpdateList();
    return true;
}

void TabConnectionDlg::UpdateList()
{
    if (GetSafeHwnd()) PostMessage(WM_APP + 10);
}

LRESULT TabConnectionDlg::OnConnectionsChanged(WPARAM, LPARAM)
{
    std::vector<ConnectionData> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        snapshot = m_ConnectionData;
    }
    m_listConnection.DeleteAllItems();
    for (const auto& data : snapshot) {
        int index = m_listConnection.InsertItem(m_listConnection.GetItemCount(), data.username);
        m_listConnection.SetItemText(index, 1, data.ip);
        m_listConnection.SetItemText(index, 2, data.status == 1 ? L"Control" : L"View");
        m_listConnection.SetItemData(index, reinterpret_cast<DWORD_PTR>(data.connection));
    }
    return 0;
}

BOOL TabConnectionDlg::IsUserLogined(CString name)
{
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    for (const auto& data : m_ConnectionData)
        if (data.username == name) return TRUE;
    return FALSE;
}

int TabConnectionDlg::GetConnectionPower(TCPConnection* connection)
{
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    for (const auto& data : m_ConnectionData)
        if (data.connection == connection) return data.status;
    return -1;
}

void TabConnectionDlg::OnBnClickedBtnBroken()
{
    POSITION position = m_listConnection.GetFirstSelectedItemPosition();
    if (!position) return;
    int index = m_listConnection.GetNextSelectedItem(position);
    auto connection = reinterpret_cast<TCPConnection*>(m_listConnection.GetItemData(index));
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        for (const auto& data : m_ConnectionData)
            if (data.connection == connection) { connection->disconnect(); break; }
    }
}

void TabConnectionDlg::DeleteItemByConnection(TCPConnection* connection)
{
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        for (auto it = m_ConnectionData.begin(); it != m_ConnectionData.end(); ++it)
            if (it->connection == connection) { m_ConnectionData.erase(it); break; }
    }
    UpdateList();
}

void TabConnectionDlg::LoginOutAllUser()
{
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        for (const auto& data : m_ConnectionData) data.connection->disconnect();
        m_ConnectionData.clear();
    }
    UpdateList();
}

void TabConnectionDlg::OnBnClickedBtOperator()
{
	// TODO: 在此添加控件通知处理程序代码
	COperationDlg operation;
	POSITION pos = m_listConnection.GetFirstSelectedItemPosition();
	if (NULL == pos)
	{
		/*TRACE(_T("No item were selected!\n"));
		AfxMessageBox(_T("请选择要断开连接的行！"));*/
		operation.DoModal();
	}
	else
	{
		int nItem = m_listConnection.GetNextSelectedItem(pos);
		TCHAR szBuff[1024];
		LVITEM lvi;
		lvi.iItem = nItem;
		lvi.iSubItem = 0;
		lvi.mask = LVIF_TEXT;
		lvi.pszText = szBuff;
		lvi.cchTextMax = 1024;
		m_listConnection.GetItem(&lvi);

		TCHAR name[20];
		lstrcpy(name,lvi.pszText);
		operation.SetNameAndType(CString(name),0);
		operation.DoModal();
	}

}
