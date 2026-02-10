#include "system_pressure_monitor.h"
#include "debug_utils.h"
#include <algorithm>

namespace ump {

SystemPressureMonitor::SystemPressureMonitor() {
#ifdef _WIN32
    // Initialize DXGI for GPU/VRAM monitoring
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&dxgi_factory_);
    if (SUCCEEDED(hr) && dxgi_factory_) {
        // Get primary adapter (GPU 0)
        hr = dxgi_factory_->EnumAdapters(0, &dxgi_adapter_);
        if (SUCCEEDED(hr) && dxgi_adapter_) {
            // Try to get IDXGIAdapter3 for QueryVideoMemoryInfo (Windows 10+)
            hr = dxgi_adapter_->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&dxgi_adapter3_);
            if (FAILED(hr)) {
                Debug::Log("SystemPressureMonitor: IDXGIAdapter3 not available (Windows 10+ required for VRAM usage)");
            }
        } else {
            Debug::Log("SystemPressureMonitor: Failed to enumerate DXGI adapter (VRAM monitoring unavailable)");
        }
    } else {
        Debug::Log("SystemPressureMonitor: Failed to create DXGI factory (VRAM monitoring unavailable)");
    }
#endif
    Debug::Log("SystemPressureMonitor: Initialized (not started)");
}

SystemPressureMonitor::~SystemPressureMonitor() {
    Stop();

#ifdef _WIN32
    // Release DXGI resources
    if (dxgi_adapter3_) {
        dxgi_adapter3_->Release();
        dxgi_adapter3_ = nullptr;
    }
    if (dxgi_adapter_) {
        dxgi_adapter_->Release();
        dxgi_adapter_ = nullptr;
    }
    if (dxgi_factory_) {
        dxgi_factory_->Release();
        dxgi_factory_ = nullptr;
    }
#endif
}

void SystemPressureMonitor::Start() {
    if (is_running_.load()) {
        Debug::Log("SystemPressureMonitor: Already running");
        return;
    }

    should_stop_.store(false);
    monitor_thread_ = std::thread(&SystemPressureMonitor::MonitorWorker, this);

#ifdef _WIN32
    // Set thread priority to BELOW_NORMAL (yields to render threads)
    SetThreadPriority(monitor_thread_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    is_running_.store(true);
    Debug::Log("SystemPressureMonitor: Background thread started (BELOW_NORMAL priority)");
}

void SystemPressureMonitor::Stop() {
    if (!is_running_.load()) {
        return;
    }

    should_stop_.store(true);
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    is_running_.store(false);
    Debug::Log("SystemPressureMonitor: Background thread stopped");
}

void SystemPressureMonitor::MonitorWorker() {
    Debug::Log("SystemPressureMonitor: Worker thread running");

    while (!should_stop_.load()) {
        auto start = std::chrono::steady_clock::now();

        // Update metrics (all Windows APIs are fast: <2ms total)
        UpdateRAMMetrics();
        UpdateCPUMetrics();
        UpdateGPUMetrics();  // Optional/best-effort
        DeterminePressureLevel();

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();

        // Log if monitoring is too slow (should be <5ms)
        if (elapsed_ms > 5.0) {
            Debug::Log("SystemPressureMonitor: Slow poll detected (" +
                      std::to_string(elapsed_ms) + "ms)");
        }

        // Sleep until next poll (interruptible for clean shutdown)
        auto sleep_duration = std::chrono::milliseconds(static_cast<int>(poll_interval_ * 1000));
        auto sleep_start = std::chrono::steady_clock::now();

        while (!should_stop_.load()) {
            auto sleep_elapsed = std::chrono::steady_clock::now() - sleep_start;
            if (sleep_elapsed >= sleep_duration) break;

            // Check every 100ms for shutdown signal
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    Debug::Log("SystemPressureMonitor: Worker thread exiting");
}

void SystemPressureMonitor::UpdateRAMMetrics() {
#ifdef _WIN32
    MEMORYSTATUSEX mem_info;
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);

    if (GlobalMemoryStatusEx(&mem_info)) {
        size_t total_mb = mem_info.ullTotalPhys / (1024 * 1024);
        size_t available_mb = mem_info.ullAvailPhys / (1024 * 1024);

        // Use Windows' own dwMemoryLoad percentage (accounts for standby/cache properly)
        // This is better than manual calculation because ullAvailPhys includes standby memory
        // that can be reclaimed instantly, so our old calculation was too aggressive
        float usage_percent = static_cast<float>(mem_info.dwMemoryLoad) / 100.0f;

        // Calculate used_mb from the percentage (not from total - available)
        // This ensures consistency between percentage and MB values
        size_t used_mb = static_cast<size_t>(total_mb * usage_percent);

        // Atomic update (lock-free)
        ram_usage_.store(usage_percent, std::memory_order_relaxed);

        // Update cached status (with lock for detailed info)
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_.ram_total_mb = total_mb;
            cached_status_.ram_used_mb = used_mb;
            cached_status_.ram_available_mb = available_mb;
            cached_status_.ram_usage_percent = usage_percent;
            cached_status_.last_update = std::chrono::steady_clock::now();
        }
    }
#endif
}

void SystemPressureMonitor::UpdateCPUMetrics() {
#ifdef _WIN32
    FILETIME idle_time, kernel_time, user_time;

    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        auto FileTimeToULongLong = [](const FILETIME& ft) -> ULONGLONG {
            ULARGE_INTEGER ul;
            ul.LowPart = ft.dwLowDateTime;
            ul.HighPart = ft.dwHighDateTime;
            return ul.QuadPart;
        };

        ULONGLONG idle = FileTimeToULongLong(idle_time);
        ULONGLONG kernel = FileTimeToULongLong(kernel_time);
        ULONGLONG user = FileTimeToULongLong(user_time);

        if (last_idle_time_ != 0) {  // Skip first calculation (no baseline)
            ULONGLONG idle_delta = idle - last_idle_time_;
            ULONGLONG kernel_delta = kernel - last_kernel_time_;
            ULONGLONG user_delta = user - last_user_time_;
            ULONGLONG total_delta = kernel_delta + user_delta;

            if (total_delta > 0) {
                ULONGLONG busy_delta = total_delta - idle_delta;
                float cpu_usage = static_cast<float>(busy_delta) / total_delta;

                // Clamp to 0-1 range
                cpu_usage = std::clamp(cpu_usage, 0.0f, 1.0f);

                cpu_usage_.store(cpu_usage, std::memory_order_relaxed);

                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    cached_status_.cpu_usage_percent = cpu_usage;
                }
            }
        }

        last_idle_time_ = idle;
        last_kernel_time_ = kernel;
        last_user_time_ = user;
    }
#endif
}

void SystemPressureMonitor::UpdateGPUMetrics() {
#ifdef _WIN32
    if (!dxgi_adapter_) {
        // DXGI not available
        std::lock_guard<std::mutex> lock(status_mutex_);
        cached_status_.gpu_usage_percent = 0.0f;
        cached_status_.vram_used_mb = 0;
        cached_status_.vram_total_mb = 0;
        return;
    }

    // Get total VRAM from adapter description
    DXGI_ADAPTER_DESC desc;
    HRESULT hr = dxgi_adapter_->GetDesc(&desc);
    size_t vram_total = 0;

    if (SUCCEEDED(hr)) {
        vram_total = desc.DedicatedVideoMemory / (1024 * 1024); // Convert to MB
    }

    // Get current VRAM usage from DXGI 1.4 (Windows 10+)
    size_t vram_used = 0;
    if (dxgi_adapter3_) {
        DXGI_QUERY_VIDEO_MEMORY_INFO mem_info;
        hr = dxgi_adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem_info);

        if (SUCCEEDED(hr)) {
            // CurrentUsage is current VRAM usage in bytes
            vram_used = mem_info.CurrentUsage / (1024 * 1024); // Convert to MB
        }
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cached_status_.vram_total_mb = vram_total;
        cached_status_.vram_used_mb = vram_used;
        cached_status_.gpu_usage_percent = 0.0f; // GPU usage % still unavailable without vendor APIs
    }
#else
    std::lock_guard<std::mutex> lock(status_mutex_);
    cached_status_.gpu_usage_percent = 0.0f;
    cached_status_.vram_used_mb = 0;
    cached_status_.vram_total_mb = 0;
#endif
}

void SystemPressureMonitor::DeterminePressureLevel() {
    float ram = ram_usage_.load(std::memory_order_relaxed);
    float cpu = cpu_usage_.load(std::memory_order_relaxed);

    SystemPressureStatus::Level level = SystemPressureStatus::OK;

    // Critical: RAM > 92% only (cache eviction doesn't help CPU pressure)
    // Note: EXR decoding is CPU-intensive and will spike CPU regardless of caching
    if (ram >= ram_critical_threshold_) {
        level = SystemPressureStatus::CRITICAL;
        ram_critical_.store(true, std::memory_order_relaxed);
        system_pressure_.store(true, std::memory_order_relaxed);
    }
    // Warning: RAM > 80% OR CPU > 85% (informational only for CPU)
    else if (ram >= ram_warning_threshold_ || cpu >= cpu_warning_threshold_) {
        level = SystemPressureStatus::WARNING;
        ram_critical_.store(false, std::memory_order_relaxed);
        system_pressure_.store(true, std::memory_order_relaxed);
    }
    // OK
    else {
        level = SystemPressureStatus::OK;
        ram_critical_.store(false, std::memory_order_relaxed);
        system_pressure_.store(false, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        cached_status_.pressure_level = level;
    }
}

SystemPressureStatus SystemPressureMonitor::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return cached_status_;
}

bool SystemPressureMonitor::IsRAMCritical() const {
    return ram_critical_.load(std::memory_order_relaxed);
}

bool SystemPressureMonitor::IsSystemUnderPressure() const {
    return system_pressure_.load(std::memory_order_relaxed);
}

void SystemPressureMonitor::SetRAMWarningThreshold(float percent) {
    ram_warning_threshold_ = std::clamp(percent, 0.5f, 0.95f);
    Debug::Log("SystemPressureMonitor: RAM warning threshold set to " +
              std::to_string(int(ram_warning_threshold_ * 100)) + "%");
}

void SystemPressureMonitor::SetRAMCriticalThreshold(float percent) {
    ram_critical_threshold_ = std::clamp(percent, 0.7f, 0.99f);
    Debug::Log("SystemPressureMonitor: RAM critical threshold set to " +
              std::to_string(int(ram_critical_threshold_ * 100)) + "%");
}

void SystemPressureMonitor::SetCPUWarningThreshold(float percent) {
    cpu_warning_threshold_ = std::clamp(percent, 0.5f, 0.99f);
    Debug::Log("SystemPressureMonitor: CPU warning threshold set to " +
              std::to_string(int(cpu_warning_threshold_ * 100)) + "%");
}

void SystemPressureMonitor::SetPollInterval(double seconds) {
    poll_interval_ = std::clamp(seconds, 1.0, 10.0);
    Debug::Log("SystemPressureMonitor: Poll interval set to " +
              std::to_string(poll_interval_) + "s");
}

}
