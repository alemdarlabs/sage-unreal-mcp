# Sage Unreal MCP For Codex

When the user asks to use Sage in an Unreal Engine project, use the `sage` MCP
server first. Do not assume project setup from memory.

## First MCP Calls

1. `sage.about`
2. `sage.project.discover`
3. `sage.doctor`
4. `sage.capabilities`
5. `list_editors`
6. `sage.workflow.suggest`

## If Setup Is Missing

Use shell commands only when the MCP checks show setup is missing or stale:

```powershell
sage doctor <Project.uproject>
sage init <Project.uproject> --codex
sage update <Project.uproject>
```

If Codex does not have the MCP server registered:

```powershell
sage setup codex
```

## Deploy Or Update Sage In A Project

When the user says to deploy or repair Sage in the current Unreal project:

1. Run `sage doctor <Project.uproject>` or call `sage.doctor`.
2. If the project plugin is missing or stale, run `sage update <Project.uproject>`.
3. If Unreal Editor is open for that project, close it first or ask the user to
   close it; the updater should refuse to replace `SageBridge` while the target
   editor process is running.
4. Recheck with `sage doctor <Project.uproject>`.
5. Verify editor connectivity with `list_editors` or `wait_for_editor` after
   the project is reopened.

## Safety

- Take `bp.full_dump` before major Blueprint mutation.
- Use `dry_run` where available.
- Do not pass `confirmed:true` unless the user explicitly approved the
  destructive action.
- Report file edits, save status, build/test status, server health, and editor
  runtime proof separately.
