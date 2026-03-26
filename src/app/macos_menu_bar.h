#pragma once
#ifdef __APPLE__

#include <string>
#include <vector>

namespace qcview {

struct NativeMenuState {
    bool fullscreen = false;
    bool show_project = true, show_inspector = true, show_timeline = true;
    bool show_color = false, show_annotations = false, show_ann_toolbar = false;
    bool show_sidebar = false;
    bool can_undo = false, can_redo = false;
    bool has_media = false, has_project = false;
};

void InitNativeMenuBar();
void ShutdownNativeMenuBar();
void SyncNativeMenuState(const NativeMenuState& state);
void UpdateNativeRecentFiles(const std::vector<std::string>& files);
bool IsNativeMenuBarActive();

} // namespace qcview
#endif
