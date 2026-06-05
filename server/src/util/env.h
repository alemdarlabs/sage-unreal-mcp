#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

namespace sage::util {

[[nodiscard]] inline std::string envValue(const char* name) {
    if (name == nullptr || *name == '\0') return {};

#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return {};
    }

    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return (value != nullptr) ? std::string{value} : std::string{};
#endif
}

}  // namespace sage::util
