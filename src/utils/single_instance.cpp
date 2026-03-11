#ifdef __linux__
#ifdef QCVIEW_HAS_SDBUS

#include "single_instance.h"
#include "debug_utils.h"

#include <systemd/sd-bus.h>
#include <cstring>

static const char* BUS_NAME = "app.qcview.QCView";
static const char* OBJECT_PATH = "/app/qcview/QCView";
static const char* INTERFACE_NAME = "app.qcview.QCView";

struct SingleInstanceImpl {
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
    qcview::SingleInstance::FileCallback file_callback;
    bool is_primary = false;
};

// D-Bus method handler for "Open" calls from other instances
static int HandleOpenMethod(sd_bus_message* msg, void* userdata, sd_bus_error* /*error*/) {
    auto* impl = static_cast<SingleInstanceImpl*>(userdata);

    // Read the array of strings (file paths)
    std::vector<std::string> files;

    int r = sd_bus_message_enter_container(msg, 'a', "s");
    if (r < 0) {
        Debug::Log("SingleInstance: Failed to read Open message");
        return sd_bus_reply_method_return(msg, "b", false);
    }

    const char* path = nullptr;
    while ((r = sd_bus_message_read(msg, "s", &path)) > 0) {
        files.emplace_back(path);
    }
    sd_bus_message_exit_container(msg);

    if (!files.empty()) {
        Debug::Log("SingleInstance: Received " + std::to_string(files.size()) +
                   " file(s) from another instance");
        if (impl->file_callback) {
            impl->file_callback(files);
        }
    } else {
        Debug::Log("SingleInstance: Received activation (no files)");
        // Still call with empty list so the app can raise its window
        if (impl->file_callback) {
            impl->file_callback(files);
        }
    }

    return sd_bus_reply_method_return(msg, "b", true);
}

// D-Bus vtable: expose the "Open" method
static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Open", "as", "b", HandleOpenMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

namespace qcview {

SingleInstance::SingleInstance() : impl_(new SingleInstanceImpl) {}

SingleInstance::~SingleInstance() {
    if (impl_) {
        if (impl_->slot) {
            sd_bus_slot_unref(impl_->slot);
        }
        if (impl_->bus) {
            sd_bus_unref(impl_->bus);
        }
        delete impl_;
    }
}

bool SingleInstance::TryAcquire(const std::vector<std::string>& files_to_send) {
    int r = sd_bus_open_user(&impl_->bus);
    if (r < 0) {
        Debug::Log("SingleInstance: Failed to connect to session bus: " + std::string(strerror(-r)));
        // Can't use D-Bus, just proceed as primary
        impl_->is_primary = true;
        return true;
    }

    // Try to request the bus name
    r = sd_bus_request_name(impl_->bus, BUS_NAME, SD_BUS_NAME_ALLOW_REPLACEMENT);
    if (r < 0) {
        if (r == -EEXIST) {
            // Another instance owns the name — send files to it
            Debug::Log("SingleInstance: Another instance is running, sending files");

            sd_bus_message* msg = nullptr;
            r = sd_bus_message_new_method_call(impl_->bus, &msg,
                                                BUS_NAME, OBJECT_PATH,
                                                INTERFACE_NAME, "Open");
            if (r >= 0) {
                r = sd_bus_message_open_container(msg, 'a', "s");
                if (r >= 0) {
                    for (const auto& file : files_to_send) {
                        sd_bus_message_append(msg, "s", file.c_str());
                    }
                    sd_bus_message_close_container(msg);
                }

                sd_bus_error error = SD_BUS_ERROR_NULL;
                sd_bus_message* reply = nullptr;
                r = sd_bus_call(impl_->bus, msg, 2000000, &error, &reply); // 2 second timeout
                if (r < 0) {
                    Debug::Log("SingleInstance: Failed to send files: " + std::string(error.message ? error.message : strerror(-r)));
                } else {
                    Debug::Log("SingleInstance: Files sent to existing instance");
                }
                sd_bus_error_free(&error);
                sd_bus_message_unref(reply);
                sd_bus_message_unref(msg);
            }

            return false; // Not the primary instance
        }

        Debug::Log("SingleInstance: Failed to request bus name: " + std::string(strerror(-r)));
        // Proceed anyway
        impl_->is_primary = true;
        return true;
    }

    // We acquired the name — register our object
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->slot,
                                  OBJECT_PATH, INTERFACE_NAME,
                                  vtable, impl_);
    if (r < 0) {
        Debug::Log("SingleInstance: Failed to register D-Bus object: " + std::string(strerror(-r)));
    }

    impl_->is_primary = true;
    Debug::Log("SingleInstance: Primary instance registered on D-Bus");
    return true;
}

void SingleInstance::SetFileCallback(FileCallback callback) {
    impl_->file_callback = std::move(callback);
}

void SingleInstance::Poll() {
    if (!impl_->bus || !impl_->is_primary) return;

    // Process any pending D-Bus messages (non-blocking)
    for (;;) {
        int r = sd_bus_process(impl_->bus, nullptr);
        if (r < 0) {
            Debug::Log("SingleInstance: D-Bus process error: " + std::string(strerror(-r)));
            break;
        }
        if (r == 0) break; // No more messages to process
    }
}

} // namespace qcview

#endif // QCVIEW_HAS_SDBUS
#endif // __linux__
