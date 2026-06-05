# Sage Unreal MCP For Claude

When asked to work on an Unreal Engine project through Sage, inspect the MCP
server before using editor mutation tools.

## First MCP Calls

1. `sage.about`
2. `sage.project.discover`
3. `sage.doctor`
4. `sage.capabilities`
5. `list_editors`
6. `sage.workflow.suggest`

## Repair Commands

```powershell
sage doctor <Project.uproject>
sage init <Project.uproject> --claude
sage update <Project.uproject>
```

Use `sage.workflow.suggest` to pick the smallest safe tool sequence for the
task.

## Deploy Or Update

For project repair or SageBridge deployment, use:

```powershell
sage doctor <Project.uproject>
sage update <Project.uproject>
```

Do not replace `Plugins/SageBridge` while the matching Unreal Editor project is
open. After reopening the editor, verify the bridge with `list_editors` or
`wait_for_editor`.
