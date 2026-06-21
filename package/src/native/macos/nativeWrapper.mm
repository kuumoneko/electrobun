/*
 * =============================================================================
 * 1. IMPORTS
 * =============================================================================
 */

#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonCrypto.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include <dlfcn.h>
#include <math.h>
#import <UserNotifications/UserNotifications.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include "../shared/pending_resize_queue.h"

#include <string>
#include <vector>
#include <list>
#include <cstdint>
#include <chrono>
#include <map>
#include <mutex>
#include <atomic>

// Shared cross-platform utilities
#include "../shared/glob_match.h"
#include "../shared/callbacks.h"
#include "../shared/permissions.h"
#include "../shared/mime_types.h"
#include "../shared/asar.h"
#include "../shared/config.h"
#include "../shared/preload_script.h"
#include "../shared/webview_storage.h"
#include "../shared/navigation_rules.h"
#include "../shared/thread_safe_map.h"
#include "../shared/shutdown_guard.h"
#include "../shared/ffi_helpers.h"
#include "../shared/download_event.h"
#include "../shared/app_paths.h"
#include "../shared/accelerator_parser.h"

using namespace electrobun;

/*
 * =============================================================================
 * 2. CONSTANTS, GLOBAL VARIABLES, FORWARD DECLARATIONS & TYPE DEFINITIONS
 * =============================================================================
 */

// Global ASAR archive handle (lazy-loaded) with thread-safe initialization
// ASAR C FFI declarations are in shared/asar.h
static AsarArchive* g_asarArchive = nullptr;
static std::once_flag g_asarArchiveInitFlag;

CGFloat OFFSCREEN_OFFSET = -20000;
std::string g_electrobunChannel = "";
std::string g_electrobunIdentifier = "";

static BOOL isMovingWindow = NO;
static NSWindow *targetWindow = nil;
static CGFloat offsetX = 0.0;
static CGFloat offsetY = 0.0;
static id mouseDraggedMonitor = nil;
static id mouseUpMonitor = nil;

// Menu role to selector mapping
// This maps Electrobun role strings to their corresponding Objective-C selectors.
// Roles are grouped by category for easier maintenance.
static NSDictionary<NSString*, NSString*>* getMenuRoleToSelectorMap() {
    static NSDictionary<NSString*, NSString*>* map = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        map = @{
            // Application roles
            @"about": @"orderFrontStandardAboutPanel:",
            @"quit": @"terminate:",
            @"hide": @"hide:",
            @"hideOthers": @"hideOtherApplications:",
            @"showAll": @"unhideAllApplications:",

            // Window roles
            @"minimize": @"performMiniaturize:",
            @"zoom": @"performZoom:",
            @"close": @"performClose:",
            @"bringAllToFront": @"arrangeInFront:",
            @"cycleThroughWindows": @"selectNextKeyView:",
            @"enterFullScreen": @"enterFullScreen:",
            @"exitFullScreen": @"exitFullScreen:",
            @"toggleFullScreen": @"toggleFullScreen:",

            // Standard edit roles
            @"undo": @"undo:",
            @"redo": @"redo:",
            @"cut": @"cut:",
            @"copy": @"copy:",
            @"paste": @"paste:",
            @"pasteAndMatchStyle": @"pasteAsPlainText:",
            @"delete": @"delete:",
            @"selectAll": @"selectAll:",

            // Speech roles
            @"startSpeaking": @"startSpeaking:",
            @"stopSpeaking": @"stopSpeaking:",

            // Help
            @"showHelp": @"showHelp:",

            // Movement - basic
            @"moveForward": @"moveForward:",
            @"moveBackward": @"moveBackward:",
            @"moveLeft": @"moveLeft:",
            @"moveRight": @"moveRight:",
            @"moveUp": @"moveUp:",
            @"moveDown": @"moveDown:",

            // Movement - by word
            @"moveWordForward": @"moveWordForward:",
            @"moveWordBackward": @"moveWordBackward:",
            @"moveWordLeft": @"moveWordLeft:",
            @"moveWordRight": @"moveWordRight:",

            // Movement - by line
            @"moveToBeginningOfLine": @"moveToBeginningOfLine:",
            @"moveToEndOfLine": @"moveToEndOfLine:",
            @"moveToLeftEndOfLine": @"moveToLeftEndOfLine:",
            @"moveToRightEndOfLine": @"moveToRightEndOfLine:",

            // Movement - by paragraph
            @"moveToBeginningOfParagraph": @"moveToBeginningOfParagraph:",
            @"moveToEndOfParagraph": @"moveToEndOfParagraph:",
            @"moveParagraphForward": @"moveParagraphForward:",
            @"moveParagraphBackward": @"moveParagraphBackward:",

            // Movement - by document
            @"moveToBeginningOfDocument": @"moveToBeginningOfDocument:",
            @"moveToEndOfDocument": @"moveToEndOfDocument:",

            // Movement with selection - basic
            @"moveForwardAndModifySelection": @"moveForwardAndModifySelection:",
            @"moveBackwardAndModifySelection": @"moveBackwardAndModifySelection:",
            @"moveLeftAndModifySelection": @"moveLeftAndModifySelection:",
            @"moveRightAndModifySelection": @"moveRightAndModifySelection:",
            @"moveUpAndModifySelection": @"moveUpAndModifySelection:",
            @"moveDownAndModifySelection": @"moveDownAndModifySelection:",

            // Movement with selection - by word
            @"moveWordForwardAndModifySelection": @"moveWordForwardAndModifySelection:",
            @"moveWordBackwardAndModifySelection": @"moveWordBackwardAndModifySelection:",
            @"moveWordLeftAndModifySelection": @"moveWordLeftAndModifySelection:",
            @"moveWordRightAndModifySelection": @"moveWordRightAndModifySelection:",

            // Movement with selection - by line
            @"moveToBeginningOfLineAndModifySelection": @"moveToBeginningOfLineAndModifySelection:",
            @"moveToEndOfLineAndModifySelection": @"moveToEndOfLineAndModifySelection:",
            @"moveToLeftEndOfLineAndModifySelection": @"moveToLeftEndOfLineAndModifySelection:",
            @"moveToRightEndOfLineAndModifySelection": @"moveToRightEndOfLineAndModifySelection:",

            // Movement with selection - by paragraph
            @"moveToBeginningOfParagraphAndModifySelection": @"moveToBeginningOfParagraphAndModifySelection:",
            @"moveToEndOfParagraphAndModifySelection": @"moveToEndOfParagraphAndModifySelection:",
            @"moveParagraphForwardAndModifySelection": @"moveParagraphForwardAndModifySelection:",
            @"moveParagraphBackwardAndModifySelection": @"moveParagraphBackwardAndModifySelection:",

            // Movement with selection - by document
            @"moveToBeginningOfDocumentAndModifySelection": @"moveToBeginningOfDocumentAndModifySelection:",
            @"moveToEndOfDocumentAndModifySelection": @"moveToEndOfDocumentAndModifySelection:",

            // Page movement
            @"pageUp": @"pageUp:",
            @"pageDown": @"pageDown:",
            @"pageUpAndModifySelection": @"pageUpAndModifySelection:",
            @"pageDownAndModifySelection": @"pageDownAndModifySelection:",

            // Scrolling
            @"scrollLineUp": @"scrollLineUp:",
            @"scrollLineDown": @"scrollLineDown:",
            @"scrollPageUp": @"scrollPageUp:",
            @"scrollPageDown": @"scrollPageDown:",
            @"scrollToBeginningOfDocument": @"scrollToBeginningOfDocument:",
            @"scrollToEndOfDocument": @"scrollToEndOfDocument:",
            @"centerSelectionInVisibleArea": @"centerSelectionInVisibleArea:",

            // Deletion - character
            @"deleteBackward": @"deleteBackward:",
            @"deleteForward": @"deleteForward:",
            @"deleteBackwardByDecomposingPreviousCharacter": @"deleteBackwardByDecomposingPreviousCharacter:",

            // Deletion - word
            @"deleteWordBackward": @"deleteWordBackward:",
            @"deleteWordForward": @"deleteWordForward:",

            // Deletion - line
            @"deleteToBeginningOfLine": @"deleteToBeginningOfLine:",
            @"deleteToEndOfLine": @"deleteToEndOfLine:",

            // Deletion - paragraph
            @"deleteToBeginningOfParagraph": @"deleteToBeginningOfParagraph:",
            @"deleteToEndOfParagraph": @"deleteToEndOfParagraph:",

            // Selection
            @"selectWord": @"selectWord:",
            @"selectLine": @"selectLine:",
            @"selectParagraph": @"selectParagraph:",
            @"selectToMark": @"selectToMark:",
            @"setMark": @"setMark:",
            @"swapWithMark": @"swapWithMark:",
            @"deleteToMark": @"deleteToMark:",

            // Text transformation
            @"capitalizeWord": @"capitalizeWord:",
            @"uppercaseWord": @"uppercaseWord:",
            @"lowercaseWord": @"lowercaseWord:",
            @"transpose": @"transpose:",
            @"transposeWords": @"transposeWords:",

            // Insertion
            @"insertNewline": @"insertNewline:",
            @"insertLineBreak": @"insertLineBreak:",
            @"insertParagraphSeparator": @"insertParagraphSeparator:",
            @"insertTab": @"insertTab:",
            @"insertBacktab": @"insertBacktab:",
            @"insertTabIgnoringFieldEditor": @"insertTabIgnoringFieldEditor:",
            @"insertNewlineIgnoringFieldEditor": @"insertNewlineIgnoringFieldEditor:",

            // Kill ring (Emacs-style)
            @"yank": @"yank:",
            @"yankAndSelect": @"yankAndSelect:",

            // Completion
            @"complete": @"complete:",
            @"cancelOperation": @"cancelOperation:",

            // Indentation
            @"indent": @"indent:",
        };
    });
    return map;
}

static bool IsPortAvailable(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    int result = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    return result == 0;
}

static int FindAvailableRemoteDebugPort(int startPort, int endPort) {
    for (int port = startPort; port <= endPort; ++port) {
        if (IsPortAvailable(port)) {
            return port;
        }
    }
    return 0;
}


// Type definitions
// Core callback types are defined in shared/callbacks.h
// Platform-specific aliases for Objective-C compatibility
typedef BOOL (*HandlePostMessageObjC)(uint32_t webviewId, const char* message);
typedef void (*callAsyncJavascriptCompletionHandler)(const char *messageId, uint32_t webviewId, uint32_t hostWebviewId, const char *responseJSON);

static dispatch_queue_t jsWorkerQueue = NULL;

// Webview content storage (replaces JSCallback approach)
static NSMutableDictionary<NSNumber*, NSString*> *webviewHTMLContent = nil;
static NSLock *webviewHTMLLock = nil;

// Forward declarations for HTML content management
extern "C" const char* getWebviewHTMLContent(uint32_t webviewId);
extern "C" void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent);

// MIME type detection function is in shared/mime_types.h

// Deadlock prevention for callJsCallbackFromMainSync
static BOOL isInSyncCallback = NO;
static NSMutableArray *queuedCallbacks = nil;

// this lets you call non-threadsafe JSCallbacks on the bun worker thread, from the main thread
// and wait for the response. 
// use it like:
// REMOVED: jsUtils.getHTMLForWebviewSync callback (now using webviewHTMLContent map)
// });
// 
// DEADLOCK PREVENTION: If called recursively (e.g., during URL scheme handling), 
// queues the callback for later execution to prevent deadlocks.
static const char* callJsCallbackFromMainSync(const char* (^callback)(void)) {
    NSLog(@"callJSCallbackFromMainSync 1");
    if (!jsWorkerQueue) {
        NSLog(@"Error: JS worker queue not initialized");
        return NULL;
    }
    
    // Initialize queue if needed
    if (!queuedCallbacks) {
        NSLog(@"callJSCallbackFromMainSync 2");
        queuedCallbacks = [[NSMutableArray alloc] init];
    }

    NSLog(@"callJSCallbackFromMainSync 3");
    
    // Prevent recursive calls that can cause deadlocks
    if (isInSyncCallback) {
        NSLog(@"callJSCallbackFromMainSync 4");
        NSLog(@"callJsCallbackFromMainSync: Preventing deadlock - queueing callback for later execution");
        // For queued callbacks, we can't return a meaningful result since they're async
        // This is fine since recursive calls are typically RPC sends that don't need return values
        [queuedCallbacks addObject:[callback copy]];
        NSLog(@"callJSCallbackFromMainSync 5");
        return NULL;
    }
    NSLog(@"callJSCallbackFromMainSync 6");
    
    isInSyncCallback = YES;
    
    __block const char* result = NULL;
    __block char* resultCopy = NULL;
    NSLog(@"callJSCallbackFromMainSync 7");
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSLog(@"callJSCallbackFromMainSync 8");
    dispatch_async(jsWorkerQueue, ^{
        NSLog(@"callJSCallbackFromMainSync 9");
        
        @try {
            // Call the provided block (which executes the JS callback)
            result = callback();
            NSLog(@"callJSCallbackFromMainSync 10");
        } @catch (NSException *exception) {
            NSLog(@"callJSCallbackFromMainSync: Exception caught during callback execution: %@", exception);
            result = NULL;
        } @catch (...) {
            NSLog(@"callJSCallbackFromMainSync: Unknown exception caught during callback execution");
            result = NULL;
        }
        
        // Duplicate the result so it won't be garbage collected.
        if (result != NULL) {
            NSLog(@"callJSCallbackFromMainSync 11");
            resultCopy = strdup(result);
        }
        NSLog(@"callJSCallbackFromMainSync 12");
        
        dispatch_semaphore_signal(semaphore);
        NSLog(@"callJSCallbackFromMainSync 13");
    });
    
    // Add timeout to prevent indefinite blocking during process failures
    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC); // 5 second timeout
    long result_wait = dispatch_semaphore_wait(semaphore, timeout);
    
    if (result_wait != 0) {
        NSLog(@"callJSCallbackFromMainSync: Timeout waiting for callback completion - possible process failure");
        isInSyncCallback = NO;
        return NULL;
    }
    
    NSLog(@"callJSCallbackFromMainSync 14");
    
    // Process any queued callbacks (these are typically fire-and-forget RPC calls)
    while (queuedCallbacks.count > 0) {
        NSLog(@"callJSCallbackFromMainSync 15");
        NSLog(@"callJsCallbackFromMainSync: Processing %lu queued callback(s)", (unsigned long)queuedCallbacks.count);
        const char* (^queuedCallback)(void) = queuedCallbacks[0];
        [queuedCallbacks removeObjectAtIndex:0];
        NSLog(@"callJSCallbackFromMainSync 16");
        // Execute queued callback asynchronously (these don't need return values)
        dispatch_async(jsWorkerQueue, ^{
            NSLog(@"callJSCallbackFromMainSync 17");
            @try {
                queuedCallback();
            } @catch (NSException *exception) {
                NSLog(@"callJSCallbackFromMainSync: Exception in queued callback: %@", exception);
            } @catch (...) {
                NSLog(@"callJSCallbackFromMainSync: Unknown exception in queued callback");
            }
            NSLog(@"callJSCallbackFromMainSync 18");
        });
    }
    
    isInSyncCallback = NO;
    NSLog(@"callJSCallbackFromMainSync 19");
    return resultCopy; // Caller is responsible for freeing this memory.
}

typedef struct {
    NSRect frame;
    uint32_t styleMask;
    const char *titleBarStyle;
} createNSWindowWithFrameAndStyleParams;

// Window, tray, menu, and snapshot callbacks are defined in shared/callbacks.h
// Platform-specific aliases
typedef SnapshotCallback zigSnapshotCallback;
typedef StatusItemHandler ZigStatusItemHandler;
static URLOpenHandler g_urlOpenHandler = nullptr;
static AppReopenHandler g_appReopenHandler = nullptr;
static QuitRequestedHandler g_quitRequestedHandler = nullptr;
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_eventLoopStopping{false};

typedef struct {
} MenuItemConfig;

// Permission cache types and functions are defined in shared/permissions.h

/*
 * =============================================================================
 * 3. UTILITY FUNCTIONS
 * =============================================================================
 */


extern "C" uint32_t getWindowStyle(
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
    bool HUDWindow
) {
    uint32_t mask = 0;
    if (Borderless) mask |= NSWindowStyleMaskBorderless;
    if (Titled) mask |= NSWindowStyleMaskTitled;
    if (Closable) mask |= NSWindowStyleMaskClosable;
    if (Miniaturizable) mask |= NSWindowStyleMaskMiniaturizable;
    if (Resizable) mask |= NSWindowStyleMaskResizable;
    if (UnifiedTitleAndToolbar) mask |= NSWindowStyleMaskUnifiedTitleAndToolbar;
    if (FullScreen) mask |= NSWindowStyleMaskFullScreen;
    if (FullSizeContentView) mask |= NSWindowStyleMaskFullSizeContentView;
    if (UtilityWindow) mask |= NSWindowStyleMaskUtilityWindow;
    if (DocModalWindow) mask |= NSWindowStyleMaskDocModalWindow;
    if (NonactivatingPanel) mask |= NSWindowStyleMaskNonactivatingPanel;
    if (HUDWindow) mask |= NSWindowStyleMaskHUDWindow;
    return mask;
}

std::string GetScriptExecutionUrl(const std::string& frameUrl) {
    // List of URL schemes that should use about:blank for script execution
    static const std::vector<std::string> specialSchemes = {
        "data:",
        "blob:",
        "file:"
        // Add other schemes as needed
    };
    
    for (const auto& scheme : specialSchemes) {
        if (frameUrl.substr(0, scheme.length()) == scheme) {
            return "data://___preload.js";
        }
    }
    
    return frameUrl;
}

NSUUID *UUIDFromString(NSString *string) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(string.UTF8String, (CC_LONG)string.length, hash);
    uuid_t uuid;
    memcpy(uuid, hash, sizeof(uuid));
    return [[NSUUID alloc] initWithUUIDBytes:uuid];
}

WKWebsiteDataStore* createDataStoreForPartition(const char* partitionIdentifier) {
    NSString *identifier = [NSString stringWithUTF8String:partitionIdentifier];
    if ([identifier hasPrefix:@"persist:"]) {
        // persistent
        identifier = [identifier substringFromIndex:8];
        NSUUID *uuid = UUIDFromString(identifier);
        if (uuid) {
            // dataStoreForIdentifier is only available on macOS 14.0+
            if (@available(macOS 14.0, *)) {
                return [WKWebsiteDataStore dataStoreForIdentifier:uuid];
            } else {
                // Fallback to default data store on older macOS versions
                NSLog(@"[Session] Partition-specific data stores require macOS 14.0+, using default store");
                return [WKWebsiteDataStore defaultDataStore];
            }
        } else {
            NSLog(@"Invalid UUID for identifier: %@", identifier);
            return [WKWebsiteDataStore defaultDataStore];
        }
    } else {
        // ephemeral
        return [WKWebsiteDataStore nonPersistentDataStore];
    }
}

static NSString* normalizeViewsRelativePath(NSString *urlString) {
    if (!urlString || ![urlString hasPrefix:@"views://"]) {
        return nil;
    }

    NSString *relativePath = [urlString substringFromIndex:8];
    while ([relativePath hasPrefix:@"/"]) {
        relativePath = [relativePath substringFromIndex:1];
    }

    return relativePath;
}

NSData* readViewsFile(const char* viewsUrl) {
    if (!viewsUrl) return nil;

    NSString *urlString = [NSString stringWithUTF8String:viewsUrl];
    NSString *relativePath = normalizeViewsRelativePath(urlString);
    if (!relativePath) {
        return nil;
    }

    // Get the current working directory and Resources path
    NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    NSString *resourcesDir = [cwd stringByAppendingPathComponent:@"../Resources"];
    NSString *asarPath = [resourcesDir stringByAppendingPathComponent:@"app.asar"];

    // Check if ASAR archive exists
    if ([[NSFileManager defaultManager] fileExistsAtPath:asarPath]) {
        // Thread-safe lazy-load ASAR archive on first use
        std::call_once(g_asarArchiveInitFlag, [asarPath]() {
            const char* asarPathCStr = [asarPath UTF8String];
            g_asarArchive = asar_open(asarPathCStr);
            if (!g_asarArchive) {
                NSLog(@"ERROR readViewsFile: Failed to open ASAR archive at %@", asarPath);
            }
        });

        // If ASAR archive is loaded, try to read from it
        if (g_asarArchive) {
            // The ASAR contains the entire app directory, so prepend "views/" to the relativePath
            NSString *asarFilePath = [NSString stringWithFormat:@"views/%@", relativePath];
            const char* asarFilePathCStr = [asarFilePath UTF8String];

            size_t fileSize = 0;
            const uint8_t* fileData = asar_read_file(g_asarArchive, asarFilePathCStr, &fileSize);

            if (fileData && fileSize > 0) {
                // Create NSData that copies the buffer (we'll free it after)
                NSData *data = [NSData dataWithBytes:fileData length:fileSize];
                // Free the ASAR buffer
                asar_free_buffer(fileData, fileSize);
                return data;
            }
        }
    }

    // Fallback: Read from flat file system (for non-ASAR builds or missing files)
    NSString *viewsDir = [resourcesDir stringByAppendingPathComponent:@"app/views"];
    NSString *filePath = [viewsDir stringByAppendingPathComponent:relativePath];

    // Read the file
    return [NSData dataWithContentsOfFile:filePath];
}

NSData* readViewsFileWithRoot(const char* viewsUrl, NSString *viewsRoot) {
    if (!viewsRoot || viewsRoot.length == 0) {
        return readViewsFile(viewsUrl);
    }

    if (!viewsUrl) return nil;

    NSString *urlString = [NSString stringWithUTF8String:viewsUrl];
    NSString *relativePath = normalizeViewsRelativePath(urlString);
    if (!relativePath) {
        return nil;
    }

    NSString *normalizedRoot = [viewsRoot stringByStandardizingPath];
    NSString *candidatePath =
        [[viewsRoot stringByAppendingPathComponent:relativePath] stringByStandardizingPath];

    if (![candidatePath isEqualToString:normalizedRoot] &&
        ![candidatePath hasPrefix:[normalizedRoot stringByAppendingString:@"/"]]) {
        NSLog(@"ERROR readViewsFileWithRoot: path escapes root %@ -> %@", normalizedRoot, candidatePath);
        return nil;
    }

    return [NSData dataWithContentsOfFile:candidatePath];
}


// Convenience functions for manual memory management
void retainObjCObject(id objcObject) {
    CFRetain((__bridge CFTypeRef)objcObject);
}
void releaseObjCObject(id objcObject) {
    CFRelease((__bridge CFTypeRef)objcObject);
}

/*
 * =============================================================================
 * 4. OBJECTIVE-C @INTERFACES
 * =============================================================================
 */

// ----------------------- Abstract Base Classes -----------------------

@interface AbstractView : NSObject
    @property (nonatomic, assign) uint32_t webviewId;
    @property (nonatomic, assign) NSView * nsView;
    @property (nonatomic, assign) BOOL isMousePassthroughEnabled;
    @property (nonatomic, assign) BOOL mirrorModeEnabled;
    @property (nonatomic, assign) BOOL fullSize;
    @property (nonatomic, assign) BOOL isRemoved;
    @property (nonatomic, assign) BOOL isInFullscreen;
    @property (nonatomic, assign) BOOL isSandboxed;  // When true, only eventBridge is active (no RPC)
    @property (nonatomic, assign) BOOL pendingStartTransparent;
    @property (nonatomic, assign) BOOL pendingStartPassthrough;
    @property (nonatomic, strong) CALayer *storedLayerMask;
    @property (nonatomic, strong) NSArray<NSString *> *navigationRules;
    @property (atomic, assign) uint32_t resizeGeneration;

    - (void)loadURL:(const char *)urlString;
    - (void)loadHTML:(const char *)htmlString;
    - (void)goBack;
    - (void)goForward;
    - (void)reload;
    - (void)remove;

    - (void)setTransparent:(BOOL)transparent;
    - (void)setPassthrough:(BOOL)enable;
    - (void)setHidden:(BOOL)hidden;

    - (BOOL)canGoBack;
    - (BOOL)canGoForward;

    - (void)evaluateJavaScriptWithNoCompletion:(const char*)jsString;
    - (void)callAsyncJavascript:(const char*)messageId 
                       jsString:(const char*)jsString 
                      webviewId:(uint32_t)webviewId 
                  hostWebviewId:(uint32_t)hostWebviewId 
              completionHandler:(callAsyncJavascriptCompletionHandler)completionHandler;
    - (void)addPreloadScriptToWebView:(const char*)jsString;
    - (void)updateCustomPreloadScript:(const char*)jsString;

    - (void)resize:(NSRect)frame withMasksJSON:(const char *)masksJson;
    - (void)resizeWithFrame:(NSRect)frame parsedMasks:(NSArray *)parsedMasks;

    - (void)setNavigationRulesFromJSON:(const char*)rulesJson;
    - (BOOL)shouldAllowNavigationToURL:(NSString *)url;

    - (void)findInPage:(const char*)searchText forward:(BOOL)forward matchCase:(BOOL)matchCase;
    - (void)stopFindInPage;

    // Developer tools methods
    - (void)openDevTools;
    - (void)closeDevTools;
    - (void)toggleDevTools;
@end

@interface AbstractView () {
@public
    std::mutex pendingResizeMutex;
    std::atomic<uint64_t> pendingResizeGeneration;
    uint64_t appliedResizeGeneration;
    BOOL hasPendingResize;
    NSRect pendingResizeFrame;
    NSArray *pendingResizeMasks;
}
- (void)storePendingResize:(NSRect)frame parsedMasks:(NSArray *)parsedMasks;
- (void)applyPendingResizeIfNeeded;
@end

// Global map to track all AbstractView instances by their webviewId
static NSMutableDictionary<NSNumber *, AbstractView *> *globalAbstractViews = nil;

@interface ContainerView : NSView
    /// An reverse ordered array of abstractViews (newest first)
    @property (nonatomic, strong) NSMutableArray<AbstractView *> *abstractViews;
    - (void)addAbstractView:(AbstractView *)webview;
    - (void)removeAbstractViewWithId:(uint32_t)webviewId;
    - (void)updateActiveWebviewForMousePosition:(NSPoint)mouseLocation;
@end

// ----------------------- URL Scheme & Navigation -----------------------

@interface MyURLSchemeHandler : NSObject <WKURLSchemeHandler>    
    @property (nonatomic, assign) uint32_t webviewId;
    @property (nonatomic, copy) NSString *viewsRoot;
@end

@interface MyNavigationDelegate : NSObject <WKNavigationDelegate, WKDownloadDelegate>
    @property (nonatomic, assign) DecideNavigationCallback zigCallback;
    @property (nonatomic, assign) WebviewEventHandler zigEventHandler;
    @property (nonatomic, assign) uint32_t webviewId;
    @property (nonatomic, strong) NSMutableDictionary<NSValue *, NSString *> *downloadPaths;
    @property (nonatomic, strong) NSMutableSet<WKDownload *> *observedDownloads;
@end

@interface MyWebViewUIDelegate : NSObject <WKUIDelegate>
    @property (nonatomic, assign) WebviewEventHandler zigEventHandler;
    @property (nonatomic, assign) uint32_t webviewId;
@end

@interface MyScriptMessageHandler : NSObject <WKScriptMessageHandler>
    @property (nonatomic, assign) HandlePostMessage zigCallback;
    @property (nonatomic, assign) uint32_t webviewId;
@end

@interface MyScriptMessageHandlerWithReply : NSObject <WKScriptMessageHandlerWithReply>
    @property (nonatomic, assign) HandlePostMessageWithReply zigCallback;
    @property (nonatomic, assign) uint32_t webviewId;
@end

// ----------------------- Webview Implementations -----------------------
@interface WKWebViewImpl : AbstractView
    @property (nonatomic, strong) WKWebView *webView;

    - (instancetype)initWithWebviewId:(uint32_t)webviewId
                            window:(NSWindow *)window
                            url:(const char *)url
                                frame:(NSRect)frame
                        autoResize:(bool)autoResize
                partitionIdentifier:(const char *)partitionIdentifier
                navigationCallback:(DecideNavigationCallback)navigationCallback
                webviewEventHandler:(WebviewEventHandler)webviewEventHandler
                eventBridgeHandler:(HandlePostMessage)eventBridgeHandler
                bunBridgeHandler:(HandlePostMessage)bunBridgeHandler
                internalBridgeHandler:(HandlePostMessage)internalBridgeHandler
                electrobunPreloadScript:(const char *)electrobunPreloadScript
                customPreloadScript:(const char *)customPreloadScript
                viewsRoot:(const char *)viewsRoot
                transparent:(bool)transparent
                sandbox:(bool)sandbox;
@end

// ----------------------- Application & Window Delegates -----------------------

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@interface WindowDelegate : NSObject <NSWindowDelegate>
    @property (nonatomic, assign) WindowCloseHandler closeHandler;
    @property (nonatomic, assign) WindowMoveHandler moveHandler;
    @property (nonatomic, assign) WindowResizeHandler resizeHandler;
    @property (nonatomic, assign) WindowFocusHandler focusHandler;
    @property (nonatomic, assign) WindowBlurHandler blurHandler;
    @property (nonatomic, assign) WindowKeyHandler keyHandler;
    @property (nonatomic, assign) uint32_t windowId;
    @property (nonatomic, strong) NSWindow *window;
@end

@interface StatusItemTarget : NSObject
    @property (nonatomic, assign) NSStatusItem *statusItem;
    @property (nonatomic, assign) ZigStatusItemHandler zigHandler;
    @property (nonatomic, assign) uint32_t trayId;
    - (void)statusItemClicked:(id)sender;
    - (void)menuItemClicked:(id)sender;
@end

// Convert a key name string to an NSMenuItem key equivalent string.
// For single characters this is just the character itself. For special keys
// (arrows, function keys, etc.) it returns the appropriate Unicode character
// that NSMenuItem expects.
static NSString *keyEquivalentFromString(NSString *key) {
    if ([key length] == 1) {
        return key;
    }

    static NSDictionary *specialKeys = nil;
    if (!specialKeys) {
        specialKeys = @{
            @"return":   @"\r",
            @"enter":    @"\r",
            @"tab":      @"\t",
            @"escape":   [NSString stringWithFormat:@"%C", (unichar)0x1B],
            @"esc":      [NSString stringWithFormat:@"%C", (unichar)0x1B],
            @"space":    @" ",
            @"backspace": [NSString stringWithFormat:@"%C", (unichar)NSBackspaceCharacter],
            @"delete":   [NSString stringWithFormat:@"%C", (unichar)NSDeleteCharacter],
            @"up":       [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey],
            @"down":     [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey],
            @"left":     [NSString stringWithFormat:@"%C", (unichar)NSLeftArrowFunctionKey],
            @"right":    [NSString stringWithFormat:@"%C", (unichar)NSRightArrowFunctionKey],
            @"home":     [NSString stringWithFormat:@"%C", (unichar)NSHomeFunctionKey],
            @"end":      [NSString stringWithFormat:@"%C", (unichar)NSEndFunctionKey],
            @"pageup":   [NSString stringWithFormat:@"%C", (unichar)NSPageUpFunctionKey],
            @"pagedown": [NSString stringWithFormat:@"%C", (unichar)NSPageDownFunctionKey],
            @"f1":  [NSString stringWithFormat:@"%C", (unichar)NSF1FunctionKey],
            @"f2":  [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey],
            @"f3":  [NSString stringWithFormat:@"%C", (unichar)NSF3FunctionKey],
            @"f4":  [NSString stringWithFormat:@"%C", (unichar)NSF4FunctionKey],
            @"f5":  [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey],
            @"f6":  [NSString stringWithFormat:@"%C", (unichar)NSF6FunctionKey],
            @"f7":  [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey],
            @"f8":  [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey],
            @"f9":  [NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey],
            @"f10": [NSString stringWithFormat:@"%C", (unichar)NSF10FunctionKey],
            @"f11": [NSString stringWithFormat:@"%C", (unichar)NSF11FunctionKey],
            @"f12": [NSString stringWithFormat:@"%C", (unichar)NSF12FunctionKey],
            @"f13": [NSString stringWithFormat:@"%C", (unichar)NSF13FunctionKey],
            @"f14": [NSString stringWithFormat:@"%C", (unichar)NSF14FunctionKey],
            @"f15": [NSString stringWithFormat:@"%C", (unichar)NSF15FunctionKey],
            @"f16": [NSString stringWithFormat:@"%C", (unichar)NSF16FunctionKey],
            @"f17": [NSString stringWithFormat:@"%C", (unichar)NSF17FunctionKey],
            @"f18": [NSString stringWithFormat:@"%C", (unichar)NSF18FunctionKey],
            @"f19": [NSString stringWithFormat:@"%C", (unichar)NSF19FunctionKey],
            @"f20": [NSString stringWithFormat:@"%C", (unichar)NSF20FunctionKey],
            @"plus": @"+",
            @"minus": @"-",
        };
    }

    NSString *equivalent = specialKeys[key];
    return equivalent ?: key;
}

// Convert shared AcceleratorParts to macOS NSEventModifierFlags.
// On macOS, CommandOrControl and Command both map to the Command key.
static NSEventModifierFlags modifierFlagsFromAccelerator(const electrobun::AcceleratorParts& parts) {
    NSEventModifierFlags flags = 0;
    if (parts.commandOrControl || parts.command) flags |= NSEventModifierFlagCommand;
    if (parts.control)                           flags |= NSEventModifierFlagControl;
    if (parts.alt)                               flags |= NSEventModifierFlagOption;
    if (parts.shift)                             flags |= NSEventModifierFlagShift;
    return flags;
}

// Parse an Electron-style accelerator string into an NSMenuItem key equivalent
// and modifier mask. When the accelerator is a bare key with no modifiers
// (e.g. "s"), Command is used as the default modifier to match macOS conventions.
static void parseMenuAccelerator(NSString *accelerator,
                                 NSString **outKeyEquivalent,
                                 NSEventModifierFlags *outModifiers) {
    auto parts = electrobun::parseAccelerator([accelerator UTF8String]);

    *outModifiers = modifierFlagsFromAccelerator(parts);

    // Bare key like "s" with no modifier prefix — default to Command
    if (parts.isBareKey) {
        *outModifiers = NSEventModifierFlagCommand;
    }

    *outKeyEquivalent = keyEquivalentFromString(
        [NSString stringWithUTF8String:parts.key.c_str()]);
}

NSMenu *createMenuFromConfig(NSArray *menuConfig, StatusItemTarget *target) {
    NSMenu *menu = [[NSMenu alloc] init];
    [menu setAutoenablesItems:NO];

    for (NSDictionary *itemData in menuConfig) {
        NSString *type = itemData[@"type"];
        NSString *label = itemData[@"label"];
        NSString *action = itemData[@"action"];
        NSArray *submenuConfig = itemData[@"submenu"];
        NSString *role = itemData[@"role"];
        NSString *accelerator = itemData[@"accelerator"];
        NSNumber *modifierMask = itemData[@"modifierMask"];

        BOOL enabled = [itemData[@"enabled"] boolValue];
        BOOL checked = [itemData[@"checked"] boolValue];
        BOOL hidden = [itemData[@"hidden"] boolValue];
        NSString *tooltip = itemData[@"tooltip"];

        NSMenuItem *menuItem;
        if ([type isEqualToString:@"divider"]) {
            menuItem = [NSMenuItem separatorItem];
        } else {
            menuItem = [[NSMenuItem alloc] initWithTitle:label ?: @""
                                                  action:@selector(menuItemClicked:)
                                           keyEquivalent:@""];
            menuItem.representedObject = action;
            if (role) {
                // Look up the selector from the role map
                NSDictionary<NSString*, NSString*>* roleMap = getMenuRoleToSelectorMap();
                NSString *selectorName = roleMap[role];
                if (selectorName) {
                    menuItem.action = NSSelectorFromString(selectorName);
                }
                if (!accelerator) {
                    if ([role isEqualToString:@"undo"]) {
                        menuItem.keyEquivalent = @"z";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    } else if ([role isEqualToString:@"redo"]) {
                        menuItem.keyEquivalent = @"Z";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
                    } else if ([role isEqualToString:@"cut"]) {
                        menuItem.keyEquivalent = @"x";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    } else if ([role isEqualToString:@"copy"]) {
                        menuItem.keyEquivalent = @"c";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    } else if ([role isEqualToString:@"paste"]) {
                        menuItem.keyEquivalent = @"v";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    } else if ([role isEqualToString:@"pasteAndMatchStyle"]) {
                        menuItem.keyEquivalent = @"V";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
                    } else if ([role isEqualToString:@"delete"]) {
                        menuItem.keyEquivalent = [NSString stringWithFormat:@"%c",(char)NSDeleteCharacter];
                        menuItem.keyEquivalentModifierMask = 0;
                    } else if ([role isEqualToString:@"selectAll"]) {
                        menuItem.keyEquivalent = @"a";
                        menuItem.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    }
                }
            } else {
                menuItem.target = target;
            }
            if (accelerator) {
                if (modifierMask) {
                    // Explicit modifierMask from JSON takes precedence
                    menuItem.keyEquivalent = [accelerator lowercaseString];
                    menuItem.keyEquivalentModifierMask = [modifierMask unsignedIntegerValue];
                } else {
                    // Parse Electron-style accelerator (e.g. "CommandOrControl+T")
                    NSString *keyEq = nil;
                    NSEventModifierFlags modFlags = 0;
                    parseMenuAccelerator(accelerator, &keyEq, &modFlags);
                    menuItem.keyEquivalent = keyEq;
                    menuItem.keyEquivalentModifierMask = modFlags;
                }
            }
            menuItem.enabled = enabled;
            menuItem.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
            menuItem.hidden = hidden;
            menuItem.toolTip = tooltip;
            if (submenuConfig) {
                NSMenu *submenu = createMenuFromConfig(submenuConfig, target);
                [menu setSubmenu:submenu forItem:menuItem];
            }
        }
        [menu addItem:menuItem];
    }
    return menu;
}

/*
 * =============================================================================
 * 5. OBJECTIVE-C @IMPLEMENTATIONS
 * =============================================================================
 */

// ----------------------- AbstractView & ContainerView -----------------------
// Todo: incorporate into AbstractView
NSArray<NSValue *> *addOverlapRects(NSArray<NSDictionary *> *rectsArray, CGFloat containerHeight) {
    NSMutableArray<NSValue *> *resultingRects = [NSMutableArray array];
    for (NSDictionary *rectDict in rectsArray) {
        CGFloat x = [rectDict[@"x"] floatValue];
        CGFloat y = [rectDict[@"y"] floatValue];
        CGFloat w = [rectDict[@"width"] floatValue];
        CGFloat h = [rectDict[@"height"] floatValue];
                
        // The measured y from the dom (origin top) needs to be inverted
        // to work with MacOs default (y origin bottom) 
        if (containerHeight > 0) {
            y = containerHeight - h - y;
        }

        NSRect newRect = NSMakeRect(x, y, w, h);

        NSMutableArray<NSValue *> *overlapRects = [NSMutableArray array];
        for (NSValue *existingRectValue in resultingRects) {
            NSRect existingRect = [existingRectValue rectValue];
            if (NSIntersectsRect(existingRect, newRect)) {
                NSRect overlapRect = NSIntersectionRect(existingRect, newRect);
                if (!NSIsEmptyRect(overlapRect)) {
                    [overlapRects addObject:[NSValue valueWithRect:overlapRect]];
                }
            }
        }
        [resultingRects addObject:[NSValue valueWithRect:newRect]];
        [resultingRects addObjectsFromArray:overlapRects];
    }
    return resultingRects;
}

@implementation AbstractView

    - (instancetype)init {
        self = [super init];
        if (self) {
            self.isRemoved = NO;
            pendingResizeGeneration = 0;
            appliedResizeGeneration = 0;
            hasPendingResize = NO;
            pendingResizeFrame = NSZeroRect;
            pendingResizeMasks = nil;
        }
        return self;
    }

    - (void)loadURL:(const char *)urlString { [self doesNotRecognizeSelector:_cmd]; }
    - (void)loadHTML:(const char *)htmlString { [self doesNotRecognizeSelector:_cmd]; }
    - (void)goBack { [self doesNotRecognizeSelector:_cmd]; }
    - (void)goForward { [self doesNotRecognizeSelector:_cmd]; }
    - (void)reload { [self doesNotRecognizeSelector:_cmd]; }
    - (void)remove { [self doesNotRecognizeSelector:_cmd]; }


    - (BOOL)canGoBack { [self doesNotRecognizeSelector:_cmd]; return NO; }
    - (BOOL)canGoForward { [self doesNotRecognizeSelector:_cmd]; return NO; }

    - (void)evaluateJavaScriptWithNoCompletion:(const char*)jsString { [self doesNotRecognizeSelector:_cmd]; }
    - (void)callAsyncJavascript:(const char*)messageId jsString:(const char*)jsString webviewId:(uint32_t)webviewId hostWebviewId:(uint32_t)hostWebviewId completionHandler:(callAsyncJavascriptCompletionHandler)completionHandler { [self doesNotRecognizeSelector:_cmd]; }
    // todo: we don't need this to be public since it's only used to set the internal electrobun preview script
    - (void)addPreloadScriptToWebView:(const char*)jsString { [self doesNotRecognizeSelector:_cmd]; }
    - (void)updateCustomPreloadScript:(const char*)jsString { [self doesNotRecognizeSelector:_cmd]; }

    // todo: rename to toggleOffscreen / isOffscreen
    // then create isInteractive that returns !isOffscreen && isPassthrough


    - (void)setHidden:(BOOL)hidden {
        [self.nsView setHidden:hidden];
    }

    - (void)setPassthrough:(BOOL)enable {    
        self.isMousePassthroughEnabled = enable;
        // Re-evaluate active view immediately so passthrough takes effect without mouse movement
        if (self.nsView && self.nsView.window && [self.nsView.superview isKindOfClass:[ContainerView class]]) {
            ContainerView *containerView = (ContainerView *)self.nsView.superview;
            NSPoint currentMousePosition = [self.nsView.window mouseLocationOutsideOfEventStream];
            [containerView updateActiveWebviewForMousePosition:currentMousePosition];
        }
    }

    - (void)setTransparent:(BOOL)transparent {
        if (self.nsView) {
            [self.nsView setWantsLayer:YES];
            self.nsView.layer.opacity = transparent ? 0 : 1;
        }
    }


    - (void)toggleMirrorMode:(BOOL)enable {
        NSView *subview = self.nsView;

        if (self.mirrorModeEnabled == enable) {
            return;
        }
        BOOL isLeftMouseButtonDown = ([NSEvent pressedMouseButtons] & (1 << 0)) != 0;
        if (isLeftMouseButtonDown) {
            return;
        }
        self.mirrorModeEnabled = enable;

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (enable) {
            CGFloat positionX = subview.frame.origin.x;
            CGFloat positionY = subview.frame.origin.y;
            subview.frame = CGRectOffset(subview.frame, OFFSCREEN_OFFSET, OFFSCREEN_OFFSET);
            subview.layer.position = CGPointMake(positionX, positionY);
        } else {
            subview.frame = CGRectMake(subview.layer.position.x,
                                    subview.layer.position.y,
                                    subview.frame.size.width,
                                    subview.frame.size.height);
        }
        [CATransaction commit];
    }


    // Internal callers (e.g. fullSize resize on window resize) use this entry point
    - (void)resize:(NSRect)frame withMasksJSON:(const char *)masksJson {
        NSArray *parsedMasks = nil;
        if (masksJson && strlen(masksJson) > 0) {
            NSString *jsonString = [NSString stringWithUTF8String:masksJson ?: ""];
            NSData *jsonData = [jsonString dataUsingEncoding:NSUTF8StringEncoding];
            if (jsonData) {
                NSError *error = nil;
                parsedMasks = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
                if (error) parsedMasks = nil;
            }
        }
        [self resizeWithFrame:frame parsedMasks:parsedMasks];
    }

    // Optimized resize — accepts pre-parsed masks (JSON parsing done off main thread)
    - (void)resizeWithFrame:(NSRect)frame parsedMasks:(NSArray *)parsedMasks {
        NSView *subview = self.nsView;
        if (!subview) {
            return;
        }

        CGFloat adjustedX = floor(frame.origin.x);
        CGFloat adjustedWidth = ceilf(frame.size.width);
        CGFloat adjustedHeight = ceilf(frame.size.height);
        CGFloat adjustedY = floor(subview.superview.bounds.size.height - ceilf(frame.origin.y) - adjustedHeight);

        [CATransaction begin];
        [CATransaction setDisableActions:YES];

        if (self.mirrorModeEnabled) {
            subview.frame = NSMakeRect(OFFSCREEN_OFFSET, OFFSCREEN_OFFSET, adjustedWidth, adjustedHeight);
            subview.layer.position = CGPointMake(adjustedX, adjustedY);
        } else {
            subview.frame = NSMakeRect(adjustedX, adjustedY, adjustedWidth, adjustedHeight);
        }

        CAShapeLayer *maskLayer = nil;
        if (parsedMasks && parsedMasks.count > 0) {
            CGFloat heightToAdjust = self.nsView.layer.geometryFlipped ? 0 : adjustedHeight;
            NSArray<NSValue *> *processedRects = addOverlapRects(parsedMasks, heightToAdjust);

            maskLayer = [CAShapeLayer layer];
            maskLayer.frame = self.nsView.layer.bounds;
            CGMutablePathRef path = CGPathCreateMutable();
            CGPathAddRect(path, NULL, maskLayer.bounds);
            for (NSValue *rectValue in processedRects) {
                NSRect rect = [rectValue rectValue];
                CGPathAddRect(path, NULL, rect);
            }
            maskLayer.fillRule = kCAFillRuleEvenOdd;
            maskLayer.path = path;
            CGPathRelease(path);
        }
        self.nsView.layer.mask = maskLayer;

        [CATransaction commit];

        if (self.nsView && [self.nsView.layer isKindOfClass:[CAMetalLayer class]]) {
            CAMetalLayer *layer = (CAMetalLayer *)self.nsView.layer;
            CGFloat scale = self.nsView.window.backingScaleFactor;
            layer.contentsScale = scale;
            CGSize size = self.nsView.bounds.size;
            layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
        }

        NSPoint currentMousePosition = [self.nsView.window mouseLocationOutsideOfEventStream];
        ContainerView *containerView = (ContainerView *)self.nsView.superview;
        [containerView updateActiveWebviewForMousePosition:currentMousePosition];
    }

    - (void)setNavigationRulesFromJSON:(const char*)rulesJson {
        if (!rulesJson || strlen(rulesJson) == 0) {
            self.navigationRules = @[];
            return;
        }

        NSString *jsonString = [NSString stringWithUTF8String:rulesJson];
        NSData *jsonData = [jsonString dataUsingEncoding:NSUTF8StringEncoding];
        if (!jsonData) {
            self.navigationRules = @[];
            return;
        }

        NSError *error = nil;
        NSArray *rulesArray = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
        if (error || ![rulesArray isKindOfClass:[NSArray class]]) {
            NSLog(@"Failed to parse navigation rules JSON: %@", error);
            self.navigationRules = @[];
            return;
        }

        self.navigationRules = rulesArray;
    }

    - (BOOL)shouldAllowNavigationToURL:(NSString *)url {
        if (!self.navigationRules || self.navigationRules.count == 0) {
            return YES; // Default allow if no rules
        }

        BOOL allowed = YES; // Default allow if no rules match
        std::string urlStr = [url UTF8String] ?: "";

        for (NSString *rule in self.navigationRules) {
            BOOL isBlockRule = [rule hasPrefix:@"^"];
            NSString *pattern = isBlockRule ? [rule substringFromIndex:1] : rule;
            std::string patternStr = [pattern UTF8String] ?: "";

            if (electrobun::globMatch(patternStr, urlStr)) {
                allowed = !isBlockRule; // Last match wins
            }
        }

        return allowed;
    }

    - (void)findInPage:(const char*)searchText forward:(BOOL)forward matchCase:(BOOL)matchCase {
        [self doesNotRecognizeSelector:_cmd];
    }

    - (void)stopFindInPage {
        [self doesNotRecognizeSelector:_cmd];
    }

    - (void)openDevTools {
        [self doesNotRecognizeSelector:_cmd];
    }

    - (void)closeDevTools {
        [self doesNotRecognizeSelector:_cmd];
    }

    - (void)toggleDevTools {
        [self doesNotRecognizeSelector:_cmd];
    }

    - (void)storePendingResize:(NSRect)frame parsedMasks:(NSArray *)parsedMasks {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        pendingResizeFrame = frame;
        pendingResizeMasks = parsedMasks;
        hasPendingResize = YES;
        pendingResizeGeneration++;
    }

    - (void)applyPendingResizeIfNeeded {
        NSRect frame = NSZeroRect;
        NSArray *masks = nil;
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(pendingResizeMutex);
            if (!hasPendingResize) {
                return;
            }
            gen = pendingResizeGeneration.load();
            if (gen == appliedResizeGeneration) {
                return;
            }
            frame = pendingResizeFrame;
            masks = pendingResizeMasks;
            appliedResizeGeneration = gen;
            hasPendingResize = NO;
        }
        [self resizeWithFrame:frame parsedMasks:masks];
    }
@end

// Pending resize queue (cross-thread)
static PendingResizeQueue g_pendingResizeQueue;
static std::atomic<bool> g_pendingResizeScheduled{false};
static CFRunLoopSourceRef g_pendingResizeSource = nullptr;

static void drainPendingResizes() {
    g_pendingResizeScheduled.store(false);
    auto items = g_pendingResizeQueue.drain();
    for (void* item : items) {
        AbstractView *view = (__bridge AbstractView *)item;
        if (!view || view.isRemoved) continue;
        [view applyPendingResizeIfNeeded];
    }
}

static void pendingResizeSourcePerform(void *info) {
    (void)info;
    drainPendingResizes();
}

static void ensurePendingResizeSource() {
    if (g_pendingResizeSource) return;
    CFRunLoopSourceContext ctx = {};
    ctx.perform = pendingResizeSourcePerform;
    g_pendingResizeSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
    CFRunLoopAddSource(CFRunLoopGetMain(), g_pendingResizeSource, kCFRunLoopCommonModes);
}

static void schedulePendingResizeDrain() {
    if (g_pendingResizeScheduled.exchange(true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        ensurePendingResizeSource();
        CFRunLoopSourceSignal(g_pendingResizeSource);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
}


@implementation ContainerView
    - (instancetype)initWithFrame:(NSRect)frameRect {
        self = [super initWithFrame:frameRect];
        if (self) {
            self.abstractViews = [NSMutableArray array]; 
            [self updateTrackingAreas];
        }
        return self;
    }

    - (void)updateTrackingAreas {    
        for (NSTrackingArea *area in self.trackingAreas) {
            [self removeTrackingArea:area];
        }
        NSTrackingArea *mouseTrackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
            options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow
            owner:self
            userInfo:nil];
        [self addTrackingArea:mouseTrackingArea];
    }

    - (void)mouseMoved:(NSEvent *)event {    
        NSPoint mouseLocation = [self convertPoint:[event locationInWindow] fromView:nil];
        [self updateActiveWebviewForMousePosition:mouseLocation];
    }

    // This function tries to figure out which "abstractView" should be interactive
    // vs mirrored, based on mouse position and layering.
    - (void)updateActiveWebviewForMousePosition:(NSPoint)mouseLocation {    
        BOOL stillSearching = YES;    

        for (AbstractView * abstractView in self.abstractViews) {           

            if (abstractView.isMousePassthroughEnabled) {
                [abstractView toggleMirrorMode:YES];
                continue;
            }
            
            NSView *subview = abstractView.nsView;

            if (stillSearching) {
                NSRect subviewRenderLayerFrame = subview.layer.frame;
                if (NSPointInRect(mouseLocation, subviewRenderLayerFrame)){// && !subview.hidden) {
                    CAShapeLayer *maskLayer = (CAShapeLayer *)subview.layer.mask;
                    CGPathRef maskPath = maskLayer ? maskLayer.path : NULL;
                    if (maskPath) {                    
                        CGFloat mouseXInWebview = mouseLocation.x - subviewRenderLayerFrame.origin.x;
                        CGFloat mouseYInWebview = mouseLocation.y - subviewRenderLayerFrame.origin.y;
                        
                        // Note: WKWebkit uses geometryFlipped so the y coordinate is from the top not the bottom
                        // (the default on osx is from the bottom). The mouse y coordinate is from the bottom
                        // so we need to invert it to match the layer geometry
                        if (subview.layer.geometryFlipped) {                                                
                            mouseYInWebview = subviewRenderLayerFrame.size.height - (mouseLocation.y - subviewRenderLayerFrame.origin.y);                        
                        }

                        CGPoint mousePositionInMaskPath = CGPointMake(mouseXInWebview, mouseYInWebview);

                        if (!CGPathContainsPoint(maskPath, NULL, mousePositionInMaskPath, true)) {                        
                            [abstractView toggleMirrorMode:YES];                                                
                            continue;
                        }
                    }
                    
                    [abstractView toggleMirrorMode:NO];
                    stillSearching = NO;
                    continue;
                }
            }        
            [abstractView toggleMirrorMode:YES];
        }    
    }


    - (void)addAbstractView:(AbstractView *)abstractView {
        // Add to front of array so it's top-most first
        [self.abstractViews insertObject:abstractView atIndex:0];
    }

    - (void)removeAbstractViewWithId:(uint32_t)webviewId {
        for (NSInteger i = 0; i < self.abstractViews.count; i++) {
            AbstractView * candidate = self.abstractViews[i];
            if (candidate.webviewId == webviewId) {
                g_pendingResizeQueue.remove((__bridge void *)candidate);
                [self.abstractViews removeObjectAtIndex:i];
                break;
            }
        }
    }

    - (void)dealloc {
        for (AbstractView *view in self.abstractViews) {
            g_pendingResizeQueue.remove((__bridge void *)view);
        }
    }
@end

// ----------------------- URL Scheme & Navigation Delegates -----------------------

@implementation MyURLSchemeHandler
    - (void)webView:(WKWebView *)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
        NSURL *url = urlSchemeTask.request.URL;
        NSData *bodyData = urlSchemeTask.request.HTTPBody;
        NSString *bodyString = bodyData ? [[NSString alloc] initWithData:bodyData encoding:NSUTF8StringEncoding] : @"";
        
        NSData *data = nil;
        size_t contentLength = 0;
        const char *contentPtr = NULL;
        
        NSString *urlString = url.absoluteString;
        
        if ([urlString hasPrefix:@"views://"]) {
            NSString *relativePath = normalizeViewsRelativePath(urlString);

            if ([relativePath isEqualToString:@"internal/index.html"]) {
                // For internal content, call the native HTML resolver.
                // Use stored HTML content instead of JSCallback
                contentPtr = getWebviewHTMLContent(self.webviewId);
                if (!contentPtr) {
                    // Fallback to default if no content set
                    contentPtr = strdup("<html><body>No content set</body></html>");
                }
                if (contentPtr) {
                    contentLength = strlen(contentPtr);
                    data = [NSData dataWithBytes:contentPtr length:contentLength];
                } else {
                    // Handle NULL content gracefully
                    NSError *error = [NSError errorWithDomain:@"MyURLSchemeHandler" 
                                                         code:404 
                                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to load internal content"}];
                    [urlSchemeTask didFailWithError:error];
                    return;
                }
            } else {
                data = readViewsFileWithRoot(urlString.UTF8String, self.viewsRoot);
                
                if (data) {
                    contentPtr = (const char *)data.bytes;
                    contentLength = data.length;
                }
            } 
        } else {
            NSLog(@"Unknown URL format: %@", urlString);
        }
        
        if (contentPtr && contentLength > 0) {
            // Determine MIME type using shared function
            std::string urlStr = [urlString UTF8String];
            std::string detectedMimeType = getMimeTypeFromUrl(urlStr);
            const char *mimeTypePtr = strdup(detectedMimeType.c_str());
            
            NSString *rawMimeType = mimeTypePtr ? [NSString stringWithUTF8String:mimeTypePtr] : @"application/octet-stream";

            NSString *mimeType;
            NSString *encodingName = nil;
            if ([rawMimeType hasPrefix:@"text/html"]) {
                mimeType = @"text/html";
                encodingName = @"UTF-8";  // Set encoding explicitly
            } else {
                // For non-text content or text content that doesn't need explicit encoding
                mimeType = rawMimeType;
            }
            
            NSURLResponse *response = [[NSURLResponse alloc] initWithURL:url
                                                    MIMEType:mimeType
                                        expectedContentLength:contentLength
                                            textEncodingName:encodingName];
            [urlSchemeTask didReceiveResponse:response];
            [urlSchemeTask didReceiveData:data];
            [urlSchemeTask didFinish];
            
            // Clean up memory
            if (mimeTypePtr) {
                free((void*)mimeTypePtr);
            }
        } else {
            NSLog(@"============== ERROR ========== empty response for URL: %@", urlString);         
            // Notify failure properly to prevent crashes
            NSError *error = [NSError errorWithDomain:@"MyURLSchemeHandler" 
                                                 code:404 
                                             userInfo:@{NSLocalizedDescriptionKey: @"Resource not found"}];
            [urlSchemeTask didFailWithError:error];
        }
       
    }
    - (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
    }
@end

@implementation MyNavigationDelegate
    - (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction
    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
        NSURL *newURL = navigationAction.request.URL;

        // Check if cmd key is held - if so, fire event and block navigation
        BOOL isCmdClick = (navigationAction.modifierFlags & NSEventModifierFlagCommand) != 0;

        if (isCmdClick && navigationAction.navigationType == WKNavigationTypeLinkActivated) {
            NSString *eventData = [NSString stringWithFormat:@"{\"url\":\"%@\",\"isCmdClick\":true,\"modifierFlags\":%lu}",
                                 newURL.absoluteString,
                                 (unsigned long)navigationAction.modifierFlags];
            self.zigEventHandler(self.webviewId, strdup("new-window-open"), strdup([eventData UTF8String]));
            decisionHandler(WKNavigationActionPolicyCancel);
            return;
        }

        // Check navigation rules synchronously from native-stored rules
        AbstractView *abstractView = [globalAbstractViews objectForKey:@(self.webviewId)];
        BOOL shouldAllow = abstractView ? [abstractView shouldAllowNavigationToURL:newURL.absoluteString] : YES;

        // Fire will-navigate event with allowed status
        NSString *eventData = [NSString stringWithFormat:@"{\"url\":\"%@\",\"allowed\":%@}",
                             newURL.absoluteString,
                             shouldAllow ? @"true" : @"false"];
        self.zigEventHandler(self.webviewId, strdup("will-navigate"), strdup([eventData UTF8String]));

        // Check if this navigation action should trigger a download
        if (navigationAction.shouldPerformDownload) {
            decisionHandler(WKNavigationActionPolicyDownload);
        } else {
            decisionHandler(shouldAllow ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
        }
    }

    - (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
    decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
        // If the response cannot be shown (e.g., binary file, attachment), trigger download
        if (!navigationResponse.canShowMIMEType) {
            NSLog(@"DEBUG WKWebView Download: Cannot show MIME type, triggering download for %@", navigationResponse.response.URL.absoluteString);
            decisionHandler(WKNavigationResponsePolicyDownload);
        } else {
            decisionHandler(WKNavigationResponsePolicyAllow);
        }
    }

    - (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
        NSString *urlString = webView.URL.absoluteString ?: @"";
        if (urlString.length > 0) {
            self.zigEventHandler(self.webviewId, strdup("did-navigate"), strdup(urlString.UTF8String));
        }
    }
    - (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation {
        NSString *urlString = webView.URL.absoluteString ?: @"";
        if (urlString.length > 0) {
            self.zigEventHandler(self.webviewId, strdup("did-commit-navigation"), strdup(urlString.UTF8String));
        }
    }

    // Called when navigationAction policy returns .download
    - (void)webView:(WKWebView *)webView navigationAction:(WKNavigationAction *)navigationAction didBecomeDownload:(WKDownload *)download API_AVAILABLE(macos(11.3)) {
        NSLog(@"DEBUG WKWebView Download: Navigation action became download");
        download.delegate = self;
    }

    // Called when navigationResponse policy returns .download
    - (void)webView:(WKWebView *)webView navigationResponse:(WKNavigationResponse *)navigationResponse didBecomeDownload:(WKDownload *)download API_AVAILABLE(macos(11.3)) {
        NSLog(@"DEBUG WKWebView Download: Navigation response became download");
        download.delegate = self;
    }

    // WKDownloadDelegate methods
    - (void)download:(WKDownload *)download
    decideDestinationUsingResponse:(NSURLResponse *)response
    suggestedFilename:(NSString *)suggestedFilename
    completionHandler:(void (^)(NSURL * _Nullable destination))completionHandler API_AVAILABLE(macos(11.3)) {
        NSLog(@"DEBUG WKWebView Download: Deciding destination for %@", suggestedFilename);

        // Get the Downloads folder
        NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
        NSString *downloadsDirectory = [paths firstObject];

        if (downloadsDirectory) {
            NSString *destinationPath = [downloadsDirectory stringByAppendingPathComponent:suggestedFilename];

            // Handle duplicate filenames by appending a number
            NSFileManager *fileManager = [NSFileManager defaultManager];
            NSString *basePath = [destinationPath stringByDeletingPathExtension];
            NSString *extension = [destinationPath pathExtension];
            int counter = 1;

            while ([fileManager fileExistsAtPath:destinationPath]) {
                if (extension.length > 0) {
                    destinationPath = [NSString stringWithFormat:@"%@ (%d).%@", basePath, counter, extension];
                } else {
                    destinationPath = [NSString stringWithFormat:@"%@ (%d)", basePath, counter];
                }
                counter++;
            }

            NSURL *destinationURL = [NSURL fileURLWithPath:destinationPath];
            NSLog(@"DEBUG WKWebView Download: Saving to %@", destinationPath);

            // Store the path for this download so we can reference it in completion handlers
            if (!self.downloadPaths) {
                self.downloadPaths = [NSMutableDictionary dictionary];
            }
            [self.downloadPaths setObject:destinationPath forKey:[NSValue valueWithNonretainedObject:download]];

            // Observe download progress via KVO
            if (!self.observedDownloads) {
                self.observedDownloads = [NSMutableSet set];
            }
            [self.observedDownloads addObject:download];
            [download.progress addObserver:self
                                forKeyPath:@"fractionCompleted"
                                   options:NSKeyValueObservingOptionNew
                                   context:NULL];

             & WindowDelegate -----------------------

@implementation AppDelegate
    - (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
        // If we're already in shutdown sequence (stopEventLoop was called), allow termination
        if (g_eventLoopStopping.load()) {
            return NSTerminateNow;
        }

        // If a quit handler is registered, ask bun to run its quit sequence
        if (g_quitRequestedHandler) {
            g_quitRequestedHandler();
            return NSTerminateCancel;
        }

        // No handler registered, allow immediate termination (fallback)
        return NSTerminateNow;
    }

    // Handle URLs opened via custom URL schemes (deep linking)
    - (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
        for (NSURL *url in urls) {
            if (g_urlOpenHandler) {
                g_urlOpenHandler([[url absoluteString] UTF8String]);
            } else {
                NSLog(@"[URL Handler] Received URL but no handler registered: %@", url);
            }
        }
    }

    - (BOOL)applicationShouldHandleReopen:(NSApplication *)application hasVisibleWindows:(BOOL)hasVisibleWindows {
        (void)hasVisibleWindows;

        [application activateIgnoringOtherApps:YES];

        if (g_appReopenHandler) {
            g_appReopenHandler();
        }

        return YES;
    }
@end

@implementation WindowDelegate
    - (BOOL)windowShouldClose:(NSWindow *)sender {
    return YES;
    }
    - (void)windowWillClose:(NSNotification *)notification {
        NSWindow *window = [notification object];
        if (self.closeHandler) {
            self.closeHandler(self.windowId);
        }
    }
   - (void)windowDidResize:(NSNotification *)notification {
        NSWindow *window = [notification object];
        NSRect windowFrame = [window frame];
        ContainerView *containerView = [window contentView];
        // Use the content view's bounds (excludes title bar) instead of the
        // window frame so fullSize webviews don't overflow the visible area.
        NSRect fullFrame = containerView.bounds;

        for (AbstractView *abstractView in containerView.abstractViews) {
            if (abstractView.fullSize) {
                [abstractView resize:fullFrame withMasksJSON:""];
            }
        }

        if (self.resizeHandler) {
            NSScreen *primaryScreen = [NSScreen screens][0];
            NSRect screenFrame = [primaryScreen frame];
            windowFrame.origin.y = screenFrame.size.height - windowFrame.origin.y - windowFrame.size.height;
            NSRect contentRect = [window contentRectForFrameRect:windowFrame];
            self.resizeHandler(self.windowId, windowFrame.origin.x, windowFrame.origin.y,
                               contentRect.size.width, contentRect.size.height);
        }
    }
    - (void)windowDidMove:(NSNotification *)notification {
        if (self.moveHandler) {
            NSWindow *window = [notification object];
            NSRect windowFrame = [window frame];
            NSScreen *primaryScreen = [NSScreen screens][0];
            NSRect screenFrame = [primaryScreen frame];
            windowFrame.origin.y = screenFrame.size.height - windowFrame.origin.y - windowFrame.size.height;
            self.moveHandler(self.windowId, windowFrame.origin.x, windowFrame.origin.y);
        }
    }
    - (void)windowDidBecomeKey:(NSNotification *)notification {
        if (self.focusHandler) {
            self.focusHandler(self.windowId);
        }

    }
    - (void)windowDidResignKey:(NSNotification *)notification {
        if (self.blurHandler) {
            self.blurHandler(self.windowId);
        }
    }
@end

/*
 * =============================================================================
 * 6. EXTERN "C" BRIDGING FUNCTIONS
 * =============================================================================
 */

// Note: This is executed from the main bun thread
// Note: `name` parameter is accepted for API consistency with Windows but not used on macOS
// Forward declaration - stopEventLoop is defined after startEventLoop
extern "C" void stopEventLoop();

extern "C" void startEventLoop(const char* identifier, const char* name, const char* channel) {
    (void)name; // Unused on macOS - kept for API consistency with Windows/Linux

    // Store identifier and channel globally for use in initialization
    if (identifier && identifier[0]) {
        g_electrobunIdentifier = std::string(identifier);
    }
    if (channel && channel[0]) {
        g_electrobunChannel = std::string(channel);
    }

    
    // Initialize the global AbstractView tracking map
    if (!globalAbstractViews) {
        globalAbstractViews = [[NSMutableDictionary alloc] init];
    }
    
    // Initialize webview HTML content storage
    if (!webviewHTMLContent) {
        webviewHTMLContent = [[NSMutableDictionary alloc] init];
        webviewHTMLLock = [[NSLock alloc] init];
    }
    
    // Set up dispatch sources for SIGINT and SIGTERM so they work regardless of
    // which event loop is running.
    // bun's process.on("SIGINT") depends on bun's event loop to forward signals
    // to the Worker, which doesn't work when the main thread is in [NSApp run].
    // Dispatch sources deliver signal events on the main queue, which [NSApp run] processes.
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    static int sigint_count = 0;

    dispatch_source_t sigintSource = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigintSource, ^{
        sigint_count++;
        if (sigint_count == 1) {
            if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                g_quitRequestedHandler();
            } else {
                stopEventLoop();
            }
        } else {
            // Second Ctrl+C: force kill entire process group
            kill(0, SIGKILL);
        }
    });
    dispatch_resume(sigintSource);

    dispatch_source_t sigtermSource = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigtermSource, ^{
        if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
            g_quitRequestedHandler();
        } else {
            stopEventLoop();
        }
    });
    dispatch_resume(sigtermSource);

    NSApplication *app = [NSApplication sharedApplication];
    AppDelegate *delegate = [[AppDelegate alloc] init];
    [app setDelegate:delegate];
    retainObjCObject(delegate);
    [app run];
    g_shutdownComplete.store(true);
}

extern "C" void stopEventLoop() {
    if (g_eventLoopStopping.exchange(true)) {
        NSLog(@"[stopEventLoop] Already stopping, ignoring duplicate call");
        return;
    }

    // Intentionally no log here - output after shell prompt return is confusing in dev mode

    // [NSApp stop:nil] is thread-safe per Apple docs
    // Post a dummy event to ensure the run loop wakes up and processes the stop
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSApplication sharedApplication] stop:nil];
            NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                               location:NSMakePoint(0, 0)
                                          modifierFlags:0
                                              timestamp:0
                                           windowNumber:0
                                                context:nil
                                                subtype:0
                                                  data1:0
                                                  data2:0];
            [[NSApplication sharedApplication] postEvent:event atStart:YES];
        });
    }
}

extern "C" void killApp() {
    // Deprecated - delegates to stopEventLoop for backward compatibility
    stopEventLoop();
}

extern "C" void waitForShutdownComplete(int timeoutMs) {
    int waited = 0;
    while (!g_shutdownComplete.load() && waited < timeoutMs) {
        usleep(10000); // 10ms
        waited += 10;
    }
    if (!g_shutdownComplete.load()) {
        NSLog(@"[waitForShutdownComplete] Timed out after %dms", timeoutMs);
    }
}

extern "C" void forceExit(int code) {
    // Last-resort exit that skips atexit handlers.
    // Used when waitForShutdownComplete times out and calling exit() would
    // deadlock on atexit handlers trying to join still-running threads.
    _exit(code);
}

extern "C" void setQuitRequestedHandler(QuitRequestedHandler handler) {
    g_quitRequestedHandler = handler;
}

extern "C" void shutdownApplication() {
    // Deprecated - shutdown now runs inline in startEventLoop after event loop returns
    stopEventLoop();
}



// Global flags set by setNextWebviewFlags, consumed by initWebview
static struct {
    bool startTransparent;
    bool startPassthrough;
} g_nextWebviewFlags = {false, false};

extern "C" void setNextWebviewFlags(bool startTransparent, bool startPassthrough) {
    g_nextWebviewFlags.startTransparent = startTransparent;
    g_nextWebviewFlags.startPassthrough = startPassthrough;
}

extern "C" AbstractView* initWebview(uint32_t webviewId,
                        NSWindow *window,
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
                        bool sandbox ) {

    // Read and clear pre-set flags
    bool startTransparent = g_nextWebviewFlags.startTransparent;
    bool startPassthrough = g_nextWebviewFlags.startPassthrough;
    g_nextWebviewFlags = {false, false};

    // Validate frame values - use defaults if NaN or invalid
    if (isnan(x) || isinf(x)) {
        NSLog(@"WARNING initWebview: x is NaN/Inf for webview %u, using 0", webviewId);
        x = 0;
    }
    if (isnan(y) || isinf(y)) {
        NSLog(@"WARNING initWebview: y is NaN/Inf for webview %u, using 0", webviewId);
        y = 0;
    }
    if (isnan(width) || isinf(width) || width <= 0) {
        NSLog(@"WARNING initWebview: width is NaN/Inf/invalid for webview %u, using 100", webviewId);
        width = 100;
    }
    if (isnan(height) || isinf(height) || height <= 0) {
        NSLog(@"WARNING initWebview: height is NaN/Inf/invalid for webview %u, using 100", webviewId);
        height = 100;
    }

    NSRect frame = NSMakeRect(x, y, width, height);

    __block AbstractView *impl = nil;

    dispatch_sync(dispatch_get_main_queue(), ^{
        Class ImplClass = [WKWebViewImpl class];

        impl = [[ImplClass alloc] initWithWebviewId:webviewId
                                        window:window
                                        url:strdup(url)
                                        frame:frame
                                        autoResize:autoResize
                                        partitionIdentifier:strdup(partitionIdentifier)
                                        navigationCallback:navigationCallback
                                        webviewEventHandler:webviewEventHandler
                                        eventBridgeHandler:eventBridgeHandler
                                        bunBridgeHandler:bunBridgeHandler
                                        internalBridgeHandler:internalBridgeHandler
                                        electrobunPreloadScript:strdup(electrobunPreloadScript)
                                        customPreloadScript:strdup(customPreloadScript)
                                        viewsRoot:strdup(viewsRoot)
                                        transparent:transparent
                                        sandbox:sandbox];

        // Store initial state flags — applied later in each impl's deferred creation block
        // (nsView is nil at this point because view creation is async)
        impl.pendingStartTransparent = startTransparent;
        impl.pendingStartPassthrough = startPassthrough;

    });

    return impl;
}

extern "C" MyScriptMessageHandlerWithReply* addScriptMessageHandlerWithReply(WKWebView *webView,
                                                                             uint32_t webviewId,
                                                                             const char *name,
                                                                             HandlePostMessageWithReply callback) {

    MyScriptMessageHandlerWithReply *handler = [[MyScriptMessageHandlerWithReply alloc] init];
    handler.zigCallback = callback;
    handler.webviewId = webviewId;
    [webView.configuration.userContentController addScriptMessageHandlerWithReply:handler
                                                                     contentWorld:WKContentWorld.pageWorld
                                                                             name:[NSString stringWithUTF8String:name ?: ""]];
    NSString *key = [NSString stringWithFormat:@"PostMessageHandlerWithReply{%s}", name];
    objc_setAssociatedObject(webView, key.UTF8String, handler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return handler;
}

extern "C" void loadURLInWebView(AbstractView *abstractView, const char *urlString) {
    if (!abstractView) {
        NSLog(@"loadURLInWebView: abstractView is null");
        return;
    }

    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"loadURLInWebView: webview %u not in tracking, skipping", abstractView.webviewId);
        return;
    }

    NSLog(@"DEBUG loadURLInWebView: webview %u loading URL: %s", abstractView.webviewId, urlString);
    [abstractView loadURL:urlString];
}

extern "C" void loadHTMLInWebView(AbstractView *abstractView, const char *htmlString) {
    if (!abstractView) {
        NSLog(@"loadHTMLInWebView: abstractView is null");
        return;
    }

    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"loadHTMLInWebView: webview %u not in tracking, skipping", abstractView.webviewId);
        return;
    }

    NSLog(@"DEBUG loadHTMLInWebView: webview %u loading HTML content", abstractView.webviewId);
    [abstractView loadHTML:htmlString];
}

extern "C" void webviewGoBack(AbstractView *abstractView) {   
    if (!abstractView) {
        NSLog(@"webviewGoBack: abstractView is null");
        return;
    }
    
    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"webviewGoBack: webview %u not in tracking, skipping", abstractView.webviewId);
        return;
    }
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView goBack];
    });
}

extern "C" void webviewGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        NSLog(@"webviewGoForward: abstractView is null");
        return;
    }
    
    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"webviewGoForward: webview %u not in tracking, skipping", abstractView.webviewId);
        return;
    }
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView goForward];
    });
}

extern "C" void webviewReload(AbstractView *abstractView) {
    if (!abstractView) {
        NSLog(@"webviewReload: abstractView is null");
        return;
    }
    
    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"webviewReload: webview %u not in tracking, skipping", abstractView.webviewId);
        return;
    }
    
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView reload];
    });
}

extern "C" void webviewRemove(AbstractView *abstractView) {
    if (!abstractView) {
        return;
    }

    // Check global tracking map instead of individual flag
    NSNumber *webviewKey = @(abstractView.webviewId);
    AbstractView *trackedView = globalAbstractViews[webviewKey];
    
    if (!trackedView) {
        return;
    }
    
    if (trackedView != abstractView) {
        NSLog(@"webviewRemove: WARNING - tracked view %p != passed view %p for webviewId %u", trackedView, abstractView, abstractView.webviewId);
    }
    
    // Remove from global tracking immediately to prevent re-entry
    [globalAbstractViews removeObjectForKey:webviewKey];
    [abstractView remove];
}

extern "C" BOOL webviewCanGoBack(AbstractView *abstractView) {
    if (!abstractView) {
        NSLog(@"webviewCanGoBack: abstractView is null");
        return NO;
    }
    
    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"webviewCanGoBack: webview %u not in tracking, returning NO", abstractView.webviewId);
        return NO;
    }
    
    return [abstractView canGoBack];
}

extern "C" BOOL webviewCanGoForward(AbstractView *abstractView) {
    if (!abstractView) {
        NSLog(@"webviewCanGoForward: abstractView is null");
        return NO;
    }
    
    // Check if webview still exists in global tracking
    if (!globalAbstractViews[@(abstractView.webviewId)]) {
        NSLog(@"webviewCanGoForward: webview %u not in tracking, returning NO", abstractView.webviewId);
        return NO;
    }
    
    return [abstractView canGoForward];
}

extern "C" void evaluateJavaScriptWithNoCompletion(AbstractView *abstractView, const char *script) {                    
    [abstractView evaluateJavaScriptWithNoCompletion:script];        
}

extern "C" void testFFI(void *ptr) {              
    NSLog(@"ObjC side - raw ptr: %p", ptr);
    
    // Dump memory contents
    uintptr_t *memory = (uintptr_t *)ptr;
    NSLog(@"Memory contents - first 4 words:");
    for(int i = 0; i < 4; i++) {
        NSLog(@"  Offset %d: %lx", i * 8, memory[i]);
    }
    
    // Try to get object type information
    Class cls = object_getClass((__bridge id)ptr);
    if (cls) {
        NSLog(@"Object appears to be of class: %@", cls);
    } else {
        NSLog(@"Not a valid Objective-C class pointer");
    }
    
    // Try to check vtable if it's a C++ object
    void **vtable = *(void***)ptr;
    NSLog(@"Possible vtable pointer: %p", vtable);
}

extern "C" void callAsyncJavaScript(const char *messageId,
                                    AbstractView *abstractView,
                                    const char *jsString,
                                    uint32_t webviewId,
                                    uint32_t hostWebviewId,
                                    callAsyncJavascriptCompletionHandler completionHandler) {

    
   [abstractView callAsyncJavascript:messageId
                        jsString:jsString
                       webviewId:webviewId
                  hostWebviewId:hostWebviewId
               completionHandler:completionHandler];
}

extern "C" void addPreloadScriptToWebView(AbstractView *abstractView, const char *scriptContent, BOOL forMainFrameOnly) {                
    [abstractView addPreloadScriptToWebView:scriptContent];    
}

// todo: remove identifier and add option forMainFrameOnly
extern "C" void updatePreloadScriptToWebView(AbstractView *abstractView,
                                             const char *scriptIdentifier,
                                             const char *scriptContent,
                                             BOOL forMainFrameOnly) {
    [abstractView updateCustomPreloadScript:scriptContent];    
}

extern "C" void invokeDecisionHandler(void (^decisionHandler)(WKNavigationActionPolicy), WKNavigationActionPolicy policy) {
    if (decisionHandler) {
        decisionHandler(policy);
    }
}

extern "C" const char* getUrlFromNavigationAction(WKNavigationAction *navigationAction) {
    NSURLRequest *request = navigationAction.request;
    NSURL *url = request.URL;
    return url.absoluteString.UTF8String;
}

extern "C" const char* getBodyFromScriptMessage(WKScriptMessage *message) {
    NSString *body = message.body;
    return body.UTF8String;
}

extern "C" void webviewSetTransparent(AbstractView *abstractView, BOOL transparent) {    
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView setTransparent:transparent];    
    });
}

extern "C" void webviewSetPassthrough(AbstractView *abstractView, BOOL enablePassthrough) {    
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView setPassthrough:enablePassthrough];    
    });
}

extern "C" void webviewSetHidden(AbstractView *abstractView, BOOL hidden) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView setHidden:hidden];
    });
}

extern "C" void setWebviewNavigationRules(AbstractView *abstractView, const char *rulesJson) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView setNavigationRulesFromJSON:rulesJson];
    });
}

extern "C" void webviewFindInPage(AbstractView *abstractView, const char *searchText, bool forward, bool matchCase) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView findInPage:searchText forward:forward matchCase:matchCase];
    });
}

extern "C" void webviewStopFind(AbstractView *abstractView) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView stopFindInPage];
    });
}

extern "C" void webviewOpenDevTools(AbstractView *abstractView) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView openDevTools];
    });
}

extern "C" void webviewCloseDevTools(AbstractView *abstractView) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView closeDevTools];
    });
}

extern "C" void webviewToggleDevTools(AbstractView *abstractView) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [abstractView toggleDevTools];
    });
}

extern "C" void webviewSetPageZoom(AbstractView *abstractView, double zoomLevel) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([abstractView isKindOfClass:[WKWebViewImpl class]]) {
            WKWebViewImpl *wkImpl = (WKWebViewImpl *)abstractView;
            if (wkImpl.webView) {
                wkImpl.webView.pageZoom = zoomLevel;
                [wkImpl.webView setNeedsDisplay:YES];
                [wkImpl.webView setNeedsLayout:YES];
            }
        }
    });
}

extern "C" double webviewGetPageZoom(AbstractView *abstractView) {
    __block double zoomLevel = 1.0;
    if ([abstractView isKindOfClass:[WKWebViewImpl class]]) {
        WKWebViewImpl *wkImpl = (WKWebViewImpl *)abstractView;
        if (wkImpl.webView) {
            if ([NSThread isMainThread]) {
                zoomLevel = wkImpl.webView.pageZoom;
            } else {
                dispatch_sync(dispatch_get_main_queue(), ^{
                    zoomLevel = wkImpl.webView.pageZoom;
                });
            }
        }
    }
    return zoomLevel;
}


extern "C" NSRect createNSRectWrapper(double x, double y, double width, double height) {
    return NSMakeRect(x, y, width, height);
}


@interface ElectrobunWindow : NSWindow
@end

@implementation ElectrobunWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

NSWindow *createNSWindowWithFrameAndStyle(uint32_t windowId,
                                                     createNSWindowWithFrameAndStyleParams config,
                                                     WindowCloseHandler zigCloseHandler,
                                                     WindowMoveHandler zigMoveHandler,
                                                     WindowResizeHandler zigResizeHandler,
                                                     WindowFocusHandler zigFocusHandler,
                                                     WindowBlurHandler zigBlurHandler,
                                                     WindowKeyHandler zigKeyHandler) {
    
    NSScreen *primaryScreen = [NSScreen screens][0];
    NSRect screenFrame = [primaryScreen frame];
    config.frame.origin.y = screenFrame.size.height - config.frame.origin.y;
    
    NSWindow *window = [[ElectrobunWindow alloc] initWithContentRect:config.frame
                                                          styleMask:config.styleMask
                                                            backing:NSBackingStoreBuffered
                                                              defer:YES
                                                             screen:primaryScreen];
    
    [window setFrameTopLeftPoint:config.frame.origin];
    if (strcmp(config.titleBarStyle, "hiddenInset") == 0) {
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility = NSWindowTitleHidden;
    }
    WindowDelegate *delegate = [[WindowDelegate alloc] init];
    delegate.closeHandler = zigCloseHandler;
    delegate.resizeHandler = zigResizeHandler;
    delegate.moveHandler = zigMoveHandler;
    delegate.focusHandler = zigFocusHandler;
    delegate.blurHandler = zigBlurHandler;
    delegate.keyHandler = zigKeyHandler;
    delegate.windowId = windowId;
    delegate.window = window;
    [window setDelegate:delegate];
    objc_setAssociatedObject(window, "WindowDelegate", delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    window.releasedWhenClosed = NO;

    ContainerView *contentView = [[ContainerView alloc] initWithFrame:[window frame]];
    contentView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [window setContentView:contentView];
    return window;

    // return (void*)window;
    
}

extern "C" void testFFI2(void (*completionHandler)()) {
    NSLog(@"C++  TEST FFI 2 0");
    completionHandler();
    NSLog(@"C++  TEST FFI 2 1");
}

extern "C" NSWindow *createWindowWithFrameAndStyleFromWorker(
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
  WindowKeyHandler zigKeyHandler
  ) {

    // Validate frame values - use defaults if NaN or invalid
    if (isnan(x) || isinf(x)) x = 100;
    if (isnan(y) || isinf(y)) y = 100;
    if (isnan(width) || isinf(width) || width <= 0) width = 800;
    if (isnan(height) || isinf(height) || height <= 0) height = 600;

    NSRect frame = NSMakeRect(x, y, width, height);

    // Create the params struct
    createNSWindowWithFrameAndStyleParams config = {
        .frame = frame,
        .styleMask = styleMask,
        .titleBarStyle = titleBarStyle
    };

    // Use a dispatch semaphore to wait for the window creation to complete
    __block NSWindow* window = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        window = createNSWindowWithFrameAndStyle(
            windowId,
            config,
            zigCloseHandler,
            zigMoveHandler,
            zigResizeHandler,
            zigFocusHandler,
            zigBlurHandler,
            zigKeyHandler
        );

        // Handle transparent window background
        if (transparent) {
            window.backgroundColor = [NSColor clearColor];
            window.opaque = NO;
            window.hasShadow = NO;

            // Also configure the content view for transparency
            NSView *contentView = window.contentView;
            contentView.wantsLayer = YES;
            contentView.layer.backgroundColor = [[NSColor clearColor] CGColor];
            contentView.layer.opaque = NO;
        }

        // Handle hidden titleBarStyle - hide native window controls (traffic lights)
        if (strcmp(titleBarStyle, "hidden") == 0) {
            [[window standardWindowButton:NSWindowCloseButton] setHidden:YES];
            [[window standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
            [[window standardWindowButton:NSWindowZoomButton] setHidden:YES];
        }
    });

    return window;
}

extern "C" void showWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        // First ensure the window is visible
        [window orderFront:nil];
        
        // Make the window key and bring to front
        [window makeKeyAndOrderFront:nil];
        
        // Activate the application to ensure it can receive focus
        [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];    
    });
}

extern "C" void setWindowTitle(NSWindow *window, const char *title) {
    NSString *titleString = [NSString stringWithUTF8String:title ?: ""];

    dispatch_sync(dispatch_get_main_queue(), ^{
        [window setTitle:titleString];
    });
}

extern "C" void closeWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        [window close];
    });
}

extern "C" void minimizeWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        [window miniaturize:nil];
    });
}

extern "C" void restoreWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        [window deminiaturize:nil];
    });
}

extern "C" bool isWindowMinimized(NSWindow *window) {
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = [window isMiniaturized];
    });
    return result;
}

extern "C" void maximizeWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        // Only zoom if not already zoomed
        if (![window isZoomed]) {
            [window zoom:nil];
        }
    });
}

extern "C" void unmaximizeWindow(NSWindow *window) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        // Only unzoom if currently zoomed
        if ([window isZoomed]) {
            [window zoom:nil];
        }
    });
}

extern "C" bool isWindowMaximized(NSWindow *window) {
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = [window isZoomed];
    });
    return result;
}

extern "C" void setWindowFullScreen(NSWindow *window, bool fullScreen) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        bool isCurrentlyFullScreen = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        if (fullScreen != isCurrentlyFullScreen) {
            [window toggleFullScreen:nil];
        }
    });
}

extern "C" bool isWindowFullScreen(NSWindow *window) {
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    });
    return result;
}

extern "C" void setWindowAlwaysOnTop(NSWindow *window, bool alwaysOnTop) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        if (alwaysOnTop) {
            [window setLevel:NSFloatingWindowLevel];
        } else {
            [window setLevel:NSNormalWindowLevel];
        }
    });
}

extern "C" bool isWindowAlwaysOnTop(NSWindow *window) {
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = [window level] >= NSFloatingWindowLevel;
    });
    return result;
}

extern "C" void setWindowPosition(NSWindow *window, double x, double y) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!window) return;
        // macOS uses bottom-left origin, so we need to convert from top-left
        NSScreen *screen = [window screen] ?: [NSScreen mainScreen];
        CGFloat screenHeight = screen.frame.size.height;
        CGFloat windowHeight = window.frame.size.height;
        // Convert from top-left origin (what users expect) to bottom-left origin (what macOS uses)
        CGFloat adjustedY = screenHeight - y - windowHeight;
        [window setFrameOrigin:NSMakePoint(x, adjustedY)];
    });
}

extern "C" void setWindowSize(NSWindow *window, double width, double height) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!window) return;
        NSRect frame = window.frame;
        // Keep the top-left corner fixed when resizing
        CGFloat oldHeight = frame.size.height;
        frame.size.width = width;
        frame.size.height = height;
        // Adjust y to keep top-left corner fixed (macOS uses bottom-left origin)
        frame.origin.y += (oldHeight - height);
        [window setFrame:frame display:YES animate:NO];
    });
}

extern "C" void setWindowFrame(NSWindow *window, double x, double y, double width, double height) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!window) return;
        // macOS uses bottom-left origin, convert from top-left
        NSScreen *screen = [window screen] ?: [NSScreen mainScreen];
        CGFloat screenHeight = screen.frame.size.height;
        CGFloat adjustedY = screenHeight - y - height;
        NSRect frame = NSMakeRect(x, adjustedY, width, height);
        [window setFrame:frame display:YES animate:NO];
    });
}

extern "C" void getWindowFrame(NSWindow *window, double *outX, double *outY, double *outWidth, double *outHeight) {
    __block NSRect frame = NSZeroRect;
    __block CGFloat screenHeight = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{
        if (!window) return;
        frame = window.frame;
        NSScreen *screen = [window screen] ?: [NSScreen mainScreen];
        screenHeight = screen.frame.size.height;
    });
    // Convert from bottom-left origin to top-left origin
    *outX = frame.origin.x;
    *outY = screenHeight - frame.origin.y - frame.size.height;
    *outWidth = frame.size.width;
    *outHeight = frame.size.height;
}

extern "C" void resizeWebview(AbstractView *abstractView, double x, double y, double width, double height, const char *masksJson) {
    // Validate frame values - use defaults if NaN or invalid
    if (isnan(x) || isinf(x)) x = 0;
    if (isnan(y) || isinf(y)) y = 0;
    if (isnan(width) || isinf(width) || width <= 0) width = 100;
    if (isnan(height) || isinf(height) || height <= 0) height = 100;

    NSRect frame = NSMakeRect(x, y, width, height);

    // Pre-parse masks JSON off the main thread (NSJSONSerialization is thread-safe)
    NSArray *parsedMasks = nil;
    if (masksJson && strlen(masksJson) > 0) {
        @autoreleasepool {
            NSString *jsonString = [NSString stringWithUTF8String:masksJson];
            NSData *jsonData = [jsonString dataUsingEncoding:NSUTF8StringEncoding];
            if (jsonData) {
                NSError *error = nil;
                parsedMasks = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
                if (error) parsedMasks = nil;
            }
        }
    }

    [abstractView storePendingResize:frame parsedMasks:parsedMasks];
    g_pendingResizeQueue.enqueue((__bridge void *)abstractView);
    schedulePendingResizeDrain();
}

extern "C" void stopWindowMove() {
    isMovingWindow = NO;
    targetWindow = nil;
    offsetX = 0.0;
    offsetY = 0.0;
    if (mouseDraggedMonitor) {
        [NSEvent removeMonitor:mouseDraggedMonitor];
        mouseDraggedMonitor = nil;
    }
    if (mouseUpMonitor) {
        [NSEvent removeMonitor:mouseUpMonitor];
        mouseUpMonitor = nil;
    }
}

extern "C" void startWindowMove(NSWindow *window) {
    targetWindow = window;
    if (!targetWindow) {
        NSLog(@"No window found for the given WebView.");
        return;
    }
    isMovingWindow = YES;
    NSPoint initialLocation = [NSEvent mouseLocation];

    mouseDraggedMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDragged | NSEventMaskMouseMoved)
                                                                handler:^NSEvent *(NSEvent *event) {
        if (isMovingWindow) {
            NSPoint currentLocation = [NSEvent mouseLocation];
            if (offsetX == 0.0 && offsetY == 0.0) {
                NSPoint windowOrigin = targetWindow.frame.origin;
                offsetX = initialLocation.x - windowOrigin.x;
                offsetY = initialLocation.y - windowOrigin.y;
            }
            CGFloat newX = currentLocation.x - offsetX;
            CGFloat newY = currentLocation.y - offsetY;
            [targetWindow setFrameOrigin:NSMakePoint(newX, newY)];
        }
        return event;
    }];
    mouseUpMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseUp
                                                           handler:^NSEvent *(NSEvent *event) {
        if (isMovingWindow) {
            stopWindowMove();
        }
        return event;
    }];
}


extern "C" BOOL moveToTrash(char *pathString) {
    NSString *path = [NSString stringWithUTF8String:pathString ?: ""];
    NSURL *fileURL = [NSURL fileURLWithPath:path];
    NSError *error = nil;
    NSURL *resultingURL = nil;

    NSFileManager *fileManager = [NSFileManager defaultManager];
    BOOL success = [fileManager trashItemAtURL:fileURL resultingItemURL:&resultingURL error:&error];
    if (success) {
        NSLog(@"Moved to Trash: %@", resultingURL);
    } else {
        NSLog(@"Error: %@", error);
    }
    return success;
}

extern "C" void showItemInFolder(char *path) {
    NSString *pathString = [NSString stringWithUTF8String:path ?: ""];
    NSURL *fileURL = [NSURL fileURLWithPath:pathString];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[fileURL]];
}

// Open a URL in the default browser or appropriate application
extern "C" BOOL openExternal(const char *urlString) {
    NSString *urlStr = [NSString stringWithUTF8String:urlString ?: ""];
    NSURL *url = [NSURL URLWithString:urlStr];

    if (!url) {
        NSLog(@"[openExternal] Invalid URL: %@", urlStr);
        return NO;
    }

    return [[NSWorkspace sharedWorkspace] openURL:url];
}

// Open a file or folder with the default application
extern "C" BOOL openPath(const char *pathString) {
    NSString *path = [NSString stringWithUTF8String:pathString ?: ""];
    NSURL *fileURL = [NSURL fileURLWithPath:path];

    BOOL success = [[NSWorkspace sharedWorkspace] openURL:fileURL];

    if (!success) {
        NSLog(@"[openPath] Failed to open path: %@", path);
    }

    return success;
}

// Show a native desktop notification
// Track notification authorization state
static BOOL notificationAuthRequested = NO;
static BOOL notificationAuthGranted = NO;
static BOOL useModernNotifications = YES;

// Fallback to deprecated NSUserNotification API (works better in dev mode without proper bundle)
static void showNotificationLegacy(NSString *titleStr, NSString *bodyStr, NSString *subtitleStr, BOOL silent) {
    dispatch_async(dispatch_get_main_queue(), ^{
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"

        NSUserNotification *notification = [[NSUserNotification alloc] init];
        notification.title = titleStr;
        notification.informativeText = bodyStr;
        if (subtitleStr) {
            notification.subtitle = subtitleStr;
        }
        notification.soundName = silent ? nil : NSUserNotificationDefaultSoundName;

        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        NSLog(@"Notification delivered via legacy API: %@", titleStr);

        #pragma clang diagnostic pop
    });
}

extern "C" void showNotification(const char *title, const char *body, const char *subtitle, BOOL silent) {
    NSString *titleStr = [NSString stringWithUTF8String:title ?: ""];
    NSString *bodyStr = [NSString stringWithUTF8String:body ?: ""];
    NSString *subtitleStr = subtitle ? [NSString stringWithUTF8String:subtitle] : nil;

    // If we've already determined modern API doesn't work, use legacy
    if (!useModernNotifications) {
        showNotificationLegacy(titleStr, bodyStr, subtitleStr, silent);
        return;
    }

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];

    // Request authorization if we haven't already
    if (!notificationAuthRequested) {
        notificationAuthRequested = YES;

        // Use a semaphore to wait for authorization result on first call
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
                              completionHandler:^(BOOL granted, NSError * _Nullable error) {
            if (error) {
                NSLog(@"Notification authorization error: %@ - falling back to legacy API", error);
                useModernNotifications = NO;
            } else if (!granted) {
                NSLog(@"Notification permission denied by user - falling back to legacy API");
                useModernNotifications = NO;
            } else {
                NSLog(@"Notification permission granted");
                notificationAuthGranted = YES;
            }
            dispatch_semaphore_signal(sem);
        }];

        // Wait briefly for authorization (with timeout)
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));

        // If modern API failed, use legacy for this and future calls
        if (!useModernNotifications) {
            showNotificationLegacy(titleStr, bodyStr, subtitleStr, silent);
            return;
        }
    }

    // Create notification content
    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = titleStr;
    content.body = bodyStr;
    if (subtitleStr) {
        content.subtitle = subtitleStr;
    }
    if (!silent) {
        content.sound = [UNNotificationSound defaultSound];
    }

    // Create a unique identifier for this notification
    NSString *identifier = [[NSUUID UUID] UUIDString];

    // Create the request with no trigger (immediate delivery)
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                          content:content
                                                                          trigger:nil];

    // Schedule the notification
    [center addNotificationRequest:request withCompletionHandler:^(NSError * _Nullable error) {
        if (error) {
            NSLog(@"Failed to schedule notification via modern API: %@ - trying legacy", error);
            // Fall back to legacy API
            useModernNotifications = NO;
            showNotificationLegacy(titleStr, bodyStr, subtitleStr, silent);
        } else {
            NSLog(@"Notification scheduled successfully: %@", titleStr);
        }
    }];
}

extern "C" const char *openFileDialog(const char *startingFolder,
                                      const char *allowedFileTypes,
                                      BOOL canChooseFiles,
                                      BOOL canChooseDirectories,
                                      BOOL allowsMultipleSelection) {


    __block NSOpenPanel *panel;
    __block NSInteger result = NSModalResponseCancel;
    __block NSString *concatenatedPaths = nil;
    
    dispatch_sync(dispatch_get_main_queue(), ^{        
        panel = [NSOpenPanel openPanel];        
        [panel setCanChooseFiles:canChooseFiles];        
        [panel setCanChooseDirectories:canChooseDirectories];        
        [panel setAllowsMultipleSelection:allowsMultipleSelection];        

        NSString *startFolder = [NSString stringWithUTF8String:startingFolder ?: ""];
        [panel setDirectoryURL:[NSURL fileURLWithPath:startFolder]];        
        
        if (allowedFileTypes && strcmp(allowedFileTypes, "*") != 0 && strcmp(allowedFileTypes, "") != 0) {            
            NSString *allowedTypesStr = [NSString stringWithUTF8String:allowedFileTypes];
            NSArray *fileTypesArray = [allowedTypesStr componentsSeparatedByString:@","];
            #pragma clang diagnostic push
            #pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:fileTypesArray];
            #pragma clang diagnostic pop
        }
                
        result = [panel runModal]; // Run the modal dialog on the main thread        
        
        if (result == NSModalResponseOK) {            
            NSArray<NSURL *> *selectedFileURLs = [panel URLs];
            NSMutableArray<NSString *> *pathStrings = [NSMutableArray array];
            for (NSURL *u in selectedFileURLs) {
                [pathStrings addObject:u.path];
            }
            concatenatedPaths = [pathStrings componentsJoinedByString:@","];
        }        
    });
    
    // Return the result after the dispatch_sync completes
    return (concatenatedPaths) ? strdup([concatenatedPaths UTF8String]) : NULL;
}

// showMessageBox - Display a native message box dialog with custom buttons
// type: 0=none, 1=info, 2=warning, 3=error, 4=question
// buttons: comma-separated list of button labels (e.g., "OK,Cancel")
// Returns: index of the clicked button (0-based), or -1 if cancelled
extern "C" int showMessageBox(const char *type,
                              const char *title,
                              const char *message,
                              const char *detail,
                              const char *buttons,
                              int defaultId,
                              int cancelId) {
    __block int result = -1;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *alert = [[NSAlert alloc] init];

        // Set the message and informative text
        if (title && strlen(title) > 0) {
            [alert setMessageText:[NSString stringWithUTF8String:title]];
        }
        if (message && strlen(message) > 0) {
            [alert setInformativeText:[NSString stringWithUTF8String:message]];
        }

        // Set the alert style based on type
        if (type) {
            NSString *typeStr = [NSString stringWithUTF8String:type];
            if ([typeStr isEqualToString:@"warning"]) {
                [alert setAlertStyle:NSAlertStyleWarning];
            } else if ([typeStr isEqualToString:@"error"] || [typeStr isEqualToString:@"critical"]) {
                [alert setAlertStyle:NSAlertStyleCritical];
            } else {
                // info, question, none all use informational style
                [alert setAlertStyle:NSAlertStyleInformational];
            }
        }

        // Add buttons from comma-separated list
        if (buttons && strlen(buttons) > 0) {
            NSString *buttonsStr = [NSString stringWithUTF8String:buttons];
            NSArray *buttonArray = [buttonsStr componentsSeparatedByString:@","];
            for (NSString *buttonTitle in buttonArray) {
                NSString *trimmedTitle = [buttonTitle stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                if (trimmedTitle.length > 0) {
                    [alert addButtonWithTitle:trimmedTitle];
                }
            }
        } else {
            // Default to OK button if none specified
            [alert addButtonWithTitle:@"OK"];
        }

        // Run the modal and get the response
        NSModalResponse response = [alert runModal];

        // Convert NSModalResponse to button index (0-based)
        // NSAlertFirstButtonReturn = 1000, NSAlertSecondButtonReturn = 1001, etc.
        result = (int)(response - NSAlertFirstButtonReturn);
    });

    return result;
}

// ============================================================================
// Clipboard API
// ============================================================================

// clipboardReadText - Read text from the system clipboard
// Returns: UTF-8 string (caller must free) or NULL if no text available
extern "C" const char* clipboardReadText() {
    __block const char* result = NULL;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSString *text = [pasteboard stringForType:NSPasteboardTypeString];
        if (text) {
            result = strdup([text UTF8String]);
        }
    });

    return result;
}

// clipboardWriteText - Write text to the system clipboard
extern "C" void clipboardWriteText(const char *text) {
    if (!text) return;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        [pasteboard setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
    });
}

// clipboardReadImage - Read image from clipboard as PNG data
// Returns: PNG data (caller must free) and sets outSize, or NULL if no image
extern "C" const uint8_t* clipboardReadImage(size_t *outSize) {
    __block const uint8_t* result = NULL;
    __block size_t size = 0;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

        // Try to read image data (supports PNG, TIFF, etc.)
        NSArray *imageTypes = @[NSPasteboardTypePNG, NSPasteboardTypeTIFF];
        NSString *bestType = [pasteboard availableTypeFromArray:imageTypes];

        if (bestType) {
            NSData *imageData = [pasteboard dataForType:bestType];
            if (imageData) {
                // Convert to PNG if not already
                if ([bestType isEqualToString:NSPasteboardTypePNG]) {
                    size = [imageData length];
                    uint8_t *buffer = (uint8_t*)malloc(size);
                    memcpy(buffer, [imageData bytes], size);
                    result = buffer;
                } else {
                    // Convert TIFF or other formats to PNG
                    NSImage *image = [[NSImage alloc] initWithData:imageData];
                    if (image) {
                        NSBitmapImageRep *bitmapRep = [[NSBitmapImageRep alloc] initWithData:[image TIFFRepresentation]];
                        NSData *pngData = [bitmapRep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                        if (pngData) {
                            size = [pngData length];
                            uint8_t *buffer = (uint8_t*)malloc(size);
                            memcpy(buffer, [pngData bytes], size);
                            result = buffer;
                        }
                    }
                }
            }
        }
    });

    if (outSize) *outSize = size;
    return result;
}

// clipboardWriteImage - Write PNG image data to clipboard
extern "C" void clipboardWriteImage(const uint8_t *pngData, size_t size) {
    if (!pngData || size == 0) return;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];

        NSData *data = [NSData dataWithBytes:pngData length:size];
        [pasteboard setData:data forType:NSPasteboardTypePNG];
    });
}

// clipboardClear - Clear the clipboard
extern "C" void clipboardClear() {
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
    });
}

// clipboardAvailableFormats - Get available formats in clipboard
// Returns: comma-separated list of formats (caller must free)
extern "C" const char* clipboardAvailableFormats() {
    __block const char* result = NULL;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSMutableArray *formats = [NSMutableArray array];

        // Check for text
        if ([pasteboard stringForType:NSPasteboardTypeString]) {
            [formats addObject:@"text"];
        }

        // Check for image
        NSArray *imageTypes = @[NSPasteboardTypePNG, NSPasteboardTypeTIFF];
        if ([pasteboard availableTypeFromArray:imageTypes]) {
            [formats addObject:@"image"];
        }

        // Check for files
        if ([pasteboard availableTypeFromArray:@[NSPasteboardTypeFileURL]]) {
            [formats addObject:@"files"];
        }

        // Check for HTML
        if ([pasteboard availableTypeFromArray:@[NSPasteboardTypeHTML]]) {
            [formats addObject:@"html"];
        }

        NSString *joined = [formats componentsJoinedByString:@","];
        result = strdup([joined UTF8String]);
    });

    return result;
}

// ============================================================================
// URL Scheme / Deep Linking API
// ============================================================================

// setURLOpenHandler - Set the callback for handling URLs opened via custom URL schemes
extern "C" void setURLOpenHandler(URLOpenHandler handler) {
    g_urlOpenHandler = handler;
}

extern "C" void setAppReopenHandler(AppReopenHandler handler) {
    g_appReopenHandler = handler;
}

extern "C" void setDockIconVisible(bool visible) {
    void (^applyVisibility)(void) = ^{
        NSApplication *app = [NSApplication sharedApplication];
        if (visible) {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            [app activateIgnoringOtherApps:YES];
        } else {
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
    };

    if ([NSThread isMainThread]) {
        applyVisibility();
    } else {
        dispatch_async(dispatch_get_main_queue(), applyVisibility);
    }
}

extern "C" bool isDockIconVisible() {
    __block bool isVisible = true;

    void (^readVisibility)(void) = ^{
        NSApplication *app = [NSApplication sharedApplication];
        isVisible = [app activationPolicy] == NSApplicationActivationPolicyRegular;
    };

    if ([NSThread isMainThread]) {
        readVisibility();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readVisibility);
    }

    return isVisible;
}

extern "C" NSStatusItem* createTray(uint32_t trayId, const char *title, const char *pathToImage, bool isTemplate,
                                    uint32_t width, uint32_t height, ZigStatusItemHandler zigTrayItemHandler) {
    
    __block NSStatusItem* trayPtr;
    
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSString *pathToImageString = [NSString stringWithUTF8String:pathToImage ?: ""];    
        NSString *titleString = [NSString stringWithUTF8String:title ?: ""];    
        NSStatusItem *statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        if (pathToImageString.length > 0) {
            statusItem.button.image = [[NSImage alloc] initWithContentsOfFile:pathToImageString];
            [statusItem.button.image setTemplate:isTemplate];
            statusItem.button.image.size = NSMakeSize(width, height);
        }    

        if (titleString.length > 0) {
            statusItem.button.title = titleString;
        }    

        if (zigTrayItemHandler) {
            StatusItemTarget *target = [[StatusItemTarget alloc] init];
            target.statusItem = statusItem;
            target.zigHandler = zigTrayItemHandler;
            target.trayId = trayId;        
            objc_setAssociatedObject(statusItem.button, "statusItemTarget", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [statusItem.button setTarget:target];
            [statusItem.button setAction:@selector(statusItemClicked:)];
            [statusItem.button sendActionOn:(NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp)];
        }

        retainObjCObject(statusItem);    

        trayPtr = statusItem;
    });

    return trayPtr;
    
}

extern "C" void setTrayTitle(NSStatusItem *statusItem, const char *title) {
    if (statusItem) {
        statusItem.button.title = [NSString stringWithUTF8String:title ?: ""];
    }
}

extern "C" void setTrayImage(NSStatusItem *statusItem, const char *image) {
    if (statusItem) {
        NSString *imgPath = [NSString stringWithUTF8String:image ?: ""];
        statusItem.button.image = [[NSImage alloc] initWithContentsOfFile:imgPath];
    }
}


extern "C" void setTrayMenuFromJSON(NSStatusItem *statusItem, const char *jsonString) {
    // Copy the string before dispatch_async since the JS-side buffer may be GC'd
    char *jsonCopy = strdup(jsonString);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (statusItem) {
            StatusItemTarget *target = objc_getAssociatedObject(statusItem.button, "statusItemTarget");
            NSData *jsonData = [NSData dataWithBytes:jsonCopy length:strlen(jsonCopy)];
            NSError *error;
            NSArray *menuArray = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
            free(jsonCopy);
            if (error) {
                NSLog(@"Failed to parse JSON: %@", error);
                return;
            }
            NSMenu *menu = createMenuFromConfig(menuArray, target);
            [statusItem setMenu:menu];
        } else {
            free(jsonCopy);
        }
    });
}

extern "C" void setTrayMenu(NSStatusItem *statusItem, const char *menuConfig) {
    if (statusItem) {
        setTrayMenuFromJSON(statusItem, menuConfig);
    }
}

extern "C" void removeTray(NSStatusItem *statusItem) {
    if (statusItem) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSStatusBar systemStatusBar] removeStatusItem:statusItem];
        });
    }
}

extern "C" const char* getTrayBounds(NSStatusItem *statusItem) {
    if (!statusItem) {
        return strdup("{\"x\":0,\"y\":0,\"width\":0,\"height\":0}");
    }

    __block NSString *json = nil;

    void (^readBounds)(void) = ^{
        NSStatusBarButton *button = statusItem.button;
        if (!button || !button.window) {
            json = @"{\"x\":0,\"y\":0,\"width\":0,\"height\":0}";
            return;
        }

        NSRect frameInWindow = button.frame;
        NSRect frameOnScreen = [button.window convertRectToScreen:frameInWindow];
        json = [NSString stringWithFormat:@"{\"x\":%.0f,\"y\":%.0f,\"width\":%.0f,\"height\":%.0f}",
            frameOnScreen.origin.x,
            frameOnScreen.origin.y,
            frameOnScreen.size.width,
            frameOnScreen.size.height];
    };

    if ([NSThread isMainThread]) {
        readBounds();
    } else {
        dispatch_sync(dispatch_get_main_queue(), readBounds);
    }

    return strdup([json UTF8String]);
}

extern "C" void setApplicationMenu(const char *jsonString, ZigStatusItemHandler zigTrayItemHandler) {
    // Copy the string before dispatch_async since the JS-side buffer may be GC'd
    char *jsonCopy = strdup(jsonString);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSData *jsonData = [NSData dataWithBytes:jsonCopy length:strlen(jsonCopy)];
        NSError *error;
        NSArray *menuArray = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
        free(jsonCopy);
        if (error) {
            NSLog(@"Failed to parse JSON: %@", error);
            return;
        }
        StatusItemTarget *target = [[StatusItemTarget alloc] init];
        target.zigHandler = zigTrayItemHandler;
        target.trayId = 0;
        NSMenu *menu = createMenuFromConfig(menuArray, target);
        objc_setAssociatedObject(NSApp, "AppMenuTarget", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [NSApp setMainMenu:menu];
    });
}

extern "C" void showContextMenu(const char *jsonString, ZigStatusItemHandler contextMenuHandler) {
    // Copy the string before dispatch_async since the JS-side buffer may be GC'd
    char *jsonCopy = strdup(jsonString);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSData *jsonData = [NSData dataWithBytes:jsonCopy length:strlen(jsonCopy)];
        NSError *error;
        NSArray *menuArray = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&error];
        free(jsonCopy);
        if (error) {
            NSLog(@"Failed to parse JSON: %@", error);
            return;
        }
        StatusItemTarget *target = [[StatusItemTarget alloc] init];
        target.zigHandler = contextMenuHandler;
        target.trayId = 0;
        NSMenu *menu = createMenuFromConfig(menuArray, target);
        objc_setAssociatedObject(menu, "ContextMenuTarget", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        NSPoint mouseLocation = [NSEvent mouseLocation];
        NSEvent *event = [NSEvent mouseEventWithType:NSEventTypeRightMouseUp
                                            location:mouseLocation
                                    modifierFlags:0
                                        timestamp:0
                                        windowNumber:0
                                            context:nil
                                        eventNumber:0
                                        clickCount:1
                                            pressure:1];
        [menu popUpMenuPositioningItem:nil atLocation:mouseLocation inView:nil];
        objc_setAssociatedObject(NSApp, "ContextMenu", target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    });
}

extern "C" void getWebviewSnapshot(uint32_t hostId, uint32_t webviewId,
                                   WKWebView *webView,
                                   zigSnapshotCallback callback) {
    WKSnapshotConfiguration *snapshotConfig = [[WKSnapshotConfiguration alloc] init];
    [webView takeSnapshotWithConfiguration:snapshotConfig completionHandler:^(NSImage *snapshotImage, NSError *error) {
        if (error) {
            NSLog(@"Error capturing snapshot: %@", error);
            return;
        }
        NSBitmapImageRep *imgRep = [[NSBitmapImageRep alloc] initWithData:[snapshotImage TIFFRepresentation]];
        NSData *pngData = [imgRep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        NSString *base64String = [pngData base64EncodedStringWithOptions:0];
        NSString *dataUrl = [NSString stringWithFormat:@"data:image/png;base64,%@", base64String];
        if (callback) {
            callback(hostId, webviewId, [dataUrl UTF8String]);
        }
    }];
}


extern "C" void setJSUtils(GetMimeType getMimeType, GetHTMLForWebviewSync getHTMLForWebviewSync) {    
    // NO-OP: jsUtils callbacks are deprecated, now using map-based approach
    // The function is kept for compatibility but does nothing
    
    // create a dispatch queue on the current thread (worker thread) that
    // can later be called from main
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_DEFAULT, 0);
    jsWorkerQueue = dispatch_queue_create("com.electrobun.jsworker", attr);    

}

// MARK: - Webview HTML Content Management (replaces JSCallback approach)

extern "C" void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent) {
    if (!webviewHTMLContent) {
        NSLog(@"ERROR: setWebviewHTMLContent called before initialization");
        return;
    }
    
    [webviewHTMLLock lock];
    NSNumber *key = @(webviewId);
    if (htmlContent) {
        webviewHTMLContent[key] = [NSString stringWithUTF8String:htmlContent];
        NSLog(@"setWebviewHTMLContent: Set HTML for webview %u", webviewId);
    } else {
        [webviewHTMLContent removeObjectForKey:key];
        NSLog(@"setWebviewHTMLContent: Cleared HTML for webview %u", webviewId);
    }
    [webviewHTMLLock unlock];
}

const char* getWebviewHTMLContent(uint32_t webviewId) {
    if (!webviewHTMLContent) {
        NSLog(@"ERROR: getWebviewHTMLContent called before initialization");
        return NULL;
    }

    [webviewHTMLLock lock];
    NSString *htmlContent = webviewHTMLContent[@(webviewId)];
    const char* result = NULL;
    if (htmlContent) {
        result = strdup([htmlContent UTF8String]);
        NSLog(@"getWebviewHTMLContent: Retrieved HTML for webview %u", webviewId);
    } else {
        NSLog(@"getWebviewHTMLContent: No HTML found for webview %u", webviewId);
    }
    [webviewHTMLLock unlock];

    return result;
}

/*
 * =============================================================================
 * GLOBAL KEYBOARD SHORTCUTS
 * =============================================================================
 */

// Callback type for global shortcut triggers
typedef void (*GlobalShortcutCallback)(const char* accelerator);
static GlobalShortcutCallback g_globalShortcutCallback = nullptr;

// Storage for registered shortcuts: accelerator string -> event monitor
static NSMutableDictionary<NSString*, id> *g_globalShortcuts = nil;
static NSLock *g_globalShortcutsLock = nil;

// Helper to parse modifier flags from accelerator string using the shared
// cross-platform parser from accelerator_parser.h.
static NSEventModifierFlags parseModifiers(NSString *accelerator, NSString **outKey) {
    auto parts = electrobun::parseAccelerator([accelerator UTF8String]);
    *outKey = [NSString stringWithUTF8String:parts.key.c_str()];
    return modifierFlagsFromAccelerator(parts);
}

// Helper to get key code from key string
static unsigned short keyCodeFromString(NSString *key) {
    // Map common key names to key codes
    static NSDictionary *keyMap = nil;
    if (!keyMap) {
        keyMap = @{
            // Letters
            @"a": @(0x00), @"b": @(0x0B), @"c": @(0x08), @"d": @(0x02),
            @"e": @(0x0E), @"f": @(0x03), @"g": @(0x05), @"h": @(0x04),
            @"i": @(0x22), @"j": @(0x26), @"k": @(0x28), @"l": @(0x25),
            @"m": @(0x2E), @"n": @(0x2D), @"o": @(0x1F), @"p": @(0x23),
            @"q": @(0x0C), @"r": @(0x0F), @"s": @(0x01), @"t": @(0x11),
            @"u": @(0x20), @"v": @(0x09), @"w": @(0x0D), @"x": @(0x07),
            @"y": @(0x10), @"z": @(0x06),
            // Numbers
            @"0": @(0x1D), @"1": @(0x12), @"2": @(0x13), @"3": @(0x14),
            @"4": @(0x15), @"5": @(0x17), @"6": @(0x16), @"7": @(0x1A),
            @"8": @(0x1C), @"9": @(0x19),
            // Function keys
            @"f1": @(0x7A), @"f2": @(0x78), @"f3": @(0x63), @"f4": @(0x76),
            @"f5": @(0x60), @"f6": @(0x61), @"f7": @(0x62), @"f8": @(0x64),
            @"f9": @(0x65), @"f10": @(0x6D), @"f11": @(0x67), @"f12": @(0x6F),
            @"f13": @(0x69), @"f14": @(0x6B), @"f15": @(0x71), @"f16": @(0x6A),
            @"f17": @(0x40), @"f18": @(0x4F), @"f19": @(0x50), @"f20": @(0x5A),
            // Special keys
            @"space": @(0x31), @" ": @(0x31),
            @"return": @(0x24), @"enter": @(0x24),
            @"tab": @(0x30),
            @"escape": @(0x35), @"esc": @(0x35),
            @"backspace": @(0x33), @"delete": @(0x33),
            @"up": @(0x7E), @"down": @(0x7D), @"left": @(0x7B), @"right": @(0x7C),
            @"home": @(0x73), @"end": @(0x77),
            @"pageup": @(0x74), @"pagedown": @(0x79),
            // Symbols
            @"-": @(0x1B), @"=": @(0x18), @"[": @(0x21), @"]": @(0x1E),
            @"\\": @(0x2A), @";": @(0x29), @"'": @(0x27), @",": @(0x2B),
            @".": @(0x2F), @"/": @(0x2C), @"`": @(0x32),
        };
    }

    NSNumber *code = keyMap[key];
    return code ? [code unsignedShortValue] : 0xFFFF;
}

// Set the callback for global shortcut events
extern "C" void setGlobalShortcutCallback(GlobalShortcutCallback callback) {
    g_globalShortcutCallback = callback;

    // Initialize storage if needed
    if (!g_globalShortcuts) {
        g_globalShortcuts = [[NSMutableDictionary alloc] init];
        g_globalShortcutsLock = [[NSLock alloc] init];
    }
}

// Register a global keyboard shortcut
extern "C" BOOL registerGlobalShortcut(const char* accelerator) {
    if (!accelerator || !g_globalShortcutCallback) {
        NSLog(@"[GlobalShortcut] Cannot register: invalid accelerator or no callback set");
        return NO;
    }

    NSString *accelStr = [NSString stringWithUTF8String:accelerator];

    [g_globalShortcutsLock lock];

    // Check if already registered
    if (g_globalShortcuts[accelStr]) {
        [g_globalShortcutsLock unlock];
        NSLog(@"[GlobalShortcut] Already registered: %@", accelStr);
        return NO;
    }

    // Parse the accelerator
    NSString *key = nil;
    NSEventModifierFlags modifiers = parseModifiers(accelStr, &key);
    unsigned short keyCode = keyCodeFromString(key);

    if (keyCode == 0xFFFF) {
        [g_globalShortcutsLock unlock];
        NSLog(@"[GlobalShortcut] Unknown key: %@", key);
        return NO;
    }

    // Create a copy of accelerator for the block
    NSString *accelCopy = [accelStr copy];

    // Create global monitor
    id monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^(NSEvent *event) {
            // Check if the key and modifiers match
            if (event.keyCode == keyCode) {
                // Mask out irrelevant modifier bits (like caps lock, fn, etc.)
                NSEventModifierFlags relevantMask = (NSEventModifierFlagCommand |
                                                     NSEventModifierFlagControl |
                                                     NSEventModifierFlagOption |
                                                     NSEventModifierFlagShift);
                NSEventModifierFlags eventMods = event.modifierFlags & relevantMask;

                if (eventMods == modifiers) {
                    // Trigger the callback
                    if (g_globalShortcutCallback) {
                        g_globalShortcutCallback([accelCopy UTF8String]);
                    }
                }
            }
        }];

    if (monitor) {
        g_globalShortcuts[accelStr] = monitor;
        [g_globalShortcutsLock unlock];
        NSLog(@"[GlobalShortcut] Registered: %@ (keyCode: %d, modifiers: 0x%lX)",
              accelStr, keyCode, (unsigned long)modifiers);
        return YES;
    }

    [g_globalShortcutsLock unlock];
    NSLog(@"[GlobalShortcut] Failed to create monitor for: %@", accelStr);
    return NO;
}

// Unregister a global keyboard shortcut
extern "C" BOOL unregisterGlobalShortcut(const char* accelerator) {
    if (!accelerator) return NO;

    NSString *accelStr = [NSString stringWithUTF8String:accelerator];

    [g_globalShortcutsLock lock];

    id monitor = g_globalShortcuts[accelStr];
    if (monitor) {
        [NSEvent removeMonitor:monitor];
        [g_globalShortcuts removeObjectForKey:accelStr];
        [g_globalShortcutsLock unlock];
        NSLog(@"[GlobalShortcut] Unregistered: %@", accelStr);
        return YES;
    }

    [g_globalShortcutsLock unlock];
    return NO;
}

// Unregister all global keyboard shortcuts
extern "C" void unregisterAllGlobalShortcuts(void) {
    [g_globalShortcutsLock lock];

    for (NSString *key in g_globalShortcuts) {
        id monitor = g_globalShortcuts[key];
        [NSEvent removeMonitor:monitor];
    }
    [g_globalShortcuts removeAllObjects];

    [g_globalShortcutsLock unlock];
    NSLog(@"[GlobalShortcut] Unregistered all shortcuts");
}

// Check if a shortcut is registered
extern "C" BOOL isGlobalShortcutRegistered(const char* accelerator) {
    if (!accelerator) return NO;

    NSString *accelStr = [NSString stringWithUTF8String:accelerator];

    [g_globalShortcutsLock lock];
    BOOL result = g_globalShortcuts[accelStr] != nil;
    [g_globalShortcutsLock unlock];

    return result;
}

/*
 * =============================================================================
 * SCREEN API
 * =============================================================================
 */

// Get all displays as JSON array
// Returns: [{"id":123,"bounds":{x,y,width,height},"workArea":{...},"scaleFactor":2.0,"isPrimary":true},...]
extern "C" const char* getAllDisplays(void) {
    @autoreleasepool {
        NSArray<NSScreen *> *screens = [NSScreen screens];
        CGDirectDisplayID primaryDisplayId = CGMainDisplayID();

        NSMutableArray *displays = [NSMutableArray array];

        for (NSScreen *screen in screens) {
            // Get the display ID from the screen's deviceDescription
            NSDictionary *deviceDescription = [screen deviceDescription];
            NSNumber *screenNumber = deviceDescription[@"NSScreenNumber"];
            CGDirectDisplayID displayId = [screenNumber unsignedIntValue];

            // Get frame (full bounds) - need to flip Y coordinate for consistency
            NSRect frame = [screen frame];
            // macOS uses bottom-left origin, convert to top-left for consistency with other platforms
            CGFloat primaryHeight = [[[NSScreen screens] firstObject] frame].size.height;
            CGFloat flippedY = primaryHeight - frame.origin.y - frame.size.height;

            // Get visible frame (excludes menu bar and dock)
            NSRect visibleFrame = [screen visibleFrame];
            CGFloat visibleFlippedY = primaryHeight - visibleFrame.origin.y - visibleFrame.size.height;

            // Get scale factor (Retina = 2.0)
            CGFloat scaleFactor = [screen backingScaleFactor];

            // Check if this is the primary display
            BOOL isPrimary = (displayId == primaryDisplayId);

            NSDictionary *displayInfo = @{
                @"id": @(displayId),
                @"bounds": @{
                    @"x": @((int)frame.origin.x),
                    @"y": @((int)flippedY),
                    @"width": @((int)frame.size.width),
                    @"height": @((int)frame.size.height)
                },
                @"workArea": @{
                    @"x": @((int)visibleFrame.origin.x),
                    @"y": @((int)visibleFlippedY),
                    @"width": @((int)visibleFrame.size.width),
                    @"height": @((int)visibleFrame.size.height)
                },
                @"scaleFactor": @(scaleFactor),
                @"isPrimary": @(isPrimary)
            };

            [displays addObject:displayInfo];
        }

        NSError *error = nil;
        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:displays options:0 error:&error];
        if (error) {
            NSLog(@"[Screen] Failed to serialize displays: %@", error);
            return strdup("[]");
        }

        NSString *jsonString = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
        return strdup([jsonString UTF8String]);
    }
}

// Get primary display as JSON
extern "C" const char* getPrimaryDisplay(void) {
    @autoreleasepool {
        NSArray<NSScreen *> *screens = [NSScreen screens];
        CGDirectDisplayID primaryDisplayId = CGMainDisplayID();

        for (NSScreen *screen in screens) {
            NSDictionary *deviceDescription = [screen deviceDescription];
            NSNumber *screenNumber = deviceDescription[@"NSScreenNumber"];
            CGDirectDisplayID displayId = [screenNumber unsignedIntValue];

            if (displayId == primaryDisplayId) {
                NSRect frame = [screen frame];
                CGFloat primaryHeight = [[[NSScreen screens] firstObject] frame].size.height;
                CGFloat flippedY = primaryHeight - frame.origin.y - frame.size.height;

                NSRect visibleFrame = [screen visibleFrame];
                CGFloat visibleFlippedY = primaryHeight - visibleFrame.origin.y - visibleFrame.size.height;

                CGFloat scaleFactor = [screen backingScaleFactor];

                NSDictionary *displayInfo = @{
                    @"id": @(displayId),
                    @"bounds": @{
                        @"x": @((int)frame.origin.x),
                        @"y": @((int)flippedY),
                        @"width": @((int)frame.size.width),
                        @"height": @((int)frame.size.height)
                    },
                    @"workArea": @{
                        @"x": @((int)visibleFrame.origin.x),
                        @"y": @((int)visibleFlippedY),
                        @"width": @((int)visibleFrame.size.width),
                        @"height": @((int)visibleFrame.size.height)
                    },
                    @"scaleFactor": @(scaleFactor),
                    @"isPrimary": @YES
                };

                NSError *error = nil;
                NSData *jsonData = [NSJSONSerialization dataWithJSONObject:displayInfo options:0 error:&error];
                if (error) {
                    NSLog(@"[Screen] Failed to serialize primary display: %@", error);
                    return strdup("{}");
                }

                NSString *jsonString = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
                return strdup([jsonString UTF8String]);
            }
        }

        return strdup("{}");
    }
}

// Get current cursor position as JSON: {"x": 123, "y": 456}
extern "C" const char* getCursorScreenPoint(void) {
    @autoreleasepool {
        NSPoint mouseLocation = [NSEvent mouseLocation];

        // Convert from bottom-left origin to top-left origin
        CGFloat primaryHeight = [[[NSScreen screens] firstObject] frame].size.height;
        CGFloat flippedY = primaryHeight - mouseLocation.y;

        NSDictionary *point = @{
            @"x": @((int)mouseLocation.x),
            @"y": @((int)flippedY)
        };

        NSError *error = nil;
        NSData *jsonData = [NSJSONSerialization dataWithJSONObject:point options:0 error:&error];
        if (error) {
            return strdup("{\"x\":0,\"y\":0}");
        }

        NSString *jsonString = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
        return strdup([jsonString UTF8String]);
    }
}

extern "C" uint64_t getMouseButtons(void) {
    return (uint64_t)[NSEvent pressedMouseButtons];
}

/*
 * =============================================================================
 * COOKIE MANAGEMENT API
 * =============================================================================
 */

// Helper to convert NSHTTPCookie to NSDictionary for JSON serialization
static NSDictionary* cookieToDictionary(NSHTTPCookie *cookie) {
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    dict[@"name"] = cookie.name ?: @"";
    dict[@"value"] = cookie.value ?: @"";
    dict[@"domain"] = cookie.domain ?: @"";
    dict[@"path"] = cookie.path ?: @"/";
    dict[@"secure"] = @(cookie.secure);
    dict[@"httpOnly"] = @(cookie.HTTPOnly);
    if (cookie.expiresDate) {
        dict[@"expirationDate"] = @([cookie.expiresDate timeIntervalSince1970]);
    }
    if (cookie.sameSitePolicy) {
        dict[@"sameSite"] = cookie.sameSitePolicy;
    }
    return dict;
}

// Get cookies for a partition (WKWebView)
// filterJson: {"url": "https://example.com"} or {"domain": ".example.com"} or {} for all
// Returns JSON array of cookies
extern "C" const char* sessionGetCookies(const char* partitionIdentifier, const char* filterJson) {
    // Copy strings for use in block
    NSString *partitionStr = partitionIdentifier ? [NSString stringWithUTF8String:partitionIdentifier] : @"";
    NSString *filterStr = filterJson ? [NSString stringWithUTF8String:filterJson] : @"{}";

    __block char* result = strdup("[]");
    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            NSData *filterData = [filterStr dataUsingEncoding:NSUTF8StringEncoding];
            NSError *parseError = nil;
            NSDictionary *filter = [NSJSONSerialization JSONObjectWithData:filterData options:0 error:&parseError];
            if (parseError) {
                filter = @{};
            }

            NSString *filterUrl = filter[@"url"];
            NSString *filterDomain = filter[@"domain"];

            // Get the data store for this partition
            WKWebsiteDataStore *dataStore = createDataStoreForPartition([partitionStr UTF8String]);
            WKHTTPCookieStore *cookieStore = dataStore.httpCookieStore;

            [cookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
                NSMutableArray *matchingCookies = [NSMutableArray array];

                for (NSHTTPCookie *cookie in cookies) {
                    BOOL matches = YES;

                    if (filterUrl) {
                        NSURL *url = [NSURL URLWithString:filterUrl];
                        NSString *host = url.host;
                        NSString *cookieDomain = cookie.domain;
                        if ([cookieDomain hasPrefix:@"."]) {
                            matches = [host hasSuffix:cookieDomain] || [host isEqualToString:[cookieDomain substringFromIndex:1]];
                        } else {
                            matches = [host isEqualToString:cookieDomain];
                        }
                        if (matches && cookie.path && url.path) {
                            matches = [url.path hasPrefix:cookie.path];
                        }
                    } else if (filterDomain) {
                        NSString *cookieDomain = cookie.domain;
                        if ([filterDomain hasPrefix:@"."]) {
                            matches = [cookieDomain isEqualToString:filterDomain] ||
                                      [cookieDomain hasSuffix:filterDomain];
                        } else {
                            matches = [cookieDomain isEqualToString:filterDomain] ||
                                      [cookieDomain isEqualToString:[@"." stringByAppendingString:filterDomain]];
                        }
                    }

                    if (matches) {
                        [matchingCookies addObject:cookieToDictionary(cookie)];
                    }
                }

                NSError *error = nil;
                NSData *jsonData = [NSJSONSerialization dataWithJSONObject:matchingCookies options:0 error:&error];
                if (!error) {
                    NSString *resultJson = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
                    free(result);
                    result = strdup([resultJson UTF8String]);
                }

                dispatch_semaphore_signal(completionSemaphore);
            }];
        }
    });

    dispatch_semaphore_wait(completionSemaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return result;
}

// Set a cookie for a partition (WKWebView)
// cookieJson: {"url":"https://example.com","name":"token","value":"abc","domain":".example.com","path":"/","secure":true,"httpOnly":true,"expirationDate":1234567890,"sameSite":"Lax"}
extern "C" bool sessionSetCookie(const char* partitionIdentifier, const char* cookieJson) {
    // Copy strings for use in block
    NSString *partitionStr = partitionIdentifier ? [NSString stringWithUTF8String:partitionIdentifier] : @"";
    NSString *jsonStr = cookieJson ? [NSString stringWithUTF8String:cookieJson] : @"{}";

    // Parse cookie JSON first (can be done off main thread)
    NSData *jsonData = [jsonStr dataUsingEncoding:NSUTF8StringEncoding];
    NSError *parseError = nil;
    NSDictionary *cookieDict = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&parseError];
    if (parseError || !cookieDict[@"name"] || !cookieDict[@"value"]) {
        NSLog(@"[Cookie] Invalid cookie JSON: %@", jsonStr);
        return false;
    }

    // Build cookie properties
    NSMutableDictionary *properties = [NSMutableDictionary dictionary];
    properties[NSHTTPCookieName] = cookieDict[@"name"];
    properties[NSHTTPCookieValue] = cookieDict[@"value"];

    // Domain - required, derive from URL if not provided
    if (cookieDict[@"domain"]) {
        properties[NSHTTPCookieDomain] = cookieDict[@"domain"];
    } else if (cookieDict[@"url"]) {
        NSURL *url = [NSURL URLWithString:cookieDict[@"url"]];
        properties[NSHTTPCookieDomain] = url.host;
    } else {
        NSLog(@"[Cookie] Missing domain or url");
        return false;
    }

    // Path
    properties[NSHTTPCookiePath] = cookieDict[@"path"] ?: @"/";

    // Secure
    if ([cookieDict[@"secure"] boolValue]) {
        properties[NSHTTPCookieSecure] = @"TRUE";
    }

    // Expiration date
    if (cookieDict[@"expirationDate"]) {
        NSTimeInterval timestamp = [cookieDict[@"expirationDate"] doubleValue];
        properties[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:timestamp];
    }

    // SameSite
    if (cookieDict[@"sameSite"]) {
        properties[NSHTTPCookieSameSitePolicy] = cookieDict[@"sameSite"];
    }

    NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:properties];
    if (!cookie) {
        NSLog(@"[Cookie] Failed to create cookie from properties");
        return false;
    }

    __block bool success = false;
    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            WKWebsiteDataStore *dataStore = createDataStoreForPartition([partitionStr UTF8String]);
            WKHTTPCookieStore *cookieStore = dataStore.httpCookieStore;

            [cookieStore setCookie:cookie completionHandler:^{
                success = true;
                dispatch_semaphore_signal(completionSemaphore);
            }];
        }
    });

    dispatch_semaphore_wait(completionSemaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return success;
}

// Remove a specific cookie for a partition (WKWebView)
extern "C" bool sessionRemoveCookie(const char* partitionIdentifier, const char* urlStr, const char* cookieName) {
    if (!urlStr || !cookieName) {
        return false;
    }

    NSString *partitionStr = partitionIdentifier ? [NSString stringWithUTF8String:partitionIdentifier] : @"";
    NSString *url = [NSString stringWithUTF8String:urlStr];
    NSString *name = [NSString stringWithUTF8String:cookieName];
    NSURL *nsUrl = [NSURL URLWithString:url];
    if (!nsUrl) {
        return false;
    }

    __block bool found = false;
    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            WKWebsiteDataStore *dataStore = createDataStoreForPartition([partitionStr UTF8String]);
            WKHTTPCookieStore *cookieStore = dataStore.httpCookieStore;

            [cookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
                for (NSHTTPCookie *cookie in cookies) {
                    if ([cookie.name isEqualToString:name]) {
                        // Check if domain matches
                        NSString *host = nsUrl.host;
                        NSString *cookieDomain = cookie.domain;
                        BOOL domainMatches = NO;
                        if ([cookieDomain hasPrefix:@"."]) {
                            domainMatches = [host hasSuffix:cookieDomain] || [host isEqualToString:[cookieDomain substringFromIndex:1]];
                        } else {
                            domainMatches = [host isEqualToString:cookieDomain];
                        }

                        if (domainMatches) {
                            [cookieStore deleteCookie:cookie completionHandler:^{
                                found = true;
                                dispatch_semaphore_signal(completionSemaphore);
                            }];
                            return;
                        }
                    }
                }
                dispatch_semaphore_signal(completionSemaphore);
            }];
        }
    });

    dispatch_semaphore_wait(completionSemaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    return found;
}

// Remove all cookies for a partition (WKWebView)
extern "C" void sessionClearCookies(const char* partitionIdentifier) {
    NSString *partitionStr = partitionIdentifier ? [NSString stringWithUTF8String:partitionIdentifier] : @"";

    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            WKWebsiteDataStore *dataStore = createDataStoreForPartition([partitionStr UTF8String]);

            NSSet *dataTypes = [NSSet setWithObject:WKWebsiteDataTypeCookies];
            NSDate *dateFrom = [NSDate dateWithTimeIntervalSince1970:0];

            [dataStore removeDataOfTypes:dataTypes modifiedSince:dateFrom completionHandler:^{
                dispatch_semaphore_signal(completionSemaphore);
            }];
        }
    });

    dispatch_semaphore_wait(completionSemaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
}

// Clear all storage data for a partition (WKWebView)
// storageTypesJson: ["cookies", "localStorage", "sessionStorage", "indexedDB", "cache"] or null for all
extern "C" void sessionClearStorageData(const char* partitionIdentifier, const char* storageTypesJson) {
    NSString *partitionStr = partitionIdentifier ? [NSString stringWithUTF8String:partitionIdentifier] : @"";
    NSString *typesStr = storageTypesJson ? [NSString stringWithUTF8String:storageTypesJson] : @"";

    dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            WKWebsiteDataStore *dataStore = createDataStoreForPartition([partitionStr UTF8String]);

            NSMutableSet *dataTypes = [NSMutableSet set];

            if (typesStr.length > 0) {
                NSData *jsonData = [typesStr dataUsingEncoding:NSUTF8StringEncoding];
                NSArray *types = [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:nil];

                for (NSString *type in types) {
                    if ([type isEqualToString:@"cookies"]) {
                        [dataTypes addObject:WKWebsiteDataTypeCookies];
                    } else if ([type isEqualToString:@"localStorage"]) {
                        [dataTypes addObject:WKWebsiteDataTypeLocalStorage];
                    } else if ([type isEqualToString:@"sessionStorage"]) {
                        [dataTypes addObject:WKWebsiteDataTypeSessionStorage];
                    } else if ([type isEqualToString:@"indexedDB"]) {
                        [dataTypes addObject:WKWebsiteDataTypeIndexedDBDatabases];
                    } else if ([type isEqualToString:@"cache"]) {
                        [dataTypes addObject:WKWebsiteDataTypeDiskCache];
                        [dataTypes addObject:WKWebsiteDataTypeMemoryCache];
                    } else if ([type isEqualToString:@"serviceWorkers"]) {
                        [dataTypes addObject:WKWebsiteDataTypeServiceWorkerRegistrations];
                    }
                }
            } else {
                // Clear all
                dataTypes = [NSMutableSet setWithSet:[WKWebsiteDataStore allWebsiteDataTypes]];
            }

            if (dataTypes.count == 0) {
                dispatch_semaphore_signal(completionSemaphore);
                return;
            }

            NSDate *dateFrom = [NSDate dateWithTimeIntervalSince1970:0];

            [dataStore removeDataOfTypes:dataTypes modifiedSince:dateFrom completionHandler:^{
                dispatch_semaphore_signal(completionSemaphore);
            }];
        }
    });

    dispatch_semaphore_wait(completionSemaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
}

// Window icon - Linux only, no-op for macOS (macOS uses app bundle icon)
extern "C" void setWindowIcon(void* window, const char* iconPath) {
    // Not supported on macOS - macOS windows use the app bundle icon
}

extern "C" void setWindowVisibleOnAllWorkspaces(NSWindow *window, bool visible) {
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSWindowCollectionBehavior behavior = [window collectionBehavior];
        if (visible) {
            behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
        } else {
            behavior &= ~NSWindowCollectionBehaviorCanJoinAllSpaces;
        }
        [window setCollectionBehavior:behavior];
    });
}

extern "C" bool isWindowVisibleOnAllWorkspaces(NSWindow *window) {
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = ([window collectionBehavior] & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0;
    });
    return result;
}
