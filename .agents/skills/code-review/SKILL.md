---
name: code-review
description: Reviews code for bugs, inefficiencies, and adherence to best practices, providing actionable improvement suggestions. Invoke when reviewing C++ code, plugin source, or server implementation in Sage.
---

# Code Review

## Overview

Analyzes code to ensure quality, efficiency, and maintainability.

**Keywords**: code, review, bugs, optimization, best practices, c++, cpp

## Features

- Error detection
- Optimization recommendations
- Style enforcement
- Sanitizer-aware checks (ASan/UBSan/TSan)

## Output Format

- Issues found (severity, file, line)
- Suggested fixes with concrete code or rationale
- Optional summary

## Instructions

- Analyze code line by line
- Highlight errors, races, undefined behavior, leaks
- Suggest concrete improvements (modern C++23 idioms preferred)
- For UE plugin code: respect engine conventions (UCLASS, UPROPERTY, FScopedTransaction, GameThread vs WorkerThread discipline)
- For server code: prefer `std::expected<T, E>`, RAII, smart pointers; flag raw new/delete
- Reference `.Codex/docs/` and AGENTS.md `Code Conventions` for project-specific rules

## Constraints

- Maintain accuracy; verify claims against the actual code
- Avoid false positives — if unsure, mark as "potential" with reasoning
- Token discipline: limit to top 5 issues per call by default; full report on `verbose: true`
