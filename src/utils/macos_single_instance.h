#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>
#include <functional>

namespace qcview {

// macOS single instance enforcement.
// Uses NSRunningApplication to detect existing instances and Apple Events
// (kAEOpenDocuments) to forward file open requests.
class MacOSSingleInstance {
public:
    using FileCallback = std::function<void(const std::vector<std::string>&)>;

    MacOSSingleInstance();
    ~MacOSSingleInstance();

    // Check if another instance is running.
    // If so, forward files to it and return false (caller should exit).
    // If not, become the primary instance and return true.
    bool TryAcquire(const std::vector<std::string>& files_to_send);

    // Register file-open handler. Call once AFTER glfwInit() so the method
    // injection targets GLFW's delegate. Early launch-time events are caught
    // by a pre-GLFW Apple Event handler and queued.
    void RegisterEventHandler();

    // Set callback for receiving files from other instances.
    // Must be called after TryAcquire returns true.
    void SetFileCallback(FileCallback callback);

    // Process any pending Apple Events. Call from main loop.
    void Poll();

private:
    bool is_primary_ = false;
    FileCallback file_callback_;
    void* app_delegate_ = nullptr;  // QCViewAppDelegate (retained)
};

} // namespace qcview

#endif // __APPLE__
