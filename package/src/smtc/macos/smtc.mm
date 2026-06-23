#import <MediaPlayer/MediaPlayer.h>
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#include <mutex>
#include <queue>
#include <string>
#include <cstdio>

static bool g_initialized = false;
static bool g_isPlaying = false;
static bool g_isList = false;
static std::mutex g_mtx;
static std::queue<int> g_buttonQueue;
static std::string g_title;
static std::string g_artist;
static std::string g_thumbnailPath;

@interface SMTCDelegate : NSObject
@property (nonatomic, assign) bool isPlaying;
@property (nonatomic, assign) bool isList;
@end

@implementation SMTCDelegate

- (instancetype)init
{
    self = [super init];
    if (self) {
        _isPlaying = false;
        _isList = false;
    }
    return self;
}

- (void)setupCommands
{
    MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];

    __weak SMTCDelegate *weakSelf = self;

    center.playCommand.enabled = YES;
    [center.playCommand addTargetUsingBlock:^(MPRemoteCommandEvent * _Nonnull event) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_buttonQueue.push(0);
    }];

    center.pauseCommand.enabled = YES;
    [center.pauseCommand addTargetUsingBlock:^(MPRemoteCommandEvent * _Nonnull event) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_buttonQueue.push(1);
    }];

    center.nextTrackCommand.enabled = YES;
    [center.nextTrackCommand addTargetUsingBlock:^(MPRemoteCommandEvent * _Nonnull event) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_buttonQueue.push(2);
    }];

    center.previousTrackCommand.enabled = YES;
    [center.previousTrackCommand addTargetUsingBlock:^(MPRemoteCommandEvent * _Nonnull event) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_buttonQueue.push(3);
    }];
}

- (void)updateNowPlaying
{
    MPNowPlayingInfoCenter *infoCenter = [MPNowPlayingInfoCenter defaultCenter];
    NSMutableDictionary *info = [NSMutableDictionary dictionary];

    if (!g_title.empty()) {
        NSString *title = [NSString stringWithUTF8String:g_title.c_str()];
        info[MPMediaItemPropertyTitle] = title;
    }

    if (!g_artist.empty()) {
        NSString *artist = [NSString stringWithUTF8String:g_artist.c_str()];
        info[MPMediaItemPropertyArtist] = artist;
    }

    info[MPNowPlayingInfoPropertyPlaybackRate] = g_isPlaying ? @1.0 : @0.0;
    info[MPMediaItemPropertyPlaybackDuration] = @0;

    if (!g_thumbnailPath.empty()) {
        NSString *path = [NSString stringWithUTF8String:g_thumbnailPath.c_str()];
        NSImage *image = nil;

        if ([path hasPrefix:@"http://"] || [path hasPrefix:@"https://"]) {
            NSURL *url = [NSURL URLWithString:path];
            if (url) {
                image = [[NSImage alloc] initWithContentsOfURL:url];
            }
        } else {
            image = [[NSImage alloc] initWithContentsOfFile:path];
        }

        if (image) {
            MPMediaItemArtwork *artwork = [[MPMediaItemArtwork alloc]
                initWithBoundsSize:NSMakeSize(512, 512)
                requestHandler:^NSImage *(CGSize size) {
                    return image;
                }];
            info[MPMediaItemPropertyArtwork] = artwork;
            [artwork release];
            [image release];
        }
    }

    infoCenter.nowPlayingInfo = info;
}

- (void)setPlaybackState:(bool)playing
{
    MPNowPlayingInfoCenter *infoCenter = [MPNowPlayingInfoCenter defaultCenter];
    NSMutableDictionary *info = [infoCenter.nowPlayingInfo mutableCopy];
    if (!info) {
        info = [NSMutableDictionary dictionary];
    }
    info[MPNowPlayingInfoPropertyPlaybackRate] = playing ? @1.0 : @0.0;
    infoCenter.nowPlayingInfo = info;
    [info release];
}

@end

static SMTCDelegate *g_delegate = nil;

extern "C" bool smtc_init()
{
    if (g_initialized) return true;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_initialized) return true;

    @autoreleasepool {
        g_delegate = [[SMTCDelegate alloc] init];
        if (!g_delegate) return false;

        [g_delegate setupCommands];
        [g_delegate updateNowPlaying];

        [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    }

    g_initialized = true;
    return true;
}

extern "C" int smtc_update_metadata(
    const char *title,
    const char *artist,
    const char *thumbnailPath,
    bool IsList)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_initialized) return 1;

    g_title = title ? title : "";
    g_artist = artist ? artist : "";
    g_thumbnailPath = thumbnailPath ? thumbnailPath : "";
    g_isList = IsList;

    @autoreleasepool {
        [g_delegate setPlaybackState:g_isPlaying];

        MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
        center.nextTrackCommand.enabled = IsList ? YES : NO;
        center.previousTrackCommand.enabled = IsList ? YES : NO;

        [g_delegate updateNowPlaying];
    }

    return 0;
}

extern "C" void smtc_set_playback_state(bool isPlaying)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_initialized) return;

    g_isPlaying = isPlaying;

    @autoreleasepool {
        [g_delegate setPlaybackState:isPlaying];
    }
}

extern "C" void smtc_set_enabled_buttons(
    bool play, bool pause, bool next, bool prev)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!g_initialized) return;

    g_isList = next || prev;

    @autoreleasepool {
        MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
        center.playCommand.enabled = play ? YES : NO;
        center.pauseCommand.enabled = pause ? YES : NO;
        center.nextTrackCommand.enabled = (next || prev) ? YES : NO;
        center.previousTrackCommand.enabled = (next || prev) ? YES : NO;
    }
}

extern "C" int smtc_poll_button()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_buttonQueue.empty()) return -1;
    int code = g_buttonQueue.front();
    g_buttonQueue.pop();
    return code;
}

extern "C" void smtc_destroy()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    while (!g_buttonQueue.empty())
        g_buttonQueue.pop();

    @autoreleasepool {
        MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
        center.playCommand.enabled = NO;
        center.pauseCommand.enabled = NO;
        center.nextTrackCommand.enabled = NO;
        center.previousTrackCommand.enabled = NO;

        [center.playCommand removeTarget:nil];
        [center.pauseCommand removeTarget:nil];
        [center.nextTrackCommand removeTarget:nil];
        [center.previousTrackCommand removeTarget:nil];

        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;

        if (g_delegate) {
            [g_delegate release];
            g_delegate = nil;
        }
    }

    g_initialized = false;
}
