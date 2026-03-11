#pragma once
#ifdef __linux__
#ifdef QCVIEW_HAS_SDBUS

#include <string>
#include <vector>
#include <functional>

struct SingleInstanceImpl;

namespace qcview {

// D-Bus based single-instance support.
// Uses the session bus to register a well-known name (com.qcview.Player).
// If the name is already owned, sends file paths to the existing instance and returns false.
// If we acquire the name, starts listening for incoming file open requests.

class SingleInstance {
public:
    using FileCallback = std::function<void(const std::vector<std::string>&)>;

    SingleInstance();
    ~SingleInstance();

    // Try to become the primary instance.
    // Returns true if we are now the primary instance (start normally).
    // Returns false if another instance is running (files were sent to it, should exit).
    bool TryAcquire(const std::vector<std::string>& files_to_send);

    // Set callback for when files are received from another instance.
    void SetFileCallback(FileCallback callback);

    // Poll for incoming D-Bus messages. Call once per frame from the main loop.
    void Poll();

private:
    friend struct ::SingleInstanceImpl;
    SingleInstanceImpl* impl_ = nullptr;
};

} // namespace qcview

#endif // QCVIEW_HAS_SDBUS
#endif // __linux__
