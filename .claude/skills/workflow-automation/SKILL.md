---
name: workflow-automation
description: Breaks complex tasks into step-by-step workflows, mapping actions to tools and agents, optimizing execution. Invoke for multi-step implementation planning, refactoring decomposition, or build pipeline design.
---

# Workflow Automation

## Overview

Converts goals into actionable workflows for AI-assisted or human execution.

**Keywords**: automation, workflow, productivity, steps, execution, planning

## Features

- Task decomposition
- Tool / agent mapping
- Dependency ordering
- Risk identification per step

## Output Format

- Goal statement (one line)
- Numbered stepwise actions
- Tools / agents per step (e.g., `/cpp-architect`, `Bash`, `Edit`, `Task`)
- Dependencies between steps (which must finish before which)
- Verification criterion per step

## Instructions

- Identify the goal precisely (what does "done" mean?)
- Break into discrete, verifiable steps — each producing observable output
- Assign the right tool or agent per step
- Order steps by dependency; mark parallel-eligible steps
- For Sage: prefer `/unreal-architect`, `/cpp-architect`, `/mcp-protocol`, `/graph-architect` for domain-specific work

## Constraints

- Avoid vague instructions ("improve performance" → "reduce p99 query latency below 200ms")
- Maintain logical flow
- Each step must be verifiable
- Token discipline: keep step descriptions terse; expand on demand
