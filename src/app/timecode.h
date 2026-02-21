#pragma once
// ============================================================================
// Timecode state, globals, and related declarations
// ============================================================================

#include <string>

// Timecode state enum
enum TimecodeState {
    NOT_CHECKED,        // Haven't looked for timecode yet
    CHECKING,           // Currently extracting metadata
    AVAILABLE,          // Found valid start timecode
    NOT_AVAILABLE       // No start timecode found
};

// Timecode globals
extern bool timecode_mode_enabled;
extern std::string cached_start_timecode;
extern bool start_timecode_checked;
extern TimecodeState timecode_state;

// Go To Timecode/Frame modal state
extern bool show_goto_timecode_modal;
extern bool goto_modal_preserve_pause_state;
extern bool goto_modal_was_playing;
extern char goto_timecode_hours[3];
extern char goto_timecode_minutes[3];
extern char goto_timecode_seconds[3];
extern char goto_timecode_frames[3];
extern char goto_frame_buffer[16];
extern bool goto_use_frame_input;
