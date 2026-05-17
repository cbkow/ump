// ProjectIo — Phase 7.6.
//
// Free functions that serialize / deserialize a ProjectManager's
// in-memory state to a `.qcvproj` v2 JSON file. Path rewriting uses
// the `${PROJECT_DIR}` token so projects relocated to a new disk
// (zip + unzip) relink without manual fix-ups.
//
// Only fields that exist in the current in-memory model are written.
// The v2 schema in Guide 07 §9 lists optional future blocks (timeline,
// description, color_preset_last_used, snapshot mtimes) — those are
// left out until the underlying struct fields exist.
//
// Symmetry is the load-bearing invariant here: every field written
// in save() must round-trip back through load() such that an
// in-memory project saved + reloaded is equivalent to the original.

#pragma once

#include <QString>

namespace qcv {

class ProjectManager;

namespace ProjectIo {

constexpr const char *kSchemaTag     = "qcview.project.v2";
constexpr int         kSchemaVersion = 2;

// Returns true on success. On failure, fills *errorOut (when non-null)
// with a human-readable message and leaves the project file unchanged.
bool save(ProjectManager &pm,
          const QString  &filePath,
          QString        *errorOut = nullptr);

// Replaces pm's in-memory state from the file. Returns true on
// success. On failure, leaves pm in a clean state (newProject) and
// fills *errorOut (when non-null).
bool load(ProjectManager &pm,
          const QString  &filePath,
          QString        *errorOut = nullptr);

} // namespace ProjectIo
} // namespace qcv
