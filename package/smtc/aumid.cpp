#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <Windows.h>
#include <Shobjidl.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <propkeydef.h>
#include <string>
#include <cstdio>

static const PROPERTYKEY PKEY_AppUserModel_ID = {
    { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5
};

extern "C" __declspec(dllexport) bool register_aumid(const char* launcherPath, const char* aumid)
{
    if (!launcherPath || !launcherPath[0] || !aumid || !aumid[0]) return false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        OutputDebugStringA("[aumid] CoInitializeEx failed\n");
        return false;
    }

    int launcherLen = MultiByteToWideChar(CP_UTF8, 0, launcherPath, -1, nullptr, 0);
    int aumidLen = MultiByteToWideChar(CP_UTF8, 0, aumid, -1, nullptr, 0);
    if (launcherLen <= 0 || aumidLen <= 0) {
        CoUninitialize();
        return false;
    }

    std::wstring launcherW(static_cast<size_t>(launcherLen) - 1, L'\0');
    std::wstring aumidW(static_cast<size_t>(aumidLen) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, launcherPath, -1, &launcherW[0], launcherLen);
    MultiByteToWideChar(CP_UTF8, 0, aumid, -1, &aumidW[0], aumidLen);

    PWSTR programsPath = nullptr;
    hr = SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programsPath);
    if (FAILED(hr)) {
        OutputDebugStringA("[aumid] SHGetKnownFolderPath failed\n");
        CoUninitialize();
        return false;
    }

    std::wstring shortcutPath = std::wstring(programsPath) + L"\\KuumoApp.lnk";
    CoTaskMemFree(programsPath);

    IShellLinkW* shellLink = nullptr;
    hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&shellLink);
    if (FAILED(hr) || !shellLink) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[aumid] CoCreateInstance IShellLinkW failed: 0x%08lX\n", hr);
        OutputDebugStringA(buf);
        CoUninitialize();
        return false;
    }

    shellLink->SetPath(launcherW.c_str());
    shellLink->SetDescription(L"KuumoApp Music Player");
    shellLink->SetIconLocation(launcherW.c_str(), 0);

    IPropertyStore* propStore = nullptr;
    hr = shellLink->QueryInterface(IID_IPropertyStore, (void**)&propStore);
    if (SUCCEEDED(hr) && propStore) {
        PROPVARIANT pv;
        pv.vt = VT_LPWSTR;
        pv.pwszVal = const_cast<PWSTR>(aumidW.c_str());
        propStore->SetValue(PKEY_AppUserModel_ID, pv);
        propStore->Commit();
        propStore->Release();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "[aumid] QI IPropertyStore failed: 0x%08lX\n", hr);
        OutputDebugStringA(buf);
    }

    IPersistFile* persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_IPersistFile, (void**)&persistFile);
    if (SUCCEEDED(hr) && persistFile) {
        persistFile->Save(shortcutPath.c_str(), TRUE);
        persistFile->Release();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "[aumid] QI IPersistFile failed: 0x%08lX\n", hr);
        OutputDebugStringA(buf);
        shellLink->Release();
        CoUninitialize();
        return false;
    }

    shellLink->Release();
    CoUninitialize();

    char buf[256];
    snprintf(buf, sizeof(buf), "[aumid] Registered AUMID shortcut at %ls\n", shortcutPath.c_str());
    OutputDebugStringA(buf);
    return true;
}
