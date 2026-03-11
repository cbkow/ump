#ifdef __linux__

#include "linux_clipboard.h"
#include "debug_utils.h"

#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <mutex>
#include <thread>

#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// ============================================================================
// Shared state
// ============================================================================

static std::vector<uint8_t> s_clipboard_data;
static std::mutex s_clipboard_mutex;

// ============================================================================
// Wayland clipboard implementation
// ============================================================================

// Extern helpers from qcview_wl_helpers.c (compiled inside GLFW, has access to _glfw internals)
extern "C" {
    uint32_t _glfwGetWaylandSerial(void);
    struct wl_data_device_manager* _glfwGetWaylandDataDeviceManager(void);
    struct wl_seat* _glfwGetWaylandSeat(void);
    struct wl_data_device* _glfwGetWaylandDataDevice(void);
}

static struct wl_data_source* s_wl_data_source = nullptr;

// Data source listener — handles send requests from pasting apps
static void dataSourceTarget(void* /*data*/, struct wl_data_source* /*source*/,
                              const char* /*mime_type*/) {}

static void dataSourceSend(void* /*data*/, struct wl_data_source* /*source*/,
                            const char* mime_type, int fd) {
    if (strcmp(mime_type, "image/png") != 0) {
        close(fd);
        return;
    }

    std::lock_guard<std::mutex> lock(s_clipboard_mutex);
    const uint8_t* ptr = s_clipboard_data.data();
    size_t remaining = s_clipboard_data.size();

    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written == -1) {
            if (errno == EINTR)
                continue;
            Debug::Log("Clipboard: write error: " + std::string(strerror(errno)));
            break;
        }
        ptr += written;
        remaining -= written;
    }

    close(fd);
}

static void dataSourceCancelled(void* /*data*/, struct wl_data_source* source) {
    wl_data_source_destroy(source);
    if (s_wl_data_source == source) {
        s_wl_data_source = nullptr;
    }
}

static const struct wl_data_source_listener s_data_source_listener = {
    dataSourceTarget,
    dataSourceSend,
    dataSourceCancelled,
};

static bool CopyImageWayland(const std::vector<uint8_t>& png_data) {
    // Get GLFW's internal Wayland objects directly
    struct wl_data_device_manager* ddm = _glfwGetWaylandDataDeviceManager();
    struct wl_data_device* data_device = _glfwGetWaylandDataDevice();
    uint32_t serial = _glfwGetWaylandSerial();

    if (!ddm) {
        Debug::Log("Clipboard: No Wayland data device manager");
        return false;
    }
    if (!data_device) {
        Debug::Log("Clipboard: No Wayland data device");
        return false;
    }

    // Destroy previous source
    if (s_wl_data_source) {
        wl_data_source_destroy(s_wl_data_source);
        s_wl_data_source = nullptr;
    }

    // Store data
    {
        std::lock_guard<std::mutex> lock(s_clipboard_mutex);
        s_clipboard_data = png_data;
    }

    // Create new data source
    s_wl_data_source = wl_data_device_manager_create_data_source(ddm);
    if (!s_wl_data_source) {
        Debug::Log("Clipboard: Failed to create Wayland data source");
        return false;
    }

    wl_data_source_add_listener(s_wl_data_source, &s_data_source_listener, nullptr);
    wl_data_source_offer(s_wl_data_source, "image/png");

    wl_data_device_set_selection(data_device, s_wl_data_source, serial);

    // Flush to ensure the compositor processes it
    struct wl_display* display = glfwGetWaylandDisplay();
    wl_display_flush(display);

    return true;
}

static void CleanupWayland() {
    if (s_wl_data_source) {
        wl_data_source_destroy(s_wl_data_source);
        s_wl_data_source = nullptr;
    }
}

// ============================================================================
// X11 clipboard implementation
// ============================================================================

#include <X11/Xlib.h>
#include <X11/Xatom.h>

static Display* s_x11_display = nullptr;
static ::Window s_x11_window = 0;
static Atom s_x11_clipboard_atom = 0;
static Atom s_x11_targets_atom = 0;
static Atom s_x11_png_atom = 0;
static std::thread s_x11_thread;
static std::atomic<bool> s_x11_running{false};

static void X11ClipboardThread() {
    XEvent event;
    while (s_x11_running.load()) {
        // Wait for events with a timeout (so we can check s_x11_running)
        while (XPending(s_x11_display)) {
            XNextEvent(s_x11_display, &event);

            if (event.type == SelectionRequest) {
                XSelectionRequestEvent& req = event.xselectionrequest;
                XSelectionEvent response;
                response.type = SelectionNotify;
                response.display = req.display;
                response.requestor = req.requestor;
                response.selection = req.selection;
                response.target = req.target;
                response.property = req.property;
                response.time = req.time;

                if (req.target == s_x11_targets_atom) {
                    // Reply with supported targets
                    Atom targets[] = { s_x11_targets_atom, s_x11_png_atom };
                    XChangeProperty(s_x11_display, req.requestor, req.property,
                                    XA_ATOM, 32, PropModeReplace,
                                    reinterpret_cast<unsigned char*>(targets), 2);
                } else if (req.target == s_x11_png_atom) {
                    // Reply with PNG data
                    std::lock_guard<std::mutex> lock(s_clipboard_mutex);
                    XChangeProperty(s_x11_display, req.requestor, req.property,
                                    s_x11_png_atom, 8, PropModeReplace,
                                    s_clipboard_data.data(),
                                    static_cast<int>(s_clipboard_data.size()));
                } else {
                    // Unsupported target
                    response.property = None;
                }

                XSendEvent(s_x11_display, req.requestor, False, 0,
                           reinterpret_cast<XEvent*>(&response));
                XFlush(s_x11_display);
            } else if (event.type == SelectionClear) {
                // We lost clipboard ownership
                s_x11_running.store(false);
                break;
            }
        }

        if (s_x11_running.load()) {
            // Sleep briefly to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

static bool CopyImageX11(const std::vector<uint8_t>& png_data) {
    // Clean up previous clipboard thread
    if (s_x11_running.load()) {
        s_x11_running.store(false);
        if (s_x11_thread.joinable()) {
            s_x11_thread.join();
        }
    }

    // Close previous display connection
    if (s_x11_display) {
        if (s_x11_window) {
            XDestroyWindow(s_x11_display, s_x11_window);
            s_x11_window = 0;
        }
        XCloseDisplay(s_x11_display);
        s_x11_display = nullptr;
    }

    // Store data
    {
        std::lock_guard<std::mutex> lock(s_clipboard_mutex);
        s_clipboard_data = png_data;
    }

    // Open our own X11 connection (separate from GLFW's, for thread safety)
    s_x11_display = XOpenDisplay(nullptr);
    if (!s_x11_display) {
        Debug::Log("Clipboard: Failed to open X11 display");
        return false;
    }

    // Create a hidden window to own the clipboard
    s_x11_window = XCreateSimpleWindow(s_x11_display,
                                        DefaultRootWindow(s_x11_display),
                                        0, 0, 1, 1, 0, 0, 0);
    if (!s_x11_window) {
        Debug::Log("Clipboard: Failed to create X11 window");
        XCloseDisplay(s_x11_display);
        s_x11_display = nullptr;
        return false;
    }

    // Intern atoms
    s_x11_clipboard_atom = XInternAtom(s_x11_display, "CLIPBOARD", False);
    s_x11_targets_atom = XInternAtom(s_x11_display, "TARGETS", False);
    s_x11_png_atom = XInternAtom(s_x11_display, "image/png", False);

    // Claim clipboard ownership
    XSetSelectionOwner(s_x11_display, s_x11_clipboard_atom, s_x11_window, CurrentTime);

    if (XGetSelectionOwner(s_x11_display, s_x11_clipboard_atom) != s_x11_window) {
        Debug::Log("Clipboard: Failed to claim X11 clipboard ownership");
        XDestroyWindow(s_x11_display, s_x11_window);
        XCloseDisplay(s_x11_display);
        s_x11_display = nullptr;
        s_x11_window = 0;
        return false;
    }

    XFlush(s_x11_display);

    // Start background thread to serve clipboard requests
    s_x11_running.store(true);
    s_x11_thread = std::thread(X11ClipboardThread);

    return true;
}

static void CleanupX11() {
    s_x11_running.store(false);
    if (s_x11_thread.joinable()) {
        s_x11_thread.join();
    }
    if (s_x11_display) {
        if (s_x11_window) {
            XDestroyWindow(s_x11_display, s_x11_window);
            s_x11_window = 0;
        }
        XCloseDisplay(s_x11_display);
        s_x11_display = nullptr;
    }
}

// ============================================================================
// Public API
// ============================================================================

namespace qcview {

bool CopyImageToClipboard(const std::vector<uint8_t>& png_data) {
    if (png_data.empty()) {
        Debug::Log("Clipboard: Empty PNG data");
        return false;
    }

    int platform = glfwGetPlatform();

    if (platform == GLFW_PLATFORM_WAYLAND) {
        return CopyImageWayland(png_data);
    } else if (platform == GLFW_PLATFORM_X11) {
        return CopyImageX11(png_data);
    }

    Debug::Log("Clipboard: Unsupported platform");
    return false;
}

void CleanupClipboardResources() {
    int platform = glfwGetPlatform();

    if (platform == GLFW_PLATFORM_WAYLAND) {
        CleanupWayland();
    } else if (platform == GLFW_PLATFORM_X11) {
        CleanupX11();
    }

    std::lock_guard<std::mutex> lock(s_clipboard_mutex);
    s_clipboard_data.clear();
    s_clipboard_data.shrink_to_fit();
}

} // namespace qcview

#endif // __linux__
