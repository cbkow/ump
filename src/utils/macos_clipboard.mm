#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

#include "macos_clipboard.h"
#include "debug_utils.h"

bool CopyImageToClipboard(const std::vector<uint8_t>& png_data) {
    if (png_data.empty()) {
        Debug::Log("CopyImageToClipboard: Empty PNG data");
        return false;
    }

    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:png_data.data() length:png_data.size()];
        NSImage* image = [[NSImage alloc] initWithData:data];

        if (!image) {
            Debug::Log("CopyImageToClipboard: Failed to create NSImage from PNG data");
            return false;
        }

        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];

        BOOL success = [pasteboard writeObjects:@[image]];
        if (success) {
            Debug::Log("CopyImageToClipboard: Image copied to clipboard");
        } else {
            Debug::Log("CopyImageToClipboard: Failed to write to pasteboard");
        }

        return success == YES;
    }
}

void CleanupClipboardResources() {
    // No-op on macOS — NSPasteboard handles lifecycle
}

#endif // __APPLE__
