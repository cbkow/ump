#pragma once

#ifdef QCVIEW_USE_METAL

#include <functional>

// =============================================================================
// Native macOS windowing bridge — replaces GLFW for the Metal path
//
// These C++ functions are implemented in macos_app_delegate.mm (ObjC++) and
// callable from .cpp files. They manage NSWindow + MTKView + NSApplication.
// MTKView provides display-linked draw callbacks, eliminating the manual
// glfwPollEvents() loop and [CATransaction flush] that caused frame pacing
// issues (~48ms intervals instead of 16.7ms).
// =============================================================================

// Lifecycle
bool MacOS_CreateWindow(int width, int height, int x, int y, const char* title);
void MacOS_DestroyWindow();
void MacOS_RunApp();      // [NSApp run] — blocks until app terminates
void MacOS_StopApp();     // [NSApp terminate:nil]
bool MacOS_ShouldClose();
void MacOS_SetShouldClose(bool close);

// Draw callback — called from MTKView's drawInMTKView: at display refresh rate
void MacOS_SetDrawCallback(std::function<void()> callback);

// Native handles (returned as void* for use in C++ code)
void* MacOS_GetNSWindow();      // NSWindow*
void* MacOS_GetNSView();        // MTKView* (NSView*)
void* MacOS_GetMetalLayer();    // CAMetalLayer*
void* MacOS_GetMetalDevice();   // id<MTLDevice>
void* MacOS_GetCommandQueue();  // id<MTLCommandQueue>

// Window geometry
void MacOS_GetWindowPos(int* x, int* y);
void MacOS_SetWindowPos(int x, int y);
void MacOS_GetWindowSize(int* w, int* h);
void MacOS_SetWindowSize(int w, int h);
void MacOS_GetFramebufferSize(int* w, int* h);
void MacOS_GetContentScale(float* x, float* y);

// Window operations
void MacOS_FocusWindow();
void MacOS_SetWindowTitle(const char* title);

// Fullscreen
void MacOS_ToggleNativeFullscreen();
bool MacOS_IsFullscreenAnimating();  // True during macOS fullscreen enter/exit animation
bool MacOS_IsFullscreenEntering();   // True only when animating INTO fullscreen
bool MacOS_IsNativeFullscreen();     // True when fully in native fullscreen (animation complete)

// Drag-drop
void MacOS_SetDropCallback(void (*callback)(int count, const char** paths));

// Current frame drawable/render pass (valid only inside draw callback)
void* MacOS_GetCurrentDrawable();           // id<CAMetalDrawable>
void* MacOS_GetCurrentRenderPassDescriptor(); // MTLRenderPassDescriptor*

#endif // QCVIEW_USE_METAL
