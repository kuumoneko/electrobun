// test.cpp - Windows SMTC via MediaPlayer with silent audio for SMTC activation

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <functional>
#include <mutex>

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Storage::Streams;
using namespace Windows::Storage;

// --- Globals ---
static bool g_initialized = false;
static MediaPlayer g_player = nullptr;
static SystemMediaTransportControls g_smtc = nullptr;
static std::mutex g_mtx;

static std::function<void()> g_onPlay;
static std::function<void()> g_onPause;
static std::function<void()> g_onNext;
static std::function<void()> g_onPrevious;

// --- Helpers ---

static hstring toHString(const char* utf8) {
  if (!utf8 || !utf8[0]) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (len <= 1) return {};
  std::wstring wstr(static_cast<size_t>(len) - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr.data(), len);
  return hstring(wstr.c_str(), static_cast<uint32_t>(wstr.size()));
}

// --- Create a silent WAV in memory for MediaPlayer to play (activates SMTC) ---

static MediaSource createSilentSource() {
  // WAV header (44 bytes): 8-bit unsigned PCM, 1 channel, 8kHz
  unsigned char header[] = {
    0x52,0x49,0x46,0x46, 0x00,0x00,0x00,0x00, 0x57,0x41,0x56,0x45,  // RIFF, WAVE
    0x66,0x6D,0x74,0x20, 0x10,0x00,0x00,0x00, 0x01,0x00, 0x01,0x00,  // fmt , PCM, mono
    0x40,0x1F,0x00,0x00, 0x40,0x1F,0x00,0x00, 0x01,0x00, 0x08,0x00,  // 8kHz, 8-bit
    0x64,0x61,0x74,0x61, 0x00,0x00,0x00,0x00                               // data
  };

  // 0.5 seconds of silence at 8kHz 8-bit = 4000 samples
  unsigned char silence[4000];
  memset(silence, 0x80, sizeof(silence)); // 0x80 = midpoint for 8-bit unsigned PCM = silence

  auto stream = InMemoryRandomAccessStream();
  auto writer = DataWriter(stream);

  // Write header (fix sizes)
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

// --- FFI Exports ---

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
    g_player.Volume(0.0); // cannot be heard
    g_player.Play();      // activates SMTC session

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

    // Register button-pressed event handler
    g_smtc.ButtonPressed([](
      const SystemMediaTransportControls&,
      const SystemMediaTransportControlsButtonPressedEventArgs& args)
    {
      std::lock_guard<std::mutex> lk(g_mtx);
      switch (args.Button())
      {
      case SystemMediaTransportControlsButton::Play:
        if (g_onPlay) g_onPlay();
        break;
      case SystemMediaTransportControlsButton::Pause:
        if (g_onPause) g_onPause();
        break;
      case SystemMediaTransportControlsButton::Next:
        if (g_onNext) g_onNext();
        break;
      case SystemMediaTransportControlsButton::Previous:
        if (g_onPrevious) g_onPrevious();
        break;
      }
    });

    g_initialized = true;
    return true;
  }
  catch (const winrt::hresult_error& e)
  {
    OutputDebugStringW(e.message().c_str());
    fprintf(stderr, "[smtc] init failed: %ls\n", e.message().c_str());
    return false;
  }
}

extern "C" __declspec(dllexport) void smtc_update_metadata(
  const char* title,
  const char* artist,
  const char* thumbnailPath)
{
  if (!g_smtc) return;
  std::lock_guard<std::mutex> lock(g_mtx);

  try
  {
    auto updater = g_smtc.DisplayUpdater();
    updater.Type(MediaPlaybackType::Music);

    auto music = updater.MusicProperties();
    music.Title(toHString(title));
    music.Artist(toHString(artist));

    if (thumbnailPath && thumbnailPath[0])
    {
      try
      {
        auto uriStr = toHString(thumbnailPath);
        // If starts with http:// or https:// use CreateFromUri, else treat as local file
        if (uriStr.c_str()[0] == L'h' && wcsstr(uriStr.c_str(), L"://"))
          updater.Thumbnail(RandomAccessStreamReference::CreateFromUri(
            winrt::Windows::Foundation::Uri(uriStr)));
        else
          updater.Thumbnail(RandomAccessStreamReference::CreateFromFile(
            StorageFile::GetFileFromPathAsync(uriStr).get()));
      }
      catch (...) { }
    }

    updater.Update();
  }
  catch (...) { }
}

extern "C" __declspec(dllexport) void smtc_set_playback_state(bool isPlaying)
{
  if (!g_smtc) return;
  try
  {
    g_smtc.PlaybackStatus(
      isPlaying ? MediaPlaybackStatus::Playing
                : MediaPlaybackStatus::Paused);
  }
  catch (...) { }
}

extern "C" __declspec(dllexport) void smtc_set_enabled_buttons(
  bool play, bool pause, bool next, bool prev)
{
  if (!g_smtc) return;
  try
  {
    g_smtc.IsPlayEnabled(play);
    g_smtc.IsPauseEnabled(pause);
    g_smtc.IsNextEnabled(next);
    g_smtc.IsPreviousEnabled(prev);
  }
  catch (...) { }
}

extern "C" __declspec(dllexport) void smtc_register_callbacks(
  void (*onPlay)(),
  void (*onPause)(),
  void (*onNext)(),
  void (*onPrevious)())
{
  std::lock_guard<std::mutex> lock(g_mtx);
  g_onPlay   = onPlay   ? std::function<void()>(onPlay)   : nullptr;
  g_onPause  = onPause  ? std::function<void()>(onPause)  : nullptr;
  g_onNext   = onNext   ? std::function<void()>(onNext)   : nullptr;
  g_onPrevious = onPrevious ? std::function<void()>(onPrevious) : nullptr;
}

extern "C" __declspec(dllexport) void smtc_destroy()
{
  std::lock_guard<std::mutex> lock(g_mtx);
  g_onPlay   = nullptr;
  g_onPause  = nullptr;
  g_onNext   = nullptr;
  g_onPrevious = nullptr;

  if (g_smtc)
  {
    try
    {
      g_smtc.IsEnabled(false);
      g_smtc.ButtonPressed(nullptr);
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
