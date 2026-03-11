// QCView helper: expose GLFW's internal Wayland serial for clipboard operations.
// This file is compiled as part of GLFW and has access to its internal structures.

#include "internal.h"

#ifdef _GLFW_WAYLAND

uint32_t _glfwGetWaylandSerial(void) {
    return _glfw.wl.serial;
}

struct wl_data_device_manager* _glfwGetWaylandDataDeviceManager(void) {
    return _glfw.wl.dataDeviceManager;
}

struct wl_seat* _glfwGetWaylandSeat(void) {
    return _glfw.wl.seat;
}

struct wl_data_device* _glfwGetWaylandDataDevice(void) {
    return _glfw.wl.dataDevice;
}

#else

#include <stdint.h>

uint32_t _glfwGetWaylandSerial(void) { return 0; }
void* _glfwGetWaylandDataDeviceManager(void) { return 0; }
void* _glfwGetWaylandSeat(void) { return 0; }
void* _glfwGetWaylandDataDevice(void) { return 0; }

#endif
