#pragma once

// ============================================================================
// Application class declaration
// ============================================================================

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <future>
#include <set>
#include <chrono>
#include <imgui.h>
#include <glad/gl.h>
#ifdef _WIN32
#include <windows.h>
#endif

// Project includes needed for member variable types
#include "player/video_player.h"
#include "overlay/safety_overlay_system.h"
#include "nodes/node_base.h"
#include "annotations/annotation_exporter.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_toolbar.h"
#include "annotations/annotation_renderer.h"

// Forward declarations
struct GLFWwindow;
struct GLFWmonitor;
class TimelineManager;
class OCIOConfigManager;
namespace qcview {
    class ProjectManager;
    class AnnotationManager;
    class AnnotationPanel;
    class NodeManager;
    class TimelineView;
    class TimelinePlaybackController;
    class TimelineCommandManager;
    class TimelineThumbnailCache;
    class MediaLinker;
    struct OTIOTrack;
    struct DualViewClip;
}
// Need full definitions for nested types used in method signatures
#include "timeline/timeline_selection.h"  // TimelineClipDragState::DraggedClipInfo
#include "project/media_item.h"           // qcview::MediaType
// ExifToolHelper is a class with nested Metadata struct - must include
#include "utils/exiftool_helper.h"

class Application {
    // Friend declarations for global wrapper functions
    friend void SaveSettings();
    friend void ScheduleImport(const std::string& path, const std::string& message);

public:
    // ------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    // ------------------------------------------------------------------------
    Application();

    // ------------------------------------------------------------------------
    // CORE LIFECYCLE METHODS
    // ------------------------------------------------------------------------
    bool Initialize(const std::vector<std::string>& initial_files = {});
    void Run();
    void Cleanup();
    void RefreshCurrentFrame();
    void UpdateColorPipeline();
    void ForceReloadCurrentMedia();

    // ------------------------------------------------------------------------
    // LOADING/SCHEDULING
    // ------------------------------------------------------------------------
    void ScheduleLoadingOperation(const std::string& message, std::function<void()> callback);
    bool IsLoadingMedia() const;
    const std::string& GetLoadingMessage() const;
    void ExecuteLoadingCallback();
    void ScheduleImport(const std::string& path, const std::string& message);

    // Static members for window procedure
    static WNDPROC original_wndproc;
    static Application* app_instance;

private:
    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Core Systems
    // ------------------------------------------------------------------------
    GLFWwindow* window;
    std::unique_ptr<VideoPlayer> video_player;
    std::unique_ptr<qcview::ProjectManager> project_manager;
    std::unique_ptr<TimelineManager> timeline_manager;
    std::unique_ptr<qcview::AnnotationManager> annotation_manager;
    std::unique_ptr<qcview::AnnotationPanel> annotation_panel;
    std::unique_ptr<qcview::Annotations::AnnotationExporter> annotation_exporter;
    std::unique_ptr<qcview::Annotations::ViewportAnnotator> viewport_annotator;
    std::unique_ptr<qcview::Annotations::AnnotationToolbar> annotation_toolbar;
    std::unique_ptr<qcview::Annotations::AnnotationRenderer> annotation_renderer;

    // Current annotation editing state
    std::vector<qcview::Annotations::ActiveStroke> current_annotation_strokes_;
    std::string current_editing_timecode_;

    // Deferred NanoVG annotation rendering
    bool pending_nvg_render_ = false;
    bool nvg_popup_open_ = false;
    ImVec2 nvg_display_pos_ = ImVec2(0, 0);
    ImVec2 nvg_display_size_ = ImVec2(0, 0);
    int nvg_video_width_ = 0;
    std::vector<qcview::Annotations::ActiveStroke> nvg_strokes_to_render_;

    // Undo/redo stacks for annotation editing
    std::vector<std::vector<qcview::Annotations::ActiveStroke>> annotation_undo_stack_;
    std::vector<std::vector<qcview::Annotations::ActiveStroke>> annotation_redo_stack_;

    void AutoSaveAnnotationOnSeek();

    bool first_time_setup;
    std::string layout_ini_path;

    // Window settings from saved preferences
    int saved_window_x = -1;
    int saved_window_y = -1;
    int saved_window_width = 1914;
    int saved_window_height = 1060;
    bool has_saved_window_settings = false;
    std::string saved_imgui_layout;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - UI State
    // ------------------------------------------------------------------------
    bool show_video = true;
    bool show_controls = true;
    bool show_timeline = true;
    bool show_status_bar = true;
    bool show_project_panel = true;
    bool show_inspector_panel = true;
    bool show_timeline_panel = true;
    bool show_transport_controls = true;
    bool show_color_panels = false;
    bool saved_show_color_panels = false;
    bool show_annotation_panel = false;
    bool saved_show_annotation_panel = false;
    bool show_annotation_toolbar = false;
    bool saved_show_annotation_toolbar = false;
    bool annotations_enabled = true;
    bool timeline_editing_mode = true;
    bool minimal_view_mode = false;
    bool is_fullscreen = false;
    bool pending_fullscreen_toggle = false;
    GLFWmonitor* fullscreen_monitor = nullptr;
    bool saved_show_project_panel = true;
    bool saved_show_inspector_panel = true;
    bool show_sidebar_panel = true;
    bool saved_show_sidebar_panel = true;
    bool show_color = false;

    // Trim mode state
    bool trim_mode_left = false;
    bool trim_mode_right = false;
    std::string original_video_path_left;
    std::string original_video_path_right;

    // Loading modal state
    std::string loading_message_;
    std::function<void()> loading_callback_;
    int loading_frames_ = 0;

    enum class VideoBackgroundType {
        DEFAULT,
        BLACK,
        DARK_CHECKERBOARD,
        LIGHT_CHECKERBOARD
    };

    SafetyGuideSettings safety_settings;
    VideoBackgroundType video_background_type = VideoBackgroundType::BLACK;
    bool show_background_panel = false;
    bool show_colorspace_panel = false;
    bool show_safety_overlay_panel = false;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Input State
    // ------------------------------------------------------------------------
    bool rewind_a_held = false;
    bool fastforward_d_held = false;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Media State
    // ------------------------------------------------------------------------
    std::string current_file_path;
    std::vector<std::string> recent_files;
    const size_t max_recent_files = 10;
    float last_volume = 1.0f;
    int current_volume = 100;
    bool is_muted = false;
    int volume_before_mute = 100;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Metadata State
    // ------------------------------------------------------------------------
    std::future<std::unique_ptr<ExifToolHelper::Metadata>> metadata_future;
    std::unique_ptr<ExifToolHelper::Metadata> cached_metadata;
    std::string metadata_for_file;
    bool metadata_loading = false;

    // ------------------------------------------------------------------------
    // MEMBER VARIABLES - Nodes
    // ------------------------------------------------------------------------
    std::unique_ptr<qcview::NodeManager> node_manager;
    struct OCIONodeDragPayload {
        qcview::NodeType type;
        char name[256];
    };
    std::vector<std::string> custom_node_trees;

    // Transparent border for docked panels
    const ImVec4 kTransparentBorder = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // ------------------------------------------------------------------------
    // NESTED TYPES - Export
    // ------------------------------------------------------------------------
    struct CaptureRequest {
        std::string output_path;
        std::vector<qcview::Annotations::ActiveStroke> strokes;
        bool pending = false;
        bool completed = false;
        bool success = false;
        bool just_queued = false;

        // Video display area for cropping (when capturing from screen)
        ImVec2 display_pos = ImVec2(0, 0);
        ImVec2 display_size = ImVec2(0, 0);

        // Export dimensions (when exporting at native resolution)
        bool use_native_resolution = false;
        int export_width = 0;
        int export_height = 0;

        // Viewport width where annotations were created (for line width scaling)
        float viewport_width_at_creation = 1920.0f;
    };

    struct ExportState {
        bool active = false;
        qcview::Annotations::AnnotationExporter::ExportFormat format;
        qcview::Annotations::AnnotationExporter::ExportOptions options;
        std::vector<qcview::AnnotationNote> notes;
        size_t current_note_index = 0;
        std::string temp_dir;
        std::vector<std::string> captured_images;
        bool waiting_for_capture = false;
        bool waiting_for_seek = false;
        int frames_to_wait_after_seek = 0;
        int frames_to_wait_for_resize = 0;

        // Data for pending capture
        std::string pending_output_path;
        std::vector<qcview::Annotations::ActiveStroke> pending_strokes;
    };

    CaptureRequest pending_capture;
    ExportState export_state;

    // ------------------------------------------------------------------------
    // NESTED TYPES - Node Presets
    // ------------------------------------------------------------------------
    struct NodePresetData {
        std::string type;
        std::string data;
        std::string display;
        std::string view;
        std::string display_alias;
        std::string view_alias;
        ImVec2 position;
    };

    struct ConnectionPresetData {
        int from_node;
        int from_pin;
        int to_node;
        int to_pin;
    };

    // ------------------------------------------------------------------------
    // NESTED TYPES - Frame.io
    // ------------------------------------------------------------------------
    struct FrameioImportState {
        bool show_dialog = false;
        char token_buffer[256] = "";
        char url_buffer[512] = "";
        bool importing = false;
        std::string status_message;
        bool import_success = false;

        // Thumbnail generation state
        bool generating_thumbnails = false;
        std::vector<qcview::AnnotationNote> imported_notes;
        size_t current_thumbnail_index = 0;
        bool waiting_for_seek = false;
        int frames_to_wait_after_seek = 0;
    };

    FrameioImportState frameio_import_state;

    // ------------------------------------------------------------------------
    // SETUP & CONFIGURATION
    // ------------------------------------------------------------------------
    void LoadCustomFonts();
    ImVec4 GetDefaultAccentColor();
    ImVec4 GetCustomAccentColor();
    ImVec4 GetWindowsAccentColor();
    ImVec4 TintColor(const ImVec4& color, float brightness, float saturation = 1.0f);
    ImVec4 MutedDark(const ImVec4& accent);
    ImVec4 MutedLight(const ImVec4& accent);
    ImVec4 Bright(const ImVec4& accent);
    ImU32 ToImU32(const ImVec4& color);
    void SetupImGuiStyle();
    void SetupDragDrop();
    void EnableDarkModeWindow(GLFWwindow* window);

    // ------------------------------------------------------------------------
    // INPUT & EVENT HANDLING
    // ------------------------------------------------------------------------
    void HandleKeyboardShortcuts();
    void HandleInput();

    // ------------------------------------------------------------------------
    // UI LAYOUT & PANELS
    // ------------------------------------------------------------------------
    void CreateDockingLayout();
    void HandleShareProjectPopups();
    void SetupDefaultLayout(ImGuiID dockspace_id);
    void CreateMenuBar();

    // ------------------------------------------------------------------------
    // DIALOGS & WINDOWS
    // ------------------------------------------------------------------------
    void CreateCacheStatsWindow();
    void CreateAudioDiagnosticsWindow();
    void CreateTranscodeProgressDialog();
    void CreateCacheClearDialogs();
    void HandleCriticalPressure();
    void RenderPressureCriticalDialog();
    void CreateCacheSettingsWindow();
    void CreateFontSettingsWindow();
    void CreateKeyboardShortcutsPopup();
    void CreateLutExportPopup();

    // ------------------------------------------------------------------------
    // VIEWPORT & RENDERING
    // ------------------------------------------------------------------------
    void CreateVideoViewport();
    void RenderBackgroundSelectionPanel(VideoBackgroundType& bg_type, bool& show_panel);
    void RenderSidebarPanel();
    void RenderTrimToolbarPanel();
    void RenderFrameioImportDialog();
    void RenderSafetyOverlayPanel(bool& show_panel);
    void RenderColorspacePresetsPanel(bool& show_panel);
    void DrawVideoBackground(ImVec2 canvas_pos, ImVec2 canvas_size, float tile_size = 20.0f);

    // ------------------------------------------------------------------------
    // TIMELINE
    // ------------------------------------------------------------------------
    std::vector<double> CollectSnapPoints(
        const std::vector<qcview::OTIOTrack>& tracks,
        const std::set<std::string>& exclude_clip_ids,
        double playhead_time,
        double timeline_duration);
    double CalculateSnappedPosition(
        double proposed_start,
        double clip_duration,
        const std::vector<double>& snap_points,
        double pixels_per_second,
        bool& snapped,
        double& snap_line_time);
    std::vector<double> CollectDualViewSnapPoints(
        const qcview::DualViewClip& left_clip,
        double playhead_time);
    double CalculateDualViewSnappedPosition(
        double proposed_offset,
        double clip_duration,
        const std::vector<double>& snap_points,
        double pixels_per_second,
        bool& snapped,
        double& snap_line_time);
    bool WouldCauseOverlap(
        const std::vector<qcview::OTIOTrack>& tracks,
        const std::vector<qcview::TimelineClipDragState::DraggedClipInfo>& moving_clips,
        double primary_new_start);
    void RenderTimelineContent();
    void RenderOTIOTimelinePanel();
    void CreateTimelineTransportPanel();
    void CreateProjectPanel();
    void CreateInspectorPanel();

    // ------------------------------------------------------------------------
    // NODE EDITOR & COLOR
    // ------------------------------------------------------------------------
    void CreateComponentPaletteContent();
    void CreateCurrentComponentsTab();
    void CreatePresetsTab();
    void CreateACESPresets();
    void CreateStandardPresets();
    void CreateBlenderPresets();
    void CreateBlender5Presets();
    void CreateACES20Presets();
    std::string GetNodeTreesFolder();
    void SaveCurrentNodeTree(const std::string& name);
    void LoadCustomNodeTrees();
    void LoadNodeTreeFromFile(const std::string& name);
    void DeleteNodeTree(const std::string& name);
    void CreateCustomPresets();
    void ApplyPreset(const std::string& json_preset);
    void ApplyAliasPreset(const std::string& input_alias, const std::string& display_alias, const std::string& view_name);
    void CreateNodeEditorContent();
    void CreateNodePropertiesContent();
    void RenderNodeSpecificProperties(qcview::NodeBase* node);
    void RenderOutputDisplayProperties(qcview::NodeBase* node);
    void RenderInputColorSpaceProperties(qcview::NodeBase* node);
    void RenderLookProperties(qcview::NodeBase* node);
    void RenderSceneLUTProperties(qcview::NodeBase* node);
    void RenderDisplayLUTProperties(qcview::NodeBase* node);
    bool CheckPipelineReadiness();
    void GenerateOCIOPipeline();
    void CreateAnnotationPanel();
    void CreateAnnotationToolbar();
    void CreateColorPanels();

    // ------------------------------------------------------------------------
    // EXPORT & FRAME.IO
    // ------------------------------------------------------------------------
    bool HasSavedFrameioToken();
    void ClearSavedToken();
    void ProcessFrameioThumbnailGeneration();
    void QueueFrameCapture(const std::string& output_path,
                          const std::vector<qcview::Annotations::ActiveStroke>& strokes,
                          int export_width = 0,
                          int export_height = 0);
    void ProcessExportStateMachine();
    void FinalizeExport(bool success);
    bool CaptureRenderedFrame();
    void StartExport(
        qcview::Annotations::AnnotationExporter::ExportFormat format,
        const qcview::Annotations::AnnotationExporter::ExportOptions& options,
        const std::vector<qcview::AnnotationNote>& notes,
        const std::string& temp_dir);

    // ------------------------------------------------------------------------
    // BASE64 HELPERS
    // ------------------------------------------------------------------------
    std::string EncodeBase64(const std::string& input);
    std::string DecodeBase64(const std::string& input);

    // ------------------------------------------------------------------------
    // SETTINGS
    // ------------------------------------------------------------------------
    std::string GetSettingsPath();
    std::string GetLayoutIniPath();
    void LoadSettings();
    void SaveSettings();
    void DeleteAllPreferences();

    // ------------------------------------------------------------------------
    // URI & SHARING
    // ------------------------------------------------------------------------
    std::string EncodeURIComponent(const std::string& str);
    std::string DecodeURIComponent(const std::string& str);
    std::string BuildProjectURI(const std::string& project_path);
    std::string ParseProjectURI(const std::string& uri);
    void ShareProject();

    // ------------------------------------------------------------------------
    // FILE & WINDOW MANAGEMENT
    // ------------------------------------------------------------------------
    void OpenFileDialog();
    void TriggerAutoPlay(qcview::MediaType media_type = qcview::MediaType::VIDEO);
    void TriggerSeekCacheStart();
    void AddToRecentFiles(const std::string& file_path);
    void ResetTimecodeStateForNewVideo();
    void OnVideoChanged(const std::string& new_file_path);
    std::string FormatTime(double seconds);
    void ShowAllPanels();
    void SetDefaultView();
    void ApplyBackgroundColor();
    void ToggleMute();
    void ToggleLoop();
#ifdef _WIN32
    static LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void SetupSingleInstanceMessaging(HWND hwnd);
#endif
    void ToggleFullscreen();

    // ------------------------------------------------------------------------
    // TIMECODE
    // ------------------------------------------------------------------------
    std::string GetTimecodeOffset() const;
    double ParseTimecodeToSeconds(const std::string& timecode_str, double fps = 23.976);
    void ResetTimecodeState();
    bool ParseFlexibleTimecode(const std::string& input, int& hours, int& minutes, int& seconds, int& frames);
    bool ParseFlexibleFrameNumber(const std::string& input, int& frame_number);
    void OpenGotoTimecodeModal();
    void RenderGotoTimecodeModal();
    void CheckStartTimecodeAvailability();
    std::string FormatRegularTimecode(double current_seconds);
    std::string FormatOffsetTimecode(double current_seconds);
    std::string FormatCurrentTimecodeWithOffset(double current_seconds);
    void ToggleTimecodeMode();

    // ------------------------------------------------------------------------
    // UTILITY
    // ------------------------------------------------------------------------
    void CopyToClipboard(const std::string& text);
    void OpenFileInExplorer(const std::string& file_path);
    void RenderPathWithButtons(const std::string& path, const std::string& id, bool show_open_button = true);
    std::string GetFileName(const std::string& path);
};
