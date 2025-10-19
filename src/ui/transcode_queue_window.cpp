#include "transcode_queue_window.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// Undefine Windows.h macros that conflict with TranscodeQueue methods
#ifdef GetJob
#undef GetJob
#endif

// External variable from main.cpp
extern bool use_windows_accent_color;

// Local color helper functions (duplicated pattern from main.cpp/project_manager.cpp)
static ImVec4 GetFallbackYellowColor() {
    return ImVec4(0.65f, 0.55f, 0.15f, 1.0f);
}

#ifdef _WIN32
static ImVec4 GetWindowsAccentColor() {
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }

    DWORD colorization_color;
    BOOL opaque_blend;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
        float r = ((colorization_color >> 16) & 0xff) / 255.0f;
        float g = ((colorization_color >> 8) & 0xff) / 255.0f;
        float b = (colorization_color & 0xff) / 255.0f;
        return ImVec4(r, g, b, 1.0f);
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue
}
#else
static ImVec4 GetWindowsAccentColor() {
    if (!use_windows_accent_color) {
        return GetFallbackYellowColor();
    }
    return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback for non-Windows
}
#endif

static ImVec4 TintColor(const ImVec4& color, float brightness, float saturation = 1.0f) {
    ImVec4 result = color;

    // Apply brightness
    result.x *= brightness;
    result.y *= brightness;
    result.z *= brightness;

    // Apply saturation
    if (saturation < 1.0f) {
        float gray = result.x * 0.299f + result.y * 0.587f + result.z * 0.114f;
        result.x = gray + (result.x - gray) * saturation;
        result.y = gray + (result.y - gray) * saturation;
        result.z = gray + (result.z - gray) * saturation;
    }

    return result;
}

static ImVec4 MutedDark(const ImVec4& accent) { return TintColor(accent, 0.7f, 0.4f); }
static ImVec4 MutedLight(const ImVec4& accent) { return TintColor(accent, 1.5f, 0.8f); }
static ImVec4 Bright(const ImVec4& accent) { return TintColor(accent, 2.2f, 0.5f); }

namespace ump {

TranscodeQueueWindow::TranscodeQueueWindow(TranscodeQueue* queue, TranscodeWorkerPool* worker_pool)
    : queue_(queue)
    , worker_pool_(worker_pool)
{
    Debug::Log("TranscodeQueueWindow: Created");
}

TranscodeQueueWindow::~TranscodeQueueWindow() {
}

void TranscodeQueueWindow::Render() {
    if (!is_open_) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1300, 850), ImGuiCond_FirstUseEver);

    // Add border and dropshadow styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 1.0f);
    ImVec4 accent = GetWindowsAccentColor();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x * 0.6f, accent.y * 0.6f, accent.z * 0.6f, 0.8f));

    if (!ImGui::Begin("Transcode Queue Manager", &is_open_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::End();
        return;
    }

    // Get current stats
    auto stats = queue_->GetStats();
    auto worker_info = worker_pool_->GetWorkerInfo();

    // Render sections
    RenderToolbar();
    ImGui::Separator();
    RenderWorkerStatusBar();
    ImGui::Separator();

    // Main content area (table + details)
    // Calculate exact remaining space for table
    float available_height = ImGui::GetContentRegionAvail().y;
    float separator_height = ImGui::GetStyle().ItemSpacing.y;
    float table_height = available_height - details_panel_height_ - separator_height;

    ImGui::BeginChild("QueueTableRegion", ImVec2(0, table_height), true);
    RenderQueueTable();
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("DetailsPanel", ImVec2(0, 0), true);  // Use 0 to fill remaining space
    RenderJobDetailsPanel();
    ImGui::EndChild();

    ImGui::End();

    // Pop border and shadow styling
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void TranscodeQueueWindow::RenderToolbar() {
    ImGui::BeginChild("Toolbar", ImVec2(0, toolbar_height_), false);

    auto stats = queue_->GetStats();
    auto pool_status = worker_pool_->GetStatus();
    ImVec4 accent = GetWindowsAccentColor();

    // Status indicator
    if (pool_status == TranscodeWorkerPool::Status::ACTIVE && stats.encoding > 0) {
        ImVec4 active_color = Bright(accent);
        RenderStatusIndicator("QUEUE STATUS", "ACTIVE", active_color.x, active_color.y, active_color.z);
    } else if (pool_status == TranscodeWorkerPool::Status::PAUSED) {
        ImVec4 paused_color = MutedLight(accent);
        RenderStatusIndicator("QUEUE STATUS", "PAUSED", paused_color.x, paused_color.y, paused_color.z);
    } else if (stats.queued > 0) {
        RenderStatusIndicator("QUEUE STATUS", "READY", accent.x, accent.y, accent.z);
    } else {
        RenderStatusIndicator("QUEUE STATUS", "IDLE", 0.5f, 0.5f, 0.5f);
    }

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // Stats display
    ImGui::BeginGroup();
    ImGui::Text("Queue Statistics:");
    ImGui::Text("Queued: %d  |  Encoding: %d  |  Completed: %d  |  Failed: %d",
                stats.queued, stats.encoding, stats.completed, stats.failed);
    ImGui::EndGroup();

    ImGui::SameLine(ImGui::GetWindowWidth() - 570);

    // Control buttons
    ImGui::BeginGroup();
    if (pool_status == TranscodeWorkerPool::Status::ACTIVE) {
        if (ImGui::Button("Pause Queue", ImVec2(120, 30))) {
            worker_pool_->Pause();
            Debug::Log("TranscodeQueueWindow: Queue paused");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop All", ImVec2(100, 30))) {
            worker_pool_->Stop();
            Debug::Log("TranscodeQueueWindow: Stopped all workers");
        }
    } else if (pool_status == TranscodeWorkerPool::Status::PAUSED) {
        if (ImGui::Button("Resume Queue", ImVec2(120, 30))) {
            worker_pool_->Resume();
            Debug::Log("TranscodeQueueWindow: Queue resumed");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop All", ImVec2(100, 30))) {
            worker_pool_->Stop();
            Debug::Log("TranscodeQueueWindow: Stopped all workers");
        }
    } else {
        if (ImGui::Button("Start Queue", ImVec2(120, 30))) {
            worker_pool_->Start();
            Debug::Log("TranscodeQueueWindow: Queue started");
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Clear Completed", ImVec2(140, 30))) {
        queue_->ClearCompleted();
        Debug::Log("TranscodeQueueWindow: Cleared completed jobs");
    }

    ImGui::SameLine();

    if (ImGui::Button("Clear Failed", ImVec2(120, 30))) {
        queue_->ClearFailed();
        Debug::Log("TranscodeQueueWindow: Cleared failed jobs");
    }
    ImGui::EndGroup();

    ImGui::EndChild();
}

void TranscodeQueueWindow::RenderWorkerStatusBar() {
    // No child window needed - auto-sizes to content
    auto worker_info = worker_pool_->GetWorkerInfo();
    int worker_count = worker_pool_->GetWorkerCount();

    ImGui::Text("Workers (%d active):", worker_count);

    ImGui::Spacing();

    // Render worker boxes (wider now)
    for (int i = 0; i < worker_count; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        RenderWorkerBox(worker_info[i]);
    }

    ImGui::Spacing();

    // Worker count slider
    ImGui::Text("Worker Count:");
    ImGui::SameLine();
    int new_count = worker_count;
    if (ImGui::SliderInt("##WorkerCount", &new_count, 1, 8)) {
        worker_pool_->SetWorkerCount(new_count);
        Debug::Log("TranscodeQueueWindow: Worker count set to " + std::to_string(new_count));
    }
}

void TranscodeQueueWindow::RenderQueueTable() {
    // Filter controls
    ImGui::Text("Filters:");
    ImGui::SameLine();
    ImGui::Checkbox("Queued", &filter_show_queued_);
    ImGui::SameLine();
    ImGui::Checkbox("Encoding", &filter_show_encoding_);
    ImGui::SameLine();
    ImGui::Checkbox("Paused", &filter_show_paused_);
    ImGui::SameLine();
    ImGui::Checkbox("Completed", &filter_show_completed_);
    ImGui::SameLine();
    ImGui::Checkbox("Failed", &filter_show_failed_);
    ImGui::SameLine();
    ImGui::Checkbox("Cancelled", &filter_show_cancelled_);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##Search", search_buffer_, sizeof(search_buffer_));
    ImGui::SameLine();
    ImGui::Text("Search");

    ImGui::Spacing();
    ImGui::Separator();

    // Build filtered job list
    ApplyFilters();

    // Table
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable;

    if (ImGui::BeginTable("JobsTable", 7, flags)) {
        // Headers
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Rows
        for (size_t i = 0; i < filtered_jobs_.size(); ++i) {
            RenderJobRow(filtered_jobs_[i], static_cast<int>(i));
        }

        ImGui::EndTable();
    }
}

void TranscodeQueueWindow::RenderJobRow(TranscodeJob* job, int row_index) {
    // Push unique ID for entire row to avoid conflicts with duplicate priority strings
    ImGui::PushID(job->GetJobID().c_str());

    ImGui::TableNextRow();

    bool is_selected = (job->GetJobID() == selected_job_id_);
    ImVec4 accent = GetWindowsAccentColor();
    ImVec4 muted_dark = MutedDark(accent);
    ImVec4 bright = Bright(accent);

    // Get row color based on status (using system accent colors)
    ImVec4 row_color;
    switch (job->GetStatus()) {
        case TranscodeJob::Status::QUEUED:
            row_color = ImVec4(muted_dark.x, muted_dark.y, muted_dark.z, 0.3f);
            break;
        case TranscodeJob::Status::ENCODING:
            row_color = ImVec4(bright.x * 0.6f, bright.y * 0.6f, bright.z * 0.6f, 0.3f);
            break;
        case TranscodeJob::Status::PAUSED:
            row_color = ImVec4(muted_dark.x * 1.2f, muted_dark.y * 1.2f, muted_dark.z * 1.2f, 0.3f);
            break;
        case TranscodeJob::Status::COMPLETED:
            row_color = ImVec4(bright.x * 0.5f, bright.y * 0.5f, bright.z * 0.5f, 0.3f);
            break;
        case TranscodeJob::Status::FAILED:
            row_color = ImVec4(0.6f, 0.2f, 0.2f, 0.3f);
            break;
        case TranscodeJob::Status::CANCELLED:
            row_color = ImVec4(0.4f, 0.4f, 0.4f, 0.3f);
            break;
    }

    // Priority column
    ImGui::TableSetColumnIndex(0);
    if (is_selected) {
        ImVec4 selected_color = ImVec4(accent.x * 0.5f, accent.y * 0.5f, accent.z * 0.5f, 0.5f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(selected_color));
    } else {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(row_color));
    }

    std::string priority_str = GetPriorityString(job->GetPriority());
    if (ImGui::Selectable(priority_str.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
        selected_job_id_ = job->GetJobID();
    }

    // Name column
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", job->GetJobName().c_str());

    // Status column
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%s", job->GetStatusString().c_str());

    // Progress column
    ImGui::TableSetColumnIndex(3);
    auto progress = job->GetProgress();
    float progress_frac = progress.progress_percent / 100.0f;
    std::string progress_text = std::to_string(static_cast<int>(progress.progress_percent)) + "%";
    if (progress.current_frame > 0) {
        progress_text += " (" + std::to_string(progress.current_frame) + "/" +
                        std::to_string(progress.total_frames) + ")";
    }
    RenderProgressBar(progress_frac, progress_text);

    // Speed column
    ImGui::TableSetColumnIndex(4);
    if (progress.encoding_fps > 0.0) {
        ImGui::Text("%s", FormatSpeed(progress.encoding_fps).c_str());
    } else {
        ImGui::Text("-");
    }

    // ETA column
    ImGui::TableSetColumnIndex(5);
    if (progress.estimated_remaining_seconds > 0.0) {
        ImGui::Text("%s", FormatTime(progress.estimated_remaining_seconds).c_str());
    } else {
        ImGui::Text("-");
    }

    // Actions column
    ImGui::TableSetColumnIndex(6);
    // Use job ID for ImGui ID instead of row index to avoid conflicts
    ImGui::PushID(job->GetJobID().c_str());

    auto status = job->GetStatus();
    if (status == TranscodeJob::Status::QUEUED || status == TranscodeJob::Status::PAUSED) {
        if (ImGui::SmallButton("Cancel")) {
            queue_->CancelJob(job->GetJobID());
        }
    } else if (status == TranscodeJob::Status::ENCODING) {
        if (ImGui::SmallButton("Pause")) {
            queue_->PauseJob(job->GetJobID());
        }
    } else if (status == TranscodeJob::Status::COMPLETED ||
               status == TranscodeJob::Status::FAILED ||
               status == TranscodeJob::Status::CANCELLED) {
        if (ImGui::SmallButton("Remove")) {
            queue_->RemoveJob(job->GetJobID());
            if (selected_job_id_ == job->GetJobID()) {
                selected_job_id_.clear();
            }
        }
    }

    ImGui::PopID();

    // Right-click context menu (use unique ID based on job ID)
    std::string context_menu_id = "JobContextMenu_" + job->GetJobID();
    if (ImGui::BeginPopupContextItem(context_menu_id.c_str())) {
        selected_job_id_ = job->GetJobID();

        if (ImGui::MenuItem("Move to Top")) MoveSelectedJobToTop();
        if (ImGui::MenuItem("Move Up")) MoveSelectedJobUp();
        if (ImGui::MenuItem("Move Down")) MoveSelectedJobDown();

        ImGui::Separator();

        if (ImGui::BeginMenu("Set Priority")) {
            if (ImGui::MenuItem("Urgent")) SetSelectedJobPriority(TranscodeJob::Priority::URGENT);
            if (ImGui::MenuItem("High")) SetSelectedJobPriority(TranscodeJob::Priority::HIGH);
            if (ImGui::MenuItem("Normal")) SetSelectedJobPriority(TranscodeJob::Priority::NORMAL);
            if (ImGui::MenuItem("Low")) SetSelectedJobPriority(TranscodeJob::Priority::LOW);
            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (status == TranscodeJob::Status::ENCODING && ImGui::MenuItem("Pause")) {
            PauseSelectedJob();
        }
        if (status == TranscodeJob::Status::PAUSED && ImGui::MenuItem("Resume")) {
            ResumeSelectedJob();
        }
        if ((status == TranscodeJob::Status::QUEUED || status == TranscodeJob::Status::PAUSED) &&
            ImGui::MenuItem("Cancel")) {
            CancelSelectedJob();
        }
        if (ImGui::MenuItem("Remove")) {
            RemoveSelectedJob();
        }

        ImGui::Separator();

        if (status == TranscodeJob::Status::COMPLETED && ImGui::MenuItem("Open Output Folder")) {
            OpenOutputFolder();
        }

        ImGui::EndPopup();
    }

    // Pop the row-level ID scope
    ImGui::PopID();
}

void TranscodeQueueWindow::RenderJobDetailsPanel() {
    if (selected_job_id_.empty()) {
        ImGui::TextDisabled("No job selected");
        return;
    }

    auto job = queue_->GetJob(selected_job_id_);
    if (!job) {
        ImGui::TextDisabled("Selected job not found");
        selected_job_id_.clear();
        return;
    }

    // Tab bar
    if (ImGui::BeginTabBar("DetailsTabs")) {
        if (ImGui::BeginTabItem("Info")) {
            details_tab_ = DetailsTab::INFO;

            ImGui::Text("Job Name: %s", job->GetJobName().c_str());
            ImGui::Text("Job ID: %s", job->GetJobID().c_str());
            ImGui::Text("Priority: %s", GetPriorityString(job->GetPriority()).c_str());
            ImGui::Text("Status: %s", job->GetStatusString().c_str());

            ImGui::Separator();

            ImGui::Text("Input Files: %zu", job->GetConfig().transcode_config.input_files.size());
            if (!job->GetConfig().transcode_config.input_files.empty()) {
                ImGui::Text("  First: %s", job->GetConfig().transcode_config.input_files.front().c_str());
                ImGui::Text("  Last: %s", job->GetConfig().transcode_config.input_files.back().c_str());
            }

            ImGui::Separator();

            // Output path with right-click context menu
            ImGui::Text("Output:");
            ImGui::SameLine();
            std::string output_label = job->GetConfig().transcode_config.output_path;
            ImGui::Selectable(output_label.c_str(), false, ImGuiSelectableFlags_DontClosePopups);

            // Context menu for output path
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
            if (ImGui::BeginPopupContextItem("OutputPathContextMenu")) {
                if (ImGui::MenuItem("Show in Explorer")) {
                    OpenOutputFolder();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor();

            ImGui::Text("Codec: %s", job->GetConfig().transcode_config.encoder_settings.codec.c_str());
            ImGui::Text("Quality: CRF %d", job->GetConfig().transcode_config.encoder_settings.crf);
            ImGui::Text("Preset: %s", job->GetConfig().transcode_config.encoder_settings.preset.c_str());

            ImGui::Separator();

            ImGui::Text("OCIO Color Space: %s", job->GetConfig().transcode_config.src_colorspace.c_str());
            ImGui::Text("Display: %s", job->GetConfig().transcode_config.display.c_str());
            ImGui::Text("View: %s", job->GetConfig().transcode_config.view.c_str());
            if (!job->GetConfig().transcode_config.looks.empty()) {
                ImGui::Text("Looks: %s", job->GetConfig().transcode_config.looks.c_str());
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Progress")) {
            details_tab_ = DetailsTab::PROGRESS;

            auto progress = job->GetProgress();

            ImGui::Text("Current Status: %s", progress.current_status_text.c_str());
            ImGui::Separator();

            ImGui::Text("Progress: %.1f%%", progress.progress_percent);
            RenderProgressBar(progress.progress_percent / 100.0f);

            ImGui::Text("Current Frame: %d / %d", progress.current_frame, progress.total_frames);
            ImGui::Text("Encoding Speed: %s", FormatSpeed(progress.encoding_fps).c_str());

            ImGui::Separator();

            ImGui::Text("Elapsed Time: %s", FormatTime(progress.elapsed_seconds).c_str());
            ImGui::Text("Estimated Remaining: %s", FormatTime(progress.estimated_remaining_seconds).c_str());

            if (progress.is_error) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error:");
                ImGui::TextWrapped("%s", progress.error_message.c_str());
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void TranscodeQueueWindow::RenderContextMenu() {
    // Not used - context menu handled per-row
}

// ============================================================================
// Helper Rendering
// ============================================================================

void TranscodeQueueWindow::RenderStatusIndicator(const std::string& label, const char* status,
                                                 float r, float g, float b) {
    ImGui::BeginGroup();
    ImGui::Text("%s", label.c_str());

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 8.0f;
    draw_list->AddCircleFilled(ImVec2(pos.x + radius, pos.y + radius), radius,
                              ImGui::GetColorU32(ImVec4(r, g, b, 1.0f)));

    ImGui::SetCursorScreenPos(ImVec2(pos.x + radius * 2 + 5, pos.y));
    ImGui::Text("%s", status);
    ImGui::EndGroup();
}

void TranscodeQueueWindow::RenderProgressBar(float progress, const std::string& text) {
    char overlay_text[64];
    if (text.empty()) {
        snprintf(overlay_text, sizeof(overlay_text), "%.0f%%", progress * 100.0f);
    } else {
        snprintf(overlay_text, sizeof(overlay_text), "%s", text.c_str());
    }

    // Use system accent color for progress bar
    ImVec4 accent = GetWindowsAccentColor();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
    ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay_text);
    ImGui::PopStyleColor();
}

void TranscodeQueueWindow::RenderWorkerBox(const TranscodeWorkerPool::WorkerInfo& worker) {
    ImGui::BeginGroup();

    ImVec4 accent = GetWindowsAccentColor();
    ImVec4 muted_dark = MutedDark(accent);

    // Worker header (using accent colors)
    ImVec4 header_color = worker.is_active ?
        ImVec4(accent.x * 0.7f, accent.y * 0.7f, accent.z * 0.7f, 1.0f) :
        muted_dark;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, header_color);
    ImGui::BeginChild(("Worker" + std::to_string(worker.worker_id)).c_str(),
                     ImVec2(240, 70), true);  // Wider and taller

    ImGui::Text("Worker %d", worker.worker_id);

    if (worker.is_active) {
        ImGui::TextWrapped("%s", worker.current_job_name.c_str());
        ImGui::Text("%.0f%% (%.1f fps)", worker.progress_percent, worker.encoding_fps);
    } else {
        ImGui::TextDisabled("Idle");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndGroup();

    // Tooltip
    if (ImGui::IsItemHovered() && worker.is_active) {
        ImGui::BeginTooltip();
        ImGui::Text("Job: %s", worker.current_job_name.c_str());
        ImGui::Text("Progress: %.1f%%", worker.progress_percent);
        ImGui::Text("Speed: %.1f fps", worker.encoding_fps);
        ImGui::Text("Status: %s", worker.status_text.c_str());
        ImGui::EndTooltip();
    }
}

// ============================================================================
// Job Operations
// ============================================================================

void TranscodeQueueWindow::StartSelectedJob() {
    if (!selected_job_id_.empty()) {
        queue_->StartJob(selected_job_id_);
    }
}

void TranscodeQueueWindow::PauseSelectedJob() {
    if (!selected_job_id_.empty()) {
        queue_->PauseJob(selected_job_id_);
    }
}

void TranscodeQueueWindow::ResumeSelectedJob() {
    if (!selected_job_id_.empty()) {
        queue_->ResumeJob(selected_job_id_);
    }
}

void TranscodeQueueWindow::CancelSelectedJob() {
    if (!selected_job_id_.empty()) {
        queue_->CancelJob(selected_job_id_);
    }
}

void TranscodeQueueWindow::RemoveSelectedJob() {
    if (!selected_job_id_.empty()) {
        queue_->RemoveJob(selected_job_id_);
        selected_job_id_.clear();
    }
}

void TranscodeQueueWindow::MoveSelectedJobUp() {
    if (!selected_job_id_.empty()) {
        queue_->MoveJobUp(selected_job_id_);
    }
}

void TranscodeQueueWindow::MoveSelectedJobDown() {
    if (!selected_job_id_.empty()) {
        queue_->MoveJobDown(selected_job_id_);
    }
}

void TranscodeQueueWindow::MoveSelectedJobToTop() {
    if (!selected_job_id_.empty()) {
        queue_->MoveJobToTop(selected_job_id_);
    }
}

void TranscodeQueueWindow::SetSelectedJobPriority(TranscodeJob::Priority priority) {
    if (!selected_job_id_.empty()) {
        queue_->SetJobPriority(selected_job_id_, priority);
    }
}

void TranscodeQueueWindow::OpenOutputFolder() {
    if (selected_job_id_.empty()) return;

    auto job = queue_->GetJob(selected_job_id_);
    if (!job) return;

    std::string output_path = job->GetOutputPath();

    // Extract directory from output path
    size_t last_slash = output_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        std::string folder = output_path.substr(0, last_slash);

#ifdef _WIN32
        // Launch explorer in a separate thread to avoid blocking the UI
        std::thread([folder]() {
            std::string windows_path = folder;
            std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

            std::filesystem::path fs_path(windows_path);
            std::string command;

            if (std::filesystem::is_directory(fs_path)) {
                command = "explorer \"" + windows_path + "\"";
            } else {
                command = "explorer /select,\"" + windows_path + "\"";
            }

            Debug::Log("TranscodeQueueWindow: Executing command: " + command);
            system(command.c_str());
        }).detach();
#elif __APPLE__
        std::thread([folder]() {
            std::string cmd = "open \"" + folder + "\"";
            system(cmd.c_str());
        }).detach();
#else
        std::thread([folder]() {
            std::string cmd = "xdg-open \"" + folder + "\"";
            system(cmd.c_str());
        }).detach();
#endif

        Debug::Log("TranscodeQueueWindow: Opening output folder: " + folder);
    }
}

// ============================================================================
// Filtering/Sorting
// ============================================================================

void TranscodeQueueWindow::ApplyFilters() {
    filtered_jobs_.clear();

    auto all_jobs = queue_->GetAllJobs();
    std::string search_str(search_buffer_);
    std::transform(search_str.begin(), search_str.end(), search_str.begin(), ::tolower);

    for (auto job : all_jobs) {
        // Status filter
        bool show = false;
        switch (job->GetStatus()) {
            case TranscodeJob::Status::QUEUED: show = filter_show_queued_; break;
            case TranscodeJob::Status::ENCODING: show = filter_show_encoding_; break;
            case TranscodeJob::Status::PAUSED: show = filter_show_paused_; break;
            case TranscodeJob::Status::COMPLETED: show = filter_show_completed_; break;
            case TranscodeJob::Status::FAILED: show = filter_show_failed_; break;
            case TranscodeJob::Status::CANCELLED: show = filter_show_cancelled_; break;
        }

        if (!show) continue;

        // Search filter
        if (!search_str.empty()) {
            std::string job_name = job->GetJobName();
            std::transform(job_name.begin(), job_name.end(), job_name.begin(), ::tolower);

            if (job_name.find(search_str) == std::string::npos) {
                continue;
            }
        }

        filtered_jobs_.push_back(job);
    }
}

std::string TranscodeQueueWindow::GetPriorityString(TranscodeJob::Priority priority) const {
    switch (priority) {
        case TranscodeJob::Priority::LOW: return "Low";
        case TranscodeJob::Priority::NORMAL: return "Normal";
        case TranscodeJob::Priority::HIGH: return "High";
        case TranscodeJob::Priority::URGENT: return "Urgent";
        default: return "Unknown";
    }
}

std::string TranscodeQueueWindow::FormatTime(double seconds) const {
    if (seconds <= 0.0 || std::isnan(seconds) || std::isinf(seconds)) {
        return "-";
    }

    int total_secs = static_cast<int>(seconds);
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;

    std::stringstream ss;
    if (hours > 0) {
        ss << hours << "h " << mins << "m";
    } else if (mins > 0) {
        ss << mins << "m " << secs << "s";
    } else {
        ss << secs << "s";
    }

    return ss.str();
}

std::string TranscodeQueueWindow::FormatSpeed(double fps) const {
    if (fps <= 0.0) {
        return "-";
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << fps << " fps";
    return ss.str();
}

void TranscodeQueueWindow::ProcessKeyboard() {
    if (!is_open_ || selected_job_id_.empty()) {
        return;
    }

    // Note: Keyboard handling would be integrated with your main input system
    // This is just a placeholder showing the structure
}

} // namespace ump
