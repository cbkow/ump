// MemoryMappedIStream — Phase 7.4.b.2 port of the old QCView's
// `direct_exr_cache.cpp:38-152` mmap reader. OpenEXR's
// `Imf::MultiPartInputFile(path)` constructor goes through plain
// file IO (read/seek system calls), which on heavy 4K/8K EXR
// sequences caps disk throughput well below the source bitrate.
// Wrapping reads in a memory-mapped region:
//
//   - macOS / Linux: mmap + madvise(MADV_SEQUENTIAL).
//   - Windows: CreateFileMapping + MapViewOfFile + PrefetchVirtualMemory.
//
// The kernel page-cache + sequential-read hints let OpenEXR
// decompress directly out of the mapped region with zero copy via
// `readMemoryMapped`. For non-mmap-aware codepaths (or when OpenEXR
// asks for a discontiguous range), `read` falls back to memcpy.
//
// Throws std::runtime_error on open / mmap failure (Imf convention;
// callers wrap construction in try/catch).

#pragma once

#include <OpenEXR/ImfIO.h>

#include <cstdint>
#include <string>

namespace qcv {

class MemoryMappedIStream : public Imf::IStream
{
public:
    explicit MemoryMappedIStream(const std::string &fileName);
    ~MemoryMappedIStream() override;

    MemoryMappedIStream(const MemoryMappedIStream &) = delete;
    MemoryMappedIStream &operator=(const MemoryMappedIStream &) = delete;

    bool       read(char c[], int n) override;
    char      *readMemoryMapped(int n) override;
    uint64_t   tellg() override;
    void       seekg(uint64_t pos) override;
    bool       isMemoryMapped() const override { return true; }

private:
    std::string m_filePath;
    char       *m_mappedData = nullptr;
    uint64_t    m_fileSize   = 0;
    uint64_t    m_currentPos = 0;

    // Platform handles. Stored as uintptr_t to keep this header
    // free of <windows.h> / <sys/mman.h>; the .cpp casts back to
    // the native types.
    intptr_t    m_fd      = -1;       // POSIX file descriptor
    void       *m_winFile = nullptr;  // Windows HANDLE
    void       *m_winMap  = nullptr;  // Windows HANDLE
};

} // namespace qcv
