'use strict';

const { spawnSync } = require('node:child_process');

function codexMcpAddArgs(name, env) {
  const args = ['mcp', 'add', name];
  for (const [key, value] of Object.entries(env)) {
    if (value !== undefined && value !== null && value !== '') {
      args.push('--env', `${key}=${value}`);
    }
  }
  args.push('--', 'sage', 'mcp');
  return args;
}

function quoteWindowsArg(value) {
  const text = String(value);
  if (!/[ \t"&()^|<>]/u.test(text)) return text;
  return `"${text.replace(/"/gu, '\\"')}"`;
}

function windowsCommandLine(command, args) {
  return [command, ...args].map(quoteWindowsArg).join(' ');
}

function runCodex(command, args, options = {}) {
  if (process.platform === 'win32') {
    return spawnSync(windowsCommandLine(command, args), {
      encoding: 'utf8',
      env: process.env,
      shell: true,
      stdio: options.stdio,
      windowsHide: true,
    });
  }
  return spawnSync(command, args, {
    encoding: 'utf8',
    env: process.env,
    stdio: options.stdio,
    windowsHide: true,
  });
}

function configureCodexMcp(options) {
  const command = options.codexCommand || process.env.SAGE_CODEX_COMMAND || 'codex';
  const name = options.name || 'sage';
  const env = options.env || {};

  runCodex(command, ['mcp', 'remove', name], { stdio: 'ignore' });

  const add = runCodex(command, codexMcpAddArgs(name, env));
  if (add.status !== 0) {
    const detail = add.error ? add.error.message : `${add.stderr || add.stdout || ''}`.trim();
    throw new Error(`failed to configure Codex MCP server '${name}': ${detail || `exit ${add.status}`}`);
  }
  return { command, name, stdout: add.stdout || '', stderr: add.stderr || '' };
}

module.exports = {
  codexMcpAddArgs,
  configureCodexMcp,
  windowsCommandLine,
};
