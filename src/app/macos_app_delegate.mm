#ifdef QCVIEW_USE_METAL

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "macos_app_delegate.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <chrono>

// =============================================================================
// Global state — accessed by bridge functions
// =============================================================================

static NSWindow* g_window = nil;
static id<MTLDevice> g_device = nil;
static id<MTLCommandQueue> g_commandQueue = nil;
static bool g_shouldClose = false;
static std::function<void()> g_drawCallback;
static void (*g_dropCallback)(int count, const char** paths) = nullptr;

// Fullscreen animation tracking
static bool g_fullscreen_animating = false;   // True during macOS fullscreen enter/exit animation
static bool g_fullscreen_entering = false;    // True if animating INTO fullscreen (false = exiting)
static bool g_native_fullscreen = false;      // True when fully in native fullscreen

// Per-frame state set by drawInMTKView: before calling the C++ draw callback
static id<CAMetalDrawable> g_currentDrawable = nil;
static MTLRenderPassDescriptor* g_currentRenderPassDesc = nil;

// =============================================================================
// QCViewMTKView — MTKView subclass with drag-drop support
// =============================================================================

@interface QCViewMTKView : MTKView <NSDraggingDestination, NSTextInputClient>
@end

@implementation QCViewMTKView

- (instancetype)initWithFrame:(CGRect)frameRect device:(id<MTLDevice>)device {
    self = [super initWithFrame:frameRect device:device];
    if (self) {
        // Register for file drag-drop — include both modern and legacy types
        // for compatibility across macOS versions and drag sources.
        [self registerForDraggedTypes:@[
            NSPasteboardTypeFileURL,
            NSPasteboardTypeURL,
            @"NSFilenamesPboardType"  // Legacy type, still used by some apps/Finder contexts
        ]];
    }
    return self;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// Suppress macOS "unhandled key" beep (NSBeep) while allowing text input.
// Call interpretKeyEvents: so macOS invokes insertText: for character input,
// which feeds into ImGui's IO. This avoids forwarding to KeyEventResponder
// (which steals first responder and breaks modal mouse focus).
- (void)keyDown:(NSEvent*)event {
    [self interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent*)event {
    // Consumed — ImGui's local event monitor handles key-up
}

// NSTextInputClient — called by interpretKeyEvents: when a key produces text
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    ImGuiIO& io = ImGui::GetIO();
    NSString* characters;
    if ([string isKindOfClass:[NSAttributedString class]])
        characters = [string string];
    else
        characters = (NSString*)string;

    for (NSUInteger i = 0; i < characters.length; i++) {
        ImWchar c = [characters characterAtIndex:i];
        io.AddInputCharacter(c);
    }
}

// Required NSTextInputClient methods (minimal stubs)
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {}
- (void)unmarkText {}
- (BOOL)hasMarkedText { return NO; }
- (NSRange)markedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange { return NSZeroRect; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { return NSNotFound; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange { return nil; }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }

// --- NSDraggingDestination ---

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard* pb = [sender draggingPasteboard];

    // Try modern NSURL-based check first
    if ([pb canReadObjectForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]) {
        return NSDragOperationCopy;
    }

    // Fallback: check for legacy filename pasteboard type
    if ([[pb types] containsObject:@"NSFilenamesPboardType"]) {
        return NSDragOperationCopy;
    }

    // Fallback: any URL type
    if ([[pb types] containsObject:NSPasteboardTypeFileURL] ||
        [[pb types] containsObject:NSPasteboardTypeURL]) {
        return NSDragOperationCopy;
    }

    NSLog(@"[DragDrop] draggingEntered rejected. Pasteboard types: %@", [pb types]);
    return NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    // Must also return the operation here — some macOS versions reset to None
    // if draggingUpdated: is not implemented or returns NSDragOperationNone.
    return [self draggingEntered:sender];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (!g_dropCallback) {
        NSLog(@"[DragDrop] performDragOperation: no callback set");
        return NO;
    }

    NSPasteboard* pb = [sender draggingPasteboard];

    // Try modern NSURL-based reading first
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

    // Fallback: legacy NSFilenamesPboardType
    if ((!urls || urls.count == 0) && [[pb types] containsObject:@"NSFilenamesPboardType"]) {
        NSArray* filenames = [pb propertyListForType:@"NSFilenamesPboardType"];
        if (filenames && [filenames isKindOfClass:[NSArray class]]) {
            NSMutableArray<NSURL*>* fileURLs = [NSMutableArray arrayWithCapacity:filenames.count];
            for (NSString* path in filenames) {
                [fileURLs addObject:[NSURL fileURLWithPath:path]];
            }
            urls = fileURLs;
        }
    }

    if (!urls || urls.count == 0) {
        NSLog(@"[DragDrop] performDragOperation: no URLs extracted. Types: %@", [pb types]);
        return NO;
    }

    // Focus window on drop
    [self.window makeKeyAndOrderFront:nil];

    // Convert to C strings
    int count = (int)urls.count;
    std::vector<std::string> pathStrings;
    std::vector<const char*> pathPtrs;
    pathStrings.reserve(count);
    pathPtrs.reserve(count);

    for (NSURL* url in urls) {
        if (url.path) {
            pathStrings.push_back(std::string([url.path UTF8String]));
        }
    }
    for (auto& s : pathStrings) {
        pathPtrs.push_back(s.c_str());
    }

    if (pathStrings.empty()) return NO;

    g_dropCallback((int)pathStrings.size(), pathPtrs.data());
    return YES;
}

@end

// =============================================================================
// QCViewMTKDelegate — MTKViewDelegate that calls into C++ DrawFrame()
// =============================================================================

@interface QCViewMTKDelegate : NSObject <MTKViewDelegate>
@end

@implementation QCViewMTKDelegate

- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        static auto _last_draw = std::chrono::steady_clock::now();
        auto _draw_start = std::chrono::steady_clock::now();

        // Store per-frame drawable/render pass from MTKView for C++ code to use
        g_currentRenderPassDesc = view.currentRenderPassDescriptor;
        auto _after_rpd = std::chrono::steady_clock::now();
        g_currentDrawable = view.currentDrawable;
        auto _after_drawable = std::chrono::steady_clock::now();

        static int _draw_count = 0;
        if (_draw_count < 5) {
            NSLog(@"[MTKView] drawInMTKView #%d: rpd=%p drawable=%p callback=%d bounds=%.0fx%.0f",
                  _draw_count, g_currentRenderPassDesc, g_currentDrawable,
                  (bool)g_drawCallback, view.bounds.size.width, view.bounds.size.height);
            _draw_count++;
        }

        if (g_drawCallback && g_currentRenderPassDesc && g_currentDrawable) {
            g_drawCallback();
        }

        auto _draw_end = std::chrono::steady_clock::now();
        double interval_ms = std::chrono::duration<double, std::milli>(_draw_start - _last_draw).count();
        double rpd_ms = std::chrono::duration<double, std::milli>(_after_rpd - _draw_start).count();
        double drawable_ms = std::chrono::duration<double, std::milli>(_after_drawable - _after_rpd).count();
        double callback_ms = std::chrono::duration<double, std::milli>(_draw_end - _after_drawable).count();
        _last_draw = _draw_start;

        (void)interval_ms; (void)rpd_ms; (void)drawable_ms; (void)callback_ms;

        // Clear per-frame state
        g_currentDrawable = nil;
        g_currentRenderPassDesc = nil;
    }
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    // Resize handled implicitly — DrawFrame reads current size each frame
}

@end

// =============================================================================
// QCViewAppDelegate — NSApplicationDelegate + NSWindowDelegate
// =============================================================================

@interface QCViewAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) QCViewMTKDelegate* mtkDelegate;
@end

@implementation QCViewAppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    // Intercept Cmd+Q / [NSApp terminate:] to route through our clean shutdown
    // path (Cleanup → _exit(0)) instead of exit() → __cxa_finalize_ranges which
    // destroys globals in undefined order and crashes on thread joins.
    // Setting g_shouldClose + [NSApp stop:] makes [NSApp run] return, which
    // goes back to Application::Run() → Cleanup() → _exit(0).
    g_shouldClose = true;

    MTKView* view = (MTKView*)[g_window contentView];
    if (view) {
        view.paused = YES;
        view.delegate = nil;
    }
    g_drawCallback = nullptr;

    [NSApp stop:nil];
    // Post dummy event to wake the run loop so stop takes effect immediately
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];

    return NSTerminateCancel;  // Cancel terminate — we handle it via Cleanup → _exit
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    [NSApp activateIgnoringOtherApps:YES];
}

// --- NSWindowDelegate ---

- (void)windowWillClose:(NSNotification*)notification {
    g_shouldClose = true;
    // Stop MTKView rendering before teardown
    MTKView* view = (MTKView*)[g_window contentView];
    if (view) {
        view.paused = YES;
        view.delegate = nil;
    }
    g_drawCallback = nullptr;
    // Post-process: stop the run loop so [NSApp run] returns
    [NSApp stop:nil];
    // Post a dummy event to wake the run loop so stop takes effect
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    g_shouldClose = true;
    return YES;
}

// --- Fullscreen animation tracking ---
// macOS native fullscreen animates for ~0.7s. During this time the window
// continuously resizes, which would cause ImGui's docking layout to rebuild
// 40+ times. We track animation state so the layout code can skip rebuilds
// until the animation completes.

- (void)windowWillEnterFullScreen:(NSNotification*)notification {
    g_fullscreen_animating = true;
    g_fullscreen_entering = true;
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    g_fullscreen_animating = false;
    g_native_fullscreen = true;
}

- (void)windowWillExitFullScreen:(NSNotification*)notification {
    g_fullscreen_animating = true;
    g_fullscreen_entering = false;
    // Immediately mark as not-fullscreen so the layout switches to windowed
    // mode BEFORE the zoom-out animation. The restructure happens at full size
    // (visually invisible), then macOS smoothly shrinks the windowed layout.
    g_native_fullscreen = false;
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
    g_fullscreen_animating = false;
    g_native_fullscreen = false;
}

// --- File open via Finder / open command ---

- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename {
    if (g_dropCallback && filename) {
        const char* path = [filename UTF8String];
        g_dropCallback(1, &path);
        return YES;
    }
    return NO;
}

- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    if (!g_dropCallback || urls.count == 0) return;

    std::vector<std::string> pathStrings;
    std::vector<const char*> pathPtrs;
    for (NSURL* url in urls) {
        if (url.filePathURL) {
            pathStrings.push_back(std::string([url.path UTF8String]));
        }
    }
    for (auto& s : pathStrings) {
        pathPtrs.push_back(s.c_str());
    }
    if (!pathPtrs.empty()) {
        g_dropCallback((int)pathPtrs.size(), pathPtrs.data());
    }
}

@end

// =============================================================================
// Static storage for ObjC objects (prevent ARC release in non-ARC builds)
// =============================================================================

static QCViewAppDelegate* g_appDelegate = nil;
static QCViewMTKView* g_mtkView = nil;
static QCViewMTKDelegate* g_mtkDelegate = nil;

// =============================================================================
// C++ Bridge Functions
// =============================================================================

bool MacOS_CreateWindow(int width, int height, int x, int y, const char* title) {
    @autoreleasepool {
        // Initialize NSApplication first — required before creating any Cocoa UI objects
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Set up app delegate early so window/event callbacks work during init
        g_appDelegate = [[QCViewAppDelegate alloc] init];
        [NSApp setDelegate:g_appDelegate];

        // Create Metal device and command queue
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            Debug::Log("MacOS_CreateWindow: Failed to create Metal device");
            return false;
        }
        g_commandQueue = [g_device newCommandQueue];
        Debug::Log("MacOS_CreateWindow: Metal device: " + std::string([g_device.name UTF8String]));

        // Create NSWindow
        NSRect contentRect = NSMakeRect(0, 0, width, height);
        NSWindowStyleMask styleMask = NSWindowStyleMaskTitled |
                                       NSWindowStyleMaskClosable |
                                       NSWindowStyleMaskResizable |
                                       NSWindowStyleMaskMiniaturizable;

        g_window = [[NSWindow alloc] initWithContentRect:contentRect
                                                styleMask:styleMask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        [g_window setTitle:[NSString stringWithUTF8String:title]];
        [g_window setMinSize:NSMakeSize(800, 600)];
        [g_window setTabbingMode:NSWindowTabbingModeDisallowed];  // Disable macOS window tabbing
        [g_window setDelegate:g_appDelegate];

        // Position window
        if (x >= 0 && y >= 0) {
            // Convert from top-left origin (our convention) to bottom-left (Cocoa)
            NSRect screenFrame = [[NSScreen mainScreen] frame];
            CGFloat cocoaY = screenFrame.size.height - y - height;
            [g_window setFrameOrigin:NSMakePoint(x, cocoaY)];
        } else {
            [g_window center];
        }

        // Create MTKView as content view
        g_mtkView = [[QCViewMTKView alloc] initWithFrame:contentRect device:g_device];
        g_mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        g_mtkView.depthStencilPixelFormat = MTLPixelFormatInvalid;
        g_mtkView.framebufferOnly = YES;
        g_mtkView.preferredFramesPerSecond = 60;
        g_mtkView.enableSetNeedsDisplay = NO;     // Continuous redraw
        g_mtkView.paused = NO;

        // Set up MTKView delegate
        g_mtkDelegate = [[QCViewMTKDelegate alloc] init];
        g_mtkView.delegate = g_mtkDelegate;

        // Attach to window
        [g_window setContentView:g_mtkView];
        [g_window makeFirstResponder:g_mtkView];
        [g_window makeKeyAndOrderFront:nil];

        g_shouldClose = false;

        Debug::Log("MacOS_CreateWindow: Window created " +
                   std::to_string(width) + "x" + std::to_string(height));
        return true;
    }
}

void MacOS_DestroyWindow() {
    @autoreleasepool {
        g_mtkView.delegate = nil;
        g_mtkDelegate = nil;
        g_mtkView = nil;
        if (g_window) {
            [g_window close];
            g_window = nil;
        }
        g_commandQueue = nil;
        g_device = nil;
        g_drawCallback = nullptr;
        g_dropCallback = nullptr;
        Debug::Log("MacOS_DestroyWindow: Cleanup complete");
    }
}

void MacOS_RunApp() {
    @autoreleasepool {
        // NSApplication and delegate already set up in MacOS_CreateWindow
        [NSApp activateIgnoringOtherApps:YES];

        NSLog(@"[MacOS_RunApp] About to call [NSApp run]. MTKView: %@, delegate: %@, paused: %d, device: %@",
              g_mtkView, g_mtkView.delegate, g_mtkView.isPaused, g_mtkView.device);

        [NSApp run];  // Blocks until app terminates

        NSLog(@"[MacOS_RunApp] [NSApp run] returned");
    }
}

void MacOS_StopApp() {
    g_shouldClose = true;
    [NSApp terminate:nil];
}

bool MacOS_ShouldClose() {
    return g_shouldClose;
}

void MacOS_SetShouldClose(bool close) {
    g_shouldClose = close;
    if (close) {
        [NSApp terminate:nil];
    }
}

void MacOS_SetDrawCallback(std::function<void()> callback) {
    g_drawCallback = std::move(callback);
}

void* MacOS_GetNSWindow() {
    return (__bridge void*)g_window;
}

void* MacOS_GetNSView() {
    return (__bridge void*)g_mtkView;
}

void* MacOS_GetMetalLayer() {
    if (!g_mtkView) return nullptr;
    // MTKView's layer is a CAMetalLayer
    return (__bridge void*)(CAMetalLayer*)g_mtkView.layer;
}

void* MacOS_GetMetalDevice() {
    return (__bridge void*)g_device;
}

void* MacOS_GetCommandQueue() {
    return (__bridge void*)g_commandQueue;
}

void MacOS_GetWindowPos(int* x, int* y) {
    if (!g_window) { *x = 0; *y = 0; return; }
    NSRect frame = [g_window frame];
    NSRect screenFrame = [[NSScreen mainScreen] frame];
    *x = (int)frame.origin.x;
    // Convert from Cocoa bottom-left to top-left origin
    *y = (int)(screenFrame.size.height - frame.origin.y - frame.size.height);
}

void MacOS_SetWindowPos(int x, int y) {
    if (!g_window) return;
    NSRect screenFrame = [[NSScreen mainScreen] frame];
    NSRect winFrame = [g_window frame];
    CGFloat cocoaY = screenFrame.size.height - y - winFrame.size.height;
    [g_window setFrameOrigin:NSMakePoint(x, cocoaY)];
}

void MacOS_GetWindowSize(int* w, int* h) {
    if (!g_window) { *w = 0; *h = 0; return; }
    NSRect content = [[g_window contentView] frame];
    *w = (int)content.size.width;
    *h = (int)content.size.height;
}

void MacOS_SetWindowSize(int w, int h) {
    if (!g_window) return;
    NSRect frame = [g_window frame];
    NSRect content = [g_window contentRectForFrameRect:frame];
    CGFloat titleBarHeight = frame.size.height - content.size.height;
    frame.size.width = w;
    frame.size.height = h + titleBarHeight;
    [g_window setFrame:frame display:YES animate:NO];
}

void MacOS_GetFramebufferSize(int* w, int* h) {
    if (!g_mtkView) { *w = 0; *h = 0; return; }
    CGSize drawableSize = g_mtkView.drawableSize;
    *w = (int)drawableSize.width;
    *h = (int)drawableSize.height;
}

void MacOS_GetContentScale(float* x, float* y) {
    if (!g_window) { *x = 1.0f; *y = 1.0f; return; }
    CGFloat scale = [g_window backingScaleFactor];
    *x = (float)scale;
    *y = (float)scale;
}

void MacOS_FocusWindow() {
    if (!g_window) return;
    [g_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void MacOS_SetWindowTitle(const char* title) {
    if (!g_window || !title) return;
    [g_window setTitle:[NSString stringWithUTF8String:title]];
}

void MacOS_SetDropCallback(void (*callback)(int count, const char** paths)) {
    g_dropCallback = callback;
}

void MacOS_ToggleNativeFullscreen() {
    if (!g_window) return;
    [g_window toggleFullScreen:nil];
}

bool MacOS_IsFullscreenAnimating() {
    return g_fullscreen_animating;
}

bool MacOS_IsFullscreenEntering() {
    return g_fullscreen_animating && g_fullscreen_entering;
}

bool MacOS_IsNativeFullscreen() {
    return g_native_fullscreen;
}

void* MacOS_GetCurrentDrawable() {
    return (__bridge void*)g_currentDrawable;
}

void* MacOS_GetCurrentRenderPassDescriptor() {
    return (__bridge void*)g_currentRenderPassDesc;
}

#endif // QCVIEW_USE_METAL
