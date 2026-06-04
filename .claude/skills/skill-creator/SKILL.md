---
name: skill-creator
description: Generates new Claude Skills in Anthropic Agent Skills format with proper frontmatter, scoped description, and structured sections. Invoke when the user wants to add a new skill to .claude/skills/ in Sage.
---

# Skill Creator

## Overview

Produces ready-to-use Claude Skills in Anthropic Agent Skills format at `.claude/skills/<name>/SKILL.md`.

## Anthropic Format Reference

```markdown
---
name: <kebab-case-name>
description: <one-line trigger condition, 50-1024 chars>
---

# <Title Case Name>

## Overview
## Instructions
## Constraints
```

Frontmatter accepts only `name` and `description`.

## Output Format

- Folder name in kebab-case
- Full `SKILL.md` content
- Description tuned with an "Invoke when..." trigger

## Instructions

- Ask for the skill purpose and trigger condition when not provided.
- Pick a clear `name`; do not add a redundant `-skill` suffix.
- Write `description` as a model-invocation hint.
- Include Sage-specific guidance only when relevant: Unreal, C++23, CMake/vcpkg, MCP protocol, plugin packaging, source-backed inspection, ADR discipline.
- Do not mention KuzuDB as an active project dependency; it was removed by ADR-018.

## Constraints

- Description must be 50-1024 characters.
- Frontmatter only has `name` and `description`.
- Output must be drop-in ready.
- Keep the skill body concise.
