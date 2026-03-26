#ifdef __APPLE__

#include "macos_single_instance.h"
#include "../utils/debug_utils.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

// ============================================================================
// Shared state for file-open events
// ============================================================================

static qcview::MacOSSingleInstance::FileCallback g_file_callback;
static bool g_has_pending_files = false;
static std::vector<std::string> g_pending_files;
static std::mutex g_pending_mutex;

static void DeliverOrQueueFiles(const std::vector<std::string>& files) {
    if (files.empty()) return;

    Debug::Log("MacOSSingleInstance: Received " + std::to_string(files.size()) + " file(s)");
    for (const auto& f : files) {
        Debug::Log("  -> " + f);
    }

    std::lock_guard<std::mutex> lock(g_pending_mutex);
    if (g_file_callback) {
        g_file_callback(files);
    } else {
        g_pending_files.insert(g_pending_files.end(), files.begin(), files.end());
        g_has_pending_files = true;
    }
}

// ============================================================================
// QCViewFileHandler — handles file opens via both Apple Events (early, pre-GLFW)
// and delegate method injection (runtime, post-GLFW)
// ============================================================================

@interface QCViewFileHandler : NSObject
- (void)handleOpenDocumentsEvent:(NSAppleEventDescriptor *)event
                  withReplyEvent:(NSAppleEventDescriptor *)replyEvent;
@end

@implementation QCViewFileHandler

- (void)handleOpenDocumentsEvent:(NSAppleEventDescriptor *)event
                  withReplyEvent:(NSAppleEventDescriptor *)replyEvent {
    NSAppleEventDescriptor *directObject = [event paramDescriptorForKeyword:keyDirectObject];
    if (!directObject) return;

    std::vector<std::string> files;
    NSInteger count = [directObject numberOfItems];

    for (NSInteger i = 1; i <= count; i++) {
        NSAppleEventDescriptor *desc = [directObject descriptorAtIndex:i];

        // Coerce to fileURL (Apple Events send aliases/file refs, not strings)
        NSAppleEventDescriptor *urlDesc = [desc coerceToDescriptorType:typeFileURL];
        if (urlDesc) {
            NSString *urlStr = [[NSString alloc] initWithData:[urlDesc data]
                                                     encoding:NSUTF8StringEncoding];
            if (urlStr) {
                NSURL *url = [NSURL URLWithString:urlStr];
                if (url && [url path]) {
                    files.push_back([[url path] UTF8String]);
                    continue;
                }
            }
        }

        // Fallback
        NSString *str = [desc stringValue];
        if (str) files.push_back([str UTF8String]);
    }

    DeliverOrQueueFiles(files);
}

@end

// Static handler instance (retained for lifetime of the process)
static QCViewFileHandler *g_file_handler = nil;

// ============================================================================
// C functions injected into GLFW's NSApplicationDelegate at runtime
// ============================================================================

static void QCView_application_openURLs(id self, SEL _cmd, NSApplication *app, NSArray<NSURL *> *urls) {
    std::vector<std::string> files;
    for (NSURL *url in urls) {
        if ([url isFileURL]) {
            files.push_back([[url path] UTF8String]);
        } else {
            files.push_back([[url absoluteString] UTF8String]);
        }
    }
    DeliverOrQueueFiles(files);
}

static BOOL QCView_application_openFile(id self, SEL _cmd, NSApplication *app, NSString *filename) {
    std::vector<std::string> files;
    files.push_back([filename UTF8String]);
    DeliverOrQueueFiles(files);
    return YES;
}

// ============================================================================
// MacOSSingleInstance implementation
// ============================================================================

namespace qcview {

MacOSSingleInstance::MacOSSingleInstance() = default;
MacOSSingleInstance::~MacOSSingleInstance() = default;

bool MacOSSingleInstance::TryAcquire(const std::vector<std::string>& files_to_send) {
    NSString *bundleId = [[NSBundle mainBundle] bundleIdentifier];
    if (!bundleId) {
        Debug::Log("MacOSSingleInstance: No bundle identifier, skipping single instance check");
        is_primary_ = true;
        return true;
    }

    // Find other running instances with the same bundle ID
    NSArray<NSRunningApplication *> *running =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:bundleId];

    pid_t our_pid = [[NSProcessInfo processInfo] processIdentifier];
    NSRunningApplication *existing = nil;
    for (NSRunningApplication *app in running) {
        if ([app processIdentifier] != our_pid && ![app isTerminated]) {
            existing = app;
            break;
        }
    }

    if (!existing) {
        Debug::Log("MacOSSingleInstance: Primary instance (pid " + std::to_string(our_pid) + ")");

        // Pre-register file-open methods on GLFW's delegate class BEFORE glfwInit().
        //
        // GLFW creates GLFWApplicationDelegate and calls [NSApp run] during
        // glfwInit(). [NSApp run] processes queued Apple Events including
        // kAEOpenDocuments (file double-click). macOS dispatches this as
        // application:openURLs: on the delegate. If the method doesn't exist,
        // macOS shows "cannot open files in X format" error.
        //
        // By injecting the method on the class before glfwInit() creates an
        // instance, the method is available when the event arrives.
        Class glfwDelegateClass = objc_getClass("GLFWApplicationDelegate");
        if (glfwDelegateClass) {
            class_addMethod(glfwDelegateClass,
                @selector(application:openURLs:),
                (IMP)QCView_application_openURLs,
                "v@:@@");
            class_addMethod(glfwDelegateClass,
                @selector(application:openFile:),
                (IMP)QCView_application_openFile,
                "B@:@@");
            Debug::Log("MacOSSingleInstance: Injected file handlers into GLFWApplicationDelegate (pre-init)");
        } else {
            // GLFW class doesn't exist yet — register Apple Event handler as fallback
            g_file_handler = [[QCViewFileHandler alloc] init];
            [[NSAppleEventManager sharedAppleEventManager]
                setEventHandler:g_file_handler
                andSelector:@selector(handleOpenDocumentsEvent:withReplyEvent:)
                forEventClass:kCoreEventClass
                andEventID:kAEOpenDocuments];
            Debug::Log("MacOSSingleInstance: GLFWApplicationDelegate not yet loaded, using Apple Event fallback");
        }

        is_primary_ = true;
        return true;
    }

    // Another instance exists — forward files to it
    Debug::Log("MacOSSingleInstance: Found existing instance (pid " +
               std::to_string([existing processIdentifier]) + ")");

    if (!files_to_send.empty()) {
        NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:files_to_send.size()];
        for (const auto& path : files_to_send) {
            NSString *nsPath = [NSString stringWithUTF8String:path.c_str()];
            NSURL *url = nil;
            // Custom URI schemes (qcview://) should be parsed as URLs, not file paths
            if ([nsPath hasPrefix:@"qcview://"] || [nsPath containsString:@"://"]) {
                url = [NSURL URLWithString:nsPath];
            } else {
                url = [NSURL fileURLWithPath:nsPath];
            }
            if (url) [urls addObject:url];
        }

        if ([urls count] > 0) {
            NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration];
            config.activates = YES;

            NSURL *appURL = [existing bundleURL];
            [[NSWorkspace sharedWorkspace] openURLs:urls
                               withApplicationAtURL:appURL
                                      configuration:config
                                  completionHandler:^(NSRunningApplication *app, NSError *error) {
                if (error) {
                    NSLog(@"MacOSSingleInstance: Failed to forward files: %@", error);
                }
            }];

            Debug::Log("MacOSSingleInstance: Forwarded " + std::to_string(files_to_send.size()) +
                       " file(s) to existing instance");
        }
    } else {
        [existing activateWithOptions:NSApplicationActivateAllWindows |
                                      NSApplicationActivateIgnoringOtherApps];
        Debug::Log("MacOSSingleInstance: Activated existing instance window");
    }

    return false;
}

void MacOSSingleInstance::RegisterEventHandler() {
    if (!is_primary_) return;

    // Inject application:openURLs: and application:openFile: into GLFW's
    // NSApplicationDelegate at runtime. This handles file opens that arrive
    // after GLFW init (when the app is already running).
    id delegate = [NSApp delegate];
    if (!delegate) {
        Debug::Log("MacOSSingleInstance: No NSApp delegate — cannot inject file handler");
        return;
    }

    Class delegateClass = [delegate class];

    BOOL added = class_addMethod(delegateClass,
        @selector(application:openURLs:),
        (IMP)QCView_application_openURLs,
        "v@:@@");
    Debug::Log(std::string("MacOSSingleInstance: openURLs: injection ") +
               (added ? "succeeded" : "already exists") +
               " on " + class_getName(delegateClass));

    BOOL addedFile = class_addMethod(delegateClass,
        @selector(application:openFile:),
        (IMP)QCView_application_openFile,
        "B@:@@");
    Debug::Log(std::string("MacOSSingleInstance: openFile: injection ") +
               (addedFile ? "succeeded" : "already exists"));
}

void MacOSSingleInstance::SetFileCallback(FileCallback callback) {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_file_callback = callback;
    file_callback_ = callback;

    if (g_has_pending_files && !g_pending_files.empty()) {
        Debug::Log("MacOSSingleInstance: Delivering " + std::to_string(g_pending_files.size()) +
                   " queued file(s)");
        callback(g_pending_files);
        g_pending_files.clear();
        g_has_pending_files = false;
    }
}

void MacOSSingleInstance::Poll() {
    // Events dispatched during glfwPollEvents()
}

} // namespace qcview

#endif // __APPLE__
