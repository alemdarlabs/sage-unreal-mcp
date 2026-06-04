#pragma once

// JSON-RPC 2.0 standard codes + Sage-specific extensions.
// Reference: docs/engineering/api-spec.md (Error Codes).

namespace sage::mcp {

enum class ErrorCode : int {
    // JSON-RPC 2.0 standard ----------------------------------------------------
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,

    // Sage-specific (api-spec.md §Error Codes) --------------------------------
    GenericSage           = -32000,
    EditorNotConnected    = -32001,
    SlotNotFound          = -32002,
    VersionConflict       = -32003,
    PieActiveRejected     = -32004,
    SourceControlRequired = -32005,
    CompileError          = -32006,
    LiveCodingUnavailable = -32007,
    HardCapExceeded       = -32008,
};

[[nodiscard]] constexpr const char* describe(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ParseError:            return "Parse error";
        case ErrorCode::InvalidRequest:        return "Invalid request";
        case ErrorCode::MethodNotFound:        return "Method not found";
        case ErrorCode::InvalidParams:         return "Invalid params";
        case ErrorCode::InternalError:         return "Internal error";
        case ErrorCode::GenericSage:           return "Generic Sage error";
        case ErrorCode::EditorNotConnected:    return "Editor not connected";
        case ErrorCode::SlotNotFound:          return "Slot not found";
        case ErrorCode::VersionConflict:       return "Version conflict";
        case ErrorCode::PieActiveRejected:     return "PIE active, modification rejected";
        case ErrorCode::SourceControlRequired: return "Source control checkout required";
        case ErrorCode::CompileError:          return "Compile error";
        case ErrorCode::LiveCodingUnavailable: return "Live Coding unavailable";
        case ErrorCode::HardCapExceeded:       return "Hard cap exceeded, refine query";
    }
    return "Unknown error";
}

}  // namespace sage::mcp
