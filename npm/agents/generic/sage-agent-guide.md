# Sage Agent Guide

Use this guide when an AI agent is asked to work on an Unreal Engine project
through Sage.

## First Calls

Call these MCP tools before editing:

1. `sage.about`
2. `sage.project.discover`
3. `sage.doctor`
4. `sage.capabilities`
5. `list_editors`
6. `sage.workflow.suggest` with the user's concrete intent

## Operating Rules

- Read before writing.
- Use `dry_run` when a tool supports it.
- Before Blueprint mutation, call `bp.full_dump`.
- Before destructive actions, require explicit user approval and pass
  `confirmed:true` only after approval.
- Treat edit success, asset save success, build success, server health, editor
  connection, and runtime proof as separate facts.
- If more than one editor is connected, pass `_editor` explicitly.
- Prefer typed Sage tools over `editor.run_python` for production workflows.

## Setup And Repair

Useful shell commands:

```powershell
npm install -g @alemdarlabs/sage-mcp@latest
sage setup codex
sage doctor <Project.uproject>
sage init <Project.uproject> --codex
sage update <Project.uproject>
```

When `sage.doctor` reports an old project plugin, run:

```powershell
sage update <Project.uproject>
```

When it reports an old CLI package, run:

```powershell
npm install -g @alemdarlabs/sage-mcp@latest
```

## Deploy Or Repair Sage In A Project

Use the product updater, not ad-hoc file copies:

1. `sage doctor <Project.uproject>`
2. `sage update <Project.uproject>` if the plugin is missing or stale
3. Reopen Unreal Editor if it was closed for the update
4. `list_editors` or `wait_for_editor`
5. `sage.workflow.suggest` for the user's actual task

If the user asks to deploy changes made through Sage tools, report these facts
separately: asset edit result, asset save result, build/test result, server
health, editor bridge health, and runtime proof.
