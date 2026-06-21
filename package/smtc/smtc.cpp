// smtc.cpp - Windows SMTC via MediaPlayer with silent audio for SMTC activation

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <Windows.h>
#include <Shobjidl.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <mutex>
#include <string>
#include <vector>
#include <queue>

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Storage::Streams;
using namespace Windows::Storage;

static bool g_initialized = false;
static MediaPlayer g_player = nullptr;
static SystemMediaTransportControls g_smtc = nullptr;
static std::mutex g_mtx;

static std::queue<int> g_buttonQueue;

static void ensure_com() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[smtc] CoInitializeEx failed: 0x%08lX\n", hr);
        OutputDebugStringA(buf);
    }
}

static hstring toHString(const char* utf8) {
    if (!utf8 || !utf8[0]) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring wstr(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr.data(), len);
    return hstring(wstr.c_str(), static_cast<uint32_t>(wstr.size()) - 1);
}

static MediaSource createSilentSource() {
    unsigned char header[] = {
        0x52,0x49,0x46,0x46, 0x00,0x00,0x00,0x00, 0x57,0x41,0x56,0x45,
        0x66,0x6D,0x74,0x20, 0x10,0x00,0x00,0x00, 0x01,0x00, 0x01,0x00,
        0x40,0x1F,0x00,0x00, 0x40,0x1F,0x00,0x00, 0x01,0x00, 0x08,0x00,
        0x64,0x61,0x74,0x61, 0x00,0x00,0x00,0x00
    };

    unsigned char silence[4000];
    memset(silence, 0x80, sizeof(silence));

    auto stream = InMemoryRandomAccessStream();
    auto writer = DataWriter(stream);

    uint32_t dataSize = sizeof(silence);
    uint32_t riffSize = sizeof(header) + dataSize - 8;
    memcpy(header + 4, &riffSize, 4);
    memcpy(header + 40, &dataSize, 4);

    writer.WriteBytes(winrt::array_view<const uint8_t>(header, header + sizeof(header)));
    writer.WriteBytes(winrt::array_view<const uint8_t>(silence, silence + sizeof(silence)));
    writer.StoreAsync().get();
    stream.Seek(0);

    return MediaSource::CreateFromStream(stream, L"audio/wav");
}

extern "C" __declspec(dllexport) bool smtc_init()
{
    if (g_initialized) return true;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_initialized) return true;

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        g_player = MediaPlayer();
        g_player.Source(createSilentSource());
        g_player.IsLoopingEnabled(true);
        g_player.Volume(0.0);
        g_player.Play();

        g_smtc = g_player.SystemMediaTransportControls();
        if (!g_smtc) return false;

        g_smtc.IsEnabled(true);
        g_smtc.IsPlayEnabled(true);
        g_smtc.IsPauseEnabled(true);
        g_smtc.IsNextEnabled(true);
        g_smtc.IsPreviousEnabled(true);

        auto updater = g_smtc.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        updater.Update();

        g_smtc.ButtonPressed([](
            const SystemMediaTransportControls&,
            const SystemMediaTransportControlsButtonPressedEventArgs& args)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            int code = -1;
            switch (args.Button())
            {
            case SystemMediaTransportControlsButton::Play:    code = 0; break;
            case SystemMediaTransportControlsButton::Pause:   code = 1; break;
            case SystemMediaTransportControlsButton::Next:    code = 2; break;
            case SystemMediaTransportControlsButton::Previous: code = 3; break;
            }
            if (code >= 0) g_buttonQueue.push(code);
        });

        HRESULT aumidHr = SetCurrentProcessExplicitAppUserModelID(L"musicapp.kuumo.dev");
        if (FAILED(aumidHr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[smtc] SetCurrentProcessExplicitAppUserModelID failed: 0x%08lX\n", aumidHr);
            OutputDebugStringA(buf);
        }

        g_initialized = true;
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[smtc] init failed: %ls\n", e.message().c_str());
        OutputDebugStringA(buf);
        return false;
    }
}

extern "C" __declspec(dllexport) int smtc_update_metadata(
    const char* title,
    const char* artist,
    const char* thumbnailPath,
    bool IsList
)
{
    ensure_com();
    if (!g_smtc) return 1;
    std::lock_guard<std::mutex> lock(g_mtx);

    try
    {
        g_smtc.IsEnabled(true);
        g_smtc.IsPlayEnabled(true);
        g_smtc.IsPauseEnabled(true);
        g_smtc.IsNextEnabled(IsList);
        g_smtc.IsPreviousEnabled(IsList);

        auto updater = g_smtc.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);

        auto music = updater.MusicProperties();
        auto hTitle = toHString(title);
        auto hArtist = toHString(artist);
        music.Title(hTitle);
        music.Artist(hArtist);

        if (thumbnailPath && thumbnailPath[0])
        {
            auto uriStr = toHString(thumbnailPath);
            try
            {
                if (uriStr.c_str()[0] == L'h' && wcsstr(uriStr.c_str(), L"://"))
                    updater.Thumbnail(RandomAccessStreamReference::CreateFromUri(
                        winrt::Windows::Foundation::Uri(uriStr)));
                else
                    updater.Thumbnail(RandomAccessStreamReference::CreateFromFile(
                        StorageFile::GetFileFromPathAsync(uriStr).get()));
            }
            catch (const winrt::hresult_error& e)
            {
                return 10;
            }
            catch (...)
            {
                return 11;
            }
        }

        updater.Update();
        return 0;
    }
    catch (const winrt::hresult_error& e)
    {
        return 20;
    }
    catch (...)
    {
        return 21;
    }
}

extern "C" __declspec(dllexport) void smtc_set_playback_state(bool isPlaying)
{
    ensure_com();
    if (!g_smtc) return;
    try
    {
        g_smtc.PlaybackStatus(
            isPlaying ? MediaPlaybackStatus::Playing
                      : MediaPlaybackStatus::Paused);
    }
    catch (const winrt::hresult_error& e)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[smtc] smtc_set_playback_state failed: %ls\n", e.message().c_str());
        OutputDebugStringA(buf);
    }
    catch (...)
    {
        OutputDebugStringA("[smtc] smtc_set_playback_state failed: unknown error\n");
    }
}

extern "C" __declspec(dllexport) void smtc_set_enabled_buttons(
    bool play, bool pause, bool next, bool prev)
{
    ensure_com();
    if (!g_smtc) return;
    try
    {
        g_smtc.IsPlayEnabled(play);
        g_smtc.IsPauseEnabled(pause);
        g_smtc.IsNextEnabled(next);
        g_smtc.IsPreviousEnabled(prev);
    }
    catch (const winrt::hresult_error& e)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[smtc] smtc_set_enabled_buttons failed: %ls\n", e.message().c_str());
        OutputDebugStringA(buf);
    }
    catch (...)
    {
        OutputDebugStringA("[smtc] smtc_set_enabled_buttons failed: unknown error\n");
    }
}

extern "C" __declspec(dllexport) int smtc_poll_button()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_buttonQueue.empty()) return -1;
    int code = g_buttonQueue.front();
    g_buttonQueue.pop();
    return code;
}

extern "C" __declspec(dllexport) void smtc_destroy()
{
    ensure_com();
    std::lock_guard<std::mutex> lock(g_mtx);
    while (!g_buttonQueue.empty()) g_buttonQueue.pop();

    if (g_smtc)
    {
        try
        {
            g_smtc.IsEnabled(false);
            g_smtc.ButtonPressed(nullptr);
        }
        catch (const winrt::hresult_error& e)
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "[smtc] smtc_destroy failed: %ls\n", e.message().c_str());
            OutputDebugStringA(buf);
        }
        catch (...) { }
        g_smtc = nullptr;
    }

    if (g_player)
    {
        try { g_player.Close(); } catch (...) { }
        g_player = nullptr;
    }
}
