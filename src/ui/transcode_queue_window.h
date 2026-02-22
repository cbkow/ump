#pragma once

#include <string>
#include <vector>
#include <memory>
#include <imgui.h>
#include "../transcode/transcode_queue.h"
#include "../transcode/transcode_worker_pool.h"

namespace qcview {

/**
 * TranscodeQueueWindow
 *
 * ImGui window for managing the transcode queue.
 *
 * Features:
 * - Worker status indicators
 * - Queue table with sorting/filtering
 * - Job details panel
 * - Progress bars & ETA
 * - Context menus (pause/cancel/reorder)
 * - Keyboard shortcuts
 */
class TranscodeQueueWindow {
public:
    TranscodeQueueWindow(TranscodeQueue* queue, TranscodeWorkerPool* worker_pool);
    ~TranscodeQueueWindow();

    // Window lifecycle
    void Show() { is_open_ = true; }
    void Hide() { is_open_ = false; }
    void Toggle() { is_open_ = !is_open_; }
    bool IsOpen() const { return is_open_; }

    // Switch to Settings tab directly
    void ShowSettings() { current_tab_ = 1; }  // Switch to Settings tab

    // Render (call every frame)
    void Render();

    // Keyboard shortcuts (call from main input handler)
    void ProcessKeyboard();

private:
    // UI Sections
    void RenderToolbar();
    void RenderWorkerStatusBar();
    void RenderQueueTable();
    void RenderJobDetailsPanel();
    void RenderContextMenu();
    void RenderQueueTab();      // Queue view (existing content)
    void RenderSettingsTab();    // Settings view (was modal dialog)

    // Helper rendering
    void RenderStatusIndicator(const std::string& label, const char* status, float r, float g, float b);
    void RenderProgressBar(float progress, const std::string& text = "", const ImVec4* color = nullptr);
    void RenderWorkerBox(const TranscodeWorkerPool::WorkerInfo& worker);
    void RenderJobRow(TranscodeJob* job, int row_index);

    // Job operations
    void StartSelectedJob();
    void PauseSelectedJob();
    void ResumeSelectedJob();
    void CancelSelectedJob();
    void RemoveSelectedJob();
    void MoveSelectedJobUp();
    void MoveSelectedJobDown();
    void MoveSelectedJobToTop();
    void SetSelectedJobPriority(TranscodeJob::Priority priority);
    void OpenOutputFolder();

    // Filtering/Sorting
    void ApplyFilters();
    void SortJobs();
    std::string GetPriorityString(TranscodeJob::Priority priority) const;
    std::string FormatTime(double seconds) const;
    std::string FormatSpeed(double fps) const;

    // State
    TranscodeQueue* queue_;  // Not owned
    TranscodeWorkerPool* worker_pool_;  // Not owned

    bool is_open_ = false;

    // Selection
    std::string selected_job_id_;

    // Filters
    bool filter_show_queued_ = true;
    bool filter_show_encoding_ = true;
    bool filter_show_paused_ = true;
    bool filter_show_completed_ = true;
    bool filter_show_failed_ = true;
    bool filter_show_cancelled_ = false;
    char search_buffer_[256] = "";

    // Sorting
    enum class SortColumn {
        NONE,
        PRIORITY,
        NAME,
        STATUS,
        PROGRESS
    };
    SortColumn sort_column_ = SortColumn::PRIORITY;
    bool sort_ascending_ = false;

    // Details panel
    enum class DetailsTab {
        INFO,
        PROGRESS
    };
    DetailsTab details_tab_ = DetailsTab::INFO;

    // Cached filtered jobs (rebuilt each frame)
    std::vector<TranscodeJob*> filtered_jobs_;

    // UI State
    float toolbar_height_ = 60.0f;
    // worker_bar_height_ removed - worker section now auto-sizes to content
    float details_panel_height_ = 300.0f;

    // Tab state (-1 = user-controlled, 0 = force Queue, 1 = force Settings)
    int current_tab_ = -1;
};

} // namespace qcview
