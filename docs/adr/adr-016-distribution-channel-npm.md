# ADR-016: Distribution Channel - npm-first

**Date:** 2026-04-29
**Status:** Accepted

## Context

Sage ships three user-facing pieces:

1. `sage-server`: native C++23 binary.
2. `SageBridge`: Unreal Engine plugin package.
3. MCP client configuration for tools such as Codex, Claude Code, Cursor, or
   Cline.

Candidate channels:

- Homebrew, apt, or winget.
- GitHub Releases.
- Docker.
- npm.
- FAB / Unreal Marketplace for the plugin.

The primary audience is an AI-agent user who also develops in Unreal. That
audience already installs tools such as Codex CLI and Claude Code through npm.

## Decision

Use `@alemdarlabs/sage-mcp` as the primary distribution package:

```bash
npm install -g @alemdarlabs/sage-mcp
sage init
```

## Mechanism

1. The npm package exposes a `sage` CLI through the `bin` field.
2. `postinstall` resolves the current platform and downloads the matching
   native `sage-server` archive from GitHub Releases into `~/.sage-mcp`.
3. The Node.js wrapper launches the native server for MCP clients.
4. `sage init` installs or updates `SageBridge`, enables it in the target
   `.uproject`, and writes MCP configuration.

## Secondary Channels

- **GitHub Releases** for air-gapped, proxy, or manual install flows.
- **Homebrew tap** for macOS power users if demand justifies it.
- **FAB / Unreal Marketplace** for a later plugin-focused channel.

## Rationale

1. npm matches the install reflex of the AI development tooling audience.
2. One command works across Windows, macOS, and Linux.
3. npm already provides versioning, update, uninstall, and discovery behavior.
4. GitHub Actions can publish both release assets and the npm package.
5. The wrapper-binary pattern is proven in AI developer tools.

## Rejected Alternatives

- **Homebrew-first**: macOS-first and fragmented for Windows/Linux users.
- **GitHub Releases-only**: Requires manual PATH setup and manual updates.
- **Docker-first**: Complicates localhost IPC with Unreal Editor and is better
  suited to headless CI.
- **FAB-only**: Covers the Unreal plugin but not the native MCP server.
- **pip / PyPI**: Common for Python MCP servers, but not the best fit for
  Sage's target audience.

## Plugin Distribution Notes

The plugin is not embedded directly in the npm package by default. Engine and
platform combinations can make plugin artifacts large. `sage init`,
`sage update <project>`, and `sage update --plugin <project>` resolve the
correct plugin package and install it into `Plugins/SageBridge/`.

## Consequences

Positive:

- Cross-platform install through a single command.
- Low onboarding friction for AI development tool users.
- Existing npm semver and update workflows.
- GitHub Releases remain available for direct asset download.

Negative:

- Postinstall depends on network access unless the user sets an override.
- Some game developers may not have Node.js installed.
- Code signing and notarization remain necessary for mature binary
  distribution.
- Full per-engine plugin binary packaging needs self-hosted runners with Unreal
  Engine installed.

## Versioning

- npm package version equals `sage-server` version.
- Plugin versions can carry Unreal Engine build metadata.
- Server and plugin protocol versions must be negotiated during handshake.
- Incompatible versions should fail with a clear update instruction.
