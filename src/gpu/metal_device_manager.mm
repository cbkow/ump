#ifdef __APPLE__

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#ifndef QCVIEW_USE_METAL
// GLFW only needed for non-native macOS path (shouldn't happen, but guard it)
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include "metal_device_manager.h"
#include "../utils/debug_utils.h"

namespace qcview {

//=============================================================================
// Singleton Instance
//=============================================================================

MetalDeviceManager& MetalDeviceManager::Instance() {
    static MetalDeviceManager instance;
    return instance;
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

MetalDeviceManager::MetalDeviceManager() {
    Debug::Log("MetalDeviceManager: Created (call Initialize to set up)");
}

MetalDeviceManager::~MetalDeviceManager() {
    Shutdown();
}

//=============================================================================
// Initialization
//=============================================================================

bool MetalDeviceManager::Initialize(void* ns_view_ptr) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        Debug::Log("MetalDeviceManager: Already initialized");
        return true;
    }

    if (!ns_view_ptr) {
        Debug::Log("MetalDeviceManager: ERROR - null view");
        return false;
    }

#ifdef QCVIEW_USE_METAL
    // Native macOS path: MTKView already has device + CAMetalLayer
    MTKView* mtkView = (__bridge MTKView*)ns_view_ptr;
    id<MTLDevice> device = mtkView.device;
    if (!device) {
        Debug::Log("MetalDeviceManager: ERROR - MTKView has no device");
        return false;
    }
    device_ = (__bridge_retained void*)device;

    Debug::Log("MetalDeviceManager: Device: " + std::string([device.name UTF8String]));

    // Create command queue
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        Debug::Log("MetalDeviceManager: ERROR - failed to create command queue");
        return false;
    }
    command_queue_ = (__bridge_retained void*)queue;

    // MTKView's layer IS a CAMetalLayer — use it directly
    CAMetalLayer* layer = (CAMetalLayer*)mtkView.layer;
    layer.displaySyncEnabled = YES;
    layer.maximumDrawableCount = 3;

    metal_layer_ = (__bridge_retained void*)layer;
    CGSize drawableSize = mtkView.drawableSize;

    initialized_ = true;
    Debug::Log("MetalDeviceManager: Initialized successfully (native MTKView)");
    Debug::Log("  Drawable size: " + std::to_string((int)drawableSize.width) + "x" +
               std::to_string((int)drawableSize.height));
#else
    // GLFW path (not used on macOS with native Metal, but kept for reference)
    Debug::Log("MetalDeviceManager: ERROR - GLFW path not available in native Metal build");
    return false;
#endif

    return true;
}

void MetalDeviceManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    Debug::Log("MetalDeviceManager: Shutting down...");

    // Release Metal objects (ARC bridge retained -> release)
    if (command_queue_) {
        CFRelease(command_queue_);
        command_queue_ = nullptr;
    }
    if (metal_layer_) {
        CFRelease(metal_layer_);
        metal_layer_ = nullptr;
    }
    if (device_) {
        CFRelease(device_);
        device_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("MetalDeviceManager: Shutdown complete");
}

//=============================================================================
// Command Buffer Helpers
//=============================================================================

void* MetalDeviceManager::CreateCommandBuffer() {
    if (!command_queue_) return nullptr;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
    id<MTLCommandBuffer> buffer = [queue commandBuffer];
    return (__bridge_retained void*)buffer;
}

void MetalDeviceManager::WaitForGPU() {
    if (!command_queue_) return;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue_;
    id<MTLCommandBuffer> syncBuf = [queue commandBuffer];
    [syncBuf commit];
    [syncBuf waitUntilCompleted];
}

//=============================================================================
// Debug/Info
//=============================================================================

std::string MetalDeviceManager::GetDeviceName() const {
    if (!device_) return "Unknown";
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    return std::string([device.name UTF8String]);
}

size_t MetalDeviceManager::GetDedicatedVideoMemory() const {
    if (!device_) return 0;
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_;
    // On Apple Silicon, unified memory — report recommended max working set
    return static_cast<size_t>(device.recommendedMaxWorkingSetSize);
}

} // namespace qcview

#endif // __APPLE__
