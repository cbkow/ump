#pragma once

#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>  // For QueryVideoMemoryInfo (Windows 10+)
#endif

namespace ump {

    struct SystemPressureStatus {
        // RAM metrics
        float ram_usage_percent = 0.0f;
        size_t ram_total_mb = 0;
        size_t ram_used_mb = 0;
        size_t ram_available_mb = 0;

        // CPU metrics
        float cpu_usage_percent = 0.0f;

        // GPU metrics (optional - may not be reliable on all systems)
        float gpu_usage_percent = 0.0f;  // Best-effort estimate
        size_t vram_used_mb = 0;         // DXGI adapter memory usage
        size_t vram_total_mb = 0;        // Total VRAM available

        // Pressure levels
        enum Level {
            OK,       // < 80% RAM, < 85% CPU
            WARNING,  // 80-92% RAM or 85%+ CPU (informational)
            CRITICAL  // > 92% RAM only (triggers emergency mode)
        };
        Level pressure_level = OK;

        // Timestamp
        std::chrono::steady_clock::time_point last_update;
    };

    class SystemPressureMonitor {
    public:
        SystemPressureMonitor();
        ~SystemPressureMonitor();

        // Lifecycle
        void Start();  // Starts background thread
        void Stop();   // Stops background thread gracefully

        // Non-blocking status reads (atomic)
        SystemPressureStatus GetStatus() const;
        bool IsRAMCritical() const;
        bool IsSystemUnderPressure() const;

        // Configuration
        void SetRAMWarningThreshold(float percent);   // Default: 80%
        void SetRAMCriticalThreshold(float percent);  // Default: 90%
        void SetCPUWarningThreshold(float percent);   // Default: 85%
        void SetPollInterval(double seconds);         // Default: 3.0s

    private:
        // Background monitoring thread
        std::thread monitor_thread_;
        std::atomic<bool> should_stop_{false};
        std::atomic<bool> is_running_{false};

        // Lock-free status (atomic updates)
        std::atomic<float> ram_usage_{0.0f};
        std::atomic<float> cpu_usage_{0.0f};
        std::atomic<float> gpu_usage_{0.0f};
        std::atomic<bool> ram_critical_{false};
        std::atomic<bool> system_pressure_{false};

        // Cached status (for detailed reads)
        mutable std::mutex status_mutex_;
        SystemPressureStatus cached_status_;

        // Configuration
        float ram_warning_threshold_ = 0.80f;
        float ram_critical_threshold_ = 0.92f;
        float cpu_warning_threshold_ = 0.85f;
        double poll_interval_ = 3.0;

        // Worker thread
        void MonitorWorker();
        void UpdateRAMMetrics();
        void UpdateCPUMetrics();
        void UpdateGPUMetrics();  // Optional/best-effort
        void DeterminePressureLevel();

#ifdef _WIN32
        // CPU tracking for delta calculation
        ULONGLONG last_idle_time_ = 0;
        ULONGLONG last_kernel_time_ = 0;
        ULONGLONG last_user_time_ = 0;

        // DXGI for GPU/VRAM monitoring
        IDXGIFactory* dxgi_factory_ = nullptr;
        IDXGIAdapter* dxgi_adapter_ = nullptr;
        IDXGIAdapter3* dxgi_adapter3_ = nullptr;  // For QueryVideoMemoryInfo
#endif
    };

} // namespace ump
