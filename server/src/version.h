#pragma once

#include <string_view>

#ifndef SAGE_SERVER_VERSION
#define SAGE_SERVER_VERSION "0.0.0-dev"
#endif

#ifndef SAGE_PROTOCOL_VERSION
#define SAGE_PROTOCOL_VERSION "0.1.0"
#endif

namespace sage {

inline constexpr std::string_view kServerVersion = SAGE_SERVER_VERSION;
inline constexpr std::string_view kProtocolVersion = SAGE_PROTOCOL_VERSION;

}  // namespace sage
