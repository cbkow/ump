#include "transcode_queue_window.h"
#include "../app/ui_scale.h"
#include "../utils/debug_utils.h"
#include "../app/app_ui_macros.h"
#include <imgui.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#include <dwmapi.h>
#include <shellapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// Undefine Windows.h macros that conflict with TranscodeQueue methods
#ifdef GetJob
#undef GetJob
#endif

// Material Icons
#define ICON_ARROW_DROP_DOWN        u8"\uE5C7"   // Dropdown arrow

// External variables from main.cpp
extern bool use_windows_accent_color;
extern ImFont* font_icons;  // Material Icons font

// External transcode performance settings (defined in main.cpp)
#include "../transcode/transcode_settings.h"

// Forward declare SaveSettings from main.cpp
extern void SaveSettings();

// External variables from main.cpp
extern int custom_accent_color_index;
extern ImVec4 custom_picker_color;
extern const ImVec4 accent_color_palette[];
extern const int accent_color_palette_count;

static ImVec4 GetDefaultAccentColor() {
    return accent_color_palette[0];  // 69797e - Default gray-blue
}

static ImVec4 GetCustomAccentColor() {
    if (custom_accent_color_index == -2) {
        return custom_picker_color;
    }
    if (custom_accent_color_index >= 0 && custom_accent_color_index < accent_color_palette_count) {
        return accent_color_palette[custom_accent_color_index];
    }
    return GetDefaultAccentColor();
}

#ifdef _WIN32
static ImVec4 GetWindowsAccentColor() {
    if (use_windows_accent_color) {
        DWORD colorization_color;
        BOOL opaque_blend;
        if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
            float r = ((colorization_color >> 16) & 0xff) / 255.0f;
            float g = ((colorization_color >> 8) & 0xff) / 255.0f;
            float b = (colorization_color & 0xff) / 255.0f;
            return ImVec4(r, g, b, 1.0f);
        }
        return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue if API fails
    }
    return GetCustomAccentColor();
}
#else
static ImVec4 GetWindowsAccentColor() {
    if (use_windows_accent_color) {
        return GetDefaultAccentColor();
    }
    return GetCustomAccentColor();
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

namespace qcview {

TranscodeQueueWindow::TranscodeQueueWindow(TranscodeQueue* queue, TranscodeWorkerPool* worker_pool)
    : queue_(queue)
    , worker_pool_(worker_pool)
{
    Debug::Log("TranscodeQueueWindow: Created");
}

TranscodeQueueWindow::~TranscodeQueueWindow() {
}

void TranscodeQueueWindow::Render() {
    // Open modal popup once when requested
    static bool was_open = false;
    if (is_open_ && !was_open) {
        ImGui::OpenPopup("Transcode Queue Manager");
        was_open = true;
    }
    if (!is_open_) {
        was_open = false;
    }

    // Center modal on screen — 80% of viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetCenter();
    ImVec2 viewport_size = viewport->Size;

    float max_width = viewport_size.x * 0.9f;
    float max_height = viewport_size.y * 0.9f;

    // Always center and size to fit viewport (prevents modal from becoming separate OS window)
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(max_width, max_height), ImGuiCond_Always);

    // Begin modal popup - NoResize/NoMove since we're managing size/position automatically
    ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    if (!ImGui::BeginPopupModal("Transcode Queue Manager", &is_open_, modal_flags)) {
        return;
    }

    // Tab bar for Queue vs Settings
    if (ImGui::BeginTabBar("TranscodeQueueTabs", ImGuiTabBarFlags_None)) {
        // Use flags to force tab selection when needed
        ImGuiTabItemFlags queue_flags = (current_tab_ == 0) ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags settings_flags = (current_tab_ == 1) ? ImGuiTabItemFlags_SetSelected : 0;

        // Clear the selection flag after use (only needed for programmatic switching)
        static bool clear_flag_next_frame = false;
        if (clear_flag_next_frame) {
            current_tab_ = -1;  // Reset to "unforced" state
            clear_flag_next_frame = false;
        }

        // If we just programmatically set a tab, clear the flag next frame
        if (current_tab_ >= 0) {
            clear_flag_next_frame = true;
        }

        // Queue Tab
        if (ImGui::BeginTabItem("Queue", nullptr, queue_flags)) {
            RenderQueueTab();
            ImGui::EndTabItem();
        }

        // Settings Tab
        if (ImGui::BeginTabItem("Settings", nullptr, settings_flags)) {
            RenderSettingsTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndPopup();
}

void TranscodeQueueWindow::RenderQueueTab() {
    // Wrap entire queue tab content in a child region for proper layout
    ImGui::BeginChild("QueueTabContent", ImVec2(0, 0), false);

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

    ImGui::EndChild();  // End QueueTabContent
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

    // Calculate button widths based on text size
    float btn_padding = ImGui::GetStyle().FramePadding.x * 2 + 8.0f;
    float pause_btn_w = ImGui::CalcTextSize("Pause Queue").x + btn_padding;
    float resume_btn_w = ImGui::CalcTextSize("Resume Queue").x + btn_padding;
    float start_btn_w = ImGui::CalcTextSize("Start Queue").x + btn_padding;
    float stop_btn_w = ImGui::CalcTextSize("Stop All").x + btn_padding;
    float actions_btn_w = ImGui::CalcTextSize("Actions").x + btn_padding;

    // Use the widest primary button width for consistent sizing
    float primary_btn_w = std::max({pause_btn_w, resume_btn_w, start_btn_w});

    // Calculate total width needed for right-aligned buttons
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_btns_width = primary_btn_w + stop_btn_w + actions_btn_w + spacing * 2;

    ImGui::SameLine(ImGui::GetWindowWidth() - total_btns_width - 15.0f);

    // Control buttons (cleaner layout with Actions menu)
    ImGui::BeginGroup();

    // Primary action button (Start/Pause/Resume)
    if (pool_status == TranscodeWorkerPool::Status::ACTIVE) {
        PushOutlineButtonStyle();
        if (ImGui::Button("Pause Queue", ImVec2(primary_btn_w, 0))) {
            worker_pool_->Pause();
            Debug::Log("TranscodeQueueWindow: Queue paused");
        }
        PopOutlineButtonStyle();
    } else if (pool_status == TranscodeWorkerPool::Status::PAUSED) {
        PushOutlineButtonStyle();
        if (ImGui::Button("Resume Queue", ImVec2(primary_btn_w, 0))) {
            worker_pool_->Resume();
            Debug::Log("TranscodeQueueWindow: Queue resumed");
        }
        PopOutlineButtonStyle();
    } else {
        PushOutlineButtonStyle();
        if (ImGui::Button("Start Queue", ImVec2(primary_btn_w, 0))) {
            worker_pool_->Start();
            Debug::Log("TranscodeQueueWindow: Queue started");
        }
        PopOutlineButtonStyle();
    }

    ImGui::SameLine();

    // Stop All button (critical action, keep visible)
    PushOutlineButtonStyle();
    if (ImGui::Button("Stop All", ImVec2(stop_btn_w, 0))) {
        // Cancel all active, queued, and paused jobs first
        auto all_jobs = queue_->GetAllJobs();
        for (auto job : all_jobs) {
            auto status = job->GetStatus();
            if (status == TranscodeJob::Status::ENCODING ||
                status == TranscodeJob::Status::QUEUED ||
                status == TranscodeJob::Status::PAUSED) {
                queue_->CancelJob(job->GetJobID());
            }
        }
        worker_pool_->Stop();
        Debug::Log("TranscodeQueueWindow: Cancelled all jobs and stopped workers");
    }
    PopOutlineButtonStyle();

    ImGui::SameLine();

    // Actions dropdown menu
    PushOutlineButtonStyle();
    if (ImGui::Button("Actions", ImVec2(actions_btn_w, 0))) {
        ImGui::OpenPopup("ActionsMenu");
    }
    PopOutlineButtonStyle();

    // Actions menu popup
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
    if (ImGui::BeginPopup("ActionsMenu")) {
        if (ImGui::MenuItem("Stop and Clear All")) {
            worker_pool_->Stop();
            queue_->ClearAll();
            Debug::Log("TranscodeQueueWindow: Stopped all workers and cleared entire queue");
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Clear Completed")) {
            auto all_jobs = queue_->GetAllJobs();
            for (auto job : all_jobs) {
                if (job->GetStatus() == TranscodeJob::Status::COMPLETED) {
                    queue_->RemoveJob(job->GetJobID());
                }
            }
            Debug::Log("TranscodeQueueWindow: Cleared completed jobs");
        }

        if (ImGui::MenuItem("Clear Failed")) {
            auto all_jobs = queue_->GetAllJobs();
            for (auto job : all_jobs) {
                if (job->GetStatus() == TranscodeJob::Status::FAILED) {
                    queue_->RemoveJob(job->GetJobID());
                }
            }
            Debug::Log("TranscodeQueueWindow: Cleared failed jobs");
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    ImGui::EndGroup();

    ImGui::EndChild();
}

void TranscodeQueueWindow::RenderWorkerStatusBar() {
    auto worker_info = worker_pool_->GetWorkerInfo();
    int worker_count = worker_pool_->GetWorkerCount();

    // Count active workers for the header
    int active_count = 0;
    for (int i = 0; i < worker_count; ++i) {
        if (worker_info[i].is_active) {
            active_count++;
        }
    }

    // Collapsible header for workers section (collapsed by default)
    char header_text[64];
    snprintf(header_text, sizeof(header_text), "Workers (%d/%d active)", active_count, worker_count);

    PushOutlineHeaderStyle();
    if (ImGui::CollapsingHeader(header_text, ImGuiTreeNodeFlags_None)) {
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

        ImGui::Spacing();
    }
    PopOutlineHeaderStyle();
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

    if (ImGui::BeginTable("JobsTable", 6, flags)) {
        // Headers
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
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

    // Set row height to match frame height for proper selection coverage
    float row_height = ImGui::GetFrameHeight();
    ImGui::TableNextRow(0, row_height);

    bool is_selected = (job->GetJobID() == selected_job_id_);
    ImVec4 accent = GetWindowsAccentColor();
    ImVec4 muted_dark = MutedDark(accent);
    ImVec4 bright = Bright(accent);
    auto status = job->GetStatus();

    // Define status-based highlight colors
    // Yellow for LUT generation, Green for encoding, Red for cancelled/failed, Neutral grey for others
    ImVec4 highlight_color;
    ImVec4 active_color;
    switch (status) {
        case TranscodeJob::Status::GENERATING_LUT:
            // Dark yellow for LUT generation phase
            highlight_color = ImVec4(0.6f, 0.5f, 0.1f, 0.3f);
            active_color = ImVec4(0.6f, 0.5f, 0.1f, 0.5f);
            break;
        case TranscodeJob::Status::ENCODING:
            // Green for active/running
            highlight_color = ImVec4(0.2f, 0.6f, 0.3f, 0.3f);
            active_color = ImVec4(0.2f, 0.6f, 0.3f, 0.5f);
            break;
        case TranscodeJob::Status::FAILED:
        case TranscodeJob::Status::CANCELLED:
            // Red for failed/cancelled
            highlight_color = ImVec4(0.6f, 0.2f, 0.2f, 0.3f);
            active_color = ImVec4(0.6f, 0.2f, 0.2f, 0.5f);
            break;
        default:
            // Neutral grey for queued, paused, completed
            highlight_color = ImVec4(0.4f, 0.4f, 0.4f, 0.3f);
            active_color = ImVec4(0.5f, 0.5f, 0.5f, 0.5f);
            break;
    }

    // Priority column
    ImGui::TableSetColumnIndex(0);

    // Set row background - only highlight if selected, otherwise use default table colors
    if (is_selected) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(highlight_color));
    }
    // Don't set background for non-selected rows - let table use default alternating colors

    // Push hover/active colors based on status
    ImGui::PushStyleColor(ImGuiCol_Header, highlight_color);  // Selected state
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, highlight_color);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, active_color);

    std::string priority_str = GetPriorityString(job->GetPriority());
    if (ImGui::Selectable(priority_str.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, row_height))) {
        selected_job_id_ = job->GetJobID();
    }

    // Right-click context menu - must be immediately after Selectable
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
    if (ImGui::BeginPopupContextItem()) {
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
        if ((status == TranscodeJob::Status::ENCODING ||
             status == TranscodeJob::Status::QUEUED ||
             status == TranscodeJob::Status::PAUSED) &&
            ImGui::MenuItem("Cancel")) {
            CancelSelectedJob();
        }
        if (status == TranscodeJob::Status::COMPLETED ||
            status == TranscodeJob::Status::FAILED ||
            status == TranscodeJob::Status::CANCELLED) {
            if (ImGui::MenuItem("Remove")) {
                RemoveSelectedJob();
            }
        }

        ImGui::Separator();

        if (status == TranscodeJob::Status::COMPLETED && ImGui::MenuItem("Open Output Folder")) {
            OpenOutputFolder();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();  // PopupBg

    ImGui::PopStyleColor(3);

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
    // Use yellow progress bar for LUT generation, green for encoding
    if (status == TranscodeJob::Status::GENERATING_LUT) {
        ImVec4 yellow_color(0.8f, 0.65f, 0.2f, 1.0f);
        RenderProgressBar(progress_frac, progress_text, &yellow_color);
    } else {
        RenderProgressBar(progress_frac, progress_text);
    }

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
                if (ImGui::MenuItem(
#ifdef _WIN32
                    "Show in Explorer"
#elif defined(__APPLE__)
                    "Reveal in Finder"
#else
                    "Show in File Browser"
#endif
                )) {
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

void TranscodeQueueWindow::RenderProgressBar(float progress, const std::string& text, const ImVec4* color) {
    char overlay_text[64];
    if (text.empty()) {
        snprintf(overlay_text, sizeof(overlay_text), "%.0f%%", progress * 100.0f);
    } else {
        snprintf(overlay_text, sizeof(overlay_text), "%s", text.c_str());
    }

    // Use provided color or default green
    ImVec4 bar_color = color ? *color : ImVec4(0.2f, 0.6f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
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

    // Scale size with font (dampened scaling)
    const float ui_scale = ImGui::GetIO().FontGlobalScale;
    const float scale_factor = 1.0f + (ui_scale - 1.0f) * 0.65f;
    float box_width = 280.0f * scale_factor;
    float box_height = 90.0f * scale_factor;
    float padding = 8.0f * scale_factor;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, header_color);
    ImGui::BeginChild(("Worker" + std::to_string(worker.worker_id)).c_str(),
                     ImVec2(box_width, box_height), true);

    // Manual padding inside the child window
    ImGui::SetCursorPos(ImVec2(padding, padding));
    ImGui::BeginGroup();

    ImGui::Text("Worker %d", worker.worker_id);

    if (worker.is_active) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + box_width - padding * 2);
        ImGui::TextWrapped("%s", worker.current_job_name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Text("%.0f%% (%.1f fps)", worker.progress_percent, worker.encoding_fps);
    } else {
        ImGui::TextDisabled("Idle");
    }

    ImGui::EndGroup();
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
        std::string windows_path = folder;
        std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

        std::wstring wide_path(windows_path.begin(), windows_path.end());
        std::wstring params;

        std::filesystem::path fs_path(windows_path);
        if (std::filesystem::is_directory(fs_path)) {
            params = L"\"" + wide_path + L"\"";
        } else {
            params = L"/select,\"" + wide_path + L"\"";
        }

        Debug::Log("TranscodeQueueWindow: Opening: " + windows_path);
        ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWNORMAL);
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
            case TranscodeJob::Status::GENERATING_LUT: show = filter_show_encoding_; break;  // Show with encoding filter
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

void TranscodeQueueWindow::RenderSettingsTab() {
    ImVec4 accent = GetWindowsAccentColor();
    static bool settings_changed = false;

    // Header
    ImGui::Text("Transcode Performance Settings");
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Configure encoding performance and worker behavior");
    ImGui::Separator();
    ImGui::Spacing();

    // Create child region for scrollable settings content
    float footer_height = 50.0f;  // Space for save button and status
    ImGui::BeginChild("SettingsContent", ImVec2(0, -footer_height), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
        // ===================================================================
        // TAB 1: Encoder Performance
        // ===================================================================
        if (ImGui::BeginTabItem("Encoder Performance")) {
            ImGui::Spacing();
            ImGui::Text("FFmpeg Encoder Threading");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Controls how many CPU cores are used per encode job");
            ImGui::Spacing();

            // Encoder thread count
            ImGui::Text("Encoder Threads:");
            if (ImGui::SliderInt("##EncoderThreads", &transcode_settings.encoder_thread_count, 0, 32)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Number of threads for video encoding.\n\n"
                    "0 = Auto (recommended) - uses all CPU cores\n"
                    "1-32 = Manual thread count\n\n"
                    "Higher = faster encoding per job\n"
                    "Note: Each worker uses these threads, so total CPU usage\n"
                    "is (workers × threads). Balance accordingly!"
                );
            }

            if (transcode_settings.encoder_thread_count == 0) {
                int hw_threads = std::thread::hardware_concurrency();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Auto mode: Using %d CPU threads (all available cores)", hw_threads);
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "Manual mode: Using %d CPU threads", transcode_settings.encoder_thread_count);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Prefetch settings
            ImGui::Text("Frame Prefetching");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "How many frames to buffer ahead during encoding");
            ImGui::Spacing();

            ImGui::Text("Prefetch Buffer Size:");
            if (ImGui::SliderInt("##PrefetchBuffer", &transcode_settings.prefetch_buffer_size, 16, 128)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Total number of frames to keep in memory.\n\n"
                    "Higher = smoother encoding, more RAM\n"
                    "Recommended: 32-64 frames"
                );
            }

            ImGui::Spacing();

            ImGui::Text("Prefetch Ahead Count:");
            if (ImGui::SliderInt("##PrefetchAhead", &transcode_settings.prefetch_ahead_count, 4, 64)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "How many frames ahead to load.\n\n"
                    "Higher = encoder never waits for frames\n"
                    "Recommended: 16-32 frames"
                );
            }

            ImGui::EndTabItem();
        }

        // ===================================================================
        // TAB 2: Worker Pool
        // ===================================================================
        if (ImGui::BeginTabItem("Worker Pool")) {
            ImGui::Spacing();
            ImGui::Text("Parallel Job Processing");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Configure how many jobs can encode simultaneously");
            ImGui::Spacing();

            ImGui::Text("Default Worker Count:");
            if (ImGui::SliderInt("##DefaultWorkers", &transcode_settings.default_worker_count, 1, 16)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Number of workers created on app startup.\n\n"
                    "Each worker can process one job at a time.\n"
                    "Recommended: 2-4 workers for most systems"
                );
            }

            ImGui::Spacing();

            ImGui::Text("Maximum Worker Count:");
            if (ImGui::SliderInt("##MaxWorkers", &transcode_settings.max_worker_count, 1, 32)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Maximum number of workers you can create.\n\n"
                    "Higher = more parallel jobs possible\n"
                    "Warning: More workers = higher CPU/RAM usage!\n"
                    "Recommended: 8-16 workers max"
                );
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Performance Tips");
            ImGui::BulletText("For single large jobs: Use 1-2 workers with max encoder threads");
            ImGui::BulletText("For many small jobs: Use 4-8 workers with fewer threads each");
            ImGui::BulletText("Total CPU threads used ≈ (workers × encoder threads)");
            ImGui::BulletText("Adjust worker count in real-time using the slider above");

            ImGui::EndTabItem();
        }

        // ===================================================================
        // TAB 3: Hardware Encoding (placeholder for future)
        // ===================================================================
        if (ImGui::BeginTabItem("Hardware Encoding")) {
            ImGui::Spacing();
            ImGui::Text("Hardware Acceleration");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Use GPU for faster encoding (when available)");
            ImGui::Spacing();

            ImGui::Checkbox("Prefer Hardware Encoding", &transcode_settings.prefer_hardware_encoding);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Try hardware encoders first before falling back to software.\n\n"
                    "Hardware encoding is 10-20x faster but may have\n"
                    "slightly lower quality at the same bitrate."
                );
            }

            ImGui::EndTabItem();
        }

        // ===================================================================
        // TAB 4: EXR Loading (Parallel I/O)
        // ===================================================================
        if (ImGui::BeginTabItem("EXR Loading")) {
            ImGui::Spacing();
            ImGui::Text("Parallel EXR Loading");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Optimize EXR sequence loading for maximum throughput");
            ImGui::Spacing();

            // Total threads indicator
            int total_threads = transcode_settings.concurrent_loads * transcode_settings.openexr_threads;
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                "Current: %d concurrent files x %d OpenEXR threads = %d total threads",
                transcode_settings.concurrent_loads,
                transcode_settings.openexr_threads,
                total_threads);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Concurrent Loads
            ImGui::Text("Concurrent File Loads:");
            if (ImGui::SliderInt("##ConcurrentLoads", &transcode_settings.concurrent_loads, 4, 16)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Number of EXR files loaded in parallel.\n\n"
                    "Higher = better disk/SSD utilization\n"
                    "Lower = less memory pressure\n\n"
                    "Recommended: 6-10 for NVMe SSD, 4-6 for SATA SSD"
                );
            }

            ImGui::Spacing();

            // OpenEXR Threads
            ImGui::Text("OpenEXR Threads per File:");
            if (ImGui::SliderInt("##OpenEXRThreads", &transcode_settings.openexr_threads, 1, 8)) {
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Threads used by OpenEXR for decompression per file.\n\n"
                    "Higher = faster decompression of compressed EXRs (PIZ, DWAA, ZIP)\n"
                    "Lower = less CPU contention with concurrent loads\n\n"
                    "Recommended: 2-4 threads per file"
                );
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Presets
            ImGui::Text("Quick Presets");
            ImGui::Spacing();

            // Calculate button width based on longest text
            float preset_btn_padding = ImGui::GetStyle().FramePadding.x * 2 + 16.0f;
            float preset_btn_w = ImGui::CalcTextSize("Conservative (12 threads)").x + preset_btn_padding;

            PushOutlineButtonStyle();
            if (ImGui::Button("Conservative (12 threads)", ImVec2(preset_btn_w, 0))) {
                transcode_settings.concurrent_loads = 6;
                transcode_settings.openexr_threads = 2;
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Laptop / background tasks");

            if (ImGui::Button("Balanced (32 threads)", ImVec2(preset_btn_w, 0))) {
                transcode_settings.concurrent_loads = 8;
                transcode_settings.openexr_threads = 4;
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Desktop workstation (default)");

            if (ImGui::Button("Aggressive (48 threads)", ImVec2(preset_btn_w, 0))) {
                transcode_settings.concurrent_loads = 12;
                transcode_settings.openexr_threads = 4;
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "High-end workstation");

            if (ImGui::Button("Maximum (72 threads)", ImVec2(preset_btn_w, 0))) {
                transcode_settings.concurrent_loads = 12;
                transcode_settings.openexr_threads = 6;
                settings_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Threadripper / HEDT");
            PopOutlineButtonStyle();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Tips
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Tips:");
            ImGui::BulletText("Monitor CPU usage during transcode to find optimal settings");
            ImGui::BulletText("NVMe SSDs benefit from higher concurrent loads (10-12)");
            ImGui::BulletText("Heavily compressed EXRs (PIZ/DWAA) benefit from more OpenEXR threads");
            ImGui::BulletText("Uncompressed EXRs are I/O-bound; use more concurrent loads, fewer threads");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();  // End scrollable content region

    // Footer with save button
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Apply button (settings take effect immediately, but save to disk on apply)
    if (settings_changed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.8f));
        if (ImGui::Button("Save Changes", ImVec2(S(150), 0))) {
            SaveSettings();
            Debug::Log("Transcode settings saved to disk");
            settings_changed = false;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "You have unsaved changes");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "All changes saved");
    }
}

void TranscodeQueueWindow::ProcessKeyboard() {
    if (!is_open_ || selected_job_id_.empty()) {
        return;
    }

    // Note: Keyboard handling would be integrated with your main input system
    // This is just a placeholder showing the structure
}

} // namespace qcview
