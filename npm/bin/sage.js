#!/usr/bin/env node
'use strict';

const { spawn } = require('node:child_process');
const path = require('node:path');

const { configureClaudeMcp } = require('../lib/claude');
const { configureCodexMcp } = require('../lib/codex');
const { ensureServerBinary } = require('../lib/download');
const {
  dataDir,
  packageRoot,
  packageVersion,
  resolveServerBinary,
} = require('../lib/paths');
const {
  doctor,
  installPlugin,
  normalizeUproject,
  projectRoot,
  readProject,
  upsertProjectMcpConfig,
} = require('../lib/install');

function usage() {
  return `Sage MCP

Usage:
  sage mcp [server args...]                 Start native server in MCP stdio mode
  sage server [--http] [server args...]     Start native server manually
  sage init <Project.uproject> [options]    Install plugin and project .mcp.json
  sage update                               Ensure native server binary is installed
  sage update --plugin <Project.uproject>   Install/update SageBridge in project
  sage doctor [Project.uproject] [--json]   Validate local install

Options for init/update --plugin:
  --plugin-source <path>   Use an unpacked SageBridge package/source directory
  --mcp-config <path>      Write MCP config to this file instead of <project>/.mcp.json
  --ue-root <path>         Add SAGE_UE_ROOT to generated MCP config
  --codex                  Also register this project in Codex CLI with codex mcp add
  --codex-name <name>      Codex MCP server name. Default: sage
  --codex-command <path>   Codex executable path. Default: codex
  --claude                 Also register this project in Claude Code with claude mcp add
  --claude-scope <scope>   Claude MCP scope: project, local, or user. Default: project
  --claude-name <name>     Claude MCP server name. Default: sage
  --claude-command <path>  Claude executable path. Default: claude
  --no-mcp-config          Do not write .mcp.json
`;
}

function parseOption(args, name) {
  const index = args.indexOf(name);
  if (index === -1) return null;
  const value = args[index + 1];
  if (!value || value.startsWith('--')) throw new Error(`${name} requires a value`);
  args.splice(index, 2);
  return value;
}

function hasFlag(args, name) {
  const index = args.indexOf(name);
  if (index === -1) return false;
  args.splice(index, 1);
  return true;
}

function requireSinglePositional(args, description) {
  if (args.length !== 1 || args[0].startsWith('--')) {
    throw new Error(`${description} is required`);
  }
  return args[0];
}

function childEnv(extra = {}) {
  return {
    ...process.env,
    SAGE_DATA_DIR: process.env.SAGE_DATA_DIR || dataDir(),
    SAGE_REPO_ROOT: process.env.SAGE_REPO_ROOT || packageRoot(),
    ...extra,
  };
}

function spawnServer(mode, args) {
  const binary = resolveServerBinary();
  if (!binary) {
    process.stderr.write('sage: native sage-server binary not found. Run `sage update` or set SAGE_SERVER_PATH.\n');
    process.exit(1);
  }

  let serverArgs = [...args];
  if (mode === 'http' && !serverArgs.includes('--http')) serverArgs.unshift('--http');
  if (mode === 'stdio') serverArgs = serverArgs.filter((arg) => arg !== '--stdio');

  const child = spawn(binary, serverArgs, {
    stdio: 'inherit',
    env: childEnv(),
    windowsHide: true,
  });
  child.on('exit', (code, signal) => {
    if (signal) {
      process.kill(process.pid, signal);
      return;
    }
    process.exit(code ?? 0);
  });
  child.on('error', (error) => {
    process.stderr.write(`sage: failed to start native server: ${error.message}\n`);
    process.exit(1);
  });
}

async function commandUpdate(args) {
  if (hasFlag(args, '--plugin')) {
    const pluginSource = parseOption(args, '--plugin-source');
    const projectPath = normalizeUproject(requireSinglePositional(args, 'Project .uproject path'));
    const result = await installPlugin(projectPath, { pluginSource, forceDownload: hasFlag(args, '--force') });
    process.stderr.write(`SageBridge installed: ${result.dest}\n`);
    if (result.backup) process.stderr.write(`Previous plugin backed up: ${result.backup}\n`);
    return;
  }
  const binary = await ensureServerBinary({ forceDownload: hasFlag(args, '--force') });
  process.stderr.write(`sage-server ready: ${binary}\n`);
}

async function commandInit(args) {
  const pluginSource = parseOption(args, '--plugin-source');
  const mcpConfigPath = parseOption(args, '--mcp-config');
  const ueRoot = parseOption(args, '--ue-root');
  const codexName = parseOption(args, '--codex-name') || 'sage';
  const codexCommand = parseOption(args, '--codex-command');
  const configureCodex = hasFlag(args, '--codex');
  const claudeName = parseOption(args, '--claude-name') || 'sage';
  const claudeScope = parseOption(args, '--claude-scope') || 'project';
  const claudeCommand = parseOption(args, '--claude-command');
  const configureClaude = hasFlag(args, '--claude');
  const noMcpConfig = hasFlag(args, '--no-mcp-config');
  const projectPath = normalizeUproject(requireSinglePositional(args, 'Project .uproject path'));
  const project = readProject(projectPath);
  const root = projectRoot(projectPath);
  const result = await installPlugin(projectPath, { pluginSource });

  let configPath = null;
  if (!noMcpConfig) {
    configPath = upsertProjectMcpConfig(projectPath, {
      mcpConfigPath,
      ueRoot,
    });
  }

  process.stderr.write(`Project: ${projectPath}\n`);
  process.stderr.write(`EngineAssociation: ${project.EngineAssociation || '<none>'}\n`);
  process.stderr.write(`SageBridge installed: ${result.dest}\n`);
  if (result.backup) process.stderr.write(`Previous plugin backed up: ${result.backup}\n`);
  if (configPath) process.stderr.write(`MCP config updated: ${configPath}\n`);
  if (configureCodex) {
    const env = {
      SAGE_PROJECT_ROOT: root,
      SAGE_REPO_ROOT: root,
    };
    if (ueRoot) env.SAGE_UE_ROOT = path.resolve(ueRoot);
    const codex = configureCodexMcp({ name: codexName, codexCommand, env });
    process.stderr.write(`Codex MCP server registered: ${codex.name}\n`);
  }
  if (configureClaude) {
    const env = {
      SAGE_PROJECT_ROOT: root,
      SAGE_REPO_ROOT: root,
    };
    if (ueRoot) env.SAGE_UE_ROOT = path.resolve(ueRoot);
    const claude = configureClaudeMcp({
      name: claudeName,
      claudeCommand,
      scope: claudeScope,
      cwd: root,
      env,
    });
    process.stderr.write(`Claude MCP server registered: ${claude.name} (${claude.scope})\n`);
  }
  process.stderr.write(`Project root: ${root}\n`);
}

function commandDoctor(args) {
  const json = hasFlag(args, '--json');
  const binary = resolveServerBinary();
  const checks = [
    { name: 'node', ok: true, version: process.version },
    { name: 'package', ok: true, version: packageVersion(), root: packageRoot() },
    { name: 'data_dir', ok: true, path: dataDir() },
    { name: 'server_binary', ok: Boolean(binary), path: binary || null },
  ];
  if (args[0]) checks.push(...doctor(args[0]));
  const ok = checks.every((check) => check.ok);
  if (json) {
    process.stdout.write(`${JSON.stringify({ ok, checks }, null, 2)}\n`);
  } else {
    for (const check of checks) {
      const suffix = check.path || check.version || check.root || '';
      process.stderr.write(`${check.ok ? 'OK ' : 'ERR'} ${check.name}${suffix ? `: ${suffix}` : ''}\n`);
    }
  }
  if (!ok) process.exit(1);
}

async function main() {
  const args = process.argv.slice(2);
  const command = args.shift();
  if (!command || command === '-h' || command === '--help') {
    process.stdout.write(usage());
    return;
  }
  if (command === '--version' || command === 'version') {
    process.stdout.write(`${packageVersion()}\n`);
    return;
  }
  if (command === 'mcp') {
    spawnServer('stdio', args);
    return;
  }
  if (command === 'server') {
    const stdio = hasFlag(args, '--stdio');
    spawnServer(stdio ? 'stdio' : 'http', args);
    return;
  }
  if (command === 'init') {
    await commandInit(args);
    return;
  }
  if (command === 'update') {
    await commandUpdate(args);
    return;
  }
  if (command === 'doctor') {
    commandDoctor(args);
    return;
  }
  process.stderr.write(`Unknown command: ${command}\n\n${usage()}`);
  process.exit(2);
}

main().catch((error) => {
  process.stderr.write(`sage: ${error.message}\n`);
  process.exit(1);
});
