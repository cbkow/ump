#pragma once

#ifdef __APPLE__

#include <mutex>
#include <string>

namespace qcview {

//=============================================================================
// MetalDeviceManager
//
// Centralized Metal device and command queue management.
// Singleton pattern ensures all components share the same device.
// Mirrors VulkanDeviceManager interface where applicable.
//
// Usage:
//   auto& mgr = MetalDeviceManager::Instance();
//   if (mgr.Initialize(glfw_window)) {
//       auto device = mgr.GetDevice();  // id<MTLDevice> as void*
//   }
//=============================================================================

class MetalDeviceManager {
public:
    static MetalDeviceManager& Instance();

    MetalDeviceManager(const MetalDeviceManager&) = delete;
    MetalDeviceManager& operator=(const MetalDeviceManager&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize Metal device and attach CAMetalLayer
    // With native macOS: ns_view is MTKView* (already has CAMetalLayer)
    // With GLFW: ns_view is GLFWwindow* (creates CAMetalLayer and attaches)
    bool Initialize(void* ns_view);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Device Access (void* to avoid ObjC in header)
    //=========================================================================

    // Returns id<MTLDevice>
    void* GetDevice() const { return device_; }

    // Returns id<MTLCommandQueue>
    void* GetCommandQueue() const { return command_queue_; }

    // Returns CAMetalLayer*
    void* GetMetalLayer() const { return metal_layer_; }

    //=========================================================================
    // Command Buffer Helpers
    //=========================================================================

    // Create a new command buffer from the queue
    // Returns id<MTLCommandBuffer>
    void* CreateCommandBuffer();

    // Submit an empty command buffer and wait for all prior GPU work to complete.
    // Call before CPU readback of GPU-written textures.
    void WaitForGPU();

    //=========================================================================
    // Debug/Info
    //=========================================================================

    std::string GetDeviceName() const;
    size_t GetDedicatedVideoMemory() const;

private:
    MetalDeviceManager();
    ~MetalDeviceManager();

    void* device_ = nullptr;         // id<MTLDevice>
    void* command_queue_ = nullptr;   // id<MTLCommandQueue>
    void* metal_layer_ = nullptr;     // CAMetalLayer*

    bool initialized_ = false;
    mutable std::mutex mutex_;
};

} // namespace qcview

#endif // __APPLE__
