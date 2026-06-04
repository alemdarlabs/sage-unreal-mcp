# Sage npm distribution plan

## Goal

Make Sage usable on a fresh machine with one global npm install and one project init command:

```bash
npm install -g @alemdarlabs/sage-mcp
sage init D:\GameDev\Kale\Kale.uproject
```

The user should not manually keep a server terminal open for normal MCP use.

## Runtime model

Default mode is MCP-managed stdio:

```json
{
  "mcpServers": {
    "sage": {
      "command": "sage",
      "args": ["mcp"],
      "env": {
        "SAGE_PROJECT_ROOT": "<ProjectRoot>",
        "SAGE_REPO_ROOT": "<ProjectRoot>",
        "SAGE_UE_ROOT": "<UE install, when provided>"
      }
    }
  }
}
```

The MCP client starts `sage mcp` as a subprocess. The Node wrapper resolves and execs the native `sage-server` binary with stdio inherited, so JSON-RPC flows directly between client and native server. Logs must go to stderr or server log files, never stdout.

For project-scoped source tools, both `SAGE_PROJECT_ROOT` and `SAGE_REPO_ROOT` point at the Unreal project root in the generated config. The npm package install root is not used as the source workspace.

Optional shared mode remains available:

```bash
sage server --http
```

Use shared HTTP mode only for multi-client debugging, long-running shared sessions, or local observability.

## Package split

- npm package: small Node launcher, installer, and command surface.
- Native server binary: downloaded from release assets into `~/.sage-mcp/bin/<version>/<platform>/`.
- Unreal plugin package: downloaded from release assets into `~/.sage-mcp/plugins/<version>/<platform>/SageBridge`, then copied into target projects by `sage init` / `sage update --plugin`.
- Runtime data: `~/.sage-mcp`.

## Public release asset contract

Default asset base URL:

```text
https://github.com/alemdarlabs/sage-unreal-mcp/releases/download/v<version>
```

Runtime asset names:

```text
sage-server-<version>-<platform-key>.zip
sagebridge-plugin-<version>-<platform-key>.zip
```

For Windows x64 `0.1.0`:

```text
sage-server-0.1.0-win32-x64.zip
sagebridge-plugin-0.1.0-win32-x64.zip
```

Development/test overrides:

- `SAGE_BINARY_BASE_URL` / `SAGE_BINARY_URL`
- `SAGE_PLUGIN_BASE_URL` / `SAGE_PLUGIN_URL`
- `SAGE_SERVER_PATH`
- `SAGE_PLUGIN_SOURCE`
- `SAGE_SKIP_DOWNLOAD=1`

## Commands

| Command | Purpose |
|---|---|
| `sage mcp` | Start native server in stdio mode for MCP clients. |
| `sage server --http` | Start native server in HTTP+SSE mode. |
| `sage init <Project.uproject>` | Install plugin, enable it in `.uproject`, and write project `.mcp.json`. |
| `sage update` | Ensure native binary is installed for this package version. |
| `sage update --plugin <Project.uproject>` | Ensure a plugin package is available and install it into the project. |
| `sage doctor [Project.uproject]` | Validate binary, plugin, project, and MCP config paths. |
| `sage init <Project.uproject> --codex` | Also register `sage mcp` in Codex global MCP config with project env vars. |
| `sage init <Project.uproject> --claude` | Also register `sage mcp` with Claude Code using project scope by default. |
| `npm run release:check` | Run JS syntax, npm smoke, global install smoke, release asset extraction, and npm publish dry-run gates. |

## First implementation slice

1. Add `package.json` with `bin.sage`.
2. Add `npm/bin/sage.js` CLI.
3. Add resolver helpers for install root, package version, platform key, native binary path, project path, and plugin source path.
4. Implement `sage mcp` and `sage server --http` as transparent native process spawns.
5. Implement `sage init` for local/dev plugin source installs and project `.mcp.json` creation.
6. Implement `sage doctor` with machine-readable checks.
7. Implement postinstall/update hooks with release URL/env-var escape hatches, but keep offline/dev mode usable with `SAGE_SERVER_PATH` and `SAGE_PLUGIN_SOURCE`.
8. Add `npm/scripts/package-assets.js` to generate server/plugin release assets plus `checksums.txt`.
9. Add `.github/workflows/release-assets.yml` for tag/manual release asset publication.
10. Add `npm run test:global` to prove tarball install + postinstall download + `sage init` plugin download against a temporary project.
11. Publish `@alemdarlabs/sage-mcp` from tag builds with `NPM_TOKEN`; release assets are uploaded to the matching GitHub Release.
12. Add explicit Codex CLI registration via `sage init --codex`; project `.mcp.json` remains the default for Claude/Cursor-style workspace MCP configs.
13. Add explicit Claude Code registration via `sage init --claude` for users who prefer `claude mcp add` over passive project config discovery.
14. Add `npm run release:check` as the shared local/CI release gate.

## Known follow-ups

- Windows/macOS code-signing.
- Full macOS/Linux release matrix.
- Auth/license gate for runtime use after binary starts.
- Protocol/version compatibility check between server and plugin.
- `restart_editor` packaging contract: it currently expects `SAGE_REPO_ROOT` for build scripts. Binary distribution should either bundle the required scripts or make project-module rebuild use installed plugin/project paths directly.
