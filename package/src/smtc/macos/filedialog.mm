#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#include <cstring>
#include <cstdio>

static char g_result[4096] = {};

extern "C" const char* open_folder_dialog(const char* startingFolder)
{
    g_result[0] = '\0';

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setCanCreateDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Select Folder"];

        if (startingFolder && startingFolder[0]) {
            NSString *path = [NSString stringWithUTF8String:startingFolder];
            NSURL *url = [NSURL fileURLWithPath:path];
            if (url) {
                [panel setDirectoryURL:url];
            }
        }

        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK) {
            return nullptr;
        }

        NSURL *url = [panel URL];
        if (!url) {
            return nullptr;
        }

        NSString *path = [url path];
        if (!path) {
            return nullptr;
        }

        const char *utf8Path = [path UTF8String];
        if (!utf8Path) {
            return nullptr;
        }

        size_t len = strlen(utf8Path);
        if (len >= sizeof(g_result))
            len = sizeof(g_result) - 1;

        memcpy(g_result, utf8Path, len);
        g_result[len] = '\0';
    }

    return g_result;
}
