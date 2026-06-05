# Sage Unreal MCP Rules

- Use the `sage` MCP server for Unreal project inspection and editor actions.
- First call `sage.about`, `sage.project.discover`, `sage.doctor`,
  `sage.capabilities`, and `sage.workflow.suggest`.
- Verify an Unreal Editor bridge with `list_editors` before remote editor tools.
- Read before write. Use `dry_run` when available.
- Capture `bp.full_dump` before Blueprint mutation.
- Do not run destructive tools with `confirmed:true` without explicit user
  approval.
- Fix stale setup with `sage update <Project.uproject>`.
- For SageBridge deployment or repair, run `sage doctor <Project.uproject>`,
  then `sage update <Project.uproject>` when needed, and verify with
  `list_editors` after the editor reopens.
