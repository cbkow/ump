#pragma once

// VideoPlayer Compatibility Header
// VideoPlayer has been replaced by VideoDisplayComponent.
// This header provides backward compatibility by aliasing VideoPlayer to VideoDisplayComponent.

#include "video_display_component.h"

// Alias VideoPlayer to VideoDisplayComponent for backward compatibility
using VideoPlayer = VideoDisplayComponent;
