#include "memory_mapped_istream.h"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <memoryapi.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace qcv {

MemoryMappedIStream::MemoryMappedIStream(const std::string &fileName)
    : Imf::IStream(fileName.c_str())
    , m_filePath(fileName)
{
#if defined(_WIN32)
    // ---- Windows: CreateFileMapping + MapViewOfFile.
    // FILE_FLAG_SEQUENTIAL_SCAN nudges the cache manager to
    // prefetch ahead of the read cursor.
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(),
                                         -1, nullptr, 0);
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1,
                        wpath.data(), wlen);

    HANDLE hFile = CreateFileW(
        wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("MemoryMappedIStream: cannot open " + fileName);
    }
    m_winFile = hFile;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)) {
        CloseHandle(hFile); m_winFile = nullptr;
        throw std::runtime_error(
            "MemoryMappedIStream: cannot get size " + fileName);
    }
    m_fileSize = static_cast<uint64_t>(size.QuadPart);

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY,
                                     0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile); m_winFile = nullptr;
        throw std::runtime_error(
            "MemoryMappedIStream: cannot create mapping " + fileName);
    }
    m_winMap = hMap;

    m_mappedData = static_cast<char *>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    if (!m_mappedData) {
        CloseHandle(hMap);  m_winMap  = nullptr;
        CloseHandle(hFile); m_winFile = nullptr;
        throw std::runtime_error(
            "MemoryMappedIStream: cannot MapViewOfFile " + fileName);
    }

    // Prefetch hint — Windows 8+. Best-effort; ignore failure.
    WIN32_MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = m_mappedData;
    range.NumberOfBytes  = m_fileSize;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
    // ---- macOS / Linux: mmap + madvise.
    int fd = ::open(fileName.c_str(), O_RDONLY);
    if (fd == -1) {
        throw std::runtime_error("MemoryMappedIStream: cannot open " + fileName);
    }
    m_fd = fd;

    struct stat st;
    if (::fstat(fd, &st) == -1) {
        ::close(fd); m_fd = -1;
        throw std::runtime_error(
            "MemoryMappedIStream: cannot stat " + fileName);
    }
    m_fileSize = static_cast<uint64_t>(st.st_size);

    void *p = ::mmap(nullptr, m_fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd); m_fd = -1;
        throw std::runtime_error(
            "MemoryMappedIStream: cannot mmap " + fileName);
    }
    m_mappedData = static_cast<char *>(p);

    // Sequential-read hint to the kernel — encourages aggressive
    // read-ahead. Best-effort; failure is non-fatal.
    ::madvise(m_mappedData, m_fileSize, MADV_SEQUENTIAL);
#endif
}

MemoryMappedIStream::~MemoryMappedIStream()
{
#if defined(_WIN32)
    if (m_mappedData) UnmapViewOfFile(m_mappedData);
    if (m_winMap)     CloseHandle(static_cast<HANDLE>(m_winMap));
    if (m_winFile)    CloseHandle(static_cast<HANDLE>(m_winFile));
#else
    if (m_mappedData) ::munmap(m_mappedData, m_fileSize);
    if (m_fd != -1)   ::close(static_cast<int>(m_fd));
#endif
}

bool MemoryMappedIStream::read(char c[], int n)
{
    if (m_currentPos + static_cast<uint64_t>(n) > m_fileSize) {
        throw std::runtime_error(
            "MemoryMappedIStream: read past end of file " + m_filePath);
    }
    std::memcpy(c, m_mappedData + m_currentPos, static_cast<size_t>(n));
    m_currentPos += static_cast<uint64_t>(n);
    // Imf::IStream::read contract: return true if MORE data may
    // be read, false at EOF.
    return m_currentPos < m_fileSize;
}

char *MemoryMappedIStream::readMemoryMapped(int n)
{
    if (m_currentPos + static_cast<uint64_t>(n) > m_fileSize) {
        throw std::runtime_error(
            "MemoryMappedIStream: readMemoryMapped past end of file " +
            m_filePath);
    }
    char *ptr = m_mappedData + m_currentPos;
    m_currentPos += static_cast<uint64_t>(n);
    return ptr;
}

uint64_t MemoryMappedIStream::tellg()
{
    return m_currentPos;
}

void MemoryMappedIStream::seekg(uint64_t pos)
{
    m_currentPos = pos;
}

} // namespace qcv
