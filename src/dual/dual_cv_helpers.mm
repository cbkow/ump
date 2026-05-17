// dual_cv_helpers — APPLE-only CFRetain/CFRelease shims for
// CVPixelBufferRefs carried through DualFrame::cvPixelBuffer (a
// shared_ptr<void> with a deleter pointing at the release shim).
//
// Mirrors the qcv_decode pattern (frame_handle.mm) so DualVideoDecoder
// can stay a plain .cpp file: it calls these via extern "C" without
// needing CoreVideo includes.

#import <CoreVideo/CoreVideo.h>

extern "C" void dualCvPixelBufferRetain(void *cvPix)
{
    if (cvPix) CFRetain(static_cast<CVPixelBufferRef>(cvPix));
}

extern "C" void dualCvPixelBufferRelease(void *cvPix)
{
    if (cvPix) CFRelease(static_cast<CVPixelBufferRef>(cvPix));
}
