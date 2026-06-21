#include <winsock2.h>   // Must come before Windows.h
#include <ws2tcpip.h>
#include <winhttp.h>
#include <Windows.h>
#include <windowsx.h>  // For GET_X_LPARAM and GET_Y_LPARAM
#include <string>
#include <cstring>
#include <functional>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <windows.h>
#include <atomic>
#include "../shared/pending_resize_queue.h"
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <map>
#include <algorithm>
#include <stdint.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>
#include <shobjidl.h>  // For IFileOpenDialog
#include <shlobj.h>    // For SHGetKnownFolderPath, FOLDERID_Downloads
#include <shlguid.h>   // For CLSID_FileOpenDialog
#include <commdlg.h>   // For COMDLG_FILTERSPEC
#include <dcomp.h>     // For DirectComposition
#include <locale>      // For string conversion
#include <codecvt>     // For UTF-8 to wide string conversion
#include <d2d1.h>      // For Direct2D
#include <direct.h>    // For _getcwd
#include <tlhelp32.h>  // For process enumeration

// Shared cross-platform utilities
#include "../shared/glob_match.h"
#include "../shared/callbacks.h"
#include "../shared/permissions.h"
#include "../shared/mime_types.h"
#include "../shared/config.h"
#include "../shared/preload_script.h"
#include "../shared/webview_storage.h"
#include "../shared/navigation_rules.h"
#include "../shared/thread_safe_map.h"
#include "../shared/shutdown_guard.h"
#include "../shared/ffi_helpers.h"
#include "../shared/json_menu_parser.h"
#include "../shared/download_event.h"
#include "../shared/app_paths.h"
#include "../shared/accelerator_parser.h"
using namespace electrobun;

// Simple ASAR reader implementation for Windows (no external dependency)
#include <fstream>
#include <map>
#include <variant>
#include <string>
#include <sstream>
#include <algorithm>

// Minimal JSON parser for ASAR headers
struct AsarFileEntry {
    size_t offset;
    size_t size;
};

struct AsarDirEntry {
    std::map<std::string, std::variant<AsarFileEntry, AsarDirEntry>> files;
};

class AsarArchive {
public:
    std::ifstream file;
    AsarDirEntry root;
    size_t dataOffset;

    static AsarArchive* open(const std::string& path) {
        auto archive = new AsarArchive();
        archive->file.open(path, std::ios::binary);
        if (!archive->file.is_open()) {
            delete archive;
            return nullptr;
        }

        // Read header size (8 bytes, little-endian)
        uint64_t headerSize;
        archive->file.read(reinterpret_cast<char*>(&headerSize), 8);
        if (!archive->file || headerSize == 0 || headerSize > 100 * 1024 * 1024) {
            delete archive;
            return nullptr;
        }

        // Read JSON header
        std::string headerJson(headerSize, '\0');
        archive->file.read(&headerJson[0], headerSize);
        if (!archive->file) {
            delete archive;
            return nullptr;
        }

        // Parse JSON header (simple parser for ASAR format)
        if (!archive->parseHeader(headerJson)) {
            delete archive;
            return nullptr;
        }

        // Calculate data offset with 4-byte alignment padding
        size_t headerEnd = 8 + headerSize;
        size_t padding = (headerEnd % 4 == 0) ? 0 : (4 - headerEnd % 4);
        archive->dataOffset = headerEnd + padding;

        return archive;
    }

    std::vector<uint8_t> readFile(const std::string& path) {
        // Split path by '/'
        std::vector<std::string> segments;
        std::string segment;
        std::istringstream pathStream(path);
        while (std::getline(pathStream, segment, '/')) {
            if (!segment.empty()) segments.push_back(segment);
        }

        // Traverse directory structure
        std::map<std::string, std::variant<AsarFileEntry, AsarDirEntry>>* current = &root.files;
        for (size_t i = 0; i < segments.size(); i++) {
            auto it = current->find(segments[i]);
            if (it == current->end()) return {};

            if (i == segments.size() - 1) {
                // Last segment should be a file
                if (std::holds_alternative<AsarFileEntry>(it->second)) {
                    const auto& entry = std::get<AsarFileEntry>(it->second);

                    // Clear any error flags and seek to file data
                    file.clear();
                    file.seekg(dataOffset + entry.offset, std::ios::beg);

                    if (!file.good()) return {};

                    std::vector<uint8_t> buffer(entry.size);
                    file.read(reinterpret_cast<char*>(buffer.data()), entry.size);

                    if (!file.good()) return {};

                    return buffer;
                }
                return {};
            } else {
                // Intermediate segment should be a directory
                if (std::holds_alternative<AsarDirEntry>(it->second)) {
                    current = &std::get<AsarDirEntry>(it->second).files;
                } else {
                    return {};
                }
            }
        }

        return {};
    }

private:
    // Simple JSON parser specifically for ASAR header format
    bool parseHeader(const std::string& json) {
        size_t pos = json.find("\"files\"");
        if (pos == std::string::npos) return false;

        pos = json.find('{', pos);
        if (pos == std::string::npos) return false;

        return parseObject(json, pos, root.files);
    }

    bool parseObject(const std::string& json, size_t& pos, std::map<std::string, std::variant<AsarFileEntry, AsarDirEntry>>& map) {
        pos++; // skip opening {

        while (pos < json.size()) {
            // Skip whitespace
            while (pos < json.size() && std::isspace(json[pos])) pos++;

            if (pos >= json.size()) return false;
            if (json[pos] == '}') {
                pos++;
                return true;
            }
            if (json[pos] == ',') {
                pos++;
                continue;
            }

            // Parse key
            if (json[pos] != '"') return false;
            std::string key = parseString(json, pos);

            // Skip whitespace and colon
            while (pos < json.size() && (std::isspace(json[pos]) || json[pos] == ':')) pos++;

            // Parse value object
            if (json[pos] != '{') return false;
            size_t valueStart = pos;

            // Check if it's a file or directory by looking for "size" or "files"
            size_t checkPos = pos;
            int braceCount = 0;
            bool hasSize = false;
            bool hasFiles = false;

            while (checkPos < json.size()) {
                if (json[checkPos] == '{') braceCount++;
                if (json[checkPos] == '}') {
                    braceCount--;
                    if (braceCount == 0) break;
                }
                if (json.substr(checkPos, 6) == "\"size\"") hasSize = true;
                if (json.substr(checkPos, 7) == "\"files\"") hasFiles = true;
                checkPos++;
            }

            if (hasFiles) {
                // Directory
                AsarDirEntry dir;
                size_t filesPos = json.find("\"files\"", pos);
                filesPos = json.find('{', filesPos);
                if (!parseObject(json, filesPos, dir.files)) return false;
                map[key] = dir;

                // Skip to end of this object
                braceCount = 1;
                pos++;
                while (pos < json.size() && braceCount > 0) {
                    if (json[pos] == '{') braceCount++;
                    if (json[pos] == '}') braceCount--;
                    pos++;
                }
            } else if (hasSize) {
                // File
                AsarFileEntry entry;

                // Parse size
                size_t sizePos = json.find("\"size\"", pos);
                sizePos = json.find(':', sizePos) + 1;
                while (std::isspace(json[sizePos])) sizePos++;
                entry.size = std::stoul(json.substr(sizePos));

                // Parse offset
                size_t offsetPos = json.find("\"offset\"", pos);
                offsetPos = json.find('\"', offsetPos + 8) + 1;
                entry.offset = std::stoul(json.substr(offsetPos));

                map[key] = entry;

                // Skip to end of this object
                braceCount = 1;
                pos++;
                while (pos < json.size() && braceCount > 0) {
                    if (json[pos] == '{') braceCount++;
                    if (json[pos] == '}') braceCount--;
                    pos++;
                }
            }
        }

        return true;
    }

    std::string parseString(const std::string& json, size_t& pos) {
        pos++; // skip opening quote
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') {
                pos++;
                if (pos < json.size()) result += json[pos++];
            } else {
                result += json[pos++];
            }
        }
        pos++; // skip closing quote
        return result;
    }
};

// Global ASAR archive handle (lazy-loaded) with thread-safe initialization
static AsarArchive* g_asarArchive = nullptr;
static std::once_flag g_asarArchiveInitFlag;
static std::mutex g_asarReadMutex; // Mutex to protect ASAR read operations

// Export ASAR functions for launcher to use (compatible with libasar.dll API)
extern "C" __declspec(dllexport) void* asar_open(const char* path) {
    AsarArchive* archive = AsarArchive::open(std::string(path));
    return static_cast<void*>(archive);
}

extern "C" __declspec(dllexport) uint8_t* asar_read_file(void* archive, const char* path, uint64_t* size) {
    if (!archive) return nullptr;

    AsarArchive* asar = static_cast<AsarArchive*>(archive);
    std::vector<uint8_t> data = asar->readFile(std::string(path));

    if (data.empty()) {
        *size = 0;
        return nullptr;
    }

    *size = data.size();
    uint8_t* buffer = new uint8_t[data.size()];
    std::memcpy(buffer, data.data(), data.size());
    return buffer;
}

extern "C" __declspec(dllexport) void asar_free_buffer(uint8_t* buffer, uint64_t size) {
    if (buffer) {
        delete[] buffer;
    }
}

extern "C" __declspec(dllexport) void asar_close(void* archive) {
    if (archive) {
        AsarArchive* asar = static_cast<AsarArchive*>(archive);
        delete asar;
    }
}

// Push macro definitions to avoid conflicts with Windows headers
#pragma push_macro("GetNextSibling")
#pragma push_macro("GetFirstChild")
#undef GetNextSibling
#undef GetFirstChild

// Restore macro definitions
#pragma pop_macro("GetFirstChild")
#pragma pop_macro("GetNextSibling")

// Link required Windows libraries
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")


using namespace Microsoft::WRL;


// Ensure the exported functions have appropriate visibility
#define ELECTROBUN_EXPORT __declspec(dllexport)
#define WM_EXECUTE_SYNC_BLOCK (WM_USER + 1)
#define WM_EXECUTE_ASYNC_BLOCK (WM_USER + 2)
#define WM_DEVTOOLS_CREATE (WM_USER + 3)

// Forward declarations
class AbstractView;
class ContainerView;
class NSWindow;
class NSStatusItem;
class WKWebView;
class MyScriptMessageHandlerWithReply;
class StatusItemTarget;

// Type definitions to match macOS types
typedef double CGFloat;

// Function pointer type definitions are in shared/callbacks.h
// Platform-specific aliases
typedef BOOL (*HandlePostMessageWin)(uint32_t webviewId, const char* message);
typedef void (*callAsyncJavascriptCompletionHandler)(const char *messageId, uint32_t webviewId, uint32_t hostWebviewId, const char *responseJSON);
typedef SnapshotCallback zigSnapshotCallback;
typedef StatusItemHandler ZigStatusItemHandler;

// Global map to store container views by window handle
static std::map<HWND, std::unique_ptr<ContainerView>> g_containerViews;
static GetMimeType g_getMimeType = nullptr;
static GetHTMLForWebviewSync g_getHTMLForWebviewSync = nullptr;

static std::string g_electrobunChannel = "";
static std::string g_electrobunIdentifier = "";
static std::string g_electrobunName = "";

// Webview content storage (replaces JSCallback approach)
static std::map<uint32_t, std::string> webviewHTMLContent;
static std::mutex webviewHTMLMutex;

// Forward declaration for AbstractView
class AbstractView;

// Global map to track all AbstractView instances by their webviewId
static std::map<uint32_t, AbstractView*> g_abstractViews;
static std::mutex g_abstractViewsMutex;

// Forward declaration for navigation rules helper (defined after AbstractView class)
bool checkNavigationRules(AbstractView* view, const std::string& url);

// Forward declarations for HTML content management
extern "C" ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId);
extern "C" ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent);

// Global mutex to serialize webview creation
static std::mutex g_webviewCreationMutex;



// Global map to store WebView2Views by container window handle (using void* to avoid forward declaration issues)
static std::map<HWND, void*> g_webview2Views;

// Global map to store pending URLs for async browser creation
static std::map<HWND, std::string> g_pendingUrls;

// Global WebView2 instances - moved to global scope
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;

// Permission cache types and functions are in shared/permissions.h

static ComPtr<ICoreWebView2Environment> g_environment;  // Add global environment
static ComPtr<ICoreWebView2CustomSchemeRegistration> g_customScheme;
static ComPtr<ICoreWebView2EnvironmentOptions> g_envOptions;

static HMENU g_applicationMenu = NULL;
static std::unique_ptr<StatusItemTarget> g_appMenuTarget = nullptr;

// Global map to store menu item actions by menu ID
static std::map<UINT, std::string> g_menuItemActions;
static UINT g_nextMenuId = WM_USER + 1000;  // Start menu IDs from a safe range

// Accelerator table management for menu keyboard shortcuts
static std::vector<ACCEL> g_menuAccelerators;
static HACCEL g_hAccelTable = NULL;

// Global state for custom window dragging
static BOOL g_isMovingWindow = FALSE;
static HWND g_targetWindow = NULL;
static POINT g_initialCursorPos = {};
static POINT g_initialWindowPos = {};

// WebView positioning constants
static const int OFFSCREEN_OFFSET = -20000;

// Quit/shutdown coordination
static QuitRequestedHandler g_quitRequestedHandler = nullptr;
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_eventLoopStopping{false};
static DWORD g_mainThreadId = 0;

// Helper functions for string conversion
std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    
    int sizeRequired = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (sizeRequired <= 0) {
        // Fallback to simple conversion (ASCII safe)
        std::wstring result;
        result.reserve(str.length());
        for (char c : str) {
            result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        return result;
    }
    
    std::wstring wstr(sizeRequired, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], sizeRequired);
    wstr.pop_back(); // Remove null terminator
    return wstr;
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    
    int sizeRequired = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeRequired <= 0) {
        // Fallback to simple conversion (ASCII safe)
        std::string result;
        result.reserve(wstr.length());
        for (wchar_t wc : wstr) {
            if (wc <= 127) { // ASCII range
                result.push_back(static_cast<char>(wc));
            } else {
                result.push_back('?'); // Replace non-ASCII with ?
            }
        }
        return result;
    }
    
    std::string str(sizeRequired, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], sizeRequired, nullptr, nullptr);
    str.pop_back(); // Remove null terminator
    return str;
}

class StatusItemTarget {
public:
    ZigStatusItemHandler zigHandler;
    uint32_t trayId;
    
    StatusItemTarget() : zigHandler(nullptr), trayId(0) {}
};



// Forward declare helper functions
void setupViewsSchemeHandler(ICoreWebView2* webview, uint32_t webviewId);
void handleViewsSchemeRequest(ICoreWebView2WebResourceRequestedEventArgs* args, 
                             const std::wstring& uri, 
                             uint32_t webviewId);
std::string loadViewsFile(const std::string& path);
std::string getMimeTypeForFile(const std::string& path);
void updateActiveWebviewForMousePosition(ContainerView* container, POINT mousePos);

void log(const std::string& message) {
    // Get current time
    std::time_t now = std::time(0);
    std::string timeStr = std::ctime(&now);
    timeStr.pop_back(); // Remove newline character
    
    // Print to console
    std::cout << "[" << timeStr << "] " << message << std::endl;
    
    // Optionally write to file
    std::ofstream logFile("app.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << timeStr << "] " << message << std::endl;
        logFile.close();
    }
}

// Generic Bridge Handler COM Object - can be used for any bridge type
class BridgeHandler : public IDispatch {
private:
    long m_refCount;
    HandlePostMessage m_callback;
    uint32_t m_webviewId;
    std::string m_bridgeName;

public:
    BridgeHandler(const std::string& bridgeName, HandlePostMessage callback, uint32_t webviewId) 
        : m_refCount(1), m_callback(callback), m_webviewId(webviewId), m_bridgeName(bridgeName) {
        
    }

    // IUnknown implementation
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) {
            *ppvObject = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        long refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0) {
            delete this;
        }
        return refCount;
    }

    // IDispatch implementation
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override {
        *pctinfo = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames, LCID lcid, DISPID* rgDispId) override {
        if (cNames == 1 && wcscmp(rgszNames[0], L"postMessage") == 0) {
            rgDispId[0] = 1; // DISPID for postMessage method
            return S_OK;
        }
        return DISP_E_UNKNOWNNAME;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr) override {
        if (dispIdMember == 1 && (wFlags & DISPATCH_METHOD)) { // postMessage method
            if (pDispParams->cArgs == 1 && pDispParams->rgvarg[0].vt == VT_BSTR) {
                printf("[Bridge:%s] Received message for webview %u\n", m_bridgeName.c_str(), m_webviewId);
                return PostMessage(pDispParams->rgvarg[0].bstrVal);
            }
            printf("[Bridge:%s] Bad param count for webview %u\n", m_bridgeName.c_str(), m_webviewId);
            return DISP_E_BADPARAMCOUNT;
        }
        printf("[Bridge:%s] Unknown method DISPID=%ld for webview %u\n", m_bridgeName.c_str(), (long)dispIdMember, m_webviewId);
        return DISP_E_MEMBERNOTFOUND;
    }

    // Bridge-specific method for posting messages
    HRESULT PostMessage(BSTR message) {
        if (!m_callback) {
            ::log("ERROR: Bridge callback is null");
            return E_FAIL;
        }

        // Convert BSTR to char*
        int size = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
        if (size <= 0) {
            ::log("ERROR: Failed to get required buffer size for message conversion");
            return E_FAIL;
        }

        char* message_char = new char[size];
        int result = WideCharToMultiByte(CP_UTF8, 0, message, -1, message_char, size, NULL, NULL);
        if (result == 0) {
            delete[] message_char;
            ::log("ERROR: Failed to convert message to UTF-8");
            return E_FAIL;
        }

        

        // Create a copy for the callback to avoid memory issues
        char* messageCopy = new char[strlen(message_char) + 1];
        strcpy_s(messageCopy, strlen(message_char) + 1, message_char);

        // Call the callback
        try {
            m_callback(m_webviewId, messageCopy);
        } catch (...) {
            ::log("ERROR: Exception in bridge callback");
            delete[] message_char;
            delete[] messageCopy;
            return E_FAIL;
        }

        // Schedule cleanup after a delay to avoid premature deallocation
        // (similar to the original delay-based cleanup)
        std::thread([messageCopy, message_char]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            delete[] messageCopy;
            delete[] message_char;
        }).detach();

        return S_OK;
    }
};

// Dispatch IDs for the bridge methods
#define DISPID_POSTMESSAGE 1

// Dispatch interface for BunBridge
class BunBridgeDispatch : public IDispatch {
private:
    long m_refCount;
    ComPtr<BridgeHandler> m_bridgeHandler;

public:
    BunBridgeDispatch(ComPtr<BridgeHandler> bridgeHandler) 
        : m_refCount(1), m_bridgeHandler(bridgeHandler) {}

    // IUnknown implementation
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) {
            *ppvObject = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        long refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0) {
            delete this;
        }
        return refCount;
    }

    // IDispatch implementation
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override {
        *pctinfo = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames, LCID lcid, DISPID* rgDispId) override {
        if (cNames != 1) return E_INVALIDARG;
        
        std::wstring name(rgszNames[0]);
        if (name == L"postMessage") {
            rgDispId[0] = DISPID_POSTMESSAGE;
            return S_OK;
        }
        
        return DISP_E_UNKNOWNNAME;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, 
                                   DISPPARAMS* pDispParams, VARIANT* pVarResult, 
                                   EXCEPINFO* pExcepInfo, UINT* puArgErr) override {
        if (dispIdMember == DISPID_POSTMESSAGE) {
            if (pDispParams->cArgs != 1) {
                return DISP_E_BADPARAMCOUNT;
            }
            
            VARIANT* arg = &pDispParams->rgvarg[0];
            if (arg->vt != VT_BSTR) {
                return DISP_E_TYPEMISMATCH;
            }
            
            return m_bridgeHandler->PostMessage(arg->bstrVal);
        }
        
        return DISP_E_MEMBERNOTFOUND;
    }
};

// Dispatch interface for InternalBridge (same implementation, different name for clarity)
class InternalBridgeDispatch : public IDispatch {
private:
    long m_refCount;
    ComPtr<BridgeHandler> m_bridgeHandler;

public:
    InternalBridgeDispatch(ComPtr<BridgeHandler> bridgeHandler) 
        : m_refCount(1), m_bridgeHandler(bridgeHandler) {}

    // IUnknown implementation
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) {
            *ppvObject = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        long refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0) {
            delete this;
        }
        return refCount;
    }

    // IDispatch implementation (identical to BunBridgeDispatch)
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override {
        *pctinfo = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames, LCID lcid, DISPID* rgDispId) override {
        if (cNames != 1) return E_INVALIDARG;
        
        std::wstring name(rgszNames[0]);
        if (name == L"postMessage") {
            rgDispId[0] = DISPID_POSTMESSAGE;
            return S_OK;
        }
        
        return DISP_E_UNKNOWNNAME;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, 
                                   DISPPARAMS* pDispParams, VARIANT* pVarResult, 
                                   EXCEPINFO* pExcepInfo, UINT* puArgErr) override {
        if (dispIdMember == DISPID_POSTMESSAGE) {
            if (pDispParams->cArgs != 1) {
                return DISP_E_BADPARAMCOUNT;
            }
            
            VARIANT* arg = &pDispParams->rgvarg[0];
            if (arg->vt != VT_BSTR) {
                return DISP_E_TYPEMISMATCH;
            }
            
            return m_bridgeHandler->PostMessage(arg->bstrVal);
        }
        
        return DISP_E_MEMBERNOTFOUND;
    }
};





class MainThreadDispatcher {
private:
    static HWND g_messageWindow;

public:
    static void initialize(HWND hwnd) {
        g_messageWindow = hwnd;
    }
    
    template<typename Func>
    static auto dispatch_sync(Func&& func) -> decltype(func()) {
        using ReturnType = decltype(func());
        
        if constexpr (std::is_void_v<ReturnType>) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            
            auto task = new std::function<void()>([func = std::forward<Func>(func), promise]() {
                try {
                    func();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            
            PostMessage(g_messageWindow, WM_EXECUTE_SYNC_BLOCK, 0, (LPARAM)task);
            future.get(); // Will re-throw any exceptions
        } else {
            auto promise = std::make_shared<std::promise<ReturnType>>();
            auto future = promise->get_future();
            
            auto task = new std::function<void()>([func = std::forward<Func>(func), promise]() {
                try {
                    promise->set_value(func());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
            
            PostMessage(g_messageWindow, WM_EXECUTE_SYNC_BLOCK, 0, (LPARAM)task);
            return future.get();
        }
    }
    
    static void handleSyncTask(LPARAM lParam) {
        auto task = (std::function<void()>*)lParam;
        (*task)();
        delete task;
    }
    
    template<typename Func>
    static void dispatch_async(Func&& func) {
        auto task = new std::function<void()>(std::forward<Func>(func));
        PostMessage(g_messageWindow, WM_EXECUTE_ASYNC_BLOCK, 0, (LPARAM)task);
    }
};

HWND MainThreadDispatcher::g_messageWindow = NULL;

// AbstractView base class - Windows implementation matching Mac pattern
class AbstractView {
public:
    uint32_t webviewId;
    HWND hwnd = NULL;
    bool isMousePassthroughEnabled = false;
    bool mirrorModeEnabled = false;
    bool fullSize = false;
    bool pendingStartTransparent = false;
    bool pendingStartPassthrough = false;

    // Common state
    bool isReceivingInput = true;
    std::string maskJSON;
    RECT visualBounds = {};
    bool creationFailed = false;

    // Pending resize state (cross-thread)
    std::mutex pendingResizeMutex;
    std::atomic<uint64_t> pendingResizeGeneration{0};
    uint64_t appliedResizeGeneration = 0;
    bool hasPendingResize = false;
    RECT pendingResizeFrame = {};
    std::string pendingResizeMasks;

    // Navigation rules for URL filtering
    std::vector<std::string> navigationRules;

    // Bridge handlers
    ComPtr<BridgeHandler> eventBridgeHandler;  // Event-only bridge (always available)
    ComPtr<BridgeHandler> bunBridgeHandler;
    ComPtr<BridgeHandler> internalBridgeHandler;
    ComPtr<BunBridgeDispatch> bunBridgeDispatch;
    ComPtr<InternalBridgeDispatch> internalBridgeDispatch;

    virtual ~AbstractView() = default;
    
    // Pure virtual methods - must be implemented by subclasses
    virtual void loadURL(const char* urlString) = 0;
    virtual void loadHTML(const char* htmlString) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;
    virtual void remove() = 0;
    virtual bool canGoBack() = 0;
    virtual bool canGoForward() = 0;
    virtual void evaluateJavaScriptWithNoCompletion(const char* jsString) = 0;
    virtual void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) = 0;
    virtual void addPreloadScriptToWebView(const char* jsString) = 0;
    virtual void updateCustomPreloadScript(const char* jsString) = 0;
    virtual void resize(const RECT& frame, const char* masksJson) = 0;
    
    // Common implementations
    virtual void setTransparent(bool transparent) {
        // Default implementation - can be overridden
    }
    
    virtual void setPassthrough(bool enable) {
        isMousePassthroughEnabled = enable;
    }
    
    virtual void setHidden(bool hidden) {
        if (hwnd) {
            ShowWindow(hwnd, hidden ? SW_HIDE : SW_SHOW);
        }
    }

    // Set navigation rules from JSON array string
    void setNavigationRulesFromJSON(const char* rulesJson) {
        navigationRules.clear();
        if (!rulesJson || strlen(rulesJson) == 0) {
            return;
        }

        // Simple JSON array parser for string arrays: ["rule1", "rule2", ...]
        std::string json(rulesJson);
        size_t pos = json.find('[');
        if (pos == std::string::npos) return;

        pos++;
        while (pos < json.length()) {
            // Find start of string
            size_t strStart = json.find('"', pos);
            if (strStart == std::string::npos) break;

            // Find end of string (handle escaped quotes)
            size_t strEnd = strStart + 1;
            while (strEnd < json.length()) {
                if (json[strEnd] == '"' && json[strEnd - 1] != '\\') break;
                strEnd++;
            }
            if (strEnd >= json.length()) break;

            // Extract string value
            std::string rule = json.substr(strStart + 1, strEnd - strStart - 1);
            navigationRules.push_back(rule);

            pos = strEnd + 1;
        }
    }

    // Check if URL should be allowed based on navigation rules
    bool shouldAllowNavigationToURL(const std::string& url) {
        if (navigationRules.empty()) {
            return true; // Default allow if no rules
        }

        bool allowed = true; // Default allow if no rules match

        for (const std::string& rule : navigationRules) {
            bool isBlockRule = !rule.empty() && rule[0] == '^';
            std::string pattern = isBlockRule ? rule.substr(1) : rule;

            if (electrobun::globMatch(pattern, url)) {
                allowed = !isBlockRule; // Last match wins
            }
        }

        return allowed;
    }

    virtual void setCreationFailed(bool failed) {
        creationFailed = failed;
    }
    
    virtual bool hasCreationFailed() const {
        return creationFailed;
    }
    
    // Check if point is in a masked (cut-out) area based on maskJSON
    bool isPointInMask(POINT localPoint) {
        if (maskJSON.empty()) {
            return false;
        }
        
        // Simple JSON parsing for mask rectangles
        // Expected format: [{"x":10,"y":20,"width":100,"height":50},...]
        size_t pos = 0;
        while ((pos = maskJSON.find("\"x\":", pos)) != std::string::npos) {
            try {
                // Extract x, y, width, height from JSON
                size_t xStart = maskJSON.find(":", pos) + 1;
                size_t xEnd = maskJSON.find(",", xStart);
                int x = std::stoi(maskJSON.substr(xStart, xEnd - xStart));
                
                size_t yPos = maskJSON.find("\"y\":", pos);
                size_t yStart = maskJSON.find(":", yPos) + 1;
                size_t yEnd = maskJSON.find(",", yStart);
                int y = std::stoi(maskJSON.substr(yStart, yEnd - yStart));
                
                size_t wPos = maskJSON.find("\"width\":", pos);
                size_t wStart = maskJSON.find(":", wPos) + 1;
                size_t wEnd = maskJSON.find(",", wStart);
                if (wEnd == std::string::npos) wEnd = maskJSON.find("}", wStart);
                int width = std::stoi(maskJSON.substr(wStart, wEnd - wStart));
                
                size_t hPos = maskJSON.find("\"height\":", pos);
                size_t hStart = maskJSON.find(":", hPos) + 1;
                size_t hEnd = maskJSON.find("}", hStart);
                int height = std::stoi(maskJSON.substr(hStart, hEnd - hStart));
                
                // Check if point is within this mask rectangle
                if (localPoint.x >= x && localPoint.x < x + width &&
                    localPoint.y >= y && localPoint.y < y + height) {
                    return true;  // Point is in a masked area
                }
                
                pos = hEnd;
            } catch (...) {
                // JSON parsing error, skip this mask
                pos++;
            }
        }
        
        return false;  // Point is not in any masked area
    }
    
    // Virtual methods for subclass-specific functionality
    virtual void applyVisualMask() = 0;
    virtual void removeMasks() = 0;
    virtual void toggleMirrorMode(bool enable) = 0;

    // Find in page methods
    virtual void findInPage(const char* searchText, bool forward, bool matchCase) = 0;
    virtual void stopFindInPage() = 0;

    // Developer tools methods
    virtual void openDevTools() = 0;
    virtual void closeDevTools() = 0;
    virtual void toggleDevTools() = 0;

    void storePendingResize(const RECT& frame, const char* masksJson) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        pendingResizeFrame = frame;
        pendingResizeMasks = masksJson ? masksJson : "";
        hasPendingResize = true;
        pendingResizeGeneration++;
    }

    bool consumePendingResize(RECT& outFrame, std::string& outMasks) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        if (!hasPendingResize) return false;
        uint64_t gen = pendingResizeGeneration.load();
        if (gen == appliedResizeGeneration) return false;
        outFrame = pendingResizeFrame;
        outMasks = pendingResizeMasks;
        appliedResizeGeneration = gen;
        hasPendingResize = false;
        return true;
    }
};

// Pending resize queue (cross-thread)
static PendingResizeQueue g_pendingResizeQueue;
static std::atomic<bool> g_pendingResizeScheduled{false};

static void drainPendingResizes() {
    g_pendingResizeScheduled.store(false);
    auto items = g_pendingResizeQueue.drain();
    for (void* item : items) {
        AbstractView* view = static_cast<AbstractView*>(item);
        if (!view) continue;
        RECT frame = {};
        std::string masks;
        if (view->consumePendingResize(frame, masks)) {
            view->resize(frame, masks.c_str());
        }
    }
}

static void schedulePendingResizeDrain() {
    if (g_pendingResizeScheduled.exchange(true)) return;
    MainThreadDispatcher::dispatch_async([]() {
        drainPendingResizes();
    });
}

// Helper function to check navigation rules
// This is defined here (after AbstractView) so it can call methods on AbstractView
bool checkNavigationRules(AbstractView* view, const std::string& url) {
    if (!view) {
        return true; // Allow navigation if no view
    }
    return view->shouldAllowNavigationToURL(url);
}

// WebView2View class - implements AbstractView for WebView2
class WebView2View : public AbstractView {
private:
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2CompositionController> compositionController;
    ComPtr<ICoreWebView2> webview;
    HandlePostMessage eventBridgeCallbackHandler;
    HandlePostMessage bunBridgeCallbackHandler;
    HandlePostMessage internalBridgeCallbackHandler;
    bool isSandboxed;
    HWND containerHwnd = nullptr;  // Container window for masking

public:
    std::string pendingUrl;
    std::string pendingHtml;  // Stored inline HTML for loading after async creation
    std::string electrobunScript;
    std::string customScript;
    bool isCreationComplete = false;
    WebviewEventHandler webviewEventHandler = nullptr;

    // Static debounce timestamp for ctrl+click handling
    static double lastCtrlClickTime;

    WebView2View(uint32_t webviewId, HandlePostMessage eventBridgeHandler, HandlePostMessage bunBridgeHandler, HandlePostMessage internalBridgeHandler, bool sandbox)
        : eventBridgeCallbackHandler(eventBridgeHandler), bunBridgeCallbackHandler(bunBridgeHandler), internalBridgeCallbackHandler(internalBridgeHandler), isSandboxed(sandbox) {
        this->webviewId = webviewId;
    }
    
    // Setter methods for COM objects (called from async creation callbacks)
    void setController(ComPtr<ICoreWebView2Controller> ctrl) {
        controller = ctrl;
    }
    
    void setCompositionController(ComPtr<ICoreWebView2CompositionController> compCtrl) {
        compositionController = compCtrl;
    }
    
    void setWebView(ComPtr<ICoreWebView2> wv) {
        webview = wv;
    }

    void setContainerHwnd(HWND hwnd) {
        containerHwnd = hwnd;
    }

    ComPtr<ICoreWebView2> getWebView() const {
        return webview;
    }

    void setCreationComplete(bool complete) {
        isCreationComplete = complete;
    }
    
    bool isReady() const {
        return isCreationComplete && !creationFailed;
    }
    
    // Set up the JavaScript bridge objects in the WebView2 context using hostObjects
    void setupJavaScriptBridges() {
        if (!webview) return;

        // eventBridge - event-only bridge (always set up for all webviews, including sandboxed)
        eventBridgeHandler = ComPtr<BridgeHandler>(new BridgeHandler("eventBridge", eventBridgeCallbackHandler, webviewId));
        VARIANT eventBridgeVariant = {};
        VariantInit(&eventBridgeVariant);
        eventBridgeVariant.vt = VT_DISPATCH;
        eventBridgeVariant.pdispVal = static_cast<IDispatch*>(eventBridgeHandler.Get());
        webview->AddHostObjectToScript(L"eventBridge", &eventBridgeVariant);
        VariantClear(&eventBridgeVariant);

        // bunBridge and internalBridge - RPC bridges (only for non-sandboxed webviews)
        if (!isSandboxed) {
            // Create COM objects for the bridge handlers
            bunBridgeHandler = ComPtr<BridgeHandler>(new BridgeHandler("bunBridge", bunBridgeCallbackHandler, webviewId));
            internalBridgeHandler = ComPtr<BridgeHandler>(new BridgeHandler("internalBridge", internalBridgeCallbackHandler, webviewId));

            // Convert COM objects to VARIANT for AddHostObjectToScript
            VARIANT bunBridgeVariant = {};
            VariantInit(&bunBridgeVariant);
            bunBridgeVariant.vt = VT_DISPATCH;
            bunBridgeVariant.pdispVal = static_cast<IDispatch*>(bunBridgeHandler.Get());

            VARIANT internalBridgeVariant = {};
            VariantInit(&internalBridgeVariant);
            internalBridgeVariant.vt = VT_DISPATCH;
            internalBridgeVariant.pdispVal = static_cast<IDispatch*>(internalBridgeHandler.Get());

            // Add the bridge objects to hostObjects
            webview->AddHostObjectToScript(L"bunBridge", &bunBridgeVariant);
            webview->AddHostObjectToScript(L"internalBridge", &internalBridgeVariant);

            // Clean up VARIANTs
            VariantClear(&bunBridgeVariant);
            VariantClear(&internalBridgeVariant);
        }
    }
    
    void loadURL(const char* urlString) override {
        if (!urlString) return;
        std::string urlStr(urlString);

        // Fire will-navigate event on the calling thread (before Navigate)
        bool isViewsUrl = (urlStr.substr(0, 8) == "views://");
        if (webviewEventHandler) {
            std::string escapedUrl;
            for (char c : urlStr) {
                switch (c) {
                    case '"': escapedUrl += "\\\""; break;
                    case '\\': escapedUrl += "\\\\"; break;
                    default: escapedUrl += c; break;
                }
            }
            std::string willNavEventData = "{\"url\":\"" + escapedUrl + "\",\"allowed\":true}";
            webviewEventHandler(webviewId, _strdup("will-navigate"), _strdup(willNavEventData.c_str()));
        }

        // Navigate must happen on the UI thread — WebView2 APIs are single-threaded
        WebviewEventHandler handler = webviewEventHandler;
        uint32_t wvId = webviewId;
        MainThreadDispatcher::dispatch_async([this, urlStr, isViewsUrl, handler, wvId]() {
            if (webview) {
                std::wstring url(urlStr.begin(), urlStr.end());
                webview->Navigate(url.c_str());

                // Fire did-navigate after Navigate() for views:// URLs only
                // For https:// URLs, NavigationCompleted will fire did-navigate
                if (isViewsUrl && handler) {
                    std::string escapedUrl;
                    for (char c : urlStr) {
                        switch (c) {
                            case '"': escapedUrl += "\\\""; break;
                            case '\\': escapedUrl += "\\\\"; break;
                            default: escapedUrl += c; break;
                        }
                    }
                    std::string didNavEventData = "{\"url\":\"" + escapedUrl + "\"}";
                    handler(wvId, _strdup("did-navigate"), _strdup(didNavEventData.c_str()));
                }
            } else {
                // WebView2 not ready — store URL for creation callback to load
                pendingUrl = urlStr;
            }
        });
    }
    
    void loadHTML(const char* htmlString) override {
        if (!htmlString) return;
        std::string htmlCopy(htmlString);
        // Dispatch to main thread to avoid race with async WebView2 creation callback.
        // Both this and the creation callback run on the main thread, so they can't interleave.
        MainThreadDispatcher::dispatch_async([this, htmlCopy]() {
            if (webview) {
                std::wstring html(htmlCopy.begin(), htmlCopy.end());
                webview->NavigateToString(html.c_str());
            } else {
                // WebView2 not ready — creation callback will load this
                pendingHtml = htmlCopy;
            }
        });
    }
    
    void goBack() override {
        if (webview) {
            webview->GoBack();
        }
    }
    
    void goForward() override {
        if (webview) {
            webview->GoForward();
        }
    }
    
    void reload() override {
        if (webview) {
            webview->Reload();
        }
    }
    
    void remove() override {
        if (controller) {
            controller->Close();
            controller = nullptr;
        }
        webview = nullptr;
    }

    // Override transparency implementation for WebView2
    void setTransparent(bool transparent) override {
        if (!controller) {
            return;
        }

        // Use WebView2's built-in visibility control
        controller->put_IsVisible(transparent ? FALSE : TRUE);
    }

    // Override passthrough implementation for WebView2
    void setPassthrough(bool enable) override {
        AbstractView::setPassthrough(enable); // Call base implementation to set the flag

        if (!controller || !containerHwnd) {
            return;
        }

        // Get the bounds of this WebView to identify its child windows
        RECT viewBounds;
        controller->get_Bounds(&viewBounds);

        // Find WebView2's Chrome child windows and apply/remove WS_EX_TRANSPARENT
        struct PassthroughEnumData {
            RECT targetBounds;
            HWND containerHwnd;
            bool enablePassthrough;
        };

        PassthroughEnumData enumData;
        enumData.targetBounds = viewBounds;
        enumData.containerHwnd = containerHwnd;
        enumData.enablePassthrough = enable;

        EnumChildWindows(containerHwnd, [](HWND child, LPARAM lParam) -> BOOL {
            PassthroughEnumData* data = (PassthroughEnumData*)lParam;

            char className[256];
            GetClassNameA(child, className, sizeof(className));

            // Look for WebView2/Chrome child windows
            if (strstr(className, "Chrome_WidgetWin") ||
                strstr(className, "Chrome_RenderWidgetHostHWND")) {

                RECT childRect;
                GetWindowRect(child, &childRect);

                // Convert to container coordinates
                POINT topLeft = {childRect.left, childRect.top};
                ScreenToClient(data->containerHwnd, &topLeft);

                // Check if this matches our WebView's bounds (with some tolerance)
                if (abs(topLeft.x - data->targetBounds.left) < 5 &&
                    abs(topLeft.y - data->targetBounds.top) < 5) {
                    // This is our WebView's child window - apply WS_EX_TRANSPARENT
                    LONG exStyle = GetWindowLong(child, GWL_EXSTYLE);
                    if (data->enablePassthrough) {
                        SetWindowLong(child, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
                    } else {
                        SetWindowLong(child, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
                    }
                    return FALSE; // Stop enumeration
                }
            }
            return TRUE; // Continue enumeration
        }, (LPARAM)&enumData);
    }

    bool canGoBack() override {
        if (webview) {
            BOOL canGoBack = FALSE;
            webview->get_CanGoBack(&canGoBack);
            return canGoBack;
        }
        return false;
    }
    
    bool canGoForward() override {
        if (webview) {
            BOOL canGoForward = FALSE;
            webview->get_CanGoForward(&canGoForward);
            return canGoForward;
        }
        return false;
    }
    
    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        if (webview) {
            // Copy string to avoid lifetime issues in lambda
            std::string jsStringCopy = jsString;
            MainThreadDispatcher::dispatch_sync([this, jsStringCopy]() {
                std::wstring js = std::wstring(jsStringCopy.begin(), jsStringCopy.end());
                webview->ExecuteScript(js.c_str(), nullptr);
            });
        }
    }
    
    void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) override {
        if (webview) {
            std::wstring js = std::wstring(jsString, jsString + strlen(jsString));
            webview->ExecuteScript(js.c_str(), (ICoreWebView2ExecuteScriptCompletedHandler*)completionHandler);
        }
    }
    
    void addPreloadScriptToWebView(const char* jsString) override {
        if (webview && jsString) {
            std::wstring js = std::wstring(jsString, jsString + strlen(jsString));
            webview->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);
            std::cout << "[WebView2] Added preload script to execute on document created (length: " << strlen(jsString) << ")" << std::endl;
        }
    }
    
    void updateCustomPreloadScript(const char* jsString) override {
        if (!jsString || !webview) return;

        std::string scriptContent;

        // Check if this is a views:// URL for a script file
        if (strncmp(jsString, "views://", 8) == 0) {
            // Remove "views://" prefix and load the file
            scriptContent = loadViewsFile(std::string(jsString + 8));
            if (scriptContent.empty()) {
                std::cout << "[WebView2] Could not read preload script from: " << jsString << std::endl;
                return;
            }
        } else {
            // Inline JavaScript
            scriptContent = jsString;
        }

        // Convert to wide string and execute
        std::wstring wScript(scriptContent.begin(), scriptContent.end());

        // Add as a script to execute on document creation for future navigations
        webview->AddScriptToExecuteOnDocumentCreated(wScript.c_str(), nullptr);

        // Also execute immediately if the page is already loaded
        webview->ExecuteScript(wScript.c_str(), nullptr);
    }

    void resize(const RECT& frame, const char* masksJson) override {
        
        if (controller) {
            // WebView2 operations must be called from main thread to avoid TYPE_E_BADVARTYPE
            MainThreadDispatcher::dispatch_async([this, frame]() {
                HRESULT result = controller->put_Bounds(frame);
                if (FAILED(result)) {
                    char errorLog[256];
                    sprintf_s(errorLog, "[WebView2] put_Bounds failed for webview %u, HRESULT: 0x%08X", webviewId, result);
                    ::log(errorLog);
                }
            });
            
            visualBounds = frame;
            bool maskChanged = false;
            // Check if masksJson is nullptr, empty, or just "[]" (empty array)
            if (masksJson && strlen(masksJson) > 0 && strcmp(masksJson, "[]") != 0) {
                std::string newMaskJSON = masksJson;
                if (newMaskJSON != maskJSON) {
                    maskJSON = newMaskJSON;
                    maskChanged = true;
                }
            } else if (!maskJSON.empty()) {
                maskJSON = "";
                maskChanged = true;
            }

            // Only apply visual mask if mask data changed
            if (maskChanged) {
                applyVisualMask();
            }
        } else {
            ::log("[WebView2] ERROR: Controller is NULL, cannot resize");
        }
    }

    ComPtr<ICoreWebView2Controller> getController() {
        return controller;
    }
    
    ComPtr<ICoreWebView2> getWebView() {
        return webview;
    }
    
    // WebView2-specific implementation of mask functionality
    void applyVisualMask() override {
        // NOTE: WebView2 visual masking is not supported.
        //
        // WebView2 uses GPU-accelerated Direct3D rendering through an "Intermediate D3D Window"
        // which does not respect traditional GDI window regions (SetWindowRgn). The rendering
        // pipeline bypasses the Windows compositor in a way that makes hole-cutting impossible
        // with standard Win32 APIs.
        //
        // Approaches that were investigated and failed:
        // 1. SetWindowRgn on Chrome_WidgetWin_0 - Ignored by GPU rendering
        // 2. SetWindowRgn on Intermediate D3D Window - Ignored by D3D surface
        // 3. SetWindowRgn on shared container - Affects all webviews, not just the target
        //
        //
        // The maskJSON value is still stored (in AbstractView::maskJSON) for potential future
        // use if WebView2 adds an API for visual clipping.
    }

    void removeMasks() override {
        // No-op for WebView2 - see applyVisualMask() for explanation
    }
    
    void toggleMirrorMode(bool enable) override {
        if (!controller) return;

        if (enable && !mirrorModeEnabled) {
            mirrorModeEnabled = true;
            // Disable input for WebView2
        } else if (!enable && mirrorModeEnabled) {
            mirrorModeEnabled = false;
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }

    void findInPage(const char* searchText, bool forward, bool matchCase) override {
        if (!webview) return;

        if (!searchText || strlen(searchText) == 0) {
            stopFindInPage();
            return;
        }

        // WebView2 doesn't have a native Find API in older versions
        // Use JavaScript window.find() instead
        std::string text(searchText);
        // Escape special characters for JavaScript string
        std::string escaped;
        for (char c : text) {
            if (c == '\\') escaped += "\\\\";
            else if (c == '\'') escaped += "\\'";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else escaped += c;
        }

        // window.find(string, caseSensitive, backwards, wrapAround)
        std::string js = "window.find('" + escaped + "', " +
            (matchCase ? "true" : "false") + ", " +
            (forward ? "false" : "true") + ", true, false, false, false)";

        std::wstring wjs(js.begin(), js.end());
        webview->ExecuteScript(wjs.c_str(), nullptr);
    }

    void stopFindInPage() override {
        if (!webview) return;

        // Clear selection to remove find highlighting
        webview->ExecuteScript(L"window.getSelection().removeAllRanges();", nullptr);
    }

    void openDevTools() override {
        if (!webview) return;
        webview->OpenDevToolsWindow();
    }

    void closeDevTools() override {
        if (!webview) return;
        // WebView2 doesn't expose a CloseDevToolsWindow API.
        // The DevTools window is user-managed; opening it again is a no-op if already open.
    }

    void toggleDevTools() override {
        if (!webview) return;
        // WebView2 handles toggle behavior internally - opening when already open is a no-op
        webview->OpenDevToolsWindow();
    }
};

// Initialize static debounce timestamp for ctrl+click handling
double WebView2View::lastCtrlClickTime = 0;

// Helper function to set webview on WebView2View (defined after WebView2View class)
void SetWebViewOnWebView2View(HWND containerWindow, void* webview) {
    std::cout << "[WebView2] Looking for WebView2View with containerWindow: " << containerWindow << std::endl;
    auto viewIt = g_webview2Views.find(containerWindow);
    if (viewIt != g_webview2Views.end()) {
        auto view = static_cast<WebView2View*>(viewIt->second);
        if (view) {
            // WebView2 is already set in the controller creation callback
            std::cout << "[WebView2] Found WebView2View for webview ID: " << view->webviewId << std::endl;
        } else {
            std::cout << "[WebView2] Found WebView2View entry but view is null" << std::endl;
        }
    } else {
        std::cout << "[WebView2] No WebView2View found for containerWindow: " << containerWindow << std::endl;
    }
}

// ContainerView class definition
class ContainerView {
private:
    HWND m_hwnd;
    HWND m_parentWindow;
    std::vector<std::shared_ptr<AbstractView>> m_abstractViews;
    
    // Input management
    AbstractView* m_activeWebView = nullptr;  // Currently active webview for input
    
    // Window procedure for the container
    static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ContainerView* container = nullptr;
        
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            container = (ContainerView*)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)container);
        } else {
            container = (ContainerView*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }
        
        if (container) {
            return container->HandleMessage(msg, wParam, lParam);
        }
        
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_SIZE: {
                // // Resize all full-size webviews when container resizes
                // int width = LOWORD(lParam);
                // int height = HIWORD(lParam);
                
                // for (auto& view : m_abstractViews) {
                //     if (view->fullSize) {
                //         // Resize the webview to match container
                //         if (view->controller) {
                //             RECT bounds = {0, 0, width, height};
                //             view->controller->put_Bounds(bounds);
                //         }
                //     }
                // }
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                
                ResizeAutoSizingViews(width, height);
                
                break;
            }
            
            case WM_MOUSEMOVE: {
                POINT mousePos = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                UpdateActiveWebviewForMousePosition(mousePos);
                break;
            }
            
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hwnd, &ps);
                // Don't draw anything - let child windows handle their own painting
                EndPaint(m_hwnd, &ps);
                return 0;
            }
        }
        
        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
    
    void UpdateActiveWebviewForMousePosition(POINT mousePos) {
        AbstractView* newActiveView = nullptr;
        
        // Iterate through webviews in reverse order (top-most first)
        for (auto it = m_abstractViews.rbegin(); it != m_abstractViews.rend(); ++it) {
            auto& view = *it;
            
            if (view->isMousePassthroughEnabled) {
                // Skip passthrough webviews
                view->toggleMirrorMode(true);
                continue;
            }
            
            if (!newActiveView) {
                // Check if mouse is over this webview's bounds
                RECT viewBounds = view->visualBounds;
                
                // For WebView2, try to get actual bounds
                auto webview2 = std::dynamic_pointer_cast<WebView2View>(view);
                
                if (webview2 && webview2->getController()) {
                    webview2->getController()->get_Bounds(&viewBounds);
                }
                
                if (PtInRect(&viewBounds, mousePos)) {
                    // Convert to local coordinates for mask checking
                    POINT localPoint = {
                        mousePos.x - viewBounds.left,
                        mousePos.y - viewBounds.top
                    };
                    
                    // Check if point is in a masked (cut-out) area
                    if (view->isPointInMask(localPoint)) {
                        // Point is in masked area, don't make this webview active
                        // Continue to check lower webviews
                        view->toggleMirrorMode(true);
                        continue;
                    }
                    
                    // Point is in unmasked area, make this webview active
                    newActiveView = view.get();
                    view->toggleMirrorMode(false);
                    continue;
                }
            }
            
            // All other webviews are non-interactive
            view->toggleMirrorMode(true);
        }
        
        // Update active webview for input routing
        m_activeWebView = newActiveView;
    }
    

    struct EnumChildData {
        RECT targetBounds;
        HWND containerHwnd;
    };
    
    static BOOL CALLBACK EnumChildCallback(HWND child, LPARAM lParam) {
        EnumChildData* data = (EnumChildData*)lParam;
        
        char className[256];
        GetClassNameA(child, className, sizeof(className));
        
        // Look for WebView2/Chrome child windows
        if (strstr(className, "Chrome_WidgetWin") || 
            strstr(className, "Chrome_RenderWidgetHostHWND")) {
            
            RECT childRect;
            GetWindowRect(child, &childRect);
            
            // Convert to container coordinates
            POINT topLeft = {childRect.left, childRect.top};
            POINT bottomRight = {childRect.right, childRect.bottom};
            ScreenToClient(data->containerHwnd, &topLeft);
            ScreenToClient(data->containerHwnd, &bottomRight);
            
            // Check if this matches our WebView's bounds (with some tolerance)
            if (abs(topLeft.x - data->targetBounds.left) < 5 && 
                abs(topLeft.y - data->targetBounds.top) < 5) {
                // This is likely our WebView's child window
                SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return FALSE; // Stop enumeration
            }
        }
        return TRUE; // Continue enumeration
    }
    
    void BringWebView2ChildWindowToFront(AbstractView* view) {
        // Cast to WebView2View to access controller
        auto webview2 = dynamic_cast<WebView2View*>(view);
        if (!webview2 || !webview2->getController()) return;
        
        // Get the bounds of this WebView to identify its child window
        RECT viewBounds;
        webview2->getController()->get_Bounds(&viewBounds);
        
        EnumChildData enumData;
        enumData.targetBounds = viewBounds;
        enumData.containerHwnd = m_hwnd;
        
        // Find and bring the WebView2's child window to front
        EnumChildWindows(m_hwnd, EnumChildCallback, (LPARAM)&enumData);
    }
    
public:
    ContainerView(HWND parentWindow) : m_parentWindow(parentWindow), m_hwnd(NULL) {
        // Double-check parent window is valid
        if (!IsWindow(parentWindow)) {
            ::log("ERROR: Parent window handle is invalid in ContainerView constructor");
            return;
        }
        
        // Get parent window client area
        RECT clientRect;
        if (!GetClientRect(parentWindow, &clientRect)) {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to get parent window client rect, error: %lu", error);
            ::log(errorMsg);
            return;
        }
        
        // Validate that we have a reasonable client area
        int width = clientRect.right - clientRect.left;
        int height = clientRect.bottom - clientRect.top;
        
        if (width <= 0 || height <= 0) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Parent window has invalid client area: %dx%d", width, height);
            ::log(errorMsg);
            return;
        }
        
        // Register our custom window class for proper event handling
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};
            wc.lpfnWndProc = ContainerWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "ContainerViewClass";
            wc.hbrBackground = NULL; // Transparent background
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            
            if (!RegisterClassA(&wc)) {
                DWORD error = GetLastError();
                if (error != ERROR_CLASS_ALREADY_EXISTS) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "ERROR: Failed to register ContainerViewClass, error: %lu", error);
                    ::log(errorMsg);
                    // Fall back to STATIC class
                    goto use_static_class;
                }
            }
            classRegistered = true;
        }
        
        // Try creating with our custom class first
        m_hwnd = CreateWindowExA(
            0,
            "ContainerViewClass",
            "",  // No title text
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, width, height,
            parentWindow,
            NULL,
            GetModuleHandle(NULL),
            this   // Pass this pointer for message handling
        );
        
        if (!m_hwnd) {
            ::log("Custom class failed, falling back to STATIC class");
            
            use_static_class:
            // Fallback to STATIC class
            m_hwnd = CreateWindowExA(
                0,
                "STATIC",
                "",  // No title text  
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                0, 0, width, height,
                parentWindow,
                NULL,
                GetModuleHandle(NULL),
                NULL
            );
            
            if (!m_hwnd) {
                DWORD error = GetLastError();
                char errorMsg[256];
                sprintf_s(errorMsg, "ERROR: Failed to create container window even with STATIC class, error: %lu", error);
                ::log(errorMsg);
                return;
            } else {
            }
        } else {
        }
        
        if (m_hwnd) {
            // Verify the container window is valid
            if (!IsWindow(m_hwnd)) {
                ::log("ERROR: Container window creation returned handle but window is not valid");
                m_hwnd = NULL;
                return;
            }
            
            char successMsg[256];
        }
    }

    void ResizeAutoSizingViews(int width, int height) {
        for (auto& view : m_abstractViews) {
            if (view->fullSize) {
                // Resize the webview to match container
                RECT bounds = {0, 0, width, height};
                view->resize(bounds, nullptr);
                
                // char logMsg[256];
                // sprintf_s(logMsg, "Resized auto-sizing WebView %u to %dx%d", 
                //         view->webviewId, width, height);
                // ::log(logMsg);
            }
        }
    }

    void BringViewToFront(uint32_t webviewId) {
        auto it = std::find_if(m_abstractViews.begin(), m_abstractViews.end(),
            [webviewId](const std::shared_ptr<AbstractView>& view) {
                return view->webviewId == webviewId;
            });
        
        if (it != m_abstractViews.end()) {
            auto view = *it;
            // Move to front of vector (most recent first)
            m_abstractViews.erase(it);
            m_abstractViews.insert(m_abstractViews.begin(), view);
            
            // Bring the appropriate child window to front
            auto webview2 = dynamic_cast<WebView2View*>(view.get());
            
            if (webview2) {
                BringWebView2ChildWindowToFront(view.get());
            } else if (view->hwnd) {
                SetWindowPos(view->hwnd, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
    }
    
    ~ContainerView() {
        // Explicitly remove each view before destroying HWNDs.
        for (auto& view : m_abstractViews) {
            g_pendingResizeQueue.remove(view.get());
            view->remove();
            {
                std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
                g_abstractViews.erase(view->webviewId);
            }
        }
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
        }
    }
    
    HWND GetHwnd() const { return m_hwnd; }
    
    void AddAbstractView(std::shared_ptr<AbstractView> view) {
    
        // Add to front of vector so it's top-most first
        m_abstractViews.insert(m_abstractViews.begin(), view); 
        BringViewToFront(view->webviewId);
        
        // TODO: Start new webviews in mirror mode (input disabled)
        // They will be made interactive when mouse hovers over them
        // view->toggleMirrorMode(true);
    }
    
    void RemoveAbstractViewWithId(uint32_t webviewId) {
        m_abstractViews.erase(
            std::remove_if(m_abstractViews.begin(), m_abstractViews.end(),
                [webviewId](const std::shared_ptr<AbstractView>& view) {
                    return view->webviewId == webviewId;
                }),
            m_abstractViews.end());
    }
};

// Helper function to get or create container for a window
ContainerView* GetOrCreateContainer(HWND parentWindow) {
    // Validate the parent window handle
    if (!IsWindow(parentWindow)) {
        ::log("ERROR: Parent window handle is invalid");
        return nullptr;
    }
    
    auto it = g_containerViews.find(parentWindow);
    if (it == g_containerViews.end()) {
        
        auto container = std::make_unique<ContainerView>(parentWindow);
        ContainerView* containerPtr = container.get();
        
        // Only store if creation was successful
        if (containerPtr->GetHwnd() != NULL) {
            g_containerViews[parentWindow] = std::move(container);
            return containerPtr;
        } else {
            ::log("ERROR: Container creation failed, not storing");
            return nullptr;
        }
    }
    
    // log("Using existing container for window");
    return it->second.get();
}

// Stub classes for compatibility
class NSWindow {
public:
    void* contentView;
};



class MyScriptMessageHandlerWithReply {
public:
    HandlePostMessageWithReply zigCallback;
    uint32_t webviewId;
};

class WKWebView {
public:
    void* configuration;
};

struct NSRect {
    double x;
    double y;
    double width;
    double height;
};

struct createNSWindowWithFrameAndStyleParams {
    NSRect frame;
    uint32_t styleMask;
    const char *titleBarStyle;
};

// Define a struct to store window data
typedef struct {
    uint32_t windowId;
    WindowCloseHandler closeHandler;
    WindowMoveHandler moveHandler;
    WindowResizeHandler resizeHandler;
    WindowFocusHandler focusHandler;
    WindowBlurHandler blurHandler;
    WindowKeyHandler keyHandler;
} WindowData;


// Handle application menu item selection
void handleApplicationMenuSelection(UINT menuId) {
    auto it = g_menuItemActions.find(menuId);
    if (it != g_menuItemActions.end()) {
        const std::string& action = it->second;
        
        // char logMsg[256];
        // sprintf_s(logMsg, "Application menu action: %s", action.c_str());
        // ::log(logMsg);
        
        if (g_appMenuTarget && g_appMenuTarget->zigHandler) {
            if (action == "__quit__") {
                if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                    g_quitRequestedHandler();
                } else {
                    PostQuitMessage(0);
                }
            } else if (action == "__undo__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_UNDO, 0, 0);
                }
            } else if (action == "__redo__") {
                // Windows doesn't have a standard WM_REDO message
                // Use Ctrl+Y keypress simulation or application-specific handling
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    // Try sending Ctrl+Y keystroke
                    keybd_event(VK_CONTROL, 0, 0, 0);
                    keybd_event('Y', 0, 0, 0);
                    keybd_event('Y', 0, KEYEVENTF_KEYUP, 0);
                    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
                }
            } else if (action == "__cut__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_CUT, 0, 0);
                }
            } else if (action == "__copy__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_COPY, 0, 0);
                }
            } else if (action == "__paste__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_PASTE, 0, 0);
                }
            } else if (action == "__pasteAndMatchStyle__") {
                // Paste as plain text: get clipboard text and paste it without formatting
                HWND focusedWindow = GetFocus();
                if (focusedWindow && OpenClipboard(NULL)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
                        if (pszText) {
                            // Clear clipboard and set as plain text
                            std::wstring text(pszText);
                            GlobalUnlock(hData);
                            EmptyClipboard();

                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.length() + 1) * sizeof(wchar_t));
                            if (hMem) {
                                wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
                                if (pMem) {
                                    wcscpy(pMem, text.c_str());
                                    GlobalUnlock(hMem);
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                }
                            }
                        }
                    }
                    CloseClipboard();
                    // Now paste the plain text
                    SendMessage(focusedWindow, WM_PASTE, 0, 0);
                }
            } else if (action == "__delete__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, WM_CLEAR, 0, 0);
                }
            } else if (action == "__selectAll__") {
                HWND focusedWindow = GetFocus();
                if (focusedWindow) {
                    SendMessage(focusedWindow, EM_SETSEL, 0, -1);
                }
            } else if (action == "__minimize__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    ShowWindow(activeWindow, SW_MINIMIZE);
                }
            } else if (action == "__toggleFullScreen__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    // Toggle between maximized and normal state
                    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                    GetWindowPlacement(activeWindow, &wp);
                    if (wp.showCmd == SW_MAXIMIZE) {
                        ShowWindow(activeWindow, SW_RESTORE);
                    } else {
                        ShowWindow(activeWindow, SW_MAXIMIZE);
                    }
                }
            } else if (action == "__zoom__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    // Zoom toggles between maximized and normal (same as toggleFullScreen on Windows)
                    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
                    GetWindowPlacement(activeWindow, &wp);
                    if (wp.showCmd == SW_MAXIMIZE) {
                        ShowWindow(activeWindow, SW_RESTORE);
                    } else {
                        ShowWindow(activeWindow, SW_MAXIMIZE);
                    }
                }
            } else if (action == "__close__") {
                HWND activeWindow = GetActiveWindow();
                if (activeWindow) {
                    PostMessage(activeWindow, WM_CLOSE, 0, 0);
                }
            } else {
                g_appMenuTarget->zigHandler(g_appMenuTarget->trayId, action.c_str());
            }
        }
    }
}


// Window procedure that will handle events and call your handlers
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Get our custom data
    WindowData* data = (WindowData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    
    switch (msg) {
        
        case WM_INPUT: {
            if (g_isMovingWindow && g_targetWindow) {
                UINT dwSize = 0;
                GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
                
                LPBYTE lpb = new BYTE[dwSize];
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                    RAWINPUT* raw = (RAWINPUT*)lpb;
                    
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        // Check for mouse button release
                        if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
                            // Stop window move
                            RAWINPUTDEVICE rid;
                            rid.usUsagePage = 0x01;
                            rid.usUsage = 0x02;
                            rid.dwFlags = RIDEV_REMOVE;
                            rid.hwndTarget = NULL;
                            
                            RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
                            g_isMovingWindow = FALSE;
                            g_targetWindow = NULL;
                        }
                        
                        // Handle mouse movement using cursor position tracking
                        else if (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0) {
                            POINT currentCursor;
                            GetCursorPos(&currentCursor);
                            
                            // Calculate delta from initial cursor position when drag started
                            int deltaX = currentCursor.x - g_initialCursorPos.x;
                            int deltaY = currentCursor.y - g_initialCursorPos.y;
                            
                            // Calculate new window position
                            int newX = g_initialWindowPos.x + deltaX;
                            int newY = g_initialWindowPos.y + deltaY;
                            
                            SetWindowPos(g_targetWindow, NULL, newX, newY, 0, 0, 
                                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        }
                    }
                }
                delete[] lpb;
            }
            break;
        }
        case WM_NCHITTEST:
            break;

        case WM_COMMAND:
            // Check if this is an application menu command
            if (HIWORD(wParam) == 0) { // Menu item selected
                UINT menuId = LOWORD(wParam);
                handleApplicationMenuSelection(menuId);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            {
                // Dispatch keyboard events to keyHandler callback
                if (data && data->keyHandler &&
                    (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
                    uint32_t keyCode = (uint32_t)wParam;
                    uint32_t modifiers = 0;
                    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= 1 << 0;
                    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= 1 << 1;
                    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= 1 << 2;
                    uint32_t isDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? 1 : 0;
                    uint32_t isRepeat = (lParam & (1 << 30)) ? 1 : 0;
                    data->keyHandler(data->windowId, keyCode, modifiers, isDown, isRepeat);
                }
            }
            break;

        case WM_CLOSE:
            if (data && data->closeHandler) {
                data->closeHandler(data->windowId);
            }
            break;
            
        case WM_MOVE:
            if (data && data->moveHandler) {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                data->moveHandler(data->windowId, x, y);
            }
            break;
            
        case WM_SIZE:
            {
                // Resize container to match window client area
                auto containerIt = g_containerViews.find(hwnd);
                if (containerIt != g_containerViews.end()) {
                    RECT clientRect;
                    GetClientRect(hwnd, &clientRect);
                    int width = clientRect.right - clientRect.left;
                    int height = clientRect.bottom - clientRect.top;
                    
                    // Resize the container window itself
                    SetWindowPos(containerIt->second->GetHwnd(), NULL, 
                        0, 0, width, height,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                    
                    // Resize all auto-resizing webviews in this container
                    containerIt->second->ResizeAutoSizingViews(width, height);
                }
                
                if (data && data->resizeHandler) {
                    int width = LOWORD(lParam);
                    int height = HIWORD(lParam);
                    data->resizeHandler(data->windowId, 0, 0, width, height);
                }
            }
            break;

        case WM_ACTIVATE:
            // Window activation - WA_ACTIVE or WA_CLICKACTIVE means window is being activated
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (data && data->blurHandler) {
                    data->blurHandler(data->windowId);
                }
            } else {
                if (data && data->focusHandler) {
                    data->focusHandler(data->windowId);
                }
            }
            break;

        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                // Don't need to do anything here, just validate the paint region
                EndPaint(hwnd, &ps);
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == 1) {
                KillTimer(hwnd, 1);
                ::log("Timer fired - forcing window refresh");
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd);
            }
            return 0;
            
        case WM_DESTROY:
            // Clean up application menu when main window is destroyed
            if (g_applicationMenu) {
                DestroyMenu(g_applicationMenu);
                g_applicationMenu = NULL;
            }
            g_appMenuTarget.reset();
            
            // Clean up container view
            g_containerViews.erase(hwnd);
            
            // Clean up window data
            if (data) {
                free(data);
                SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            }
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// handles window things on Windows
LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_EXECUTE_SYNC_BLOCK:
            MainThreadDispatcher::handleSyncTask(lParam);
            return 0;
        case WM_EXECUTE_ASYNC_BLOCK:
            MainThreadDispatcher::handleSyncTask(lParam);
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}


class NSStatusItem {
public:
    NOTIFYICONDATA nid;
    HWND hwnd;
    uint32_t trayId;
    ZigStatusItemHandler handler;
    HMENU contextMenu;
    std::string title;
    std::string imagePath;
    
    NSStatusItem() {
        memset(&nid, 0, sizeof(NOTIFYICONDATA));
        hwnd = NULL;
        trayId = 0;
        handler = nullptr;
        contextMenu = NULL;
    }
    
    ~NSStatusItem() {
        if (contextMenu) {
            DestroyMenu(contextMenu);
        }
        // Remove from system tray
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
};

// Global map to store tray items by their window handle
static std::map<HWND, NSStatusItem*> g_trayItems;
static UINT g_trayMessageId = WM_USER + 100;

struct SimpleJsonValue {
    enum Type { STRING, BOOL, ARRAY, OBJECT, UNKNOWN };
    Type type = UNKNOWN;
    std::string stringValue;
    bool boolValue = false;
    std::vector<SimpleJsonValue> arrayValue;
    std::map<std::string, SimpleJsonValue> objectValue;
};

// Simple JSON parsing functions
std::string trimWhitespace(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::string extractQuotedString(const std::string& json, size_t& pos) {
    if (pos >= json.length() || json[pos] != '"') return "";
    pos++; // Skip opening quote
    
    std::string result;
    while (pos < json.length() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.length()) {
            pos++; // Skip escape character
            switch (json[pos]) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '\\': result += '\\'; break;
                case '"': result += '"'; break;
                default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    
    if (pos < json.length() && json[pos] == '"') {
        pos++; // Skip closing quote
    }
    
    return result;
}

SimpleJsonValue parseJsonValue(const std::string& json, size_t& pos);

SimpleJsonValue parseJsonObject(const std::string& json, size_t& pos) {
    SimpleJsonValue obj;
    obj.type = SimpleJsonValue::OBJECT;
    
    if (pos >= json.length() || json[pos] != '{') return obj;
    pos++; // Skip '{'
    
    while (pos < json.length()) {
        // Skip whitespace
        while (pos < json.length() && isspace(json[pos])) pos++;
        
        if (pos >= json.length()) break;
        if (json[pos] == '}') {
            pos++; // Skip '}'
            break;
        }
        
        // Parse key
        std::string key = extractQuotedString(json, pos);
        
        // Skip whitespace and ':'
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ':')) pos++;
        
        // Parse value
        SimpleJsonValue value = parseJsonValue(json, pos);
        obj.objectValue[key] = value;
        
        // Skip whitespace and optional ','
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ',')) pos++;
    }
    
    return obj;
}

SimpleJsonValue parseJsonArray(const std::string& json, size_t& pos) {
    SimpleJsonValue arr;
    arr.type = SimpleJsonValue::ARRAY;
    
    if (pos >= json.length() || json[pos] != '[') return arr;
    pos++; // Skip '['
    
    while (pos < json.length()) {
        // Skip whitespace
        while (pos < json.length() && isspace(json[pos])) pos++;
        
        if (pos >= json.length()) break;
        if (json[pos] == ']') {
            pos++; // Skip ']'
            break;
        }
        
        // Parse value
        SimpleJsonValue value = parseJsonValue(json, pos);
        arr.arrayValue.push_back(value);
        
        // Skip whitespace and optional ','
        while (pos < json.length() && (isspace(json[pos]) || json[pos] == ',')) pos++;
    }
    
    return arr;
}

SimpleJsonValue parseJsonValue(const std::string& json, size_t& pos) {
    SimpleJsonValue value;
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return value;
    
    if (json[pos] == '"') {
        // String value
        value.type = SimpleJsonValue::STRING;
        value.stringValue = extractQuotedString(json, pos);
    } else if (json[pos] == '{') {
        // Object value
        value = parseJsonObject(json, pos);
    } else if (json[pos] == '[') {
        // Array value
        value = parseJsonArray(json, pos);
    } else if (json.substr(pos, 4) == "true") {
        // Boolean true
        value.type = SimpleJsonValue::BOOL;
        value.boolValue = true;
        pos += 4;
    } else if (json.substr(pos, 5) == "false") {
        // Boolean false
        value.type = SimpleJsonValue::BOOL;
        value.boolValue = false;
        pos += 5;
    } else {
        // Skip unknown values
        while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') pos++;
    }
    
    return value;
}

SimpleJsonValue parseJson(const std::string& json) {
    size_t pos = 0;
    return parseJsonValue(json, pos);
}

// Helper to parse virtual key code from key string for menu accelerators
static UINT getMenuVirtualKeyCode(const std::string& key) {
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // Letters
    if (lowerKey.length() == 1 && lowerKey[0] >= 'a' && lowerKey[0] <= 'z') {
        return 'A' + (lowerKey[0] - 'a');
    }
    // Numbers
    if (lowerKey.length() == 1 && lowerKey[0] >= '0' && lowerKey[0] <= '9') {
        return '0' + (lowerKey[0] - '0');
    }
    // Function keys
    if (lowerKey[0] == 'f' && lowerKey.length() >= 2) {
        try {
            int fNum = std::stoi(lowerKey.substr(1));
            if (fNum >= 1 && fNum <= 24) return VK_F1 + (fNum - 1);
        } catch (...) {}
    }
    // Special keys
    if (lowerKey == "space" || lowerKey == " ") return VK_SPACE;
    if (lowerKey == "return" || lowerKey == "enter") return VK_RETURN;
    if (lowerKey == "tab") return VK_TAB;
    if (lowerKey == "escape" || lowerKey == "esc") return VK_ESCAPE;
    if (lowerKey == "backspace") return VK_BACK;
    if (lowerKey == "delete" || lowerKey == "del") return VK_DELETE;
    if (lowerKey == "insert") return VK_INSERT;
    if (lowerKey == "up") return VK_UP;
    if (lowerKey == "down") return VK_DOWN;
    if (lowerKey == "left") return VK_LEFT;
    if (lowerKey == "right") return VK_RIGHT;
    if (lowerKey == "home") return VK_HOME;
    if (lowerKey == "end") return VK_END;
    if (lowerKey == "pageup") return VK_PRIOR;
    if (lowerKey == "pagedown") return VK_NEXT;
    // Symbols
    if (lowerKey == "plus") return VK_OEM_PLUS;
    if (lowerKey == "minus") return VK_OEM_MINUS;
    if (lowerKey == "-") return VK_OEM_MINUS;
    if (lowerKey == "=" || lowerKey == "+") return VK_OEM_PLUS;
    if (lowerKey == "[") return VK_OEM_4;
    if (lowerKey == "]") return VK_OEM_6;
    if (lowerKey == "\\") return VK_OEM_5;
    if (lowerKey == ";") return VK_OEM_1;
    if (lowerKey == "'") return VK_OEM_7;
    if (lowerKey == ",") return VK_OEM_COMMA;
    if (lowerKey == ".") return VK_OEM_PERIOD;
    if (lowerKey == "/") return VK_OEM_2;
    if (lowerKey == "`") return VK_OEM_3;

    return 0;
}

// Parse modifiers from accelerator string for menu accelerators using the
// shared cross-platform parser. Returns FCONTROL, FALT, FSHIFT flags.
static BYTE parseMenuModifiers(const std::string& accelerator, std::string& outKey) {
    auto parts = electrobun::parseAccelerator(accelerator);
    outKey = parts.key;

    BYTE modifiers = FVIRTKEY;
    if (parts.commandOrControl || parts.command || parts.control) modifiers |= FCONTROL;
    if (parts.alt)                                                modifiers |= FALT;
    if (parts.shift)                                              modifiers |= FSHIFT;
    return modifiers;
}

// Build display string for accelerator (e.g., "Ctrl+S", "Ctrl+Shift+N")
static std::string buildAcceleratorDisplayString(const std::string& accelerator) {
    std::string keyPart;
    BYTE modifiers = parseMenuModifiers(accelerator, keyPart);

    std::string display;
    if (modifiers & FCONTROL) {
        display += "Ctrl+";
    }
    if (modifiers & FALT) {
        display += "Alt+";
    }
    if (modifiers & FSHIFT) {
        display += "Shift+";
    }

    // Capitalize the key for display
    std::string upperKey = keyPart;
    if (!upperKey.empty()) {
        upperKey[0] = toupper(upperKey[0]);
    }

    // Handle special key display names
    std::string lowerKey = keyPart;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
    if (lowerKey == "return" || lowerKey == "enter") {
        upperKey = "Enter";
    } else if (lowerKey == "escape" || lowerKey == "esc") {
        upperKey = "Esc";
    } else if (lowerKey == "delete" || lowerKey == "del") {
        upperKey = "Del";
    } else if (lowerKey == "backspace") {
        upperKey = "Backspace";
    } else if (lowerKey == "space") {
        upperKey = "Space";
    } else if (lowerKey == "pageup") {
        upperKey = "PgUp";
    } else if (lowerKey == "pagedown") {
        upperKey = "PgDn";
    } else if (lowerKey == "plus") {
        upperKey = "+";
    } else if (lowerKey == "minus") {
        upperKey = "-";
    }

    display += upperKey;
    return display;
}

// Function to create Windows menu from JSON config (equivalent to createMenuFromConfig)
HMENU createMenuFromConfig(const SimpleJsonValue& menuConfig, NSStatusItem* statusItem) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        ::log("ERROR: Failed to create popup menu");
        return NULL;
    }
    
    if (menuConfig.type != SimpleJsonValue::ARRAY) {
        ::log("ERROR: Menu config is not an array");
        return menu;
    }
    
    for (const auto& itemValue : menuConfig.arrayValue) {
        if (itemValue.type != SimpleJsonValue::OBJECT) continue;
        
        const auto& itemData = itemValue.objectValue;
        
        // Helper lambda to get string value
        auto getString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::STRING) {
                return it->second.stringValue;
            }
            return defaultVal;
        };
        
        // Helper lambda to get bool value
        auto getBool = [&](const std::string& key, bool defaultVal = false) -> bool {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                return it->second.boolValue;
            }
            return defaultVal;
        };
        
        std::string type = getString("type");
        std::string label = getString("label");
        std::string action = getString("action");
        std::string role = getString("role");
        std::string accelerator = getString("accelerator");

        bool enabled = getBool("enabled", true);
        bool checked = getBool("checked", false);
        bool hidden = getBool("hidden", false);
        std::string tooltip = getString("tooltip");

        if (hidden) {
            continue;
        } else if (type == "divider") {
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        } else {
            UINT flags = MF_STRING;
            if (!enabled) flags |= MF_GRAYED;

            UINT menuId = g_nextMenuId++;

            // Store the action for this menu ID
            if (!action.empty()) {
                g_menuItemActions[menuId] = action;
            }

            // Handle system roles (similar to macOS implementation)
            if (!role.empty()) {
                if (role == "quit") {
                    // For quit, we'll handle it specially in the menu callback
                    g_menuItemActions[menuId] = "__quit__";
                }

                // Set default accelerators for common roles if not specified
                if (accelerator.empty()) {
                    if (role == "undo") {
                        accelerator = "z";
                    } else if (role == "redo") {
                        accelerator = "y";
                    } else if (role == "cut") {
                        accelerator = "x";
                    } else if (role == "copy") {
                        accelerator = "c";
                    } else if (role == "paste") {
                        accelerator = "v";
                    } else if (role == "selectAll") {
                        accelerator = "a";
                    }
                }
            }

            // Build the label with accelerator display for context menus
            // On Windows, context menus use mnemonic keys (just the letter, not Ctrl+Letter)
            std::string displayLabel = label;
            if (!accelerator.empty()) {
                // For context menus, display just the letter (mnemonic key)
                // The user presses just the letter while the menu is open
                if (accelerator.length() == 1 && isalpha(accelerator[0])) {
                    displayLabel += "\t" + std::string(1, (char)toupper(accelerator[0]));
                } else {
                    // For complex accelerators, extract just the key part
                    std::string accelDisplay = buildAcceleratorDisplayString(accelerator);
                    // Remove "Ctrl+" prefix for context menus since they use mnemonics
                    size_t ctrlPos = accelDisplay.find("Ctrl+");
                    if (ctrlPos != std::string::npos) {
                        accelDisplay = accelDisplay.substr(ctrlPos + 5); // Skip "Ctrl+"
                    }
                    if (!accelDisplay.empty()) {
                        displayLabel += "\t" + accelDisplay;
                    }
                }
            }

            // Append the menu item
            AppendMenuA(menu, flags, menuId, displayLabel.c_str());

            if (checked) {
                CheckMenuItem(menu, menuId, MF_BYCOMMAND | MF_CHECKED);
            }

            // Handle submenus
            auto submenuIt = itemData.find("submenu");
            if (submenuIt != itemData.end() && submenuIt->second.type == SimpleJsonValue::ARRAY) {
                HMENU submenu = createMenuFromConfig(submenuIt->second, statusItem);
                if (submenu) {
                    ModifyMenuA(menu, menuId, MF_BYCOMMAND | MF_POPUP, (UINT_PTR)submenu, displayLabel.c_str());
                }
            }
        }
    }
    
    return menu;
}

// Function to handle menu item selection
void handleMenuItemSelection(UINT menuId, NSStatusItem* statusItem) {
    auto it = g_menuItemActions.find(menuId);
    if (it != g_menuItemActions.end()) {
        const std::string& action = it->second;

        if (statusItem && statusItem->handler) {
            if (action == "__quit__") {
                if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                    g_quitRequestedHandler();
                } else {
                    PostQuitMessage(0);
                }
            } else {
                statusItem->handler(statusItem->trayId, action.c_str());
            }
        }
    }
}

// Rebuild the accelerator table from collected accelerators
static void rebuildAcceleratorTable() {
    if (g_hAccelTable) {
        DestroyAcceleratorTable(g_hAccelTable);
        g_hAccelTable = NULL;
    }

    if (!g_menuAccelerators.empty()) {
        g_hAccelTable = CreateAcceleratorTableA(g_menuAccelerators.data(), (int)g_menuAccelerators.size());
        if (g_hAccelTable) {
            // ::log("Created accelerator table with " + std::to_string(g_menuAccelerators.size()) + " entries");
        }
    }
}

// Clear all menu accelerators (call before rebuilding menu)
static void clearMenuAccelerators() {
    g_menuAccelerators.clear();
    if (g_hAccelTable) {
        DestroyAcceleratorTable(g_hAccelTable);
        g_hAccelTable = NULL;
    }
}

// Function to set accelerator keys for menu items
// Returns the display string to append to the menu label
std::string setMenuItemAccelerator(HMENU menu, UINT menuId, const std::string& accelerator, UINT modifierMask = 0) {
    if (accelerator.empty()) return "";

    std::string keyPart;
    BYTE modifiers;
    UINT vkCode;

    // Check if this is a simple single-letter accelerator (for role defaults)
    if (accelerator.length() == 1 && isalpha(accelerator[0])) {
        // Single letter with Ctrl modifier (from role defaults)
        vkCode = toupper(accelerator[0]);
        modifiers = FVIRTKEY | FCONTROL;
        keyPart = accelerator;
    } else {
        // Parse the full accelerator string
        modifiers = parseMenuModifiers(accelerator, keyPart);
        vkCode = getMenuVirtualKeyCode(keyPart);
    }

    // Apply modifierMask override if specified
    if (modifierMask > 0) {
        modifiers = FVIRTKEY;
        if (modifierMask & 1) modifiers |= FCONTROL;
        if (modifierMask & 2) modifiers |= FSHIFT;
        if (modifierMask & 4) modifiers |= FALT;
    }

    if (vkCode == 0) {
        // ::log("Failed to parse accelerator key: " + accelerator);
        return "";
    }

    // Add to accelerator table
    ACCEL accel;
    accel.fVirt = modifiers;
    accel.key = (WORD)vkCode;
    accel.cmd = (WORD)menuId;
    g_menuAccelerators.push_back(accel);

    // Build and return the display string
    if (accelerator.length() == 1 && isalpha(accelerator[0])) {
        return "Ctrl+" + std::string(1, (char)toupper(accelerator[0]));
    }
    return buildAcceleratorDisplayString(accelerator);
}

// Enhanced createMenuFromConfig for application menu
HMENU createApplicationMenuFromConfig(const SimpleJsonValue& menuConfig, StatusItemTarget* target) {
    HMENU menuBar = CreateMenu();
    if (!menuBar) {
        ::log("ERROR: Failed to create menu bar");
        return NULL;
    }
    
    if (menuConfig.type != SimpleJsonValue::ARRAY) {
        ::log("ERROR: Application menu config is not an array");
        DestroyMenu(menuBar);
        return NULL;
    }
    
    for (const auto& topLevelItem : menuConfig.arrayValue) {
        if (topLevelItem.type != SimpleJsonValue::OBJECT) continue;
        
        const auto& itemData = topLevelItem.objectValue;
        
        // Helper lambda to get string value
        auto getString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::STRING) {
                return it->second.stringValue;
            }
            return defaultVal;
        };
        
        // Helper lambda to get bool value
        auto getBool = [&](const std::string& key, bool defaultVal = false) -> bool {
            auto it = itemData.find(key);
            if (it != itemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                return it->second.boolValue;
            }
            return defaultVal;
        };
        
        std::string label = getString("label");
        bool hidden = getBool("hidden", false);
        
        if (hidden) continue;
        
        // Check if this has a submenu
        auto submenuIt = itemData.find("submenu");
        if (submenuIt != itemData.end() && submenuIt->second.type == SimpleJsonValue::ARRAY) {
            HMENU popupMenu = CreatePopupMenu();
            if (!popupMenu) continue;
            
            // Process submenu items
            for (const auto& subItemValue : submenuIt->second.arrayValue) {
                if (subItemValue.type != SimpleJsonValue::OBJECT) continue;
                
                const auto& subItemData = subItemValue.objectValue;
                
                // Helper lambdas for subitem data
                auto getSubString = [&](const std::string& key, const std::string& defaultVal = "") -> std::string {
                    auto it = subItemData.find(key);
                    if (it != subItemData.end() && it->second.type == SimpleJsonValue::STRING) {
                        return it->second.stringValue;
                    }
                    return defaultVal;
                };
                
                auto getSubBool = [&](const std::string& key, bool defaultVal = false) -> bool {
                    auto it = subItemData.find(key);
                    if (it != subItemData.end() && it->second.type == SimpleJsonValue::BOOL) {
                        return it->second.boolValue;
                    }
                    return defaultVal;
                };
                
                std::string subType = getSubString("type");
                std::string subLabel = getSubString("label");
                std::string subAction = getSubString("action");
                std::string subRole = getSubString("role");
                std::string subAccelerator = getSubString("accelerator");
                
                bool subEnabled = getSubBool("enabled", true);
                bool subChecked = getSubBool("checked", false);
                bool subHidden = getSubBool("hidden", false);
                
                if (subHidden) {
                    continue;
                } else if (subType == "divider") {
                    AppendMenuA(popupMenu, MF_SEPARATOR, 0, NULL);
                } else {
                    UINT flags = MF_STRING;
                    if (!subEnabled) flags |= MF_GRAYED;
                    
                    UINT menuId = g_nextMenuId++;
                    
                    // Store the action for this menu ID
                    if (!subAction.empty()) {
                        g_menuItemActions[menuId] = subAction;
                    }
                    
                    // Handle system roles
                    if (!subRole.empty()) {
                        if (subRole == "quit") {
                            g_menuItemActions[menuId] = "__quit__";
                        } else if (subRole == "undo") {
                            g_menuItemActions[menuId] = "__undo__";
                        } else if (subRole == "redo") {
                            g_menuItemActions[menuId] = "__redo__";
                        } else if (subRole == "cut") {
                            g_menuItemActions[menuId] = "__cut__";
                        } else if (subRole == "copy") {
                            g_menuItemActions[menuId] = "__copy__";
                        } else if (subRole == "paste") {
                            g_menuItemActions[menuId] = "__paste__";
                        } else if (subRole == "pasteAndMatchStyle") {
                            g_menuItemActions[menuId] = "__pasteAndMatchStyle__";
                        } else if (subRole == "delete") {
                            g_menuItemActions[menuId] = "__delete__";
                        } else if (subRole == "selectAll") {
                            g_menuItemActions[menuId] = "__selectAll__";
                        } else if (subRole == "minimize") {
                            g_menuItemActions[menuId] = "__minimize__";
                        } else if (subRole == "toggleFullScreen" || subRole == "togglefullscreen") {
                            g_menuItemActions[menuId] = "__toggleFullScreen__";
                        } else if (subRole == "zoom") {
                            g_menuItemActions[menuId] = "__zoom__";
                        } else if (subRole == "close") {
                            g_menuItemActions[menuId] = "__close__";
                        }
                        // Note: The following roles are macOS-only and not implemented on Windows:
                        // hide, hideOthers, showAll, startSpeaking, stopSpeaking, bringAllToFront

                        // Set default accelerators for common roles if not specified
                        if (subAccelerator.empty()) {
                            if (subRole == "undo") {
                                subAccelerator = "z";
                            } else if (subRole == "redo") {
                                subAccelerator = "y";
                            } else if (subRole == "cut") {
                                subAccelerator = "x";
                            } else if (subRole == "copy") {
                                subAccelerator = "c";
                            } else if (subRole == "paste" || subRole == "pasteAndMatchStyle") {
                                subAccelerator = "v";
                            } else if (subRole == "delete") {
                                subAccelerator = "Delete";
                            } else if (subRole == "selectAll") {
                                subAccelerator = "a";
                            } else if (subRole == "toggleFullScreen" || subRole == "togglefullscreen") {
                                subAccelerator = "F11";
                            }
                        }
                    }
                    
                    // Build the label with accelerator display
                    std::string displayLabel = subLabel;
                    if (!subAccelerator.empty()) {
                        std::string accelDisplay = setMenuItemAccelerator(popupMenu, menuId, subAccelerator, 0);
                        if (!accelDisplay.empty()) {
                            displayLabel += "\t" + accelDisplay;
                        }
                    }

                    // Append the menu item
                    AppendMenuA(popupMenu, flags, menuId, displayLabel.c_str());

                    if (subChecked) {
                        CheckMenuItem(popupMenu, menuId, MF_BYCOMMAND | MF_CHECKED);
                    }
                    
                    // Handle nested submenus
                    auto nestedSubmenuIt = subItemData.find("submenu");
                    if (nestedSubmenuIt != subItemData.end() && nestedSubmenuIt->second.type == SimpleJsonValue::ARRAY) {
                        HMENU nestedSubmenu = createMenuFromConfig(nestedSubmenuIt->second, reinterpret_cast<NSStatusItem*>(target));
                        if (nestedSubmenu) {
                            ModifyMenuA(popupMenu, menuId, MF_BYCOMMAND | MF_POPUP, (UINT_PTR)nestedSubmenu, subLabel.c_str());
                        }
                    }
                }
            }
            
            // Add the popup menu to the menu bar
            AppendMenuA(menuBar, MF_POPUP, (UINT_PTR)popupMenu, label.c_str());
        } else {
            // Top-level item without submenu
            UINT menuId = g_nextMenuId++;
            std::string action = getString("action");
            
            if (!action.empty()) {
                g_menuItemActions[menuId] = action;
            }
            
            UINT flags = MF_STRING;
            if (!getBool("enabled", true)) flags |= MF_GRAYED;
            
            AppendMenuA(menuBar, flags, menuId, label.c_str());
        }
    }
    
    return menuBar;
}


















// Internal factory method for creating WebView2 instances
static std::shared_ptr<WebView2View> createWebView2View(uint32_t webviewId,
                                                 HWND hwnd,
                                                 const char *url,
                                                 double x, double y,
                                                 double width, double height,
                                                 bool autoResize,
                                                 const char *partitionIdentifier,
                                                 DecideNavigationCallback navigationCallback,
                                                 WebviewEventHandler webviewEventHandler,
                                                 HandlePostMessage eventBridgeHandler,
                                                 HandlePostMessage bunBridgeHandler,
                                                 HandlePostMessage internalBridgeHandler,
                                                 const char *electrobunPreloadScript,
                                                 const char *customPreloadScript,
                                                 bool transparent,
                                                 bool sandbox) {
    // Check if WebView2 runtime is available
    LPWSTR versionInfo = nullptr;
    HRESULT result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionInfo);
    if (FAILED(result)) {
        ::log("ERROR: WebView2 runtime is not available. Please install Microsoft Edge WebView2 Runtime");
        auto view = std::make_shared<WebView2View>(webviewId, eventBridgeHandler, bunBridgeHandler, internalBridgeHandler, sandbox);
        view->setCreationFailed(true);
        return view;
    }
    if (versionInfo) {
        CoTaskMemFree(versionInfo);
    }
    
    
    // Make safe copies of string parameters to avoid memory corruption in lambda captures
    std::string urlString = url ? std::string(url) : "";
    std::string electrobunScript = electrobunPreloadScript ? std::string(electrobunPreloadScript) : "";
    std::string customScript = customPreloadScript ? std::string(customPreloadScript) : "";
    std::string partitionStr = partitionIdentifier ? std::string(partitionIdentifier) : "";

    auto view = std::make_shared<WebView2View>(webviewId, eventBridgeHandler, bunBridgeHandler, internalBridgeHandler, sandbox);
    view->hwnd = hwnd;
    view->fullSize = autoResize;
    view->webviewEventHandler = webviewEventHandler;

    // Store URL and scripts in view to survive async callbacks
    view->pendingUrl = urlString;
    view->electrobunScript = electrobunScript;
    view->customScript = customScript;

    // Create WebView2 on main thread
    MainThreadDispatcher::dispatch_sync([view, urlString, x, y, width, height, hwnd, partitionStr, transparent]() {
        // Initialize COM for this thread
        HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to initialize COM, HRESULT: 0x%08X", comResult);
            ::log(errorMsg);
            return;
        }
        
        // Get or create container
        auto container = GetOrCreateContainer(hwnd);
        if (!container) {
            ::log("ERROR: Failed to create container");
            return;
        }
        
        HWND containerHwnd = container->GetHwnd();
        // char debugMsg[256];
        // sprintf_s(debugMsg, "[WebView2] Creating controller for container HWND: %p, parent HWND: %p", containerHwnd, hwnd);
        // ::log(debugMsg);
        
        // Verify the container window is valid
        if (!IsWindow(containerHwnd)) {
            ::log("ERROR: Container window handle is invalid");
            return;
        }
        
        // Get window info for debugging
        RECT windowRect;
        GetWindowRect(containerHwnd, &windowRect);
        DWORD windowStyle = GetWindowLong(containerHwnd, GWL_STYLE);
        // char windowDebug[512];
        // sprintf_s(windowDebug, "[WebView2] Container window - Rect: (%d,%d,%d,%d), Style: 0x%08X", 
        //          windowRect.left, windowRect.top, windowRect.right, windowRect.bottom, windowStyle);
        // ::log(windowDebug);
        
        // Make sure the window is visible (WebView2 requirement)
        ShowWindow(containerHwnd, SW_SHOW);
        UpdateWindow(containerHwnd);
        
        // Create WebView2 environment
        // Store values to avoid complex object captures in lambda
        uint32_t webviewId = view->webviewId;
        HWND parentHwnd = hwnd;
        
        auto environmentCompletedHandler = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [view, container, x, y, width, height, transparent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "ERROR: Failed to create WebView2 environment, HRESULT: 0x%08X", result);
                    ::log(errorMsg);
                    view->setCreationFailed(true);
                    return result;
                }
                
                // Create WebView2 controller - MINIMAL VERSION
                HWND targetHwnd = container->GetHwnd();
                
                if (!IsWindow(targetHwnd)) {
                    ::log("ERROR: Target window is no longer valid");
                    view->setCreationFailed(true);
                    return S_OK;
                }
                
                return env->CreateCoreWebView2Controller(targetHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [view, container, x, y, width, height, env, transparent](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result)) {
                                char errorMsg[256];
                                sprintf_s(errorMsg, "ERROR: Failed to create WebView2 controller, HRESULT: 0x%08X", result);
                                ::log(errorMsg);
                                view->setCreationFailed(true);
                                return result;
                            }
                            
                            
                            // Controller setup with composition fallback
                            ComPtr<ICoreWebView2Controller> ctrl(controller);
                            ComPtr<ICoreWebView2> webview;
                            ctrl->get_CoreWebView2(&webview);
                            
                            view->setController(ctrl);
                            view->setWebView(webview);
                            
                            // Try to get composition controller interface if available
                            ComPtr<ICoreWebView2CompositionController> compCtrl;
                            HRESULT compResult = ctrl->QueryInterface(IID_PPV_ARGS(&compCtrl));
                            if (SUCCEEDED(compResult) && compCtrl) {
                                view->setCompositionController(compCtrl);
                                // ::log("[WebView2] Composition controller interface available");
                            } else {
                            }

                            // Store container HWND for masking support
                            view->setContainerHwnd(container->GetHwnd());

                            // Set up JavaScript bridge objects
                            view->setupJavaScriptBridges();
                            
                            // Set bounds and visibility
                            RECT bounds = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};
                            ctrl->put_Bounds(bounds);

                            // Make sure the controller is visible
                            ctrl->put_IsVisible(TRUE);

                            // Set transparent background if requested
                            if (transparent) {
                                ComPtr<ICoreWebView2Controller2> ctrl2;
                                HRESULT hr = ctrl->QueryInterface(IID_PPV_ARGS(&ctrl2));
                                if (SUCCEEDED(hr) && ctrl2) {
                                    // Set background color to transparent (0x00000000 = ARGB fully transparent)
                                    COREWEBVIEW2_COLOR transparentColor = {0, 0, 0, 0}; // A, R, G, B
                                    ctrl2->put_DefaultBackgroundColor(transparentColor);
                                }
                            }

                            // Capture webviewId and handler for event handlers
                            uint32_t capturedWebviewId = view->webviewId;
                            WebviewEventHandler capturedHandler = view->webviewEventHandler;

                            // Add views:// scheme support - TEST ADDITION
                            webview->AddWebResourceRequestedFilter(L"views://*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

                            // Set up WebResourceRequested event handler for views:// scheme
                            webview->add_WebResourceRequested(
                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [env, capturedWebviewId, capturedHandler](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                        // ::log("[WebView2] WebResourceRequested event triggered");
                                        ComPtr<ICoreWebView2WebResourceRequest> request;
                                        args->get_Request(&request);
                                        
                                        LPWSTR uri;
                                        request->get_Uri(&uri);
                                        
                                        // Safe string conversion
                                        std::string uriStr;
                                        int size = WideCharToMultiByte(CP_UTF8, 0, uri, -1, nullptr, 0, nullptr, nullptr);
                                        if (size > 0) {
                                            uriStr.resize(size - 1);
                                            WideCharToMultiByte(CP_UTF8, 0, uri, -1, &uriStr[0], size, nullptr, nullptr);
                                        }
                                        
                                        // ::log("[WebView2] Request URI converted successfully");
                                        
                                        if (uriStr.substr(0, 8) == "views://") {
                                            std::string filePath = uriStr.substr(8);
                                            std::string content;

                                            // Check for internal/index.html (inline HTML content)
                                            if (filePath == "internal/index.html") {
                                                const char* htmlContent = getWebviewHTMLContent(capturedWebviewId);
                                                if (htmlContent && strlen(htmlContent) > 0) {
                                                    content = std::string(htmlContent);
                                                    free((void*)htmlContent);
                                                } else {
                                                    content = "<html><body><h1>No content set</h1></body></html>";
                                                }
                                            } else {
                                                content = loadViewsFile(filePath);
                                            }

                                            if (!content.empty()) {
                                                // ::log("[WebView2] Loaded views file content, creating response");

                                                // Create response (simplified)
                                                std::string mimeType = "text/html";
                                                bool isDocument = false;
                                                if (filePath.find(".js") != std::string::npos) mimeType = "application/javascript";
                                                else if (filePath.find(".css") != std::string::npos) mimeType = "text/css";
                                                else if (filePath.find(".png") != std::string::npos) mimeType = "image/png";
                                                else {
                                                    isDocument = true; // HTML document
                                                }

                                                // For HTML documents (main frame navigation), fire navigation events manually
                                                // since WebResourceRequested bypasses NavigationStarting/NavigationCompleted
                                                // These events are already fired in loadURL, so we don't need to fire them here
                                                // This block can be removed if we want to clean up
                                                if (isDocument && capturedHandler) {
                                                    // Events are now fired in loadURL() for consistency
                                                    // This avoids duplicate events and ensures proper timing
                                                }

                                                std::wstring wMimeType(mimeType.begin(), mimeType.end());

                                                // Create memory stream
                                                ComPtr<IStream> contentStream;
                                                HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, content.size());
                                                if (hGlobal) {
                                                    void* pData = GlobalLock(hGlobal);
                                                    memcpy(pData, content.c_str(), content.size());
                                                    GlobalUnlock(hGlobal);
                                                    CreateStreamOnHGlobal(hGlobal, TRUE, &contentStream);
                                                }

                                                std::wstring headers = L"Content-Type: " + wMimeType + L"\r\nAccess-Control-Allow-Origin: *";

                                                ComPtr<ICoreWebView2WebResourceResponse> response;
                                                env->CreateWebResourceResponse(
                                                    contentStream.Get(),
                                                    200,
                                                    L"OK",
                                                    headers.c_str(),
                                                    &response);

                                                args->put_Response(response.Get());
                                                // ::log("[WebView2] Successfully served views:// file");
                                            }
                                        }
                                        
                                        CoTaskMemFree(uri);
                                        return S_OK;
                                    }).Get(),
                                nullptr);
                            
                            
                            // Add preload scripts
                            std::string combinedScript;
                            if (!view->electrobunScript.empty()) {
                                combinedScript += view->electrobunScript;
                            }
                            if (!view->customScript.empty()) {
                                if (!combinedScript.empty()) {
                                    combinedScript += "\n";
                                }
                                // Resolve views:// URLs to file content (matching macOS behavior)
                                if (view->customScript.substr(0, 8) == "views://") {
                                    std::string fileContent = loadViewsFile(view->customScript.substr(8));
                                    if (!fileContent.empty()) {
                                        combinedScript += fileContent;
                                    } else {
                                        std::cout << "[WebView2] Could not read custom preload script from: " << view->customScript << std::endl;
                                    }
                                } else {
                                    combinedScript += view->customScript;
                                }
                            }

                            // Add Ctrl+Click detection and navigation rules handler
                            webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [capturedWebviewId, capturedHandler](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        printf("[WebView2] NavigationStarting fired for webview %u\n", capturedWebviewId);
                                        // Get URL first - needed for both ctrl+click and navigation rules
                                        wchar_t* uriWStr = nullptr;
                                        args->get_Uri(&uriWStr);
                                        std::string uri;
                                        if (uriWStr) {
                                            int size = WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, nullptr, 0, nullptr, nullptr);
                                            if (size > 0) {
                                                uri.resize(size - 1);
                                                WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, &uri[0], size, nullptr, nullptr);
                                            }
                                            CoTaskMemFree(uriWStr);
                                        }

                                        // Check if Ctrl key is held
                                        SHORT ctrlState = GetKeyState(VK_CONTROL);
                                        bool isCtrlHeld = (ctrlState & 0x8000) != 0;

                                        // Handle Ctrl+click for new window
                                        if (isCtrlHeld && capturedHandler) {
                                            printf("[WebView2 NavigationStarting] Ctrl+click detected, url=%s\n", uri.c_str());

                                            // Debounce: ignore ctrl+click navigations within 500ms
                                            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;

                                            if (now - WebView2View::lastCtrlClickTime >= 0.5) {
                                                WebView2View::lastCtrlClickTime = now;

                                                // Escape URL for JSON
                                                std::string escapedUrl;
                                                for (char c : uri) {
                                                    switch (c) {
                                                        case '"': escapedUrl += "\\\""; break;
                                                        case '\\': escapedUrl += "\\\\"; break;
                                                        default: escapedUrl += c; break;
                                                    }
                                                }

                                                std::string eventData = "{\"url\":\"" + escapedUrl +
                                                                       "\",\"isCmdClick\":true,\"modifierFlags\":0}";
                                                printf("[WebView2 NavigationStarting] Firing new-window-open: %s\n", eventData.c_str());
                                                capturedHandler(capturedWebviewId, _strdup("new-window-open"), _strdup(eventData.c_str()));

                                                args->put_Cancel(TRUE);
                                                return S_OK;
                                            } else {
                                                printf("[WebView2 NavigationStarting] Debounced\n");
                                            }
                                        }

                                        // Check navigation rules synchronously from native-stored rules
                                        bool shouldAllow = true;
                                        {
                                            std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
                                            auto it = g_abstractViews.find(capturedWebviewId);
                                            if (it != g_abstractViews.end() && it->second != nullptr) {
                                                shouldAllow = it->second->shouldAllowNavigationToURL(uri);
                                            }
                                        }

                                        // Fire will-navigate event with allowed status
                                        if (capturedHandler) {
                                            // Escape URL for JSON
                                            std::string escapedUrl;
                                            for (char c : uri) {
                                                switch (c) {
                                                    case '"': escapedUrl += "\\\""; break;
                                                    case '\\': escapedUrl += "\\\\"; break;
                                                    default: escapedUrl += c; break;
                                                }
                                            }
                                            std::string eventData = "{\"url\":\"" + escapedUrl + "\",\"allowed\":" +
                                                                   (shouldAllow ? "true" : "false") + "}";
                                            capturedHandler(capturedWebviewId, _strdup("will-navigate"), _strdup(eventData.c_str()));
                                        }

                                        // Cancel navigation if not allowed
                                        if (!shouldAllow) {
                                            args->put_Cancel(TRUE);
                                        }

                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // Add NavigationCompleted handler for did-navigate event
                            webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [capturedWebviewId, capturedHandler](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        printf("[WebView2] NavigationCompleted fired for webview %u\n", capturedWebviewId);
                                        // Get current URL
                                        wchar_t* uriWStr = nullptr;
                                        sender->get_Source(&uriWStr);
                                        std::string uri;
                                        if (uriWStr) {
                                            int size = WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, nullptr, 0, nullptr, nullptr);
                                            if (size > 0) {
                                                uri.resize(size - 1);
                                                WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, &uri[0], size, nullptr, nullptr);
                                            }
                                            CoTaskMemFree(uriWStr);
                                        }

                                        // Fire did-navigate event
                                        if (capturedHandler && !uri.empty()) {
                                            // Escape URL for JSON
                                            std::string escapedUrl;
                                            for (char c : uri) {
                                                switch (c) {
                                                    case '"': escapedUrl += "\\\""; break;
                                                    case '\\': escapedUrl += "\\\\"; break;
                                                    default: escapedUrl += c; break;
                                                }
                                            }
                                            std::string eventData = "{\"url\":\"" + escapedUrl + "\"}";
                                            capturedHandler(capturedWebviewId, _strdup("did-navigate"), _strdup(eventData.c_str()));
                                        }

                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            if (!combinedScript.empty()) {
                                std::wstring wScript(combinedScript.begin(), combinedScript.end());
                                webview->AddScriptToExecuteOnDocumentCreated(wScript.c_str(), nullptr);

                                // NOTE: Do NOT re-run the preload via NavigationStarting + ExecuteScript.
                                // AddScriptToExecuteOnDocumentCreated already handles this correctly.
                                // Re-running the preload after NavigationCompleted (when queued ExecuteScript
                                // fires) would replace handlers and reset pendingRequests, breaking
                                // any in-flight internal bridge requests.

                                // Add permission request handler
                                webview->add_PermissionRequested(
                                    Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                        [](ICoreWebView2* sender, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                            COREWEBVIEW2_PERMISSION_KIND kind;
                                            args->get_PermissionKind(&kind);
                                            
                                            wchar_t* uriWStr = nullptr;
                                            args->get_Uri(&uriWStr);
                                            
                                            std::string uri;
                                            if (uriWStr) {
                                                int size = WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, nullptr, 0, nullptr, nullptr);
                                                if (size > 0) {
                                                    uri.resize(size - 1);
                                                    WideCharToMultiByte(CP_UTF8, 0, uriWStr, -1, &uri[0], size, nullptr, nullptr);
                                                }
                                                CoTaskMemFree(uriWStr);
                                            }
                                            
                                            std::string origin = getOriginFromUrl(uri);
                                            PermissionType permType = PermissionType::OTHER;
                                            std::string permissionName = "Permission";
                                            
                                            // Determine permission type
                                            switch (kind) {
                                                case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
                                                case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
                                                    permType = PermissionType::USER_MEDIA;
                                                    permissionName = "Camera & Microphone Access";
                                                    break;
                                                case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
                                                    permType = PermissionType::GEOLOCATION;
                                                    permissionName = "Location Access";
                                                    break;
                                                case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
                                                    permType = PermissionType::NOTIFICATIONS;
                                                    permissionName = "Notification Permission";
                                                    break;
                                                default:
                                                    permType = PermissionType::OTHER;
                                                    permissionName = "Permission Request";
                                                    break;
                                            }
                                            
                                            printf("WebView2: %s requested for %s\n", permissionName.c_str(), origin.c_str());
                                            
                                            // Check cache first
                                            PermissionStatus cachedStatus = getPermissionFromCache(origin, permType);
                                            
                                            if (cachedStatus == PermissionStatus::ALLOWED) {
                                                printf("WebView2: Using cached permission: User previously allowed %s for %s\n", permissionName.c_str(), origin.c_str());
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                                return S_OK;
                                            } else if (cachedStatus == PermissionStatus::DENIED) {
                                                printf("WebView2: Using cached permission: User previously blocked %s for %s\n", permissionName.c_str(), origin.c_str());
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                                return S_OK;
                                            }
                                            
                                            // No cached permission, show dialog
                                            printf("WebView2: No cached permission found for %s, showing dialog\n", origin.c_str());
                                            
                                            std::string message = "This page wants to access ";
                                            switch (kind) {
                                                case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
                                                    message += "your camera.\n\nDo you want to allow this?";
                                                    break;
                                                case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
                                                    message += "your microphone.\n\nDo you want to allow this?";
                                                    break;
                                                case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
                                                    message += "your location.\n\nDo you want to allow this?";
                                                    break;
                                                case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
                                                    message += "show notifications.\n\nDo you want to allow this?";
                                                    break;
                                                default:
                                                    message += "additional permissions.\n\nDo you want to allow this?";
                                                    break;
                                            }
                                            
                                            // Show Windows message box
                                            int result = MessageBoxA(
                                                nullptr,
                                                message.c_str(),
                                                permissionName.c_str(),
                                                MB_YESNO | MB_ICONQUESTION | MB_TOPMOST
                                            );
                                            
                                            // Handle response and cache the decision
                                            if (result == IDYES) {
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                                cachePermission(origin, permType, PermissionStatus::ALLOWED);
                                                printf("WebView2: User allowed %s for %s (cached)\n", permissionName.c_str(), origin.c_str());
                                            } else {
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                                cachePermission(origin, permType, PermissionStatus::DENIED);
                                                printf("WebView2: User blocked %s for %s (cached)\n", permissionName.c_str(), origin.c_str());
                                            }
                                            
                                            return S_OK;
                                        }).Get(),
                                    nullptr);
                                
                                // Add file dialog handler for <input type="file">
                                // Note: WebView2 generally handles file dialogs automatically,
                                // but we can enhance support by enabling the necessary permissions
                                // in the AdditionalBrowserArguments (already done above with --disable-web-security)

                                // Add download handler - requires ICoreWebView2_4
                                Microsoft::WRL::ComPtr<ICoreWebView2_4> webview4;
                                if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview4)))) {
                                    webview4->add_DownloadStarting(
                                        Callback<ICoreWebView2DownloadStartingEventHandler>(
                                            [](ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                                                printf("WebView2: Download starting\n");

                                                // Get the download operation
                                                Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> downloadOp;
                                                args->get_DownloadOperation(&downloadOp);

                                                if (downloadOp) {
                                                    // Get suggested filename from URI
                                                    wchar_t* uriWStr = nullptr;
                                                    downloadOp->get_Uri(&uriWStr);

                                                    // Get the content disposition filename if available
                                                    wchar_t* contentDisp = nullptr;
                                                    downloadOp->get_ContentDisposition(&contentDisp);

                                                    // Get Downloads folder path
                                                    wchar_t* downloadsPath = nullptr;
                                                    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &downloadsPath);

                                                    if (SUCCEEDED(hr) && downloadsPath) {
                                                        // Get the suggested filename from the args
                                                        wchar_t* resultFilePath = nullptr;
                                                        args->get_ResultFilePath(&resultFilePath);

                                                        std::wstring suggestedName;
                                                        if (resultFilePath) {
                                                            // Extract just the filename from the full path
                                                            std::wstring fullPath(resultFilePath);
                                                            size_t lastSlash = fullPath.find_last_of(L"\\/");
                                                            if (lastSlash != std::wstring::npos) {
                                                                suggestedName = fullPath.substr(lastSlash + 1);
                                                            } else {
                                                                suggestedName = fullPath;
                                                            }
                                                            CoTaskMemFree(resultFilePath);
                                                        } else if (uriWStr) {
                                                            // Extract filename from URI
                                                            std::wstring uri(uriWStr);
                                                            size_t lastSlash = uri.find_last_of(L'/');
                                                            size_t queryStart = uri.find(L'?');
                                                            if (lastSlash != std::wstring::npos) {
                                                                if (queryStart != std::wstring::npos && queryStart > lastSlash) {
                                                                    suggestedName = uri.substr(lastSlash + 1, queryStart - lastSlash - 1);
                                                                } else {
                                                                    suggestedName = uri.substr(lastSlash + 1);
                                                                }
                                                            } else {
                                                                suggestedName = L"download";
                                                            }
                                                        } else {
                                                            suggestedName = L"download";
                                                        }

                                                        // Build full destination path
                                                        std::wstring destPath = downloadsPath;
                                                        destPath += L"\\";
                                                        destPath += suggestedName;

                                                        // Handle duplicate filenames
                                                        std::wstring basePath = destPath;
                                                        std::wstring extension;
                                                        size_t dotPos = destPath.find_last_of(L'.');
                                                        size_t slashPos = destPath.find_last_of(L"\\/");
                                                        if (dotPos != std::wstring::npos && (slashPos == std::wstring::npos || dotPos > slashPos)) {
                                                            basePath = destPath.substr(0, dotPos);
                                                            extension = destPath.substr(dotPos);
                                                        }

                                                        int counter = 1;
                                                        while (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                                                            destPath = basePath + L" (" + std::to_wstring(counter) + L")" + extension;
                                                            counter++;
                                                        }

                                                        // Set the download destination
                                                        args->put_ResultFilePath(destPath.c_str());

                                                        // Hide the default download dialog
                                                        args->put_Handled(TRUE);

                                                        // Log the download
                                                        int size = WideCharToMultiByte(CP_UTF8, 0, destPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                                                        if (size > 0) {
                                                            std::string utf8Path(size - 1, '\0');
                                                            WideCharToMultiByte(CP_UTF8, 0, destPath.c_str(), -1, &utf8Path[0], size, nullptr, nullptr);
                                                            printf("WebView2: Downloading to %s\n", utf8Path.c_str());
                                                        }

                                                        CoTaskMemFree(downloadsPath);
                                                    } else {
                                                        printf("WebView2: Could not get Downloads folder, using default behavior\n");
                                                    }

                                                    if (uriWStr) CoTaskMemFree(uriWStr);
                                                    if (contentDisp) CoTaskMemFree(contentDisp);
                                                }

                                                return S_OK;
                                            }).Get(),
                                        nullptr);
                                    printf("WebView2: Download handler registered successfully\n");
                                } else {
                                    printf("WebView2: Warning - Could not get ICoreWebView2_4 interface for download handling\n");
                                }

                            } else {
                            }
                            
                            // Navigate to URL or load pending HTML
                            if (!view->pendingHtml.empty()) {
                                std::wstring html(view->pendingHtml.begin(), view->pendingHtml.end());
                                webview->NavigateToString(html.c_str());
                                view->pendingHtml.clear();
                            } else if (!view->pendingUrl.empty()) {
                                view->loadURL(view->pendingUrl.c_str());
                            }
                            
                            view->setCreationComplete(true);
                            container->AddAbstractView(view);

                            // Apply deferred initial transparent/passthrough state now that view is ready
                            if (view->pendingStartTransparent) {
                                view->setTransparent(true);
                                view->pendingStartTransparent = false;
                            }
                            if (view->pendingStartPassthrough) {
                                view->setPassthrough(true);
                                view->pendingStartPassthrough = false;
                            }

                            // Register in global AbstractView map for navigation rules
                            {
                                std::lock_guard<std::mutex> lock(g_abstractViewsMutex);
                                g_abstractViews[view->webviewId] = view.get();
                            }

                            // Store WebView2View in global map for JavaScript execution
                            HWND containerHwnd = container->GetHwnd();
                            g_webview2Views[containerHwnd] = view.get();


                            return S_OK;
                        }).Get());
            });
        
        
        
        // Create WebView2 environment with custom scheme support
        try {
            auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
            options->put_AdditionalBrowserArguments(L"--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --allow-insecure-localhost --disable-web-security");

            // Get the interface that supports custom scheme registration
            Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions4> options4;
            if (SUCCEEDED(options.As(&options4))) {
                // ::log("Setting up views:// custom scheme registration");

                // Set allowed origins for the custom scheme
                const WCHAR* allowedOrigins[1] = {L"*"};

                // Create custom scheme registration for "views"
                auto viewsSchemeRegistration = Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(L"views");
                viewsSchemeRegistration->put_TreatAsSecure(TRUE);
                viewsSchemeRegistration->put_HasAuthorityComponent(TRUE); // This allows views://host/path format
                viewsSchemeRegistration->SetAllowedOrigins(1, allowedOrigins);

                // Set the custom scheme registrations
                ICoreWebView2CustomSchemeRegistration* registrations[1] = {
                    viewsSchemeRegistration.Get()
                };

                HRESULT schemeResult = options4->SetCustomSchemeRegistrations(1, registrations);

                if (SUCCEEDED(schemeResult)) {
                    // ::log("views:// custom scheme registration set successfully");
                } else {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to set views:// custom scheme registration: 0x%lx", schemeResult);
                    ::log(errorMsg);
                }
            } else {
                ::log("ERROR: Failed to get ICoreWebView2EnvironmentOptions4 interface for custom scheme registration");
            }

            // Create user data folder path based on partition
            // Build path with identifier/channel structure (consistent with CLI and updater)
            std::wstring userDataFolder;
            char* localAppData = getenv("LOCALAPPDATA");
            if (localAppData) {
                std::string userDataPath = buildAppDataPath(localAppData, g_electrobunIdentifier, g_electrobunChannel, "WebView2", '\\');

                // Handle partition-specific storage
                if (!partitionStr.empty()) {
                    bool isPersistent = partitionStr.substr(0, 8) == "persist:";
                    if (isPersistent) {
                        // Persistent partition: use named subfolder
                        std::string partitionName = partitionStr.substr(8);
                        userDataPath += "\\Partitions\\" + partitionName;
                    } else {
                        // Ephemeral partition: use unique temp folder per webview
                        // Note: WebView2 doesn't support true ephemeral sessions,
                        // so we use a timestamped folder that gets cleaned up
                        userDataPath += "\\Ephemeral\\" + std::to_string(view->webviewId);
                    }
                }
                // If no partition specified, use default WebView2 folder (shared)

                // Convert to wide string for WebView2 API
                int wideSize = MultiByteToWideChar(CP_UTF8, 0, userDataPath.c_str(), -1, nullptr, 0);
                if (wideSize > 0) {
                    userDataFolder.resize(wideSize - 1);
                    MultiByteToWideChar(CP_UTF8, 0, userDataPath.c_str(), -1, &userDataFolder[0], wideSize);
                }

                // Create directory if it doesn't exist
                // Use SHCreateDirectoryExW for recursive creation
                SHCreateDirectoryExW(NULL, userDataFolder.c_str(), NULL);
            }

            // Use partition-specific user data folder (nullptr if empty for default behavior)
            LPCWSTR userDataFolderPtr = userDataFolder.empty() ? nullptr : userDataFolder.c_str();

            HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolderPtr, options.Get(), environmentCompletedHandler.Get());
            
            
            if (FAILED(hr)) {
                char errorMsg[256];
                sprintf_s(errorMsg, "ERROR: CreateCoreWebView2EnvironmentWithOptions failed with HRESULT: 0x%08X", hr);
                ::log(errorMsg);
            } else {
                // ::log("[WebView2] CreateCoreWebView2EnvironmentWithOptions succeeded");
            }
        } catch (const std::exception& e) {
            std::cout << "[WebView2] Exception in WebView2 creation: " << e.what() << std::endl;
        } catch (...) {
            std::cout << "[WebView2] Unknown exception in WebView2 creation" << std::endl;
        }
    });
    
    return view;
}

// Console control handler for graceful shutdown
BOOL WINAPI ConsoleControlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            std::cout << "[shutdown] Received console shutdown signal" << std::endl;

            if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                // Route through bun's quit sequence for proper beforeQuit handling
                g_quitRequestedHandler();
                // Wait for orderly shutdown (Windows gives ~5s for CTRL_CLOSE_EVENT)
                int waited = 0;
                while (!g_shutdownComplete.load() && waited < 4000) {
                    Sleep(10);
                    waited += 10;
                }
            } else {
                // Fallback: direct shutdown - post WM_QUIT to exit the message loop
                PostQuitMessage(0);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

extern "C" {

ELECTROBUN_EXPORT void startEventLoop(const char* identifier, const char* name, const char* channel) {
    g_mainThreadId = GetCurrentThreadId();

    if (identifier && identifier[0]) {
        g_electrobunIdentifier = std::string(identifier);
    }
    if (name && name[0]) {
        g_electrobunName = std::string(name);
    }
    if (channel && channel[0]) {
        g_electrobunChannel = std::string(channel);
    }

    // Set up console control handler for graceful shutdown on Ctrl+C
    if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE)) {
        std::cout << "[shutdown] Warning: Failed to set console control handler" << std::endl;
    }
    
    // Create a hidden message-only window for dispatching
    WNDCLASSA wc = {0};  // Use ANSI version
    wc.lpfnWndProc = MessageWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MessageWindowClass";  // Use ANSI string
    RegisterClassA(&wc);  // Use ANSI version
    
    HWND messageWindow = CreateWindowA(  // Use ANSI version
        "MessageWindowClass",  // Use ANSI string
        "", 
        0, 0, 0, 0, 0,
        HWND_MESSAGE, // This makes it a message-only window
        NULL, 
        GetModuleHandle(NULL), 
        NULL
    );
    
    // Initialize the dispatcher
    MainThreadDispatcher::initialize(messageWindow);
    
    // Standard Windows message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // Check for menu accelerators first
        if (g_hAccelTable && TranslateAccelerator(msg.hwnd, g_hAccelTable, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_shutdownComplete.store(true);
}


ELECTROBUN_EXPORT void stopEventLoop() {
    if (g_eventLoopStopping.exchange(true)) {
        return;
    }

    std::cout << "[stopEventLoop] Initiating clean event loop exit" << std::endl;

    if (g_mainThreadId != 0) {
            PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
        }
}

ELECTROBUN_EXPORT void killApp() {
    // Deprecated - delegates to stopEventLoop for backward compatibility
    stopEventLoop();
}

ELECTROBUN_EXPORT void waitForShutdownComplete(int timeoutMs) {
    int waited = 0;
    while (!g_shutdownComplete.load() && waited < timeoutMs) {
        Sleep(10);
        waited += 10;
    }
}

ELECTROBUN_EXPORT void forceExit(int code) {
    _exit(code);
}

ELECTROBUN_EXPORT void setQuitRequestedHandler(QuitRequestedHandler handler) {
    g_quitRequestedHandler = handler;
}

ELECTROBUN_EXPORT void shutdownApplication() {
    // Deprecated - use stopEventLoop() instead
    stopEventLoop();
}

// Global flags set by setNextWebviewFlags, consumed by initWebview
static struct {
    bool startTransparent;
    bool startPassthrough;
} g_nextWebviewFlags = {false, false};

ELECTROBUN_EXPORT void setNextWebviewFlags(bool startTransparent, bool startPassthrough) {
    g_nextWebviewFlags.startTransparent = startTransparent;
    g_nextWebviewFlags.startPassthrough = startPassthrough;
}

// Clean, elegant initWebview function - Windows version matching Mac pattern
ELECTROBUN_EXPORT AbstractView* initWebview(uint32_t webviewId,
                         NSWindow *window,  // Actually HWND on Windows
                         const char *renderer,
                         const char *url,
                         double x, double y,
                         double width, double height,
                         bool autoResize,
                         const char *partitionIdentifier,
                         DecideNavigationCallback navigationCallback,
                         WebviewEventHandler webviewEventHandler,
                         HandlePostMessage eventBridgeHandler,
                         HandlePostMessage bunBridgeHandler,
                         HandlePostMessage internalBridgeHandler,
                         const char *electrobunPreloadScript,
                         const char *customPreloadScript,
                         const char *viewsRoot,
                         bool transparent,
                         bool sandbox) {

    // Read and clear pre-set flags
    bool startTransparent = g_nextWebviewFlags.startTransparent;
    bool startPassthrough = g_nextWebviewFlags.startPassthrough;
    g_nextWebviewFlags = {false, false};

    // Serialize webview creation
    std::lock_guard<std::mutex> lock(g_webviewCreationMutex);


    HWND hwnd = reinterpret_cast<HWND>(window);

    // Factory pattern - choose implementation based on renderer
    AbstractView* view = nullptr;

    auto webview2View = createWebView2View(webviewId, hwnd, url, x, y, width, height, autoResize,
                                          partitionIdentifier, navigationCallback, webviewEventHandler,
                                          eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                                          electrobunPreloadScript, customPreloadScript, transparent, sandbox);
    view = webview2View.get();

    // Note: Object lifetime is managed by the ContainerView which holds shared_ptr references
    // The factories add the views to containers, so they remain alive after this function returns

    // Store initial state flags — applied later when the view is fully initialized
    // (browser/HWND may not be available yet due to async creation)
    if (view) {
        view->pendingStartTransparent = startTransparent;
        view->pendingStartPassthrough = startPassthrough;
    }

    return view;

}

ELECTROBUN_EXPORT MyScriptMessageHandlerWithReply* addScriptMessageHandlerWithReply(WKWebView *webView,
                                                              uint32_t webviewId,
                                                              const char *name,
                                                              HandlePostMessageWithReply callback) {
    // Stub implementation
    MyScriptMessageHandlerWithReply* handler = new MyScriptMessageHandlerWithReply();
    handler->zigCallback = callback;
    handler->webviewId = webviewId;
    return handler;
}
ELECTROBUN_EXPORT void loadURLInWebView(AbstractView *abstractView, const char *urlString) {
    if (!abstractView || !urlString) {
        ::log("ERROR: Invalid parameters passed to loadURLInWebView");
        return;
    }
    
    // Use virtual method which handles threading and implementation details
    
    abstractView->loadURL(urlString);
}

ELECTROBUN_EXPORT void loadHTMLInWebView(AbstractView *abstractView, const char *htmlString) {
    if (!abstractView || !htmlString) {
        ::log("ERROR: Invalid parameters passed to loadHTMLInWebView");
        return;
    }

    abstractView->loadHTML(htmlString);
}

ELECTROBUN_EXPORT void webviewGoBack(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewGoBack");
        return;
    }
    
    abstractView->goBack();
}

ELECTROBUN_EXPORT void webviewGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewGoForward");
        return;
    }
    
    abstractView->goForward();
}

ELECTROBUN_EXPORT void webviewReload(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewReload");
        return;
    }
    
    abstractView->reload();
}

ELECTROBUN_EXPORT void webviewRemove(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView in webviewRemove");
        return;
    }

    abstractView->remove();
}

ELECTROBUN_EXPORT BOOL webviewCanGoBack(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewCanGoBack");
        return FALSE;
    }
    
    return abstractView->canGoBack();
}

ELECTROBUN_EXPORT BOOL webviewCanGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView or webview in webviewCanGoForward");
        return FALSE;
    }
    
    return abstractView->canGoForward();
}

ELECTROBUN_EXPORT void evaluateJavaScriptWithNoCompletion(AbstractView *abstractView, const char *script) {
    if (!abstractView || !script) {
        ::log("ERROR: Invalid parameters passed to evaluateJavaScriptWithNoCompletion");
        return;
    }

    abstractView->evaluateJavaScriptWithNoCompletion(script);
    
}

ELECTROBUN_EXPORT void testFFI(void *ptr) {
    // Stub implementation
}

ELECTROBUN_EXPORT void callAsyncJavaScript(const char *messageId,
                        AbstractView *abstractView,
                        const char *jsString,
                        uint32_t webviewId,
                        uint32_t hostWebviewId,
                        callAsyncJavascriptCompletionHandler completionHandler) {
    // Stub implementation
    if (completionHandler) {
        completionHandler(messageId, webviewId, hostWebviewId, "\"\"");
    }
}

ELECTROBUN_EXPORT void addPreloadScriptToWebView(AbstractView *abstractView, const char *scriptContent, BOOL forMainFrameOnly) {
    if (abstractView && scriptContent) {
        MainThreadDispatcher::dispatch_sync([abstractView, scriptContent]() {
            abstractView->addPreloadScriptToWebView(scriptContent);
        });
    }
}

ELECTROBUN_EXPORT void updatePreloadScriptToWebView(AbstractView *abstractView,
                                 const char *scriptIdentifier,
                                 const char *scriptContent,
                                 BOOL forMainFrameOnly) {
    if (abstractView && scriptContent) {
        MainThreadDispatcher::dispatch_sync([abstractView, scriptContent]() {
            abstractView->updateCustomPreloadScript(scriptContent);
        });
    }
}

ELECTROBUN_EXPORT void invokeDecisionHandler(void (*decisionHandler)(int), int policy) {
    // Stub implementation
    if (decisionHandler) {
        decisionHandler(policy);
    }
}

ELECTROBUN_EXPORT const char* getUrlFromNavigationAction(void *navigationAction) {
    // Stub implementation
    static const char* defaultUrl = "about:blank";
    return defaultUrl;
}

ELECTROBUN_EXPORT const char* getBodyFromScriptMessage(void *message) {
    // Stub implementation
    static const char* emptyString = "";
    return emptyString;
}

ELECTROBUN_EXPORT void webviewSetTransparent(AbstractView *abstractView, BOOL transparent) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, transparent]() {
            abstractView->setTransparent(transparent);
        });
    }
}

ELECTROBUN_EXPORT void webviewSetPassthrough(AbstractView *abstractView, BOOL enablePassthrough) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, enablePassthrough]() {
            abstractView->setPassthrough(enablePassthrough);
        });
    }
}

ELECTROBUN_EXPORT void webviewSetHidden(AbstractView *abstractView, BOOL hidden) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, hidden]() {
            abstractView->setTransparent(hidden);
        });
    }
}

ELECTROBUN_EXPORT void setWebviewNavigationRules(AbstractView *abstractView, const char *rulesJson) {
    if (abstractView) {
        // UI operations must be performed on the main thread
        MainThreadDispatcher::dispatch_sync([abstractView, rulesJson]() {
            abstractView->setNavigationRulesFromJSON(rulesJson);
        });
    }
}

ELECTROBUN_EXPORT void webviewFindInPage(AbstractView *abstractView, const char *searchText, bool forward, bool matchCase) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView, searchText, forward, matchCase]() {
            abstractView->findInPage(searchText, forward, matchCase);
        });
    }
}

// Remote DevTools helper functions for Windows
void openRemoteDevTools(uint32_t webviewId) {
    // TODO: Implement remote debugger approach for Windows
}

void closeRemoteDevTools(uint32_t webviewId) {
    // TODO: Close remote debugger window
}

void toggleRemoteDevTools(uint32_t webviewId) {
    // TODO: Toggle remote debugger window
    openRemoteDevTools(webviewId);
}

ELECTROBUN_EXPORT void webviewStopFind(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->stopFindInPage();
        });
    }
}

ELECTROBUN_EXPORT void webviewOpenDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->openDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewCloseDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->closeDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewToggleDevTools(AbstractView *abstractView) {
    if (abstractView) {
        MainThreadDispatcher::dispatch_sync([abstractView]() {
            abstractView->toggleDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewSetPageZoom(AbstractView *abstractView, double zoomLevel) {
    // pageZoom is WebKit-specific, not available on Windows
    // TODO: implement WebView2 zoom if needed
}

ELECTROBUN_EXPORT double webviewGetPageZoom(AbstractView *abstractView) {
    // pageZoom is WebKit-specific, not available on Windows
    return 1.0;
}

ELECTROBUN_EXPORT NSRect createNSRectWrapper(double x, double y, double width, double height) {
    // Stub implementation
    NSRect rect = {x, y, width, height};
    return rect;
}

ELECTROBUN_EXPORT NSWindow* createNSWindowWithFrameAndStyle(uint32_t windowId,
                                         createNSWindowWithFrameAndStyleParams config,
                                         WindowCloseHandler zigCloseHandler,
                                         WindowMoveHandler zigMoveHandler,
                                         WindowResizeHandler zigResizeHandler,
                                         WindowFocusHandler zigFocusHandler,
                                         WindowBlurHandler zigBlurHandler,
                                         WindowKeyHandler zigKeyHandler) {
    // Stub implementation
    return new NSWindow();
}

ELECTROBUN_EXPORT void testFFI2(void (*completionHandler)()) {
    // Stub implementation
    if (completionHandler) {
        completionHandler();
    }
}

ELECTROBUN_EXPORT HWND createWindowWithFrameAndStyleFromWorker(
    uint32_t windowId,
    double x, double y,
    double width, double height,
    uint32_t styleMask,
    const char* titleBarStyle,
    bool transparent,
    WindowCloseHandler zigCloseHandler,
    WindowMoveHandler zigMoveHandler,
    WindowResizeHandler zigResizeHandler,
    WindowFocusHandler zigFocusHandler,
    WindowBlurHandler zigBlurHandler,
    WindowKeyHandler zigKeyHandler) {

    // Everything GUI-related needs to be dispatched to main thread
    HWND hwnd = MainThreadDispatcher::dispatch_sync([=]() -> HWND {

        // Register window class with our custom procedure
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};  // Use ANSI version
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "BasicWindowClass";  // Use ANSI string
            RegisterClassA(&wc);  // Use ANSI version
            classRegistered = true;
        }

        // Create window data structure to store callbacks
        WindowData* data = (WindowData*)malloc(sizeof(WindowData));
        if (!data) return NULL;

        data->windowId = windowId;
        data->closeHandler = zigCloseHandler;
        data->moveHandler = zigMoveHandler;
        data->resizeHandler = zigResizeHandler;
        data->focusHandler = zigFocusHandler;
        data->blurHandler = zigBlurHandler;
        data->keyHandler = zigKeyHandler;

        // Map style mask to Windows style
        DWORD windowStyle = WS_OVERLAPPEDWINDOW; // Default
        DWORD windowExStyle = WS_EX_APPWINDOW;

        // Handle titleBarStyle options
        if (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0) {
            // "hidden" = borderless window (no titlebar, no native controls)
            // This is for completely custom chrome
            windowStyle = WS_POPUP | WS_VISIBLE;
        } else if (titleBarStyle && strcmp(titleBarStyle, "hiddenInset") == 0) {
            // "hiddenInset" = window with border but custom titlebar area
            // On Windows, we can't easily do the exact macOS inset style,
            // so we provide a borderless window with shadow for similar effect
            windowStyle = WS_POPUP | WS_VISIBLE | WS_THICKFRAME;
        }
        // else: default titleBarStyle = WS_OVERLAPPEDWINDOW (standard window)

        // Handle transparent windows
        if (transparent) {
            // For transparent windows, we need WS_EX_LAYERED to support per-pixel alpha
            windowExStyle |= WS_EX_LAYERED;
        }

        // Create the window
        HWND hwnd = CreateWindowExA(  // Use CreateWindowExA to support extended styles
            windowExStyle,
            "BasicWindowClass",  // Use ANSI string
            "",
            windowStyle,
            (int)x, (int)y,
            (int)width, (int)height,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        if (hwnd) {
            // Store our data with the window
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)data);

            // Apply transparent window background if requested
            if (transparent) {
                // For transparent windows using OSR, UpdateLayeredWindow will handle
                // the rendering with per-pixel alpha. We don't use SetLayeredWindowAttributes.
                // OSR rendering uses UpdateLayeredWindow with per-pixel alpha.
            }

            // Don't apply application menu to transparent or custom chrome windows
            // Only apply to windows with default titleBarStyle
            bool isCustomChrome = transparent ||
                                 (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0) ||
                                 (titleBarStyle && strcmp(titleBarStyle, "hiddenInset") == 0);

            if (!isCustomChrome && g_applicationMenu) {
                if (SetMenu(hwnd, g_applicationMenu)) {
                    DrawMenuBar(hwnd);
                    // char logMsg[256];
                    // sprintf_s(logMsg, "Applied application menu to new window: HWND=%p", hwnd);
                    // ::log(logMsg);
                } else {
                    ::log("Failed to apply application menu to new window");
                }
            }


            // Show the window
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        } else {
            // Clean up if window creation failed
            free(data);
        }

        return hwnd;
    });

    return hwnd;
}

ELECTROBUN_EXPORT void showWindow(void *window) {
    // On Windows, window ptr is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in showWindow");
        return;
    }
    
    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {      
        // Show the window if it's hidden
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOW);
        }
        
        // Bring window to foreground - this is more complex on Windows
        // due to foreground window restrictions
        
        // First, try the simple approach
        if (SetForegroundWindow(hwnd)) {
        } else {
            // If that fails, we need to work around Windows' foreground restrictions
            DWORD currentThreadId = GetCurrentThreadId();
            DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
            
            if (currentThreadId != foregroundThreadId) {
                // Attach to the foreground thread's input queue temporarily
                if (AttachThreadInput(currentThreadId, foregroundThreadId, TRUE)) {
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                    AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
                } else {
                    // Last resort - flash the window to get user attention
                    FLASHWINFO fwi = {0};
                    fwi.cbSize = sizeof(FLASHWINFO);
                    fwi.hwnd = hwnd;
                    fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
                    fwi.uCount = 3;
                    fwi.dwTimeout = 0;
                    FlashWindowEx(&fwi);
                    
                }
            }
        }
        
        // Ensure the window is active and focused
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        
        // Bring to top of Z-order
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, 
                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        
    });
}

ELECTROBUN_EXPORT void setWindowTitle(NSWindow *window, const char *title) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowTitle");
        return;
    }
    
    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {
        if (title && strlen(title) > 0) {
            // Convert UTF-8 to wide string for Unicode support
            int size = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
            if (size > 0) {
                std::wstring wTitle(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], size);
                
                // Set the window title
                if (SetWindowTextW(hwnd, wTitle.c_str())) {
                    
                } else {
                    DWORD error = GetLastError();
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to set window title, error: %lu", error);
                    ::log(errorMsg);
                }
            } else {
                ::log("ERROR: Failed to convert title to wide string");
            }
        } else {
            // Set empty title
            if (SetWindowTextW(hwnd, L"")) {
            } else {
                DWORD error = GetLastError();
                char errorMsg[256];
                sprintf_s(errorMsg, "Failed to clear window title, error: %lu", error);
                ::log(errorMsg);
            }
        }
    });
}

ELECTROBUN_EXPORT void closeWindow(NSWindow *window) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in closeWindow");
        return;
    }

    // Dispatch to main thread to ensure thread safety
    MainThreadDispatcher::dispatch_sync([=]() {


        // Clean up any associated container views before closing
        auto containerIt = g_containerViews.find(hwnd);
        if (containerIt != g_containerViews.end()) {
            g_containerViews.erase(containerIt);
        }

        // Send WM_CLOSE message to the window
        // This will trigger the window's close handler if one is set
        if (PostMessage(hwnd, WM_CLOSE, 0, 0)) {
        } else {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "Failed to send WM_CLOSE message, error: %lu", error);
            ::log(errorMsg);

            // If PostMessage fails, try DestroyWindow as a fallback
            ::log("Attempting DestroyWindow as fallback");
            if (DestroyWindow(hwnd)) {
            } else {
                DWORD destroyError = GetLastError();
                char destroyErrorMsg[256];
                sprintf_s(destroyErrorMsg, "DestroyWindow also failed, error: %lu", destroyError);
                ::log(destroyErrorMsg);
            }
        }
    });
}

ELECTROBUN_EXPORT void minimizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in minimizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_MINIMIZE);
    });
}

ELECTROBUN_EXPORT void restoreWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in restoreWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_RESTORE);
    });
}

ELECTROBUN_EXPORT bool isWindowMinimized(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    return IsIconic(hwnd) != 0;
}

ELECTROBUN_EXPORT void maximizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in maximizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_MAXIMIZE);
    });
}

ELECTROBUN_EXPORT void unmaximizeWindow(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in unmaximizeWindow");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        ShowWindow(hwnd, SW_RESTORE);
    });
}

ELECTROBUN_EXPORT bool isWindowMaximized(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    return IsZoomed(hwnd) != 0;
}

ELECTROBUN_EXPORT void setWindowFullScreen(NSWindow *window, bool fullScreen) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowFullScreen");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        static std::map<HWND, WINDOWPLACEMENT> savedPlacements;
        static std::map<HWND, LONG> savedStyles;

        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        bool isCurrentlyFullScreen = (style & WS_POPUP) && !(style & WS_OVERLAPPEDWINDOW);

        if (fullScreen && !isCurrentlyFullScreen) {
            // Save current state
            WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
            GetWindowPlacement(hwnd, &wp);
            savedPlacements[hwnd] = wp;
            savedStyles[hwnd] = style;

            // Get the monitor info for the window
            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfo(monitor, &mi);

            // Remove window decorations and set to fullscreen
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW | WS_POPUP);
            SetWindowPos(hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else if (!fullScreen && isCurrentlyFullScreen) {
            // Restore saved state
            auto styleIt = savedStyles.find(hwnd);
            if (styleIt != savedStyles.end()) {
                SetWindowLong(hwnd, GWL_STYLE, styleIt->second);
                savedStyles.erase(styleIt);
            }

            auto placementIt = savedPlacements.find(hwnd);
            if (placementIt != savedPlacements.end()) {
                SetWindowPlacement(hwnd, &placementIt->second);
                savedPlacements.erase(placementIt);
            }

            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    });
}

ELECTROBUN_EXPORT bool isWindowFullScreen(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    return (style & WS_POPUP) && !(style & WS_OVERLAPPEDWINDOW);
}

ELECTROBUN_EXPORT void setWindowAlwaysOnTop(NSWindow *window, bool alwaysOnTop) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in setWindowAlwaysOnTop");
        return;
    }

    MainThreadDispatcher::dispatch_sync([=]() {
        SetWindowPos(hwnd,
            alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE);
    });
}

ELECTROBUN_EXPORT bool isWindowAlwaysOnTop(NSWindow *window) {
    HWND hwnd = reinterpret_cast<HWND>(window);

    if (!IsWindow(hwnd)) {
        return false;
    }

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    return (exStyle & WS_EX_TOPMOST) != 0;
}

ELECTROBUN_EXPORT void setWindowVisibleOnAllWorkspaces(NSWindow *window, bool visible) {
    // Not applicable on Windows - no-op
}

ELECTROBUN_EXPORT bool isWindowVisibleOnAllWorkspaces(NSWindow *window) {
    // Not applicable on Windows
    return false;
}

ELECTROBUN_EXPORT void setWindowPosition(NSWindow *window, double x, double y) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void setWindowSize(NSWindow *window, double width, double height) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, 0, 0, (int)width, (int)height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void setWindowFrame(NSWindow *window, double x, double y, double width, double height) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) return;

    SetWindowPos(hwnd, NULL, (int)x, (int)y, (int)width, (int)height, SWP_NOZORDER | SWP_NOACTIVATE);
}

ELECTROBUN_EXPORT void getWindowFrame(NSWindow *window, double *outX, double *outY, double *outWidth, double *outHeight) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!IsWindow(hwnd)) {
        *outX = 0;
        *outY = 0;
        *outWidth = 0;
        *outHeight = 0;
        return;
    }

    RECT rect;
    GetWindowRect(hwnd, &rect);
    *outX = (double)rect.left;
    *outY = (double)rect.top;
    *outWidth = (double)(rect.right - rect.left);
    *outHeight = (double)(rect.bottom - rect.top);
}

ELECTROBUN_EXPORT void resizeWebview(AbstractView *abstractView, double x, double y, double width, double height, const char *masksJson) {
    if (!abstractView) {
        ::log("ERROR: Invalid AbstractView in resizeWebview");
        return;
    }
    
    
    RECT bounds = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};
    abstractView->storePendingResize(bounds, masksJson);
    g_pendingResizeQueue.enqueue(abstractView);
    schedulePendingResizeDrain();
}

// Internal function to stop window movement (without export linkage)



ELECTROBUN_EXPORT void stopWindowMove() {
    if (g_isMovingWindow) {
        // Unregister raw input device
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = NULL;
        
        RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE));
        g_isMovingWindow = FALSE;
        g_targetWindow = NULL;
    }
}

ELECTROBUN_EXPORT void startWindowMove(NSWindow *window) {
    // On Windows, NSWindow* is actually HWND
    HWND hwnd = reinterpret_cast<HWND>(window);
    
    if (!IsWindow(hwnd)) {
        ::log("ERROR: Invalid window handle in startWindowMove");
        return;
    }
    
    // Set up window dragging state
    g_targetWindow = hwnd;
    g_isMovingWindow = TRUE;
    
    // Get initial cursor and window positions
    GetCursorPos(&g_initialCursorPos);
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    g_initialWindowPos.x = windowRect.left;
    g_initialWindowPos.y = windowRect.top;
    
    // Register for raw mouse input to bypass WebView2 event consumption
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK; // Receive input even when not in foreground
    rid.hwndTarget = hwnd;   // Send messages to our window
    
    if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE))) {
        ::log("ERROR: Failed to register raw input device - error: " + std::to_string(GetLastError()));
        g_isMovingWindow = FALSE;
        g_targetWindow = NULL;
    }
}

ELECTROBUN_EXPORT BOOL moveToTrash(char *pathString) {
    if (!pathString) {
        ::log("ERROR: NULL path string passed to moveToTrash");
        return FALSE;
    }
    
    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, pathString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string");
        return FALSE;
    }
    
    std::vector<wchar_t> widePath(wideCharLen + 1);  // +1 for double null terminator
    MultiByteToWideChar(CP_UTF8, 0, pathString, -1, widePath.data(), wideCharLen);
    widePath[wideCharLen] = L'\0';  // Ensure double null termination
    
    // Use SHFileOperation to move to recycle bin
    SHFILEOPSTRUCTW fileOp = {};
    fileOp.hwnd = NULL;
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = widePath.data();
    fileOp.pTo = NULL;
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    fileOp.fAnyOperationsAborted = FALSE;
    fileOp.hNameMappings = NULL;
    fileOp.lpszProgressTitle = NULL;
    
    int result = SHFileOperationW(&fileOp);
    
    if (result == 0 && !fileOp.fAnyOperationsAborted) {
        ::log("Successfully moved to trash: " + std::string(pathString));
        return TRUE;
    } else {
        ::log("ERROR: Failed to move to trash: " + std::string(pathString) + " (error code: " + std::to_string(result) + ")");
        return FALSE;
    }
}

ELECTROBUN_EXPORT void showItemInFolder(char *path) {
    if (!path) {
        ::log("ERROR: NULL path passed to showItemInFolder");
        return;
    }
    
    std::string pathString(path);
    if (pathString.empty()) {
        ::log("ERROR: Empty path passed to showItemInFolder");
        return;
    }
    
    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string in showItemInFolder");
        return;
    }
    
    std::vector<wchar_t> widePath(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath.data(), wideCharLen);
    
    // Use ShellExecute to open Explorer and select the file
    std::wstring selectParam = L"/select,\"" + std::wstring(widePath.data()) + L"\"";
    
    HINSTANCE result = ShellExecuteW(
        NULL,                    // parent window
        L"open",                 // operation
        L"explorer.exe",         // executable
        selectParam.c_str(),     // parameters
        NULL,                    // working directory
        SW_SHOWNORMAL           // show command
    );
    
    // Check if the operation was successful
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to show item in folder: " + pathString + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
    } else {
        ::log("Successfully opened folder for: " + pathString);
    }
}

// Open a URL in the default browser or appropriate application
ELECTROBUN_EXPORT BOOL openExternal(const char *urlString) {
    if (!urlString) {
        ::log("ERROR: NULL URL passed to openExternal");
        return FALSE;
    }

    std::string url(urlString);
    if (url.empty()) {
        ::log("ERROR: Empty URL passed to openExternal");
        return FALSE;
    }

    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, urlString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert URL to wide string");
        return FALSE;
    }

    std::vector<wchar_t> wideUrl(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, urlString, -1, wideUrl.data(), wideCharLen);

    // Use ShellExecuteW to open the URL
    HINSTANCE result = ShellExecuteW(
        NULL,           // parent window
        L"open",        // operation
        wideUrl.data(), // URL to open
        NULL,           // parameters
        NULL,           // working directory
        SW_SHOWNORMAL   // show command
    );

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to open external URL: " + url + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
        return FALSE;
    }

    ::log("Successfully opened external URL: " + url);
    return TRUE;
}

// Open a file or folder with the default application
ELECTROBUN_EXPORT BOOL openPath(const char *pathString) {
    if (!pathString) {
        ::log("ERROR: NULL path passed to openPath");
        return FALSE;
    }

    std::string path(pathString);
    if (path.empty()) {
        ::log("ERROR: Empty path passed to openPath");
        return FALSE;
    }

    // Convert to wide string for Windows API
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, pathString, -1, NULL, 0);
    if (wideCharLen == 0) {
        ::log("ERROR: Failed to convert path to wide string");
        return FALSE;
    }

    std::vector<wchar_t> widePath(wideCharLen);
    MultiByteToWideChar(CP_UTF8, 0, pathString, -1, widePath.data(), wideCharLen);

    // Use ShellExecuteW to open the file/folder with default application
    HINSTANCE result = ShellExecuteW(
        NULL,            // parent window
        L"open",         // operation
        widePath.data(), // file/folder to open
        NULL,            // parameters
        NULL,            // working directory
        SW_SHOWNORMAL    // show command
    );

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ::log("ERROR: Failed to open path: " + path + " (error code: " + std::to_string(reinterpret_cast<INT_PTR>(result)) + ")");
        return FALSE;
    }

    ::log("Successfully opened path: " + path);
    return TRUE;
}

// Show a native desktop notification using Shell_NotifyIcon balloon
ELECTROBUN_EXPORT void showNotification(const char *title, const char *body, const char *subtitle, BOOL silent) {
    if (!title) {
        ::log("ERROR: NULL title passed to showNotification");
        return;
    }

    // Convert strings to wide chars
    int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    std::vector<wchar_t> wideTitle(titleLen);
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle.data(), titleLen);

    std::wstring wideBody;
    if (body) {
        int bodyLen = MultiByteToWideChar(CP_UTF8, 0, body, -1, NULL, 0);
        std::vector<wchar_t> bodyBuf(bodyLen);
        MultiByteToWideChar(CP_UTF8, 0, body, -1, bodyBuf.data(), bodyLen);
        wideBody = bodyBuf.data();
    }

    // If subtitle is provided, prepend it to body
    if (subtitle) {
        int subtitleLen = MultiByteToWideChar(CP_UTF8, 0, subtitle, -1, NULL, 0);
        std::vector<wchar_t> subtitleBuf(subtitleLen);
        MultiByteToWideChar(CP_UTF8, 0, subtitle, -1, subtitleBuf.data(), subtitleLen);
        if (!wideBody.empty()) {
            wideBody = std::wstring(subtitleBuf.data()) + L"\n" + wideBody;
        } else {
            wideBody = subtitleBuf.data();
        }
    }

    // Create notification icon data
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = NULL;  // No window handle needed for balloon
    nid.uID = 1;
    nid.uFlags = NIF_INFO | NIF_ICON;
    nid.dwInfoFlags = NIIF_INFO | (silent ? NIIF_NOSOUND : 0);

    // Copy title (max 63 chars)
    wcsncpy_s(nid.szInfoTitle, wideTitle.data(), _TRUNCATE);

    // Copy body (max 255 chars)
    if (!wideBody.empty()) {
        wcsncpy_s(nid.szInfo, wideBody.c_str(), _TRUNCATE);
    }

    // Use app icon or default
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    // Add the notification icon (required before showing balloon)
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Show the balloon notification
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    // Remove the icon after a delay (fire and forget - icon will be cleaned up)
    // Note: In a real app, you might want to keep the icon around
    // For now, we schedule removal after notification timeout
    std::thread([nid]() mutable {
        Sleep(5000);  // Wait for notification to be shown
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();

    ::log("Notification shown: " + std::string(title));
}

ELECTROBUN_EXPORT const char* openFileDialog(const char *startingFolder,
                          const char *allowedFileTypes,
                          BOOL canChooseFiles,
                          BOOL canChooseDirectories,
                          BOOL allowsMultipleSelection) {
    if (!canChooseFiles && !canChooseDirectories) {
        ::log("ERROR: Both canChooseFiles and canChooseDirectories are false");
        return nullptr;
    }
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        ::log("ERROR: Failed to initialize COM");
        return nullptr;
    }
    
    IFileOpenDialog *pFileDialog = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void**)&pFileDialog);
    if (FAILED(hr)) {
        ::log("ERROR: Failed to create file dialog");
        CoUninitialize();
        return nullptr;
    }
    
    // Set dialog options
    DWORD dwFlags = 0;
    pFileDialog->GetOptions(&dwFlags);
    
    if (canChooseDirectories) {
        dwFlags |= FOS_PICKFOLDERS;
    }
    if (allowsMultipleSelection) {
        dwFlags |= FOS_ALLOWMULTISELECT;
    }
    if (!canChooseFiles) {
        dwFlags |= FOS_PICKFOLDERS;
    }
    
    pFileDialog->SetOptions(dwFlags);
    
    // Set starting folder
    if (startingFolder && strlen(startingFolder) > 0) {
        int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, nullptr, 0);
        if (wideCharLen > 0) {
            std::vector<wchar_t> wideStartingFolder(wideCharLen);
            MultiByteToWideChar(CP_UTF8, 0, startingFolder, -1, wideStartingFolder.data(), wideCharLen);
            
            IShellItem *pStartingFolder = nullptr;
            hr = SHCreateItemFromParsingName(wideStartingFolder.data(), nullptr, IID_IShellItem, (void**)&pStartingFolder);
            if (SUCCEEDED(hr)) {
                pFileDialog->SetFolder(pStartingFolder);
                pStartingFolder->Release();
            }
        }
    }
    
    // Set file type filters
    if (allowedFileTypes && strlen(allowedFileTypes) > 0 && strcmp(allowedFileTypes, "*") != 0) {
        std::string typesStr(allowedFileTypes);
        std::vector<std::string> extensions;
        std::stringstream ss(typesStr);
        std::string extension;
        
        while (std::getline(ss, extension, ',')) {
            // Trim whitespace
            extension.erase(0, extension.find_first_not_of(" \t"));
            extension.erase(extension.find_last_not_of(" \t") + 1);
            if (!extension.empty()) {
                extensions.push_back(extension);
            }
        }
        
        if (!extensions.empty()) {
            // Create filter specification
            std::vector<COMDLG_FILTERSPEC> filterSpecs;
            std::vector<std::wstring> filterNames;
            std::vector<std::wstring> filterPatterns;
            
            for (const auto& ext : extensions) {
                std::wstring wExt = std::wstring(ext.begin(), ext.end());
                if (wExt.find(L".") != 0) {
                    wExt = L"." + wExt;
                }
                std::wstring pattern = L"*" + wExt;
                std::wstring name = wExt.substr(1) + L" files";
                
                filterNames.push_back(name);
                filterPatterns.push_back(pattern);
                
                COMDLG_FILTERSPEC spec;
                spec.pszName = filterNames.back().c_str();
                spec.pszSpec = filterPatterns.back().c_str();
                filterSpecs.push_back(spec);
            }
            
            pFileDialog->SetFileTypes(static_cast<UINT>(filterSpecs.size()), filterSpecs.data());
        }
    }
    
    // Show the dialog
    hr = pFileDialog->Show(nullptr);
    std::string result;
    
    if (SUCCEEDED(hr)) {
        if (allowsMultipleSelection) {
            IShellItemArray *pShellItemArray = nullptr;
            hr = pFileDialog->GetResults(&pShellItemArray);
            if (SUCCEEDED(hr)) {
                DWORD itemCount = 0;
                pShellItemArray->GetCount(&itemCount);
                
                std::vector<std::string> paths;
                for (DWORD i = 0; i < itemCount; i++) {
                    IShellItem *pShellItem = nullptr;
                    hr = pShellItemArray->GetItemAt(i, &pShellItem);
                    if (SUCCEEDED(hr)) {
                        PWSTR pszPath = nullptr;
                        hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                        if (SUCCEEDED(hr)) {
                            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                            if (utf8Len > 0) {
                                std::vector<char> utf8Path(utf8Len);
                                WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
                                paths.push_back(std::string(utf8Path.data()));
                            }
                            CoTaskMemFree(pszPath);
                        }
                        pShellItem->Release();
                    }
                }
                pShellItemArray->Release();
                
                // Join paths with comma
                for (size_t i = 0; i < paths.size(); i++) {
                    if (i > 0) result += ",";
                    result += paths[i];
                }
            }
        } else {
            IShellItem *pShellItem = nullptr;
            hr = pFileDialog->GetResult(&pShellItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszPath = nullptr;
                hr = pShellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
                if (SUCCEEDED(hr)) {
                    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Len > 0) {
                        std::vector<char> utf8Path(utf8Len);
                        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
                        result = std::string(utf8Path.data());
                    }
                    CoTaskMemFree(pszPath);
                }
                pShellItem->Release();
            }
        }
    }
    
    pFileDialog->Release();
    CoUninitialize();
    
    if (result.empty()) {
        ::log("File dialog cancelled or no selection made");
        return nullptr;
    }
    
    return strdup(result.c_str());
}

ELECTROBUN_EXPORT int showMessageBox(const char *type,
                                     const char *title,
                                     const char *message,
                                     const char *detail,
                                     const char *buttons,
                                     int defaultId,
                                     int cancelId) {
    return MainThreadDispatcher::dispatch_sync([=]() -> int {
        // Convert strings to wide
        std::wstring wTitle, wMessage;
        if (title && strlen(title) > 0) {
            int len = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
            wTitle.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], len);
        }

        // Combine message and detail
        std::string fullMsg;
        if (message && strlen(message) > 0) {
            fullMsg = message;
        }
        if (detail && strlen(detail) > 0) {
            if (!fullMsg.empty()) fullMsg += "\n\n";
            fullMsg += detail;
        }
        if (!fullMsg.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, nullptr, 0);
            wMessage.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, &wMessage[0], len);
        }

        // Determine icon based on type
        UINT uType = MB_OK;
        if (type) {
            std::string typeStr(type);
            if (typeStr == "warning") {
                uType |= MB_ICONWARNING;
            } else if (typeStr == "error" || typeStr == "critical") {
                uType |= MB_ICONERROR;
            } else if (typeStr == "question") {
                uType |= MB_ICONQUESTION;
            } else {
                uType |= MB_ICONINFORMATION;
            }
        } else {
            uType |= MB_ICONINFORMATION;
        }

        // Parse button labels to determine button type
        // MessageBox only supports predefined button combinations
        std::vector<std::string> buttonLabels;
        if (buttons && strlen(buttons) > 0) {
            std::string buttonsStr(buttons);
            std::stringstream ss(buttonsStr);
            std::string buttonLabel;
            while (std::getline(ss, buttonLabel, ',')) {
                // Trim whitespace
                buttonLabel.erase(0, buttonLabel.find_first_not_of(" \t"));
                buttonLabel.erase(buttonLabel.find_last_not_of(" \t") + 1);
                // Convert to lowercase for comparison
                std::transform(buttonLabel.begin(), buttonLabel.end(), buttonLabel.begin(), ::tolower);
                if (!buttonLabel.empty()) {
                    buttonLabels.push_back(buttonLabel);
                }
            }
        }

        // Map common button combinations to MessageBox types
        if (buttonLabels.size() == 2) {
            if ((buttonLabels[0] == "ok" && buttonLabels[1] == "cancel") ||
                (buttonLabels[0] == "yes" && buttonLabels[1] == "no")) {
                uType = (uType & ~MB_OK) | MB_OKCANCEL;
            } else if (buttonLabels[0] == "yes" && buttonLabels[1] == "no") {
                uType = (uType & ~MB_OK) | MB_YESNO;
            }
        } else if (buttonLabels.size() == 3) {
            if (buttonLabels[0] == "yes" && buttonLabels[1] == "no" && buttonLabels[2] == "cancel") {
                uType = (uType & ~MB_OK) | MB_YESNOCANCEL;
            }
        }

        int result = MessageBoxW(nullptr, wMessage.c_str(), wTitle.c_str(), uType);

        // Map MessageBox result to button index
        switch (result) {
            case IDOK:
            case IDYES:
                return 0;
            case IDNO:
                return 1;
            case IDCANCEL:
                return cancelId >= 0 ? cancelId : (buttonLabels.size() > 2 ? 2 : 1);
            default:
                return -1;
        }
    });
}

// ============================================================================
// Clipboard API
// ============================================================================

// clipboardReadText - Read text from the system clipboard
// Returns: UTF-8 string (caller must free) or NULL if no text available
ELECTROBUN_EXPORT const char* clipboardReadText() {
    return MainThreadDispatcher::dispatch_sync([=]() -> const char* {
        if (!OpenClipboard(nullptr)) {
            return nullptr;
        }

        const char* result = nullptr;
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
            if (wText) {
                // Convert wide string to UTF-8
                int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
                if (utf8Len > 0) {
                    char* utf8Text = static_cast<char*>(malloc(utf8Len));
                    WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8Text, utf8Len, nullptr, nullptr);
                    result = utf8Text;
                }
                GlobalUnlock(hData);
            }
        }

        CloseClipboard();
        return result;
    });
}

// clipboardWriteText - Write text to the system clipboard
ELECTROBUN_EXPORT void clipboardWriteText(const char* text) {
    if (!text) return;

    MainThreadDispatcher::dispatch_sync([=]() {
        if (!OpenClipboard(nullptr)) {
            return;
        }

        EmptyClipboard();

        // Convert UTF-8 to wide string
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (wideLen > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideLen * sizeof(wchar_t));
            if (hMem) {
                wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hMem));
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wText, wideLen);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }

        CloseClipboard();
    });
}

// clipboardReadImage - Read image from clipboard as PNG data
// Returns: PNG data (caller must free) and sets outSize, or NULL if no image
ELECTROBUN_EXPORT const uint8_t* clipboardReadImage(size_t* outSize) {
    return MainThreadDispatcher::dispatch_sync([=]() -> const uint8_t* {
        if (outSize) *outSize = 0;

        if (!OpenClipboard(nullptr)) {
            return nullptr;
        }

        const uint8_t* result = nullptr;

        // Try CF_DIB format (Device Independent Bitmap)
        HANDLE hData = GetClipboardData(CF_DIB);
        if (hData) {
            BITMAPINFO* bmi = static_cast<BITMAPINFO*>(GlobalLock(hData));
            if (bmi) {
                // For now, return raw DIB data - full PNG conversion would require
                // additional libraries like libpng or GDI+
                // TODO: Implement proper PNG conversion using GDI+ or similar
                size_t dataSize = GlobalSize(hData);
                uint8_t* buffer = static_cast<uint8_t*>(malloc(dataSize));
                memcpy(buffer, bmi, dataSize);
                if (outSize) *outSize = dataSize;
                result = buffer;
                GlobalUnlock(hData);
            }
        }

        CloseClipboard();
        return result;
    });
}

// clipboardWriteImage - Write PNG image data to clipboard
ELECTROBUN_EXPORT void clipboardWriteImage(const uint8_t* pngData, size_t size) {
    if (!pngData || size == 0) return;

    MainThreadDispatcher::dispatch_sync([=]() {
        if (!OpenClipboard(nullptr)) {
            return;
        }

        EmptyClipboard();

        // For now, store as raw data - proper PNG to DIB conversion would require
        // additional libraries
        // TODO: Implement proper PNG to DIB conversion
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hMem) {
            void* data = GlobalLock(hMem);
            memcpy(data, pngData, size);
            GlobalUnlock(hMem);
            // Register a custom format for PNG data
            UINT pngFormat = RegisterClipboardFormatA("PNG");
            SetClipboardData(pngFormat, hMem);
        }

        CloseClipboard();
    });
}

// clipboardClear - Clear the clipboard
ELECTROBUN_EXPORT void clipboardClear() {
    MainThreadDispatcher::dispatch_sync([=]() {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
        }
    });
}

// clipboardAvailableFormats - Get available formats in clipboard
// Returns: comma-separated list of formats (caller must free)
ELECTROBUN_EXPORT const char* clipboardAvailableFormats() {
    return MainThreadDispatcher::dispatch_sync([=]() -> const char* {
        if (!OpenClipboard(nullptr)) {
            return strdup("");
        }

        std::vector<std::string> formats;

        // Check for text
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) {
            formats.push_back("text");
        }

        // Check for image
        if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP)) {
            formats.push_back("image");
        }

        // Check for files
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            formats.push_back("files");
        }

        // Check for HTML
        UINT htmlFormat = RegisterClipboardFormatA("HTML Format");
        if (IsClipboardFormatAvailable(htmlFormat)) {
            formats.push_back("html");
        }

        CloseClipboard();

        // Join formats with comma
        std::string result;
        for (size_t i = 0; i < formats.size(); i++) {
            if (i > 0) result += ",";
            result += formats[i];
        }

        return strdup(result.c_str());
    });
}

// Window procedure for handling tray messages
LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            // Don't allow the tray window to be closed/destroyed by default handlers
            ::log("Preventing tray window close/destroy");
            return 0;
            
        case WM_COMMAND:
        // Handle menu item clicks
        {
            auto it = g_trayItems.find(hwnd);
            if (it != g_trayItems.end()) {
                NSStatusItem* trayItem = it->second;
                UINT menuItemId = LOWORD(wParam);
                
                // Use your existing function to handle the menu selection
                handleMenuItemSelection(menuItemId, trayItem);
            }
            return 0;
        }
            
        default:
            // Check if this is our tray message
            if (msg == g_trayMessageId) {
                // Find the tray item
                auto it = g_trayItems.find(hwnd);
                if (it != g_trayItems.end()) {
                    NSStatusItem* trayItem = it->second;
                    
                    switch (LOWORD(lParam)) {
                        case WM_LBUTTONUP:
                           
                            
                        case WM_RBUTTONUP:
                            // Right click - show context menu if it exists, otherwise call handler
                            if (trayItem->contextMenu) {
                                
                                
                                POINT pt;
                                GetCursorPos(&pt);
                                
                                // This is required for the menu to work properly
                                SetForegroundWindow(hwnd);
                                
                                // Show the menu
                                BOOL menuResult = TrackPopupMenu(
                                    trayItem->contextMenu, 
                                    TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                                    pt.x, pt.y, 
                                    0, 
                                    hwnd, 
                                    NULL
                                );
                                
                                // This message helps ensure the menu closes properly
                                PostMessage(hwnd, WM_NULL, 0, 0);
                                
                                if (!menuResult) {
                                    ::log("TrackPopupMenu failed");
                                }
                            } else {
                                // No menu exists yet, call handler (this will trigger menu creation)
                                
                                
                                if (trayItem->handler) {
                                    // Use a separate thread or async call to prevent blocking
                                    std::thread([trayItem]() {
                                        try {
                                            trayItem->handler(trayItem->trayId, "");
                                        } catch (...) {
                                            ::log("Exception in tray handler");
                                        }
                                    }).detach();
                                }
                            }
                            return 0;
                            
                        default:
                            break;
                    }
                }
                return 0;
            }
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

ELECTROBUN_EXPORT NSStatusItem* createTray(uint32_t trayId, const char *title, const char *pathToImage, bool isTemplate,
                        uint32_t width, uint32_t height, ZigStatusItemHandler zigTrayItemHandler) {
    
    return MainThreadDispatcher::dispatch_sync([=]() -> NSStatusItem* {
        // ::log("Creating system tray icon");
        
        NSStatusItem* statusItem = new NSStatusItem();
        statusItem->trayId = trayId;
        statusItem->handler = zigTrayItemHandler;
        
        if (title) {
            statusItem->title = std::string(title);
        }
        if (pathToImage) {
            statusItem->imagePath = std::string(pathToImage);
        }
        
        // Create a hidden window to receive tray messages
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSA wc = {0};
            wc.lpfnWndProc = TrayWindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "TrayWindowClass";
            wc.hbrBackground = NULL;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.style = 0; // No special styles
            
            if (!RegisterClassA(&wc)) {
                DWORD error = GetLastError();
                if (error != ERROR_CLASS_ALREADY_EXISTS) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to register TrayWindowClass: %lu", error);
                    ::log(errorMsg);
                    delete statusItem;
                    return nullptr;
                }
            }
            classRegistered = true;
        }
        
        // Create message-only window (safer for tray operations)
        statusItem->hwnd = CreateWindowA(
            "TrayWindowClass", 
            "TrayWindow", 
            0,                    // No visible style
            0, 0, 0, 0,          // Position and size (ignored for message-only)
            HWND_MESSAGE,        // Message-only window
            NULL, 
            GetModuleHandle(NULL), 
            NULL
        );
        
        if (!statusItem->hwnd) {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to create tray window: %lu", error);
            ::log(errorMsg);
            delete statusItem;
            return nullptr;
        }
        
        
        
        // Store in global map before setting up the tray icon
        g_trayItems[statusItem->hwnd] = statusItem;
        
        // Set up NOTIFYICONDATA
        statusItem->nid.cbSize = sizeof(NOTIFYICONDATA);
        statusItem->nid.hWnd = statusItem->hwnd;
        statusItem->nid.uID = trayId;
        statusItem->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        statusItem->nid.uCallbackMessage = g_trayMessageId;
        
        // Set title/tooltip
        if (!statusItem->title.empty()) {
            strncpy_s(statusItem->nid.szTip, sizeof(statusItem->nid.szTip), 
                     statusItem->title.c_str(), sizeof(statusItem->nid.szTip) - 1);
        }
        
        // Load icon
        if (!statusItem->imagePath.empty()) {
            // Convert to wide string for LoadImage
            int size = MultiByteToWideChar(CP_UTF8, 0, statusItem->imagePath.c_str(), -1, NULL, 0);
            if (size > 0) {
                std::wstring wImagePath(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, statusItem->imagePath.c_str(), -1, &wImagePath[0], size);
                
                statusItem->nid.hIcon = (HICON)LoadImageW(NULL, wImagePath.c_str(), IMAGE_ICON,
                                                         width, height, LR_LOADFROMFILE);
                
                if (!statusItem->nid.hIcon) {
                    char errorMsg[256];
                    sprintf_s(errorMsg, "Failed to load icon from: %s", statusItem->imagePath.c_str());
                    ::log(errorMsg);
                }
            }
        }
        
        // Use default icon if loading failed
        if (!statusItem->nid.hIcon) {
            statusItem->nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
            ::log("Using default application icon");
        }
        
        // Add to system tray
        if (Shell_NotifyIcon(NIM_ADD, &statusItem->nid)) {
            // char successMsg[256];
            // sprintf_s(successMsg, "System tray icon created successfully: ID=%u, HWND=%p", trayId, statusItem->hwnd);
            // ::log(successMsg);
        } else {
            DWORD error = GetLastError();
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Failed to add icon to system tray: %lu", error);
            ::log(errorMsg);
            
            DestroyWindow(statusItem->hwnd);
            g_trayItems.erase(statusItem->hwnd);
            delete statusItem;
            return nullptr;
        }
        
        return statusItem;
    });
}

ELECTROBUN_EXPORT void setTrayTitle(NSStatusItem *statusItem, const char *title) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        
        if (title) {
            statusItem->title = std::string(title);
            strncpy_s(statusItem->nid.szTip, title, sizeof(statusItem->nid.szTip) - 1);
        } else {
            statusItem->title.clear();
            statusItem->nid.szTip[0] = '\0';
        }
        
        // Update the tray icon
        Shell_NotifyIcon(NIM_MODIFY, &statusItem->nid);
    });
}

ELECTROBUN_EXPORT void setTrayImage(NSStatusItem *statusItem, const char *image) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        
        HICON oldIcon = statusItem->nid.hIcon;
        
        if (image && strlen(image) > 0) {
            statusItem->imagePath = std::string(image);
            
            // Convert to wide string
            int size = MultiByteToWideChar(CP_UTF8, 0, image, -1, NULL, 0);
            if (size > 0) {
                std::wstring wImagePath(size - 1, 0);
                MultiByteToWideChar(CP_UTF8, 0, image, -1, &wImagePath[0], size);
                
                statusItem->nid.hIcon = (HICON)LoadImageW(NULL, wImagePath.c_str(), IMAGE_ICON,
                                                         0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
            }
        }
        
        // Use default icon if loading failed
        if (!statusItem->nid.hIcon) {
            statusItem->nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        }
        
        // Update the tray icon
        if (Shell_NotifyIcon(NIM_MODIFY, &statusItem->nid)) {
            // Clean up old icon if it's not the default
            if (oldIcon && oldIcon != LoadIcon(NULL, IDI_APPLICATION)) {
                DestroyIcon(oldIcon);
            }
        } else {
            ::log("ERROR: Failed to update tray image");
            // Restore old icon on failure
            statusItem->nid.hIcon = oldIcon;
        }
    });
}

// Updated setTrayMenuFromJSON function
ELECTROBUN_EXPORT void setTrayMenuFromJSON(NSStatusItem *statusItem, const char *jsonString) {
    if (!statusItem || !jsonString) return;
        
    MainThreadDispatcher::dispatch_sync([=]() {
        
        if (!statusItem->handler) {
            ::log("ERROR: No handler found for status item");
            return;
        }
        
        try {
            // Parse JSON using our simple parser
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));
            
            if (menuConfig.type != SimpleJsonValue::ARRAY) {
                ::log("ERROR: JSON menu configuration is not an array");
                return;
            }
            
            // Clean up existing menu
            if (statusItem->contextMenu) {
                DestroyMenu(statusItem->contextMenu);
                statusItem->contextMenu = NULL;
            }
            
            // Create new menu from JSON config
            statusItem->contextMenu = createMenuFromConfig(menuConfig, statusItem);
            
            if (statusItem->contextMenu) {
            } else {
                ::log("ERROR: Failed to create context menu from JSON configuration");
            }
            
        } catch (const std::exception& e) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Exception parsing JSON: %s", e.what());
            ::log(errorMsg);
        } catch (...) {
            ::log("ERROR: Unknown exception parsing JSON");
        }
    });
}

// You'll also need to update your tray click handler to process menu selections
// This should be called from your window procedure when handling tray icon messages
void handleTrayIconMessage(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    NSStatusItem* statusItem = nullptr;
    
    // Find the status item from the global map
    auto it = g_trayItems.find(hwnd);
    if (it != g_trayItems.end()) {
        statusItem = it->second;
    }
    
    switch (lParam) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            if (statusItem && statusItem->contextMenu) {
                POINT pt;
                GetCursorPos(&pt);
                
                // Required for popup menus to work correctly
                SetForegroundWindow(hwnd);
                
                UINT cmd = TrackPopupMenu(
                    statusItem->contextMenu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    pt.x, pt.y,
                    0, hwnd, NULL
                );
                
                if (cmd != 0) {
                    handleMenuItemSelection(cmd, statusItem);
                }
                
                // Required cleanup
                PostMessage(hwnd, WM_NULL, 0, 0);
            }
            break;
            
        case WM_LBUTTONUP:
            // Handle left click on tray icon
            if (statusItem && statusItem->handler) {
                statusItem->handler(statusItem->trayId, "");
            }
            break;
    }
}

ELECTROBUN_EXPORT void setTrayMenu(NSStatusItem *statusItem, const char *menuConfig) {
    // Delegate to JSON version for now
    setTrayMenuFromJSON(statusItem, menuConfig);
}

ELECTROBUN_EXPORT void removeTray(NSStatusItem *statusItem) {
    if (!statusItem) return;
    
    MainThreadDispatcher::dispatch_sync([=]() {
        // Remove from global map first
        g_trayItems.erase(statusItem->hwnd);
        
        // Clean up the tray item
        delete statusItem;
    });
}

ELECTROBUN_EXPORT const char* getTrayBounds(NSStatusItem *statusItem) {
    (void)statusItem;
    return _strdup("{\"x\":0,\"y\":0,\"width\":0,\"height\":0}");
}

ELECTROBUN_EXPORT void setApplicationMenu(const char *jsonString, ZigStatusItemHandler zigTrayItemHandler) {
    if (!jsonString) {
        ::log("ERROR: NULL JSON string passed to setApplicationMenu");
        return;
    }
    
    
    MainThreadDispatcher::dispatch_sync([=]() {
        try {
            // Parse JSON using our simple parser
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));
            
            if (menuConfig.type != SimpleJsonValue::ARRAY) {
                ::log("ERROR: Application menu JSON configuration is not an array");
                return;
            }
            
            // Create target for handling menu actions
            g_appMenuTarget = std::make_unique<StatusItemTarget>();
            g_appMenuTarget->zigHandler = zigTrayItemHandler;
            g_appMenuTarget->trayId = 0;
            
            // Clean up existing application menu and accelerators
            if (g_applicationMenu) {
                DestroyMenu(g_applicationMenu);
                g_applicationMenu = NULL;
            }
            clearMenuAccelerators();

            // Create new application menu from JSON config
            g_applicationMenu = createApplicationMenuFromConfig(menuConfig, g_appMenuTarget.get());

            // Rebuild the accelerator table after menu creation
            rebuildAcceleratorTable();
            
            if (g_applicationMenu) {
                
                // Find the main application window to set the menu
                HWND mainWindow = GetActiveWindow();
                if (!mainWindow) {
                    mainWindow = FindWindowA("BasicWindowClass", NULL);
                }
                
                if (mainWindow) {
                    if (SetMenu(mainWindow, g_applicationMenu)) {
                        DrawMenuBar(mainWindow);
                        
                       
                    } else {
                        DWORD error = GetLastError();
                        char errorMsg[256];
                        sprintf_s(errorMsg, "Failed to set application menu on window: %lu", error);
                        ::log(errorMsg);
                    }
                } else {
                    ::log("Warning: No main window found to attach application menu");
                }
            } else {
                ::log("ERROR: Failed to create application menu from JSON configuration");
            }
            
        } catch (const std::exception& e) {
            char errorMsg[256];
            sprintf_s(errorMsg, "ERROR: Exception in setApplicationMenu: %s", e.what());
            ::log(errorMsg);
        } catch (...) {
            ::log("ERROR: Unknown exception in setApplicationMenu");
        }
    });
}


ELECTROBUN_EXPORT void showContextMenu(const char *jsonString, ZigStatusItemHandler contextMenuHandler) {
    if (!jsonString) {
        ::log("ERROR: NULL JSON string passed to showContextMenu");
        return;
    }
    
    if (!contextMenuHandler) {
        ::log("ERROR: NULL context menu handler passed to showContextMenu");
        return;
    }
    
    MainThreadDispatcher::dispatch_sync([=]() {
        try {
            SimpleJsonValue menuConfig = parseJson(std::string(jsonString));

            std::unique_ptr<NSStatusItem> target = std::make_unique<NSStatusItem>();
            target->handler = contextMenuHandler;
            target->trayId = 0;

            HMENU menu = createMenuFromConfig(menuConfig, target.get());
            if (!menu) {
                ::log("ERROR: Failed to create context menu");
                return;
            }
            
            // Get cursor position for menu display
            POINT pt;
            GetCursorPos(&pt);
            
            // Get the foreground window or use desktop
            HWND hwnd = GetForegroundWindow();
            if (!hwnd) {
                hwnd = GetDesktopWindow();
            }
            
            // Required for proper menu operation
            SetForegroundWindow(hwnd);
                        
            // Show the context menu
            UINT cmd = TrackPopupMenu(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y,
                0, hwnd, NULL
            );
            
            // Handle menu selection
            if (cmd != 0) {
                handleMenuItemSelection(cmd, target.get());
            }
            
            // Required for proper cleanup
            PostMessage(hwnd, WM_NULL, 0, 0);
            
            // Cleanup menu
            DestroyMenu(menu);
            
        } catch (const std::exception& e) {
            ::log("ERROR: Exception in showContextMenu: " + std::string(e.what()));
        }
    });
}

ELECTROBUN_EXPORT void getWebviewSnapshot(uint32_t hostId, uint32_t webviewId,
                       WKWebView *webView,
                       zigSnapshotCallback callback) {
    // Stub implementation
    if (callback) {
        static const char* emptyDataUrl = "data:image/png;base64,";
        callback(hostId, webviewId, emptyDataUrl);
    }
}

ELECTROBUN_EXPORT void setJSUtils(GetMimeType getMimeType, GetHTMLForWebviewSync getHTMLForWebviewSync) {
    ::log("setJSUtils called but using map-based approach instead of callbacks");
}

// MARK: - Webview HTML Content Management (replaces JSCallback approach)

extern "C" ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    if (htmlContent) {
        webviewHTMLContent[webviewId] = std::string(htmlContent);
        char logMsg[256];
        sprintf_s(logMsg, "setWebviewHTMLContent: Set HTML for webview %u", webviewId);
        ::log(logMsg);
    } else {
        webviewHTMLContent.erase(webviewId);
        char logMsg[256];
        sprintf_s(logMsg, "setWebviewHTMLContent: Cleared HTML for webview %u", webviewId);
        ::log(logMsg);
    }
}

extern "C" ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    auto it = webviewHTMLContent.find(webviewId);
    if (it != webviewHTMLContent.end()) {
        char* result = _strdup(it->second.c_str());
        char logMsg[256];
        sprintf_s(logMsg, "getWebviewHTMLContent: Retrieved HTML for webview %u", webviewId);
        ::log(logMsg);
        return result;
    } else {
        char logMsg[256];
        sprintf_s(logMsg, "getWebviewHTMLContent: No HTML found for webview %u", webviewId);
        ::log(logMsg);
        return nullptr;
    }
}

// Adding a few Windows-specific functions for interop if needed
ELECTROBUN_EXPORT uint32_t getWindowStyle(
    bool Borderless,
    bool Titled,
    bool Closable,
    bool Miniaturizable,
    bool Resizable,
    bool UnifiedTitleAndToolbar,
    bool FullScreen,
    bool FullSizeContentView,
    bool UtilityWindow,
    bool DocModalWindow,
    bool NonactivatingPanel,
    bool HUDWindow) {
    // Stub implementation that returns a composite style mask
    uint32_t mask = 0;
    if (Borderless) mask |= 1;
    if (Titled) mask |= 2;
    if (Closable) mask |= 4;
    if (Resizable) mask |= 8;
    return mask;
}

} // extern "C"

// New function for handling views:// scheme requests
void setupViewsSchemeHandler(ICoreWebView2* webview, uint32_t webviewId) {
    
    // Add web resource request filter for views:// scheme
    EventRegistrationToken resourceToken;
    HRESULT hr = webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [webviewId](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                args->get_Request(&request);
                
                LPWSTR uri;
                request->get_Uri(&uri);
                
                std::wstring wUri(uri);
                
                // Convert to string for logging
                int size = WideCharToMultiByte(CP_UTF8, 0, uri, -1, NULL, 0, NULL, NULL);
                std::string uriStr(size - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, uri, -1, &uriStr[0], size, NULL, NULL);
                
                
                
                // Check if this is a views:// URL
                if (wUri.find(L"views://") == 0) {
                    handleViewsSchemeRequest(args, wUri, webviewId);
                }
                
                CoTaskMemFree(uri);
                return S_OK;
            }).Get(), 
        &resourceToken);
    
    if (FAILED(hr)) {
        char errorMsg[256];
        sprintf_s(errorMsg, "Failed to add WebResourceRequested handler: 0x%lx", hr);
        ::log(errorMsg);
        return;
    }
    
    // Add filter for views:// scheme
    hr = webview->AddWebResourceRequestedFilter(L"views://*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    if (FAILED(hr)) {
        char errorMsg[256];
        sprintf_s(errorMsg, "Failed to add resource filter for views://: 0x%lx", hr);
        ::log(errorMsg);
    } else {
    }
}

// Updated function to handle views:// scheme requests
void handleViewsSchemeRequest(ICoreWebView2WebResourceRequestedEventArgs* args, 
                             const std::wstring& uri, 
                             uint32_t webviewId) {
    
    
    // Convert URI to std::string for processing
    int size = WideCharToMultiByte(CP_UTF8, 0, uri.c_str(), -1, NULL, 0, NULL, NULL);
    std::string uriStr(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, uri.c_str(), -1, &uriStr[0], size, NULL, NULL);

    
    // Extract the path after "views://"
    std::string path;
    if (uriStr.length() > 8) {
        path = uriStr.substr(8); // Remove "views://" prefix
    } else {
        path = "index.html"; // Default
    }
    
    std::string responseData;
    std::string mimeType = "text/html";
    
    if (path == "internal/index.html") {
        // Handle internal HTML content using stored content
        ::log("DEBUG Windows: Handling views://internal/index.html");
        const char* htmlContent = getWebviewHTMLContent(webviewId);
        if (htmlContent && strlen(htmlContent) > 0) {
            responseData = std::string(htmlContent);
            free((void*)htmlContent); // Free the strdup'd memory
            ::log("DEBUG Windows: Retrieved HTML content from storage");
        } else {
            responseData = "<html><body><h1>No content set</h1></body></html>";
            ::log("DEBUG Windows: No HTML content found, using fallback");
        }
        mimeType = "text/html";
    } else {
        // Handle other file requests
        responseData = loadViewsFile(path);
        mimeType = getMimeTypeForFile(path);
        
        if (responseData.empty()) {
            responseData = "<html><body><h1>404 - Views file not found</h1><p>Path: " + path + "</p></body></html>";
            mimeType = "text/html";
            ::log("Views file not found, returning 404");
        }
    }
    
    // sprintf_s(logMsg, "Response data length: %zu bytes, MIME type: %s", responseData.length(), mimeType.c_str());
    // log(logMsg);
    
    // Create the response using the global environment
    if (!g_environment) {
        ::log("ERROR: No global environment available for creating response");
        return;
    }
    
    try {
        // Create memory stream first
        ComPtr<IStream> stream;
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, responseData.length());
        if (!hGlobal) {
            ::log("ERROR: Failed to allocate global memory");
            return;
        }
        
        void* pData = GlobalLock(hGlobal);
        if (!pData) {
            GlobalFree(hGlobal);
            ::log("ERROR: Failed to lock global memory");
            return;
        }
        
        memcpy(pData, responseData.c_str(), responseData.length());
        GlobalUnlock(hGlobal);
        
        HRESULT streamResult = CreateStreamOnHGlobal(hGlobal, TRUE, &stream);
        if (FAILED(streamResult)) {
            GlobalFree(hGlobal);
            ::log("ERROR: Failed to create stream on global");
            return;
        }
        
        // Create the response
        ComPtr<ICoreWebView2WebResourceResponse> response;
        std::wstring mimeTypeW(mimeType.begin(), mimeType.end());
        std::wstring headers = L"Content-Type: " + mimeTypeW + L"\r\nAccess-Control-Allow-Origin: *";
        
        HRESULT responseResult = g_environment->CreateWebResourceResponse(
            stream.Get(),               // content stream
            200,                       // status code
            L"OK",                     // reason phrase
            headers.c_str(),           // headers
            &response);
        
        if (FAILED(responseResult)) {
            ::log("ERROR: Failed to create web resource response");
            return;
        }
        
        // Set the response
        HRESULT setResult = args->put_Response(response.Get());
        if (FAILED(setResult)) {
            ::log("ERROR: Failed to set response");
            return;
        }
        
        
    } catch (...) {
        ::log("ERROR: Exception occurred while creating response");
    }
}

// Helper functions
std::string loadViewsFile(const std::string& path) {
    // Get the current working directory instead of executable directory
    char currentDir[MAX_PATH];
    DWORD result = GetCurrentDirectoryA(MAX_PATH, currentDir);

    if (result == 0 || result > MAX_PATH) {
        ::log("ERROR: Failed to get current working directory");
        return "";
    }

    std::string resourcesDir = std::string(currentDir) + "\\..\\Resources";
    std::string asarPath = resourcesDir + "\\app.asar";

    // Check if ASAR archive exists
    std::ifstream asarCheck(asarPath);
    if (asarCheck.good()) {
        asarCheck.close();

        // Thread-safe lazy-load ASAR archive on first use
        std::call_once(g_asarArchiveInitFlag, [&asarPath]() {
            g_asarArchive = AsarArchive::open(asarPath);
            if (g_asarArchive) {
                ::log("DEBUG loadViewsFile: Opened ASAR archive at " + asarPath);
            } else {
                ::log("ERROR loadViewsFile: Failed to open ASAR archive at " + asarPath);
            }
        });

        // If ASAR archive is loaded, try to read from it
        if (g_asarArchive) {
            // The ASAR contains the entire app directory, so prepend "views/" to the path
            std::string asarFilePath = "views/" + path;

            // Protect ASAR read operations with mutex to prevent race conditions
            // when multiple assets are requested concurrently
            std::vector<uint8_t> fileData;
            {
                std::lock_guard<std::mutex> lock(g_asarReadMutex);
                fileData = g_asarArchive->readFile(asarFilePath);
            }

            if (!fileData.empty()) {
                ::log("DEBUG loadViewsFile: Read " + std::to_string(fileData.size()) + " bytes from ASAR for " + path);
                return std::string(fileData.begin(), fileData.end());
            } else {
                ::log("DEBUG loadViewsFile: File not found in ASAR: " + path);
                // Fall through to flat file reading
            }
        }
    }

    // Fallback: Read from flat file system (for non-ASAR builds or missing files)
    std::string fullPath = resourcesDir + "\\app\\views\\" + path;

    ::log("DEBUG loadViewsFile: Attempting flat file read: " + fullPath);

    // Try to read the file
    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        ::log("ERROR: Could not open views file: " + fullPath);
        return "";
    }

    // Read file contents
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    return content;
}

// Shared MIME type detection function
// Based on Bun runtime supported file types and web development standards
std::string getMimeTypeForFile(const std::string& path) {
    // Web/Code Files (Bun native support)
    if (path.find(".html") != std::string::npos || path.find(".htm") != std::string::npos) {
        return "text/html";
    } else if (path.find(".js") != std::string::npos || path.find(".mjs") != std::string::npos || path.find(".cjs") != std::string::npos) {
        return "text/javascript";
    } else if (path.find(".ts") != std::string::npos || path.find(".mts") != std::string::npos || path.find(".cts") != std::string::npos) {
        return "text/typescript";
    } else if (path.find(".jsx") != std::string::npos) {
        return "text/jsx";
    } else if (path.find(".tsx") != std::string::npos) {
        return "text/tsx";
    } else if (path.find(".css") != std::string::npos) {
        return "text/css";
    } else if (path.find(".json") != std::string::npos) {
        return "application/json";
    } else if (path.find(".xml") != std::string::npos) {
        return "application/xml";
    } else if (path.find(".md") != std::string::npos) {
        return "text/markdown";
    } else if (path.find(".txt") != std::string::npos) {
        return "text/plain";
    } else if (path.find(".toml") != std::string::npos) {
        return "application/toml";
    } else if (path.find(".yaml") != std::string::npos || path.find(".yml") != std::string::npos) {
        return "application/x-yaml";
    
    // Image Files
    } else if (path.find(".png") != std::string::npos) {
        return "image/png";
    } else if (path.find(".jpg") != std::string::npos || path.find(".jpeg") != std::string::npos) {
        return "image/jpeg";
    } else if (path.find(".gif") != std::string::npos) {
        return "image/gif";
    } else if (path.find(".webp") != std::string::npos) {
        return "image/webp";
    } else if (path.find(".svg") != std::string::npos) {
        return "image/svg+xml";
    } else if (path.find(".ico") != std::string::npos) {
        return "image/x-icon";
    } else if (path.find(".avif") != std::string::npos) {
        return "image/avif";
    
    // Font Files
    } else if (path.find(".woff") != std::string::npos) {
        return "font/woff";
    } else if (path.find(".woff2") != std::string::npos) {
        return "font/woff2";
    } else if (path.find(".ttf") != std::string::npos) {
        return "font/ttf";
    } else if (path.find(".otf") != std::string::npos) {
        return "font/otf";
    
    // Media Files
    } else if (path.find(".mp3") != std::string::npos) {
        return "audio/mpeg";
    } else if (path.find(".mp4") != std::string::npos) {
        return "video/mp4";
    } else if (path.find(".webm") != std::string::npos) {
        return "video/webm";
    } else if (path.find(".ogg") != std::string::npos) {
        return "audio/ogg";
    } else if (path.find(".wav") != std::string::npos) {
        return "audio/wav";
    
    // Document Files
    } else if (path.find(".pdf") != std::string::npos) {
        return "application/pdf";
    
    // WebAssembly (Bun support)
    } else if (path.find(".wasm") != std::string::npos) {
        return "application/wasm";
    
    // Compressed Files
    } else if (path.find(".zip") != std::string::npos) {
        return "application/zip";
    } else if (path.find(".gz") != std::string::npos) {
        return "application/gzip";
    }

    return "application/octet-stream"; // default
}

/*
 * =============================================================================
 * GLOBAL KEYBOARD SHORTCUTS
 * =============================================================================
 */

// Callback type for global shortcut triggers
typedef void (*GlobalShortcutCallback)(const char* accelerator);
static GlobalShortcutCallback g_globalShortcutCallback = nullptr;

// Custom Windows messages for hotkey thread communication
#define WM_REGISTER_HOTKEY (WM_USER + 100)
#define WM_UNREGISTER_HOTKEY (WM_USER + 101)
#define WM_UNREGISTER_ALL_HOTKEYS (WM_USER + 102)

// Structure to pass hotkey registration data between threads
struct HotkeyRegisterData {
    int hotkeyId;
    UINT modifiers;
    UINT vkCode;
    std::string accelerator;
    BOOL* result;  // Output: success/failure
    HANDLE completionEvent;  // Signal when operation is complete
};

// Storage for registered shortcuts: accelerator string -> hotkey ID
static std::map<std::string, int> g_globalShortcuts;
static std::map<int, std::string> g_hotkeyIdToAccelerator;
static int g_nextHotkeyId = 1;
static HWND g_hotkeyWindow = NULL;
static std::thread g_hotkeyThread;
static bool g_hotkeyThreadRunning = false;
static std::mutex g_hotkeyMutex;  // Protect access to g_globalShortcuts and g_hotkeyIdToAccelerator

// Helper to parse virtual key code from key string
static UINT getVirtualKeyCode(const std::string& key) {
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // Letters
    if (lowerKey.length() == 1 && lowerKey[0] >= 'a' && lowerKey[0] <= 'z') {
        return 'A' + (lowerKey[0] - 'a');
    }
    // Numbers
    if (lowerKey.length() == 1 && lowerKey[0] >= '0' && lowerKey[0] <= '9') {
        return '0' + (lowerKey[0] - '0');
    }
    // Function keys
    if (lowerKey[0] == 'f' && lowerKey.length() >= 2) {
        int fNum = std::stoi(lowerKey.substr(1));
        if (fNum >= 1 && fNum <= 24) return VK_F1 + (fNum - 1);
    }
    // Special keys
    if (lowerKey == "space" || lowerKey == " ") return VK_SPACE;
    if (lowerKey == "return" || lowerKey == "enter") return VK_RETURN;
    if (lowerKey == "tab") return VK_TAB;
    if (lowerKey == "escape" || lowerKey == "esc") return VK_ESCAPE;
    if (lowerKey == "backspace") return VK_BACK;
    if (lowerKey == "delete") return VK_DELETE;
    if (lowerKey == "up") return VK_UP;
    if (lowerKey == "down") return VK_DOWN;
    if (lowerKey == "left") return VK_LEFT;
    if (lowerKey == "right") return VK_RIGHT;
    if (lowerKey == "home") return VK_HOME;
    if (lowerKey == "end") return VK_END;
    if (lowerKey == "pageup") return VK_PRIOR;
    if (lowerKey == "pagedown") return VK_NEXT;
    // Symbols
    if (lowerKey == "-") return VK_OEM_MINUS;
    if (lowerKey == "=") return VK_OEM_PLUS;
    if (lowerKey == "[") return VK_OEM_4;
    if (lowerKey == "]") return VK_OEM_6;
    if (lowerKey == "\\") return VK_OEM_5;
    if (lowerKey == ";") return VK_OEM_1;
    if (lowerKey == "'") return VK_OEM_7;
    if (lowerKey == ",") return VK_OEM_COMMA;
    if (lowerKey == ".") return VK_OEM_PERIOD;
    if (lowerKey == "/") return VK_OEM_2;
    if (lowerKey == "`") return VK_OEM_3;

    return 0;
}

// Parse modifiers from accelerator string for global shortcuts using the
// shared cross-platform parser. Returns MOD_CONTROL, MOD_ALT, MOD_SHIFT flags.
static UINT parseModifiers(const std::string& accelerator, std::string& outKey) {
    auto parts = electrobun::parseAccelerator(accelerator);
    outKey = parts.key;

    UINT modifiers = 0;
    if (parts.commandOrControl || parts.command || parts.control) modifiers |= MOD_CONTROL;
    if (parts.alt)                                                modifiers |= MOD_ALT;
    if (parts.shift)                                              modifiers |= MOD_SHIFT;
    if (parts.super)                                              modifiers |= MOD_WIN;
    return modifiers;
}

// Window procedure for hotkey window
static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        int hotkeyId = (int)wParam;
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        auto it = g_hotkeyIdToAccelerator.find(hotkeyId);
        if (it != g_hotkeyIdToAccelerator.end() && g_globalShortcutCallback) {
            g_globalShortcutCallback(it->second.c_str());
        }
        return 0;
    }
    else if (msg == WM_REGISTER_HOTKEY) {
        HotkeyRegisterData* data = reinterpret_cast<HotkeyRegisterData*>(lParam);
        BOOL success = RegisterHotKey(hwnd, data->hotkeyId, data->modifiers, data->vkCode);
        if (success) {
            std::lock_guard<std::mutex> lock(g_hotkeyMutex);
            g_globalShortcuts[data->accelerator] = data->hotkeyId;
            g_hotkeyIdToAccelerator[data->hotkeyId] = data->accelerator;
            ::log("GlobalShortcut registered successfully: '" + data->accelerator + "' (id=" + std::to_string(data->hotkeyId) + ", total=" + std::to_string(g_globalShortcuts.size()) + ")");
        } else {
            DWORD error = GetLastError();
            ::log("ERROR: Failed to register hotkey '" + data->accelerator + "' - Win32 error: " + std::to_string(error));
        }
        *data->result = success;
        SetEvent(data->completionEvent);
        return 0;
    }
    else if (msg == WM_UNREGISTER_HOTKEY) {
        int hotkeyId = (int)wParam;
        UnregisterHotKey(hwnd, hotkeyId);
        return 0;
    }
    else if (msg == WM_UNREGISTER_ALL_HOTKEYS) {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        for (const auto& pair : g_globalShortcuts) {
            UnregisterHotKey(hwnd, pair.second);
        }
        g_globalShortcuts.clear();
        g_hotkeyIdToAccelerator.clear();
        ::log("GlobalShortcut: Unregistered all shortcuts");
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Message loop thread for hotkey window
static void hotkeyMessageLoop() {
    // Create a message-only window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = HotkeyWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ElectrobunHotkeyWindow";

    RegisterClassExW(&wc);

    g_hotkeyWindow = CreateWindowExW(0, L"ElectrobunHotkeyWindow", L"",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

    if (!g_hotkeyWindow) {
        ::log("ERROR: Failed to create hotkey window");
        return;
    }

    MSG msg;
    while (g_hotkeyThreadRunning && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DestroyWindow(g_hotkeyWindow);
    g_hotkeyWindow = NULL;
}

// Set the callback for global shortcut events
extern "C" ELECTROBUN_EXPORT void setGlobalShortcutCallback(GlobalShortcutCallback callback) {
    g_globalShortcutCallback = callback;

    // Start the hotkey message loop thread if not running
    if (!g_hotkeyThreadRunning && callback) {
        g_hotkeyThreadRunning = true;
        g_hotkeyThread = std::thread(hotkeyMessageLoop);
        // Wait for window to be created
        while (!g_hotkeyWindow && g_hotkeyThreadRunning) {
            Sleep(10);
        }
    }
}

// Register a global keyboard shortcut
extern "C" ELECTROBUN_EXPORT BOOL registerGlobalShortcut(const char* accelerator) {
    if (!accelerator) {
        ::log("ERROR: Cannot register shortcut - invalid accelerator");
        return FALSE;
    }

    // Wait for hotkey window to be ready (with timeout)
    int waitCount = 0;
    const int maxWaitMs = 5000; // 5 second timeout

    while (!g_hotkeyWindow && waitCount < maxWaitMs) {
        Sleep(10);
        waitCount += 10;
    }

    if (!g_hotkeyWindow) {
        ::log("ERROR: Cannot register shortcut - hotkey window not ready after " + std::to_string(waitCount) + "ms");
        return FALSE;
    }

    std::string accelStr(accelerator);

    // Check if already registered (with mutex protection)
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        if (g_globalShortcuts.find(accelStr) != g_globalShortcuts.end()) {
            ::log("GlobalShortcut already registered: " + accelStr);
            return FALSE;
        }
    }

    // Parse the accelerator
    std::string key;
    UINT modifiers = parseModifiers(accelStr, key);
    UINT vkCode = getVirtualKeyCode(key);

    if (vkCode == 0) {
        ::log("ERROR: Unknown key: " + key);
        return FALSE;
    }

    // Prepare registration data
    int hotkeyId = g_nextHotkeyId++;
    BOOL result = FALSE;
    HANDLE completionEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HotkeyRegisterData data;
    data.hotkeyId = hotkeyId;
    data.modifiers = modifiers | MOD_NOREPEAT;
    data.vkCode = vkCode;
    data.accelerator = accelStr;
    data.result = &result;
    data.completionEvent = completionEvent;

    ::log("GlobalShortcut: Posting registration request for '" + accelStr + "' with modifiers=" + std::to_string(modifiers) + " vkCode=" + std::to_string(vkCode));

    // Post message to hotkey thread to register the hotkey
    PostMessage(g_hotkeyWindow, WM_REGISTER_HOTKEY, 0, reinterpret_cast<LPARAM>(&data));

    // Wait for registration to complete (with timeout)
    DWORD waitResult = WaitForSingleObject(completionEvent, 5000);
    CloseHandle(completionEvent);

    if (waitResult != WAIT_OBJECT_0) {
        ::log("ERROR: Registration timeout for '" + accelStr + "'");
        return FALSE;
    }

    return result;
}

// Unregister a global keyboard shortcut
extern "C" ELECTROBUN_EXPORT BOOL unregisterGlobalShortcut(const char* accelerator) {
    if (!accelerator) return FALSE;

    std::string accelStr(accelerator);
    int hotkeyId = -1;

    {
        std::lock_guard<std::mutex> lock(g_hotkeyMutex);
        auto it = g_globalShortcuts.find(accelStr);
        if (it != g_globalShortcuts.end()) {
            hotkeyId = it->second;
            g_hotkeyIdToAccelerator.erase(hotkeyId);
            g_globalShortcuts.erase(it);
        }
    }

    if (hotkeyId != -1 && g_hotkeyWindow) {
        PostMessage(g_hotkeyWindow, WM_UNREGISTER_HOTKEY, hotkeyId, 0);
        ::log("GlobalShortcut unregistered: " + accelStr);
        return TRUE;
    }

    return FALSE;
}

// Unregister all global keyboard shortcuts
extern "C" ELECTROBUN_EXPORT void unregisterAllGlobalShortcuts() {
    if (g_hotkeyWindow) {
        PostMessage(g_hotkeyWindow, WM_UNREGISTER_ALL_HOTKEYS, 0, 0);
    }
}

// Check if a shortcut is registered
extern "C" ELECTROBUN_EXPORT BOOL isGlobalShortcutRegistered(const char* accelerator) {
    if (!accelerator) return FALSE;

    std::string accelStr(accelerator);
    std::lock_guard<std::mutex> lock(g_hotkeyMutex);
    bool found = g_globalShortcuts.find(accelStr) != g_globalShortcuts.end();
    ::log("GlobalShortcut.isRegistered: Checking '" + accelStr + "' - " + (found ? "FOUND" : "NOT FOUND") + " (total shortcuts=" + std::to_string(g_globalShortcuts.size()) + ")");
    return found;
}

/*
 * =============================================================================
 * SCREEN API
 * =============================================================================
 */

// Structure to collect monitor info during enumeration
struct MonitorEnumData {
    std::vector<std::string> displays;
};

// Callback for EnumDisplayMonitors
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorEnumData* data = reinterpret_cast<MonitorEnumData*>(dwData);

    MONITORINFOEX monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEX);

    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
        // Get DPI/scale factor using GetDpiForMonitor if available (Windows 8.1+)
        double scaleFactor = 1.0;

        // Try to get DPI - load dynamically as it may not be available on all Windows versions
        typedef HRESULT(WINAPI *GetDpiForMonitorFunc)(HMONITOR, int, UINT*, UINT*);
        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            GetDpiForMonitorFunc getDpi = (GetDpiForMonitorFunc)GetProcAddress(shcore, "GetDpiForMonitor");
            if (getDpi) {
                UINT dpiX, dpiY;
                // MDT_EFFECTIVE_DPI = 0
                if (SUCCEEDED(getDpi(hMonitor, 0, &dpiX, &dpiY))) {
                    scaleFactor = dpiX / 96.0;  // 96 DPI is 100% scaling
                }
            }
            FreeLibrary(shcore);
        }

        // Check if primary
        bool isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

        // Build JSON for this display
        std::ostringstream json;
        json << "{";
        json << "\"id\":" << reinterpret_cast<uintptr_t>(hMonitor) << ",";
        json << "\"bounds\":{";
        json << "\"x\":" << monitorInfo.rcMonitor.left << ",";
        json << "\"y\":" << monitorInfo.rcMonitor.top << ",";
        json << "\"width\":" << (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) << ",";
        json << "\"height\":" << (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
        json << "},";
        json << "\"workArea\":{";
        json << "\"x\":" << monitorInfo.rcWork.left << ",";
        json << "\"y\":" << monitorInfo.rcWork.top << ",";
        json << "\"width\":" << (monitorInfo.rcWork.right - monitorInfo.rcWork.left) << ",";
        json << "\"height\":" << (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
        json << "},";
        json << "\"scaleFactor\":" << scaleFactor << ",";
        json << "\"isPrimary\":" << (isPrimary ? "true" : "false");
        json << "}";

        data->displays.push_back(json.str());
    }

    return TRUE;  // Continue enumeration
}

// Get all displays as JSON array
extern "C" ELECTROBUN_EXPORT const char* getAllDisplays() {
    MonitorEnumData data;

    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    // Build JSON array
    std::ostringstream result;
    result << "[";
    for (size_t i = 0; i < data.displays.size(); i++) {
        if (i > 0) result << ",";
        result << data.displays[i];
    }
    result << "]";

    return _strdup(result.str().c_str());
}

// Callback for finding primary display
struct PrimaryMonitorData {
    std::string json;
    bool found;
};

static BOOL CALLBACK PrimaryMonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    PrimaryMonitorData* data = reinterpret_cast<PrimaryMonitorData*>(dwData);

    MONITORINFOEX monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEX);

    if (GetMonitorInfo(hMonitor, &monitorInfo)) {
        if (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) {
            // Get DPI/scale factor
            double scaleFactor = 1.0;
            HMODULE shcore = LoadLibraryW(L"Shcore.dll");
            if (shcore) {
                typedef HRESULT(WINAPI *GetDpiForMonitorFunc)(HMONITOR, int, UINT*, UINT*);
                GetDpiForMonitorFunc getDpi = (GetDpiForMonitorFunc)GetProcAddress(shcore, "GetDpiForMonitor");
                if (getDpi) {
                    UINT dpiX, dpiY;
                    if (SUCCEEDED(getDpi(hMonitor, 0, &dpiX, &dpiY))) {
                        scaleFactor = dpiX / 96.0;
                    }
                }
                FreeLibrary(shcore);
            }

            std::ostringstream json;
            json << "{";
            json << "\"id\":" << reinterpret_cast<uintptr_t>(hMonitor) << ",";
            json << "\"bounds\":{";
            json << "\"x\":" << monitorInfo.rcMonitor.left << ",";
            json << "\"y\":" << monitorInfo.rcMonitor.top << ",";
            json << "\"width\":" << (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) << ",";
            json << "\"height\":" << (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top);
            json << "},";
            json << "\"workArea\":{";
            json << "\"x\":" << monitorInfo.rcWork.left << ",";
            json << "\"y\":" << monitorInfo.rcWork.top << ",";
            json << "\"width\":" << (monitorInfo.rcWork.right - monitorInfo.rcWork.left) << ",";
            json << "\"height\":" << (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
            json << "},";
            json << "\"scaleFactor\":" << scaleFactor << ",";
            json << "\"isPrimary\":true";
            json << "}";

            data->json = json.str();
            data->found = true;
            return FALSE;  // Stop enumeration
        }
    }

    return TRUE;  // Continue enumeration
}

// Get primary display as JSON
extern "C" ELECTROBUN_EXPORT const char* getPrimaryDisplay() {
    PrimaryMonitorData data;
    data.found = false;

    EnumDisplayMonitors(NULL, NULL, PrimaryMonitorEnumProc, reinterpret_cast<LPARAM>(&data));

    if (data.found) {
        return _strdup(data.json.c_str());
    }

    return _strdup("{}");
}

// Get current cursor position as JSON: {"x": 123, "y": 456}
extern "C" ELECTROBUN_EXPORT const char* getCursorScreenPoint() {
    POINT cursorPos;
    if (GetCursorPos(&cursorPos)) {
        std::ostringstream json;
        json << "{\"x\":" << cursorPos.x << ",\"y\":" << cursorPos.y << "}";
        return _strdup(json.str().c_str());
    }

    return _strdup("{\"x\":0,\"y\":0}");
}

extern "C" ELECTROBUN_EXPORT uint64_t getMouseButtons() {
    uint64_t buttons = 0;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) buttons |= 1ull << 0;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) buttons |= 1ull << 1;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) buttons |= 1ull << 2;
    return buttons;
}

/*
 * =============================================================================
 * COOKIE MANAGEMENT API
 * =============================================================================
 */

// Helper to find a WebView2View by webview ID
static WebView2View* findWebView2ById(uint32_t webviewId) {
    for (auto& pair : g_webview2Views) {
        WebView2View* view = static_cast<WebView2View*>(pair.second);
        if (view && view->webviewId == webviewId) {
            return view;
        }
    }
    return nullptr;
}

// Get cookies for a webview (WebView2)
// Note: WebView2 requires a live webview to access cookies. Pass webviewId of an existing webview.
// filterJson: {"url": "https://example.com"} or {} for all
extern "C" ELECTROBUN_EXPORT const char* sessionGetCookies(const char* partitionIdentifier, const char* filterJson) {
    // For WebView2, we need a webview to access cookies
    // We'll try to find any webview with the matching partition
    // For now, return empty array - full implementation requires webview access

    std::string result = "[]";

    // Parse filter to get URL
    std::string filterStr = filterJson ? filterJson : "{}";
    std::string filterUrl;

    // Simple JSON parsing for url field
    size_t urlPos = filterStr.find("\"url\"");
    if (urlPos != std::string::npos) {
        size_t colonPos = filterStr.find(':', urlPos);
        size_t quoteStart = filterStr.find('"', colonPos);
        size_t quoteEnd = filterStr.find('"', quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            filterUrl = filterStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
    }

    // Find a WebView2 instance (ideally matching partition)
    WebView2View* view = nullptr;
    for (auto& pair : g_webview2Views) {
        if (pair.second) {
            view = static_cast<WebView2View*>(pair.second);
            break; // Use first available view
        }
    }

    if (!view || !view->getWebView()) {
        return _strdup("[]");
    }

    // Get cookie manager
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(view->getWebView()->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        return _strdup("[]");
    }

    ComPtr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(webview2->get_CookieManager(&cookieManager)) || !cookieManager) {
        return _strdup("[]");
    }

    // Get cookies synchronously using event
    std::string cookiesJson = "[]";
    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);

    std::wstring wFilterUrl;
    if (!filterUrl.empty()) {
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, filterUrl.c_str(), -1, nullptr, 0);
        wFilterUrl.resize(wideSize - 1);
        MultiByteToWideChar(CP_UTF8, 0, filterUrl.c_str(), -1, &wFilterUrl[0], wideSize);
    }

    LPCWSTR uri = filterUrl.empty() ? nullptr : wFilterUrl.c_str();

    cookieManager->GetCookies(uri,
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [&cookiesJson, event](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT {
                if (SUCCEEDED(result) && cookieList) {
                    UINT count;
                    cookieList->get_Count(&count);

                    std::ostringstream json;
                    json << "[";
                    for (UINT i = 0; i < count; i++) {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (SUCCEEDED(cookieList->GetValueAtIndex(i, &cookie))) {
                            LPWSTR name, value, domain, path;
                            BOOL secure, httpOnly;
                            double expires;

                            cookie->get_Name(&name);
                            cookie->get_Value(&value);
                            cookie->get_Domain(&domain);
                            cookie->get_Path(&path);
                            cookie->get_IsSecure(&secure);
                            cookie->get_IsHttpOnly(&httpOnly);
                            cookie->get_Expires(&expires);

                            // Convert to UTF-8
                            auto toUtf8 = [](LPWSTR wstr) -> std::string {
                                if (!wstr) return "";
                                int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
                                std::string str(size - 1, '\0');
                                WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size, nullptr, nullptr);
                                return str;
                            };

                            if (i > 0) json << ",";
                            json << "{";
                            json << "\"name\":\"" << toUtf8(name) << "\",";
                            json << "\"value\":\"" << toUtf8(value) << "\",";
                            json << "\"domain\":\"" << toUtf8(domain) << "\",";
                            json << "\"path\":\"" << toUtf8(path) << "\",";
                            json << "\"secure\":" << (secure ? "true" : "false") << ",";
                            json << "\"httpOnly\":" << (httpOnly ? "true" : "false");
                            if (expires > 0) {
                                json << ",\"expirationDate\":" << expires;
                            }
                            json << "}";

                            CoTaskMemFree(name);
                            CoTaskMemFree(value);
                            CoTaskMemFree(domain);
                            CoTaskMemFree(path);
                        }
                    }
                    json << "]";
                    cookiesJson = json.str();
                }
                SetEvent(event);
                return S_OK;
            }).Get());

    WaitForSingleObject(event, 5000);
    CloseHandle(event);

    return _strdup(cookiesJson.c_str());
}

// Set a cookie (WebView2)
extern "C" ELECTROBUN_EXPORT bool sessionSetCookie(const char* partitionIdentifier, const char* cookieJson) {
    if (!cookieJson) return false;

    // Find a WebView2 instance
    WebView2View* view = nullptr;
    for (auto& pair : g_webview2Views) {
        if (pair.second) {
            view = static_cast<WebView2View*>(pair.second);
            break;
        }
    }

    if (!view || !view->getWebView()) {
        return false;
    }

    // Get cookie manager
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(view->getWebView()->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        return false;
    }

    ComPtr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(webview2->get_CookieManager(&cookieManager)) || !cookieManager) {
        return false;
    }

    // Parse JSON
    std::string jsonStr = cookieJson;
    auto extractString = [&jsonStr](const std::string& key) -> std::string {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = jsonStr.find(searchKey);
        if (pos == std::string::npos) return "";
        size_t colonPos = jsonStr.find(':', pos);
        size_t quoteStart = jsonStr.find('"', colonPos);
        size_t quoteEnd = jsonStr.find('"', quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            return jsonStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
        return "";
    };

    auto extractBool = [&jsonStr](const std::string& key) -> bool {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = jsonStr.find(searchKey);
        if (pos == std::string::npos) return false;
        return jsonStr.find("true", pos) < jsonStr.find(',', pos);
    };

    auto extractDouble = [&jsonStr](const std::string& key) -> double {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = jsonStr.find(searchKey);
        if (pos == std::string::npos) return 0;
        size_t colonPos = jsonStr.find(':', pos);
        size_t numStart = colonPos + 1;
        while (numStart < jsonStr.size() && (jsonStr[numStart] == ' ' || jsonStr[numStart] == '\t')) numStart++;
        return std::stod(jsonStr.substr(numStart));
    };

    std::string name = extractString("name");
    std::string value = extractString("value");
    std::string domain = extractString("domain");
    std::string path = extractString("path");
    std::string url = extractString("url");
    bool secure = extractBool("secure");
    bool httpOnly = extractBool("httpOnly");
    double expirationDate = extractDouble("expirationDate");

    if (name.empty() || (domain.empty() && url.empty())) {
        return false;
    }

    // Derive domain from URL if not provided
    if (domain.empty() && !url.empty()) {
        size_t start = url.find("://");
        if (start != std::string::npos) {
            start += 3;
            size_t end = url.find('/', start);
            domain = url.substr(start, end - start);
        }
    }

    if (path.empty()) path = "/";

    // Convert to wide strings
    auto toWide = [](const std::string& str) -> std::wstring {
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring wstr(size - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
        return wstr;
    };

    // Create cookie - need to use CreateCookie which requires a URI
    std::string cookieUrl = url.empty() ? ("https://" + domain + "/") : url;
    std::wstring wUrl = toWide(cookieUrl);

    ComPtr<ICoreWebView2Cookie> cookie;
    if (FAILED(cookieManager->CreateCookie(toWide(name).c_str(), toWide(value).c_str(),
                                           toWide(domain).c_str(), toWide(path).c_str(), &cookie))) {
        return false;
    }

    cookie->put_IsSecure(secure);
    cookie->put_IsHttpOnly(httpOnly);
    if (expirationDate > 0) {
        cookie->put_Expires(expirationDate);
    }

    bool success = false;
    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);

    cookieManager->AddOrUpdateCookie(cookie.Get());
    success = true; // AddOrUpdateCookie doesn't have a callback

    return success;
}

// Remove a specific cookie (WebView2)
extern "C" ELECTROBUN_EXPORT bool sessionRemoveCookie(const char* partitionIdentifier, const char* urlStr, const char* cookieName) {
    if (!urlStr || !cookieName) return false;

    // Find a WebView2 instance
    WebView2View* view = nullptr;
    for (auto& pair : g_webview2Views) {
        if (pair.second) {
            view = static_cast<WebView2View*>(pair.second);
            break;
        }
    }

    if (!view || !view->getWebView()) {
        return false;
    }

    // Get cookie manager
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(view->getWebView()->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        return false;
    }

    ComPtr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(webview2->get_CookieManager(&cookieManager)) || !cookieManager) {
        return false;
    }

    std::string url = urlStr;
    std::string name = cookieName;

    // Convert to wide strings
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wUrl(wideSize - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wUrl[0], wideSize);

    wideSize = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wName(wideSize - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wName[0], wideSize);

    // Get cookies matching URL, then delete the one with matching name
    bool found = false;
    HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);

    cookieManager->GetCookies(wUrl.c_str(),
        Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [&found, &wName, &cookieManager, event](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT {
                if (SUCCEEDED(result) && cookieList) {
                    UINT count;
                    cookieList->get_Count(&count);

                    for (UINT i = 0; i < count; i++) {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (SUCCEEDED(cookieList->GetValueAtIndex(i, &cookie))) {
                            LPWSTR cookieName;
                            cookie->get_Name(&cookieName);
                            if (wcscmp(cookieName, wName.c_str()) == 0) {
                                cookieManager->DeleteCookie(cookie.Get());
                                found = true;
                            }
                            CoTaskMemFree(cookieName);
                        }
                    }
                }
                SetEvent(event);
                return S_OK;
            }).Get());

    WaitForSingleObject(event, 5000);
    CloseHandle(event);

    return found;
}

// Clear all cookies (WebView2)
extern "C" ELECTROBUN_EXPORT void sessionClearCookies(const char* partitionIdentifier) {
    // Find a WebView2 instance
    WebView2View* view = nullptr;
    for (auto& pair : g_webview2Views) {
        if (pair.second) {
            view = static_cast<WebView2View*>(pair.second);
            break;
        }
    }

    if (!view || !view->getWebView()) {
        return;
    }

    // Get cookie manager
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(view->getWebView()->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        return;
    }

    ComPtr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(webview2->get_CookieManager(&cookieManager)) || !cookieManager) {
        return;
    }

    // DeleteAllCookies deletes all cookies
    cookieManager->DeleteAllCookies();
}

// Clear storage data (WebView2) - uses Profile API
extern "C" ELECTROBUN_EXPORT void sessionClearStorageData(const char* partitionIdentifier, const char* storageTypesJson) {
    // Find a WebView2 instance
    WebView2View* view = nullptr;
    for (auto& pair : g_webview2Views) {
        if (pair.second) {
            view = static_cast<WebView2View*>(pair.second);
            break;
        }
    }

    if (!view || !view->getWebView()) {
        return;
    }

    // Try to get Profile interface for clearing browsing data
    ComPtr<ICoreWebView2_13> webview13;
    if (SUCCEEDED(view->getWebView()->QueryInterface(IID_PPV_ARGS(&webview13)))) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(webview13->get_Profile(&profile))) {
            ComPtr<ICoreWebView2Profile2> profile2;
            if (SUCCEEDED(profile->QueryInterface(IID_PPV_ARGS(&profile2)))) {
                // Determine what to clear
                COREWEBVIEW2_BROWSING_DATA_KINDS dataKinds = COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE;

                if (storageTypesJson && strlen(storageTypesJson) > 2) {
                    dataKinds = (COREWEBVIEW2_BROWSING_DATA_KINDS)0;
                    std::string types = storageTypesJson;

                    if (types.find("cookies") != std::string::npos) {
                        dataKinds = (COREWEBVIEW2_BROWSING_DATA_KINDS)(dataKinds | COREWEBVIEW2_BROWSING_DATA_KINDS_COOKIES);
                    }
                    if (types.find("cache") != std::string::npos) {
                        dataKinds = (COREWEBVIEW2_BROWSING_DATA_KINDS)(dataKinds | COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE);
                    }
                    if (types.find("localStorage") != std::string::npos ||
                        types.find("sessionStorage") != std::string::npos ||
                        types.find("indexedDB") != std::string::npos) {
                        dataKinds = (COREWEBVIEW2_BROWSING_DATA_KINDS)(dataKinds | COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE);
                    }
                }

                HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
                profile2->ClearBrowsingData(dataKinds,
                    Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                        [event](HRESULT result) -> HRESULT {
                            SetEvent(event);
                            return S_OK;
                        }).Get());
                WaitForSingleObject(event, 10000);
                CloseHandle(event);
            }
        }
    }
}

// URL scheme handler - macOS only, stub for Windows
extern "C" ELECTROBUN_EXPORT void setURLOpenHandler(void (*callback)(const char*)) {
    (void)callback;
    // Not supported on Windows - stub to prevent dlopen failure
    // Windows URL protocol handling is done via registry
}

// App reopen handler - macOS only, stub for Windows
extern "C" ELECTROBUN_EXPORT void setAppReopenHandler(void (*callback)()) {
    (void)callback;
    // Not supported on Windows - stub to prevent dlopen failure
}

// Dock icon visibility - macOS only, stubs for Windows
extern "C" ELECTROBUN_EXPORT void setDockIconVisible(bool visible) {
    (void)visible;
    // Not supported on Windows - stub to prevent dlopen failure
}

extern "C" ELECTROBUN_EXPORT bool isDockIconVisible() {
    // Not supported on Windows
    return true;
}

// Window icon - Linux only, no-op for Windows
extern "C" ELECTROBUN_EXPORT void setWindowIcon(void* window, const char* iconPath) {
    // Not yet implemented on Windows
    // TODO: Implement using SetWindowIcon/LoadImage APIs
}
