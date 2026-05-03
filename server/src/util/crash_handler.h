#pragma once

namespace sage {

// Install Windows crash handlers:
//   - SetUnhandledExceptionFilter writes a full minidump to
//     $SAGE_DATA_DIR/crashdumps (or %LOCALAPPDATA%/CrashDumps/sage-server/
//     fallback) before the process unwinds.
//   - _set_invalid_parameter_handler turns the modal MessageBox raised by
//     debug-CRT precondition failures (xmemory:209 etc.) into a structured
//     stderr line + minidump, so the process exits non-interactively.
//   - _CrtSetReportMode + _set_abort_behavior keep abort() from popping
//     up Windows Error Reporting dialogs.
//
// Non-Windows: no-op.
//
// Call once at the very top of main() — before any logger or library
// init — so a precondition failure during static/dynamic init still
// produces a dump and never blocks on a modal dialog.
void installCrashHandlers();

}  // namespace sage
