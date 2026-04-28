#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Phase 4.6 round 2 — Dialog policy + manual modal control.
 *
 * Without these, agent flows stall whenever UE pops a modal (save?, reload?,
 * confirm delete?) because the editor's main loop blocks until a human
 * clicks. We hook FCoreDelegates::ModalMessageDialog and pattern-match the
 * title/message; matching policies auto-respond, the rest fall through to
 * UE's default response (no/cancel) so we never silently say "yes".
 *
 *   editor.set_dialog_policy(pattern, response)
 *     Substring match against title or message. response ∈ {yes, no, ok,
 *     cancel, retry, continue, yesall, noall}. Replaces an existing policy
 *     with the same pattern. Lazy-installs the hook on first call.
 *
 *   editor.clear_dialog_policy(pattern?)
 *     Remove one (exact pattern) or all policies.
 *
 *   editor.get_dialog_policy()
 *     List active policies + hook install status.
 *
 *   editor.list_dialogs()
 *     Walk Slate to describe the active modal (title, body, buttons[]).
 *     Returns 0 or 1 dialog — UE only shows one at a time.
 *
 *   editor.respond_to_dialog(button_index|button_label|action)
 *     Click a button by index, by label substring, or send Escape.
 *     -32004 if no modal is currently active.
 */
SAGEBRIDGE_API void RegisterDialogTools(FSageToolDispatch& Dispatch);

/**
 * Module-level hook lifecycle. InstallDialogHook is called lazily by
 * set_dialog_policy on first use (so no policy = no behavioural change).
 * RemoveDialogHook is called from FSageBridgeModule::ShutdownModule.
 */
SAGEBRIDGE_API void InstallDialogHook();
SAGEBRIDGE_API void RemoveDialogHook();

}  // namespace sage::tools
