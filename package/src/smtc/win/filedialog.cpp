#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <Windows.h>
#include <shlobj.h>
#include <string>
#include <cstdio>

static char g_result[MAX_PATH] = {};

extern "C" __declspec(dllexport) const char* open_folder_dialog(const char* startingFolder)
{
    g_result[0] = '\0';

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        OutputDebugStringA("[filedialog] CoInitializeEx failed\n");
        return nullptr;
    }

    IFileOpenDialog* pDialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&pDialog);
    if (FAILED(hr) || !pDialog) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[filedialog] CoCreateInstance failed: 0x%08lX\n", hr);
        OutputDebugStringA(buf);
        return nullptr;
    }

    DWORD dwFlags = 0;
    pDialog->GetOptions(&dwFlags);
    dwFlags |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    pDialog->SetOptions(dwFlags);

    if (startingFolder && startingFolder[0]) {
        int len = MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, nullptr, 0);
        if (len > 0) {
            std::wstring wpath(static_cast<size_t>(len) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, &wpath[0], len);
            IShellItem* pItem = nullptr;
            hr = SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_IShellItem, (void**)&pItem);
            if (SUCCEEDED(hr) && pItem) {
                pDialog->SetFolder(pItem);
                pItem->Release();
            }
        }
    }

    hr = pDialog->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        hr = pDialog->GetResult(&pItem);
        if (SUCCEEDED(hr) && pItem) {
            PWSTR pszPath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
            if (SUCCEEDED(hr) && pszPath) {
                int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                if (utf8Len > 0 && utf8Len <= MAX_PATH) {
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, g_result, MAX_PATH, nullptr, nullptr);
                }
                CoTaskMemFree(pszPath);
            }
            pItem->Release();
        }
    }

    pDialog->Release();

    return g_result[0] != '\0' ? g_result : nullptr;
}
