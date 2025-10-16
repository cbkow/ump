#pragma once

namespace ump {
namespace FrameIndexing {

/**
 * Frame Indexing Utility
 *
 * This utility provides clear conversion between three indexing domains:
 * 1. File Domain: Actual frame numbers from image sequence files (can start at any number)
 * 2. Internal Domain: 0-based indexing used throughout the application
 * 3. Display Domain: 1-based indexing shown to users
 */

/**
 * Convert file frame number to internal 0-based index
 * @param file_frame Frame number as it appears in the filename
 * @param start_number First frame number in the sequence
 * @return 0-based internal index
 */
inline int FileFrameToInternal(int file_frame, int start_number) {
    return file_frame - start_number;
}

/**
 * Convert internal 0-based index to file frame number
 * @param internal_index 0-based index used internally
 * @param start_number First frame number in the sequence
 * @return Frame number as it appears in filenames
 */
inline int InternalToFileFrame(int internal_index, int start_number) {
    return internal_index + start_number;
}

/**
 * Convert internal 0-based index to display frame number (always 1-based)
 * @param internal_index 0-based index used internally
 * @return 1-based frame number for user display
 */
inline int InternalToDisplay(int internal_index) {
    return internal_index + 1;
}

/**
 * Convert display frame number (1-based) to internal index
 * @param display_frame 1-based frame number from user interface
 * @return 0-based internal index
 */
inline int DisplayToInternal(int display_frame) {
    return display_frame - 1;
}

/**
 * Convert file frame directly to display frame (bypassing internal conversion)
 * @param file_frame Frame number as it appears in filename
 * @param start_number First frame number in the sequence
 * @return 1-based frame number for display
 */
inline int FileFrameToDisplay(int file_frame, int start_number) {
    return InternalToDisplay(FileFrameToInternal(file_frame, start_number));
}

/**
 * Convert display frame directly to file frame (bypassing internal conversion)
 * @param display_frame 1-based frame number from user interface
 * @param start_number First frame number in the sequence
 * @return Frame number as it appears in filenames
 */
inline int DisplayToFileFrame(int display_frame, int start_number) {
    return InternalToFileFrame(DisplayToInternal(display_frame), start_number);
}

/**
 * Convert internal frame to appropriate display frame for image sequences
 * For image sequences, shows the actual file frame number
 * For regular videos, shows 1-based frame numbers
 * @param internal_index 0-based index used internally
 * @param start_number First frame number in the sequence (use 1 for regular videos)
 * @return Frame number for display (file frame for sequences, 1-based for videos)
 */
inline int InternalToSequenceDisplay(int internal_index, int start_number) {
    return InternalToFileFrame(internal_index, start_number);
}

} // namespace FrameIndexing
} // namespace ump