---
name: skill-creator
description: Generates or updates Codex Skills in Anthropic Agent Skills format with valid frontmatter, scoped trigger descriptions, and concise project-specific instructions. Invoke when adding or revising skills under .agents/skills/ in Sage.
---

# Skill Creator

## Overview

Produces ready-to-use Codex skills in Anthropic Agent Skills format. In this repository, canonical skills live under `.agents/skills/<name>/SKILL.md`.

## Format Reference

```markdown
---
name: <kebab-case-name>
description: <one-line trigger condition; 50-1024 chars>
---

# <Title Case Name>

## Overview
## Keywords
## Output Format
## Instructions
## Constraints
```

Frontmatter accepts only `name` and `description`.

## Instructions

- Pick a clear kebab-case `name` matching the folder name.
- Write `description` as an invocation hint: action + context + when to use.
- Keep bodies concise and operational; avoid session-history dumps.
- Use current repo paths: `AGENTS.md`, `docs/`, `.agents/skills/`, `server/`, `plugin/`, `scripts/`, `npm/`.
- Make platform guidance Windows-first when the workflow targets the current Sage production environment; include macOS/Linux fallback only when real and tested enough to be useful.
- For Sage-specific guidance, reference C++23, CMake/vcpkg, Unreal plugin packaging, MCP schemas, source-backed inspection, ADR-018 retired graph runtime, and npm-first distribution where relevant.

## Constraints

- Description must be 50-1024 characters.
- Frontmatter must contain only `name` and `description`.
- Do not point new skills at removed hidden workspaces.
- Do not write macOS-only recipes for Windows production workflows.
- Token discipline: skill body should be short enough to load without bloating context.
