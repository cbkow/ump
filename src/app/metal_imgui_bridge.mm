// metal_imgui_bridge.mm — ObjC++ bridge functions for Metal/ImGui
// These functions are called from .cpp files via extern "C++" declarations.
// They exist because .cpp files cannot use ObjC syntax (id<>, @autoreleasepool, etc.)

#ifdef QCVIEW_USE_METAL

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_osx.h>
#include <chrono>

// Cached render pass descriptor with a valid texture for NewFrame
static MTLRenderPassDescriptor* g_renderPassDesc = nil;
static CAMetalLayer* g_metalLayer = nil;
static id<MTLTexture> g_dummyTexture = nil;

// EDR state — set by MetalHDRSwapchain, read by MetalRenderFrame
static bool g_edr_active = false;
static float g_edr_scale = 1.0f;
static bool g_edr_linear = false;  // true = linear encoding, false = gamma encoding

void SetMetalEDRState(bool active, float edr_scale) {
    g_edr_active = active;
    g_edr_scale = edr_scale;
    // Invalidate dummy texture since pixel format may have changed
    g_dummyTexture = nil;
}

void SetMetalEDRLinear(bool is_linear) {
    g_edr_linear = is_linear;
}

bool GetMetalEDRActive() {
    return g_edr_active;
}

bool GetMetalEDRLinear() {
    return g_edr_linear;
}

float GetMetalEDRScale() {
    return g_edr_scale;
}

// Initialize ImGui OSX platform backend (bridge for .cpp callers)
bool InitImGuiOSXBackend(void* ns_view) {
    if (!ns_view) return false;
    NSView* view = (__bridge NSView*)ns_view;
    return ImGui_ImplOSX_Init(view);
}

// NewFrame for OSX platform backend (bridge for .cpp callers)
void ImGuiOSXNewFrame(void* ns_view) {
    NSView* view = (__bridge NSView*)ns_view;
    ImGui_ImplOSX_NewFrame(view);
}

// Shutdown OSX platform backend (bridge for .cpp callers)
void ShutdownImGuiOSXBackend() {
    ImGui_ImplOSX_Shutdown();
}

// Initialize ImGui Metal backend
bool InitMetalImGui(void* window, void* device) {
    if (!device) return false;

    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
    ImGui_ImplMetal_Init(mtl_device);

    return true;
}

// Set the metal layer for NewFrame (call after device init)
void SetMetalLayerForImGui(void* metal_layer) {
    g_metalLayer = (__bridge CAMetalLayer*)metal_layer;
}

// Shutdown ImGui Metal backend
void ShutdownMetalImGui() {
    ImGui_ImplMetal_Shutdown();
    g_renderPassDesc = nil;
    g_metalLayer = nil;
}

// Combined NewFrame + Render + Present
// Returns false if drawable not available.
bool MetalRenderFrame(void* metal_layer, void* command_queue, void* imgui_draw_data) {
    @autoreleasepool {
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue;
        ImDrawData* draw_data = static_cast<ImDrawData*>(imgui_draw_data);

        // Frame timing diagnostic
        static auto last_present = std::chrono::steady_clock::now();
        auto before_drawable = std::chrono::steady_clock::now();

#ifdef QCVIEW_USE_METAL
        // Native macOS: use MTKView's drawable and render pass descriptor
        // (MTKView owns the drawable lifecycle — don't call [layer nextDrawable])
        extern void* MacOS_GetCurrentDrawable();
        extern void* MacOS_GetCurrentRenderPassDescriptor();

        id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)MacOS_GetCurrentDrawable();
        MTLRenderPassDescriptor* renderPassDesc = (__bridge MTLRenderPassDescriptor*)MacOS_GetCurrentRenderPassDescriptor();
        if (!drawable || !renderPassDesc) return false;
#else
        // GLFW path: manually acquire drawable from CAMetalLayer
        CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return false;

        MTLRenderPassDescriptor* renderPassDesc = [MTLRenderPassDescriptor new];
        renderPassDesc.colorAttachments[0].texture = drawable.texture;
        renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif

        auto after_drawable = std::chrono::steady_clock::now();
        double drawable_wait_ms = std::chrono::duration<double, std::milli>(after_drawable - before_drawable).count();
        double frame_interval_ms = std::chrono::duration<double, std::milli>(after_drawable - last_present).count();
        last_present = after_drawable;

        (void)drawable_wait_ms; (void)frame_interval_ms; // FRAME TIMING logging disabled

        // EDR-aware clear color
        if (g_edr_active && g_edr_linear) {
            float linear_bg = 0.01003f * g_edr_scale;
            renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(linear_bg, linear_bg, linear_bg, 1.0);
        } else {
            renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
        }

        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDesc];

        // Render ImGui draw data
        ImGui_ImplMetal_RenderDrawData(draw_data, commandBuffer, encoder);

        [encoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];

#ifndef QCVIEW_USE_METAL
        // CATransaction flush only needed for GLFW+Metal (not native MTKView)
        [CATransaction flush];
#endif

        return true;
    }
}

// NewFrame for Metal
void MetalImGuiNewFrame() {
    @autoreleasepool {

#ifdef QCVIEW_USE_METAL
    // Native macOS: use MTKView's render pass descriptor (has real drawable texture)
    extern void* MacOS_GetCurrentRenderPassDescriptor();
    MTLRenderPassDescriptor* rpd = (__bridge MTLRenderPassDescriptor*)MacOS_GetCurrentRenderPassDescriptor();
    if (!rpd) return;
    ImGui_ImplMetal_NewFrame(rpd);
#else
    // GLFW path: create dummy render pass descriptor for sampleCount
    if (!g_metalLayer) return;

    if (!g_renderPassDesc) {
        g_renderPassDesc = [MTLRenderPassDescriptor new];
        g_renderPassDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
        g_renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (g_edr_active && g_edr_linear) {
        float linear_bg = 0.01003f * g_edr_scale;
        g_renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(linear_bg, linear_bg, linear_bg, 1.0);
    } else {
        g_renderPassDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.1, 1.0);
    }

    if (!g_dummyTexture) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_metalLayer.pixelFormat
                                                                                        width:1
                                                                                       height:1
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget;
        g_dummyTexture = [g_metalLayer.device newTextureWithDescriptor:desc];
    }
    g_renderPassDesc.colorAttachments[0].texture = g_dummyTexture;

    ImGui_ImplMetal_NewFrame(g_renderPassDesc);
#endif
    } // @autoreleasepool
}

#endif // QCVIEW_USE_METAL
