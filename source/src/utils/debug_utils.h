#pragma once
#include <string>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <debugapi.h>
#endif

namespace Debug {
#ifdef _WIN32
    inline void Log(const std::string& message) {
        OutputDebugStringA((message + "\n").c_str());
        // Also print to console if available
        std::cout << message << std::endl;
    }
#else
    inline void Log(const std::string& message) {
        std::cout << message << std::endl;
    }
#endif
}