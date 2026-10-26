#include <afxwin.h>
#include "../RemoteCtrolSystem/RemoteCtrolSystemServer/UserFile.h"
#include "../RemoteCtrolSystem/RemoteCtrolSystemServer/Config.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main()
{
    if (!AfxWinInit(GetModuleHandle(NULL), NULL, GetCommandLine(), 0)) return 2;
    wchar_t original[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
    GetCurrentDirectory(MAX_PATH, original);
    GetTempPath(MAX_PATH, temporary);
    if (!GetTempFileName(temporary, L"rcs", 0, directory)) return 2;
    DeleteFile(directory);
    if (!CreateDirectory(directory, NULL) || !SetCurrentDirectory(directory)) return 2;
    int result = 0;
    try {
        Config config;
        CFile legacy(L"user.crypt", CFile::modeCreate | CFile::modeWrite);
        const char marker[] = "legacy fixture must be preserved";
        legacy.Write(marker, sizeof(marker)); legacy.Close();
        UserData user = {};
        user.power = 0; wcscpy_s(user.name, L"testuser"); wcscpy_s(user.pwd, L"test-password");
        {
            CUserFile users;
            Check(users.GetAllUsers().empty(), "new store is not empty");
            Check(users.Add(user) != FALSE, "create DPAPI account");
            Check(users.Add(user) == FALSE, "reject duplicate account");
        }
        Check(GetFileAttributes(L"user.dpapi") != INVALID_FILE_ATTRIBUTES, "DPAPI file missing");
        Check(GetFileAttributes(L"user.plain") == INVALID_FILE_ATTRIBUTES, "plaintext left behind");
        {
            CUserFile users;
            Check(users.GetUserPower(L"testuser", L"test-password") == 0, "DPAPI reload");
            Check(users.GetUserPower(L"testuser", L"wrong") == -1, "reject incorrect password");
            config.SetIsEncryptFile(FALSE);
            user.power = 1;
            Check(users.Update(user) != FALSE, "update and switch to plaintext");
        }
        {
            CUserFile users;
            Check(users.GetUserPower(L"testuser", L"test-password") == 1, "plaintext reload");
            config.SetIsEncryptFile(TRUE);
            Check(users.Update(user) != FALSE, "switch back to DPAPI");
        }
        {
            CUserFile users;
            Check(users.GetUserPower(L"testuser", L"test-password") == 1, "second DPAPI reload");
            Check(users.Delete(user) != FALSE, "delete account");
        }
        CUserFile empty;
        Check(empty.GetAllUsers().empty(), "empty encrypted store reload");
        Check(GetFileAttributes(L"user.crypt") != INVALID_FILE_ATTRIBUTES, "legacy data was removed");
        std::cout << "PASS: DPAPI account roundtrip, permissions, format switch, legacy preservation\n";
    } catch (CException* e) {
        e->Delete(); std::cerr << "MFC account file operation failed\n"; result = 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; result = 1;
    }
    // Only remove files created in this unique test directory.
    for (const wchar_t* file : {L"RemoteConfig.ini", L"user.dpapi", L"user.plain", L"user.crypt", L"user.dpapi.tmp", L"user.plain.tmp"})
        DeleteFile(file);
    SetCurrentDirectory(original);
    RemoveDirectory(directory);
    return result;
}
