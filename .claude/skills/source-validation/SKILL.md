---
name: source-validation
description: Validates the credibility of information sources, highlighting reliability, relevance, and bias. Invoke when integrating external claims (forum posts, blog tutorials, GitHub issues, third-party docs) into Sage design decisions or ADRs.
---

# Source Validation

## Overview

Filters external information for trustworthiness before it influences design or implementation decisions.

**Keywords**: validation, sources, credibility, bias, research, references, citations

## Features

- Reliability tier classification
- Bias detection (vendor-pushed, marketing, unverifiable)
- Relevance filtering (matches Sage's UE / C++ / MCP context)
- Recency check (engine version, API stability)

## Output Format

- Source citation (URL / file / author)
- **Reliability tier**:
  - **primary** — Epic docs, UE source code, official MCP spec, Anthropic docs
  - **secondary** — recognized community authorities (Tom Looman, Ben Ui, official Discord excerpts)
  - **unverified** — random forum threads, AI-generated content, marketing
- **Relevance**: direct / adjacent / off-topic
- Notes on bias or freshness concerns

## Instructions

- Locate the original source (not aggregator/SEO copies)
- Check author credentials and publication date
- Cross-reference with primaries (Epic / Anthropic / MCP spec) when possible
- For Sage: prefer Epic's own UE source code (e.g., `Engine/Source/.../AssetRegistry/`) and Live Coding implementation over forum threads
- Mark "claimed but untested" findings explicitly

## Constraints

- Never accept marketing claims as primary (most "MCP server" comparisons online are inflated)
- Avoid unverified info as basis for ADR decisions
- Token discipline: tier + 1-line note per source; expand only on demand
