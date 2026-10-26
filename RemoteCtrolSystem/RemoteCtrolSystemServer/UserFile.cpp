#include "StdAfx.h"
#include "UserFile.h"
#include "Config.h"
#include <wincrypt.h>
#include <vector>
#include <cstring>

// New data uses the current Windows user's DPAPI key. Legacy .crypt data is
// never rewritten: its historical key-size bug makes automatic migration unsafe.
CUserFile::CUserFile() : m_pszFilePath(_T("user")) { GetDataFromFile(); }
CUserFile::~CUserFile() {}
BOOL CUserFile::InitFile() { return TRUE; }

void CUserFile::GetDataFromFile()
{
    m_DataInFile.clear();
    CString path = m_pszFilePath + _T(".dpapi");
    bool encrypted = GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
    if (!encrypted) path = m_pszFilePath + _T(".plain");
    CFile file;
    if (!file.Open(path, CFile::modeRead | CFile::shareDenyWrite)) return;
    ULONGLONG size = file.GetLength();
    if (!size || size > 1024 * 1024) { file.Close(); return; }
    std::vector<BYTE> bytes(static_cast<size_t>(size));
    UINT read = file.Read(bytes.data(), static_cast<UINT>(size));
    file.Close();
    if (read != size) return;
    DATA_BLOB clear = {};
    if (encrypted) {
        DATA_BLOB input = {static_cast<DWORD>(bytes.size()), bytes.data()};
        if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &clear)) return;
        bytes.assign(clear.pbData, clear.pbData + clear.cbData);
        LocalFree(clear.pbData);
    }
    if (bytes.size() % sizeof(UserData)) return;
    for (size_t offset = 0; offset < bytes.size(); offset += sizeof(UserData)) {
        UserData user = {};
        memcpy(&user, bytes.data() + offset, sizeof(user));
        user.name[19] = user.pwd[19] = 0;
        if ((user.power == 0 || user.power == 1) && user.name[0] && user.pwd[0])
            m_DataInFile.push_back(user);
    }
}

void CUserFile::SetDataToFile()
{
    Config config;
    bool encrypted = config.GetIsEncryptFile() != FALSE;
    CString path = m_pszFilePath + (encrypted ? _T(".dpapi") : _T(".plain"));
    DWORD size = static_cast<DWORD>(m_DataInFile.size() * sizeof(UserData));
    BYTE empty = 0;
    BYTE* data = size ? reinterpret_cast<BYTE*>(m_DataInFile.data()) : &empty;
    DATA_BLOB protectedData = {};
    if (encrypted) {
        DATA_BLOB input = {size, data};
        if (!CryptProtectData(&input, L"RemoteControlSystem accounts", NULL, NULL, NULL,
                              CRYPTPROTECT_UI_FORBIDDEN, &protectedData))
            AfxThrowFileException(CFileException::accessDenied);
        data = protectedData.pbData; size = protectedData.cbData;
    }
    CString tempPath = path + _T(".tmp");
    CFile file;
    try {
        if (!file.Open(tempPath, CFile::modeCreate | CFile::modeWrite | CFile::shareExclusive))
            AfxThrowFileException(CFileException::accessDenied);
        if (size) file.Write(data, size);
        file.Close();
    } catch (...) {
        if (protectedData.pbData) LocalFree(protectedData.pbData);
        throw;
    }
    if (protectedData.pbData) LocalFree(protectedData.pbData);
    if (!MoveFileEx(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        AfxThrowFileException(CFileException::accessDenied);
    DeleteFile(m_pszFilePath + (encrypted ? _T(".plain") : _T(".dpapi")));
}

BOOL CUserFile::Add(UserData& user)
{
    if (!user.name[0] || !user.pwd[0] || (user.power != 0 && user.power != 1)) return FALSE;
    for (const auto& existing : m_DataInFile)
        if (lstrcmp(existing.name, user.name) == 0) return FALSE;
    user.name[19] = user.pwd[19] = 0;
    m_DataInFile.push_back(user); SetDataToFile(); return TRUE;
}
BOOL CUserFile::Delete(UserData& user)
{
    for (auto it = m_DataInFile.begin(); it != m_DataInFile.end(); ++it)
        if (lstrcmp(it->name, user.name) == 0) { m_DataInFile.erase(it); SetDataToFile(); return TRUE; }
    return FALSE;
}
BOOL CUserFile::Update(UserData& user)
{
    if (user.power != 0 && user.power != 1) return FALSE;
    for (auto& existing : m_DataInFile)
        if (lstrcmp(existing.name, user.name) == 0) { existing.power = user.power; SetDataToFile(); return TRUE; }
    return FALSE;
}
int CUserFile::GetUserPower(const TCHAR* name, const TCHAR* password)
{
    for (const auto& user : m_DataInFile)
        if (lstrcmp(user.name, name) == 0 && lstrcmp(user.pwd, password) == 0) return user.power;
    return -1;
}
std::vector<UserData> CUserFile::GetAllUsers() { return m_DataInFile; }
