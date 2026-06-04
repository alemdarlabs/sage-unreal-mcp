'use strict';

const { spawnSync } = require('node:child_process');

function quoteWindowsArg(value) {
  const text = String(value);
  if (!/[ \t"&()^|<>]/u.test(text)) return text;
  return `"${text.replace(/"/gu, '\\"')}"`;
}

function commandLine(command, args) {
  return [command, ...args].map(quoteWindowsArg).join(' ');
}

function claudeMcpAddArgs(name, env, scope = 'project') {
  const args = ['mcp', 'add', name, '--scope', scope];
  for (const [key, value] of Object.entries(env)) {
    if (value !== undefined && value !== null && value !== '') {
      args.push('-e', `${key}=${value}`);
    }
  }
  args.push('--', 'sage', 'mcp');
  return args;
}

function run(command, args, options = {}) {
  if (process.platform === 'win32') {
    return spawnSync(commandLine(command, args), {
      cwd: options.cwd,
      encoding: 'utf8',
      env: process.env,
      shell: true,
      stdio: options.stdio,
      windowsHide: true,
    });
  }
  return spawnSync(command, args, {
    cwd: options.cwd,
    encoding: 'utf8',
    env: process.env,
    stdio: options.stdio,
    windowsHide: true,
  });
}

function configureClaudeMcp(options) {
  const command = options.claudeCommand || process.env.SAGE_CLAUDE_COMMAND || 'claude';
  const name = options.name || 'sage';
  const scope = options.scope || 'project';
  const env = options.env || {};
  const cwd = options.cwd;

  if (!['project', 'local', 'user'].includes(scope)) {
    throw new Error(`unsupported Claude MCP scope: ${scope}`);
  }
  if (scope === 'project' && !cwd) {
    throw new Error('Claude project-scope MCP registration requires cwd');
  }

  run(command, ['mcp', 'remove', name], { cwd, stdio: 'ignore' });
  const add = run(command, claudeMcpAddArgs(name, env, scope), { cwd });
  if (add.status !== 0) {
    const detail = add.error ? add.error.message : `${add.stderr || add.stdout || ''}`.trim();
    throw new Error(`failed to configure Claude MCP server '${name}': ${detail || `exit ${add.status}`}`);
  }
  return { command, name, scope, stdout: add.stdout || '', stderr: add.stderr || '' };
}

module.exports = {
  claudeMcpAddArgs,
  commandLine,
  configureClaudeMcp,
};
