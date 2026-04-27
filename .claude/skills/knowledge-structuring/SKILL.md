---
name: knowledge-structuring
description: Organizes unstructured information into clear frameworks, hierarchies, or structured notes. Invoke for graph schema design, ADR drafting, domain modeling, or taxonomy work in Sage.
---

# Knowledge Structuring

## Overview

Transforms messy input into structured, usable knowledge artifacts (schemas, taxonomies, ADRs, frameworks).

**Keywords**: knowledge, structuring, frameworks, organization, schema, hierarchy, taxonomy, adr

## Features

- Categorizes ideas into named groups
- Creates logical hierarchy (parent / child / sibling)
- Distinguishes entities, attributes, and relationships
- Highlights gaps and dependencies

## Output Format

- Structured framework with named sections
- Key points per section (bulleted, ≤7 per group)
- Optional notes / open questions block
- For schema work: explicit Entity / Attribute / Relationship layers

## Instructions

- Identify major topics first; reorganize content under them
- Group related ideas; prefer nesting up to 3 levels deep
- For graph schema (KuzuDB / Cypher in Sage): use Node / Edge / Property layers explicitly
- For ADR drafting: follow `.claude/decisions/` template — Bağlam, Kararlar (with sub-decisions), Sonuçlar
- Mark uncertain or disputed items explicitly

## Constraints

- Avoid ambiguity — every label must be unambiguous in context
- Maintain readability — no walls of bullets without structure
- Conform to existing project conventions if present
- Token discipline: tight bullets, expand on demand
