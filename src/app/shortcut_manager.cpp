// ============================================================================
// Customizable Keyboard Shortcuts Manager - Implementation
// ============================================================================

#include "app/shortcut_manager.h"
#include "utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cstdlib>

static std::vector<ShortcutEntry> g_shortcuts;

// ============================================================================
// KeyCombo ToString / FromString
// ============================================================================

std::string KeyCombo::ToString() const {
    if (!IsSet()) return "";

    std::string result;
    if (ctrl)  result += "Ctrl+";
    if (shift) result += "Shift+";
    if (alt)   result += "Alt+";

    // Map ImGuiKey to display name
    switch (key) {
        case ImGuiKey_Space:        result += "Space"; break;
        case ImGuiKey_Enter:        result += "Enter"; break;
        case ImGuiKey_Escape:       result += "Escape"; break;
        case ImGuiKey_Tab:          result += "Tab"; break;
        case ImGuiKey_Backspace:    result += "Backspace"; break;
        case ImGuiKey_Delete:       result += "Delete"; break;
        case ImGuiKey_Insert:       result += "Insert"; break;
        case ImGuiKey_UpArrow:      result += "Up"; break;
        case ImGuiKey_DownArrow:    result += "Down"; break;
        case ImGuiKey_LeftArrow:    result += "Left"; break;
        case ImGuiKey_RightArrow:   result += "Right"; break;
        case ImGuiKey_Home:         result += "Home"; break;
        case ImGuiKey_End:          result += "End"; break;
        case ImGuiKey_PageUp:       result += "PageUp"; break;
        case ImGuiKey_PageDown:     result += "PageDown"; break;
        case ImGuiKey_LeftBracket:  result += "["; break;
        case ImGuiKey_RightBracket: result += "]"; break;
        case ImGuiKey_Minus:        result += "-"; break;
        case ImGuiKey_Equal:        result += "="; break;
        case ImGuiKey_Slash:        result += "/"; break;
        case ImGuiKey_Backslash:    result += "\\"; break;
        case ImGuiKey_Semicolon:    result += ";"; break;
        case ImGuiKey_Apostrophe:   result += "'"; break;
        case ImGuiKey_Comma:        result += ","; break;
        case ImGuiKey_Period:       result += "."; break;
        case ImGuiKey_GraveAccent:  result += "`"; break;
        case ImGuiKey_F1:  result += "F1"; break;
        case ImGuiKey_F2:  result += "F2"; break;
        case ImGuiKey_F3:  result += "F3"; break;
        case ImGuiKey_F4:  result += "F4"; break;
        case ImGuiKey_F5:  result += "F5"; break;
        case ImGuiKey_F6:  result += "F6"; break;
        case ImGuiKey_F7:  result += "F7"; break;
        case ImGuiKey_F8:  result += "F8"; break;
        case ImGuiKey_F9:  result += "F9"; break;
        case ImGuiKey_F10: result += "F10"; break;
        case ImGuiKey_F11: result += "F11"; break;
        case ImGuiKey_F12: result += "F12"; break;
        default:
            // A-Z and 0-9 keys
            if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
                result += (char)('A' + (key - ImGuiKey_A));
            } else if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
                result += (char)('0' + (key - ImGuiKey_0));
            } else {
                result += "?";
            }
            break;
    }
    return result;
}

static ImGuiKey ParseKeyName(const std::string& name) {
    if (name == "Space")     return ImGuiKey_Space;
    if (name == "Enter")     return ImGuiKey_Enter;
    if (name == "Escape")    return ImGuiKey_Escape;
    if (name == "Tab")       return ImGuiKey_Tab;
    if (name == "Backspace") return ImGuiKey_Backspace;
    if (name == "Delete")    return ImGuiKey_Delete;
    if (name == "Insert")    return ImGuiKey_Insert;
    if (name == "Up")        return ImGuiKey_UpArrow;
    if (name == "Down")      return ImGuiKey_DownArrow;
    if (name == "Left")      return ImGuiKey_LeftArrow;
    if (name == "Right")     return ImGuiKey_RightArrow;
    if (name == "Home")      return ImGuiKey_Home;
    if (name == "End")       return ImGuiKey_End;
    if (name == "PageUp")    return ImGuiKey_PageUp;
    if (name == "PageDown")  return ImGuiKey_PageDown;
    if (name == "[")         return ImGuiKey_LeftBracket;
    if (name == "]")         return ImGuiKey_RightBracket;
    if (name == "-")         return ImGuiKey_Minus;
    if (name == "=")         return ImGuiKey_Equal;
    if (name == "/")         return ImGuiKey_Slash;
    if (name == "\\")        return ImGuiKey_Backslash;
    if (name == ";")         return ImGuiKey_Semicolon;
    if (name == "'")         return ImGuiKey_Apostrophe;
    if (name == ",")         return ImGuiKey_Comma;
    if (name == ".")         return ImGuiKey_Period;
    if (name == "`")         return ImGuiKey_GraveAccent;
    if (name == "F1")  return ImGuiKey_F1;
    if (name == "F2")  return ImGuiKey_F2;
    if (name == "F3")  return ImGuiKey_F3;
    if (name == "F4")  return ImGuiKey_F4;
    if (name == "F5")  return ImGuiKey_F5;
    if (name == "F6")  return ImGuiKey_F6;
    if (name == "F7")  return ImGuiKey_F7;
    if (name == "F8")  return ImGuiKey_F8;
    if (name == "F9")  return ImGuiKey_F9;
    if (name == "F10") return ImGuiKey_F10;
    if (name == "F11") return ImGuiKey_F11;
    if (name == "F12") return ImGuiKey_F12;

    // Single character A-Z
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return (ImGuiKey)(ImGuiKey_A + (c - 'A'));
        if (c >= 'a' && c <= 'z') return (ImGuiKey)(ImGuiKey_A + (c - 'a'));
        if (c >= '0' && c <= '9') return (ImGuiKey)(ImGuiKey_0 + (c - '0'));
    }

    return ImGuiKey_None;
}

KeyCombo KeyCombo::FromString(const std::string& s) {
    KeyCombo combo;
    if (s.empty()) return combo;

    // Split by '+'
    std::string remaining = s;
    while (true) {
        // Find the last '+' that is a separator (not the key itself)
        size_t pos = remaining.find('+');
        if (pos == std::string::npos) break;

        std::string part = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 1);

        if (part == "Ctrl")       combo.ctrl = true;
        else if (part == "Shift") combo.shift = true;
        else if (part == "Alt")   combo.alt = true;
        else {
            // Unexpected modifier — treat remaining as key
            combo.key = ParseKeyName(part);
            return combo;
        }
    }

    // remaining is the key name
    combo.key = ParseKeyName(remaining);
    return combo;
}

// ============================================================================
// Helper to build entries
// ============================================================================

static ShortcutEntry MakeEntry(const std::string& id, const std::string& label, const std::string& category,
                                KeyCombo primary, KeyCombo alt = {}, bool hold = false) {
    ShortcutEntry e;
    e.id = id;
    e.label = label;
    e.category = category;
    e.default_primary = primary;
    e.default_alt = alt;
    e.is_hold = hold;
    return e;
}

static KeyCombo K(ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false) {
    KeyCombo c;
    c.key = key;
    c.ctrl = ctrl;
    c.shift = shift;
    c.alt = alt;
    return c;
}

// ============================================================================
// Registry
// ============================================================================

void InitShortcutDefaults() {
    g_shortcuts.clear();

    // --- Playback ---
    g_shortcuts.push_back(MakeEntry("play_pause",     "Play / Pause",        "Playback", K(ImGuiKey_Space), K(ImGuiKey_W)));
    g_shortcuts.push_back(MakeEntry("frame_back",     "Step back one frame", "Playback", K(ImGuiKey_Q)));
    g_shortcuts.push_back(MakeEntry("frame_forward",  "Step forward one frame", "Playback", K(ImGuiKey_E)));
    g_shortcuts.push_back(MakeEntry("rewind",         "Rewind (hold)",       "Playback", K(ImGuiKey_A), {}, true));
    g_shortcuts.push_back(MakeEntry("fast_forward",   "Fast forward (hold)", "Playback", K(ImGuiKey_D), {}, true));
    g_shortcuts.push_back(MakeEntry("toggle_loop",    "Toggle loop",         "Playback", K(ImGuiKey_L)));
    g_shortcuts.push_back(MakeEntry("toggle_mute",    "Toggle mute",         "Playback", K(ImGuiKey_M)));
    g_shortcuts.push_back(MakeEntry("volume_up",      "Volume up",           "Playback", K(ImGuiKey_UpArrow)));
    g_shortcuts.push_back(MakeEntry("volume_down",    "Volume down",         "Playback", K(ImGuiKey_DownArrow)));
    g_shortcuts.push_back(MakeEntry("set_in_point",   "Set In point (toggle)", "Playback", K(ImGuiKey_I)));
    g_shortcuts.push_back(MakeEntry("set_out_point",  "Set Out point (toggle)", "Playback", K(ImGuiKey_O)));
    g_shortcuts.push_back(MakeEntry("cycle_background", "Cycle background",  "Playback", K(ImGuiKey_B)));
    g_shortcuts.push_back(MakeEntry("fullscreen",     "Toggle fullscreen",   "Playback", K(ImGuiKey_F)));

    // --- File / Project ---
    g_shortcuts.push_back(MakeEntry("open_file",      "Open file",           "File / Project", K(ImGuiKey_O, true)));
    g_shortcuts.push_back(MakeEntry("open_project",   "Open project",        "File / Project", K(ImGuiKey_O, true, true)));
    g_shortcuts.push_back(MakeEntry("save_project",   "Save project",        "File / Project", K(ImGuiKey_S, true)));
    g_shortcuts.push_back(MakeEntry("save_project_as","Save project as",     "File / Project", K(ImGuiKey_S, true, true)));
    g_shortcuts.push_back(MakeEntry("screenshot_clip","Screenshot to clipboard", "File / Project", K(ImGuiKey_LeftBracket, true)));
    g_shortcuts.push_back(MakeEntry("screenshot_desk","Screenshot to desktop",   "File / Project", K(ImGuiKey_RightBracket, true)));
    g_shortcuts.push_back(MakeEntry("delete_item",    "Remove selected items",   "File / Project", K(ImGuiKey_Delete)));
    g_shortcuts.push_back(MakeEntry("delete_node",    "Delete nodes (alt)",      "File / Project", K(ImGuiKey_X)));
    g_shortcuts.push_back(MakeEntry("rename_item",    "Rename selected item",    "File / Project", K(ImGuiKey_F2)));
    g_shortcuts.push_back(MakeEntry("new_playlist",   "New Playlist",            "File / Project", K(ImGuiKey_P, true, true)));
    g_shortcuts.push_back(MakeEntry("add_to_transcode","Add to transcode queue", "File / Project", K(ImGuiKey_Q, true, true)));
    g_shortcuts.push_back(MakeEntry("transcode_window","Toggle transcode window","File / Project", K(ImGuiKey_T, true, true)));

    // --- View / Panels ---
    g_shortcuts.push_back(MakeEntry("toggle_project",     "Toggle Project panel",     "View / Panels", K(ImGuiKey_1, true)));
    g_shortcuts.push_back(MakeEntry("toggle_inspector",   "Toggle Inspector panel",   "View / Panels", K(ImGuiKey_2, true)));
    g_shortcuts.push_back(MakeEntry("toggle_timeline",    "Toggle Timeline panel",    "View / Panels", K(ImGuiKey_3, true)));
    g_shortcuts.push_back(MakeEntry("toggle_color",       "Toggle Color Panels",      "View / Panels", K(ImGuiKey_4, true)));
    g_shortcuts.push_back(MakeEntry("toggle_annotations", "Toggle Annotations panel", "View / Panels", K(ImGuiKey_5, true)));
    g_shortcuts.push_back(MakeEntry("toggle_ann_toolbar", "Toggle Annotation toolbar","View / Panels", K(ImGuiKey_6, true)));
    g_shortcuts.push_back(MakeEntry("toggle_sidebar",     "Toggle Sidebar",           "View / Panels", K(ImGuiKey_7, true)));
    g_shortcuts.push_back(MakeEntry("default_view",       "Default view",             "View / Panels", K(ImGuiKey_0, true)));
    g_shortcuts.push_back(MakeEntry("show_all_panels",    "Show all panels",          "View / Panels", K(ImGuiKey_9, true)));
    g_shortcuts.push_back(MakeEntry("toggle_minimal",     "Toggle minimal view",      "View / Panels", K(ImGuiKey_Minus, true)));
    g_shortcuts.push_back(MakeEntry("toggle_colorspace",  "Toggle colorspace panel",  "View / Panels", K(ImGuiKey_C, true)));
    g_shortcuts.push_back(MakeEntry("toggle_safety",      "Toggle safety overlay",    "View / Panels", K(ImGuiKey_Slash, true)));
    g_shortcuts.push_back(MakeEntry("toggle_bg_panel",    "Toggle background panel",  "View / Panels", K(ImGuiKey_B, true, true)));
    g_shortcuts.push_back(MakeEntry("reset_layout",       "Reset layout",             "View / Panels", K(ImGuiKey_R, true)));

    // --- Annotations ---
    g_shortcuts.push_back(MakeEntry("escape_action",      "Exit fullscreen / Cancel", "Annotations", K(ImGuiKey_Escape)));
    g_shortcuts.push_back(MakeEntry("annotation_save",    "Save annotation",          "Annotations", K(ImGuiKey_Enter)));

    // --- Timeline (OTIO) ---
    g_shortcuts.push_back(MakeEntry("tl_cut_clips",       "Cut clips at playhead",    "Timeline", K(ImGuiKey_K, true)));
    g_shortcuts.push_back(MakeEntry("tl_delete_clips",    "Delete selected clips",    "Timeline", K(ImGuiKey_Delete), K(ImGuiKey_X)));
    g_shortcuts.push_back(MakeEntry("tl_undo",            "Undo",                     "Timeline", K(ImGuiKey_Z, true)));
    g_shortcuts.push_back(MakeEntry("tl_redo",            "Redo",                     "Timeline", K(ImGuiKey_Y, true), K(ImGuiKey_Z, true, true)));
    g_shortcuts.push_back(MakeEntry("tl_deselect",        "Deselect all",             "Timeline", K(ImGuiKey_Escape)));
    g_shortcuts.push_back(MakeEntry("tl_follow_playhead", "Toggle follow playhead",   "Timeline", K(ImGuiKey_F)));
    g_shortcuts.push_back(MakeEntry("tl_zoom_in",         "Zoom in",                  "Timeline", K(ImGuiKey_Equal)));
    g_shortcuts.push_back(MakeEntry("tl_zoom_out",        "Zoom out",                 "Timeline", K(ImGuiKey_Minus)));
    g_shortcuts.push_back(MakeEntry("tl_pan_left",        "Pan left",                 "Timeline", K(ImGuiKey_LeftArrow)));
    g_shortcuts.push_back(MakeEntry("tl_pan_right",       "Pan right",                "Timeline", K(ImGuiKey_RightArrow)));

    Debug::Log("Initialized " + std::to_string(g_shortcuts.size()) + " shortcut defaults");
}

std::vector<ShortcutEntry>& GetShortcutRegistry() {
    return g_shortcuts;
}

// ============================================================================
// Persistence
// ============================================================================

std::string GetShortcutsPath() {
    const char* localappdata = std::getenv("LOCALAPPDATA");
    if (localappdata) {
        std::string base_path = std::string(localappdata) + "\\qcview";
        std::filesystem::create_directories(base_path);
        return base_path + "\\shortcuts.qcv";
    }
    return "shortcuts.qcv";
}

void LoadShortcuts() {
    std::string path = GetShortcutsPath();
    if (!std::filesystem::exists(path)) {
        Debug::Log("No shortcuts file found, using defaults");
        return;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) return;

        nlohmann::json j = nlohmann::json::parse(file);
        file.close();

        if (!j.contains("shortcuts")) return;

        auto& shortcuts_json = j["shortcuts"];
        int loaded = 0;

        for (auto& entry : g_shortcuts) {
            // Check primary override
            if (shortcuts_json.contains(entry.id)) {
                std::string val = shortcuts_json[entry.id].get<std::string>();
                entry.user_primary = KeyCombo::FromString(val);
                entry.has_user_primary = true;
                loaded++;
            }
            // Check alt override
            std::string alt_id = entry.id + "_alt";
            if (shortcuts_json.contains(alt_id)) {
                std::string val = shortcuts_json[alt_id].get<std::string>();
                entry.user_alt = KeyCombo::FromString(val);
                entry.has_user_alt = true;
                loaded++;
            }
        }

        Debug::Log("Loaded " + std::to_string(loaded) + " shortcut overrides from " + path);
    } catch (const std::exception& e) {
        Debug::Log("Error loading shortcuts: " + std::string(e.what()));
    }
}

void SaveShortcuts() {
    try {
        nlohmann::json j;
        j["version"] = 1;
        nlohmann::json shortcuts_json;

        int saved = 0;
        for (auto& entry : g_shortcuts) {
            if (entry.has_user_primary) {
                shortcuts_json[entry.id] = entry.user_primary.ToString();
                saved++;
            }
            if (entry.has_user_alt) {
                shortcuts_json[entry.id + "_alt"] = entry.user_alt.ToString();
                saved++;
            }
        }

        if (saved == 0) {
            // No overrides — delete file if it exists
            std::string path = GetShortcutsPath();
            if (std::filesystem::exists(path)) {
                std::filesystem::remove(path);
                Debug::Log("Removed empty shortcuts file");
            }
            return;
        }

        j["shortcuts"] = shortcuts_json;

        std::string path = GetShortcutsPath();
        std::ofstream file(path);
        if (file.is_open()) {
            file << j.dump(2);
            file.close();
            Debug::Log("Saved " + std::to_string(saved) + " shortcut overrides to " + path);
        }
    } catch (const std::exception& e) {
        Debug::Log("Error saving shortcuts: " + std::string(e.what()));
    }
}

// ============================================================================
// Reset
// ============================================================================

void ResetAllShortcuts() {
    for (auto& entry : g_shortcuts) {
        entry.has_user_primary = false;
        entry.has_user_alt = false;
        entry.user_primary = {};
        entry.user_alt = {};
    }
    Debug::Log("Reset all shortcuts to defaults");
}

void ResetShortcut(const std::string& id) {
    for (auto& entry : g_shortcuts) {
        if (entry.id == id) {
            entry.has_user_primary = false;
            entry.has_user_alt = false;
            entry.user_primary = {};
            entry.user_alt = {};
            Debug::Log("Reset shortcut: " + id);
            return;
        }
    }
}

// ============================================================================
// Input checking
// ============================================================================

static bool CheckKeyCombo(const KeyCombo& combo, bool pressed, bool repeat) {
    if (!combo.IsSet()) return false;

    ImGuiIO& io = ImGui::GetIO();

    // Check modifiers match exactly
    if (combo.ctrl  != io.KeyCtrl)  return false;
    if (combo.shift != io.KeyShift) return false;
    if (combo.alt   != io.KeyAlt)   return false;

    if (pressed) {
        return ImGui::IsKeyPressed(combo.key, repeat);
    } else {
        return ImGui::IsKeyDown(combo.key);
    }
}

static ShortcutEntry* FindEntry(const std::string& id) {
    for (auto& entry : g_shortcuts) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

bool IsShortcutPressed(const std::string& id, bool repeat) {
    ShortcutEntry* entry = FindEntry(id);
    if (!entry) return false;

    KeyCombo primary = entry->GetActivePrimary();
    KeyCombo alt = entry->GetActiveAlt();

    return CheckKeyCombo(primary, true, repeat) || CheckKeyCombo(alt, true, repeat);
}

bool IsShortcutDown(const std::string& id) {
    ShortcutEntry* entry = FindEntry(id);
    if (!entry) return false;

    KeyCombo primary = entry->GetActivePrimary();
    KeyCombo alt = entry->GetActiveAlt();

    return CheckKeyCombo(primary, false, false) || CheckKeyCombo(alt, false, false);
}

// ============================================================================
// Capture helper for UI
// ============================================================================

KeyCombo CaptureKeyCombo() {
    KeyCombo combo;
    ImGuiIO& io = ImGui::GetIO();

    // Check all non-modifier keys
    for (int k = ImGuiKey_Tab; k < ImGuiKey_COUNT; k++) {
        ImGuiKey key = (ImGuiKey)k;

        // Skip modifier keys themselves
        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
            key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
            key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
            key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper)
            continue;

        // Skip internal/mouse keys
        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseX2)
            continue;

        if (ImGui::IsKeyPressed(key, false)) {
            combo.key = key;
            combo.ctrl = io.KeyCtrl;
            combo.shift = io.KeyShift;
            combo.alt = io.KeyAlt;
            return combo;
        }
    }

    return combo; // key == ImGuiKey_None means nothing captured yet
}
