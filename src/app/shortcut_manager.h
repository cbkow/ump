#pragma once

// ============================================================================
// Customizable Keyboard Shortcuts Manager
// ============================================================================

#include <string>
#include <vector>
#include <imgui.h>

struct KeyCombo {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool IsSet() const { return key != ImGuiKey_None; }
    bool operator==(const KeyCombo& other) const {
        return key == other.key && ctrl == other.ctrl && shift == other.shift && alt == other.alt;
    }
    bool operator!=(const KeyCombo& other) const { return !(*this == other); }
    std::string ToString() const;
    static KeyCombo FromString(const std::string& s);
};

struct ShortcutEntry {
    std::string id;              // "play_pause"
    std::string label;           // "Play / Pause"
    std::string category;        // "Playback"
    KeyCombo default_primary;
    KeyCombo default_alt;        // Optional second default binding
    KeyCombo user_primary;       // User override
    KeyCombo user_alt;           // User override for alt
    bool has_user_primary = false;  // Distinguishes "user cleared" from "not set"
    bool has_user_alt = false;
    bool is_hold = false;        // true for rewind/fast-forward (uses IsKeyDown)

    KeyCombo GetActivePrimary() const { return has_user_primary ? user_primary : default_primary; }
    KeyCombo GetActiveAlt() const { return has_user_alt ? user_alt : default_alt; }
};

// Registry management
void InitShortcutDefaults();
std::vector<ShortcutEntry>& GetShortcutRegistry();

// Persistence
std::string GetShortcutsPath();
void LoadShortcuts();
void SaveShortcuts();

// Reset
void ResetAllShortcuts();
void ResetShortcut(const std::string& id);

// Input checking
bool IsShortcutPressed(const std::string& id, bool repeat = true);
bool IsShortcutDown(const std::string& id);

// UI helper for press-to-capture
KeyCombo CaptureKeyCombo();
