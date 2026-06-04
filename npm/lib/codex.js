'use strict';

const { spawnSync } = require('node:child_process');

function codexMcpAddArgs(name, env = {}) {
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

function codexMcpGet(command, name) {
  return runCodex(command, ['mcp', 'get', name]);
}

function codexServerIsSageStdio(output) {
  const text = String(output || '');
  return /transport:\s*stdio/u.test(text)
    && /command:\s*sage/u.test(text)
    && /args:\s*mcp/u.test(text);
}

function configureCodexMcp(options = {}) {
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

function ensureCodexMcp(options = {}) {
  const command = options.codexCommand || process.env.SAGE_CODEX_COMMAND || 'codex';
  const name = options.name || 'sage';
  const env = options.env || {};
  const replace = options.replace !== false;

  const get = codexMcpGet(command, name);
  if (get.error) {
    return {
      ok: false,
      status: 'codex_unavailable',
      command,
      name,
      message: get.error.message,
    };
  }

  if (get.status === 0 && codexServerIsSageStdio(`${get.stdout || ''}\n${get.stderr || ''}`)) {
    return {
      ok: true,
      status: 'already_configured',
      command,
      name,
    };
  }

  if (get.status === 0 && !replace) {
    return {
      ok: false,
      status: 'existing_different',
      command,
      name,
      message: 'A Codex MCP server with this name already exists and is not configured as `sage mcp`.',
    };
  }

  if (get.status !== 0) {
    const detail = `${get.stderr || get.stdout || ''}`.trim();
    if (/unknown command|unrecognized|not recognized|command not found|invalid|Usage:/iu.test(detail)) {
      return {
        ok: false,
        status: 'codex_unavailable',
        command,
        name,
        message: detail,
      };
    }
  }

  try {
    const configured = configureCodexMcp({ codexCommand: command, name, env });
    return {
      ok: true,
      status: get.status === 0 ? 'reconfigured' : 'configured',
      command: configured.command,
      name: configured.name,
      stdout: configured.stdout,
      stderr: configured.stderr,
    };
  } catch (error) {
    return {
      ok: false,
      status: 'configure_failed',
      command,
      name,
      message: error.message,
    };
  }
}

module.exports = {
  codexMcpAddArgs,
  configureCodexMcp,
  codexServerIsSageStdio,
  ensureCodexMcp,
  windowsCommandLine,
};
