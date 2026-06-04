'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { claudeMcpAddArgs, commandLine: claudeCommandLine } = require('../lib/claude');
const { codexMcpAddArgs, windowsCommandLine } = require('../lib/codex');

const repoRoot = path.resolve(__dirname, '..', '..');
const cli = path.join(repoRoot, 'npm', 'bin', 'sage.js');

function run(args, options = {}) {
  const result = spawnSync(process.execPath, [cli, ...args], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: {
      ...process.env,
      SAGE_SKIP_DOWNLOAD: '1',
      SAGE_DATA_DIR: options.dataDir || path.join(os.tmpdir(), 'sage-mcp-test-data'),
    },
  });
  if (options.allowFailure) return result;
  assert.equal(result.status, 0, `${result.stderr}\n${result.stdout}`);
  return result;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

const version = run(['--version']).stdout.trim();
assert.match(version, /^\d+\.\d+\.\d+$/);

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-cli-smoke-'));
const projectPath = path.join(tempRoot, 'SmokeProject.uproject');
const mcpPath = path.join(tempRoot, '.mcp.json');
fs.writeFileSync(
  projectPath,
  `${JSON.stringify({ FileVersion: 3, EngineAssociation: '5.7', Category: '', Description: '' }, null, 2)}\n`,
  'utf8'
);

run(['init', projectPath, '--plugin-source', path.join(repoRoot, 'plugin'), '--mcp-config', mcpPath]);

const project = readJson(projectPath);
assert.equal(project.Plugins.some((plugin) => plugin.Name === 'SageBridge' && plugin.Enabled === true), true);

const installedPlugin = path.join(tempRoot, 'Plugins', 'SageBridge');
assert.equal(fs.existsSync(path.join(installedPlugin, 'SageBridge.uplugin')), true);
assert.equal(fs.existsSync(path.join(installedPlugin, 'Source')), true);

const mcp = readJson(mcpPath);
assert.deepEqual(mcp.mcpServers.sage.command, 'sage');
assert.deepEqual(mcp.mcpServers.sage.args, ['mcp']);
assert.equal(mcp.mcpServers.sage.env.SAGE_PROJECT_ROOT, tempRoot);
assert.equal(mcp.mcpServers.sage.env.SAGE_REPO_ROOT, tempRoot);

assert.deepEqual(
  codexMcpAddArgs('sage-kale', {
    SAGE_PROJECT_ROOT: tempRoot,
    SAGE_REPO_ROOT: tempRoot,
    SAGE_UE_ROOT: 'C:\\UE_5.7',
  }),
  [
    'mcp',
    'add',
    'sage-kale',
    '--env',
    `SAGE_PROJECT_ROOT=${tempRoot}`,
    '--env',
    `SAGE_REPO_ROOT=${tempRoot}`,
    '--env',
    'SAGE_UE_ROOT=C:\\UE_5.7',
    '--',
    'sage',
    'mcp',
  ]
);
assert.equal(
  windowsCommandLine('codex', codexMcpAddArgs('sage-kale', {
    SAGE_UE_ROOT: 'C:\\Program Files\\Epic Games\\UE_5.7',
  })),
  'codex mcp add sage-kale --env "SAGE_UE_ROOT=C:\\Program Files\\Epic Games\\UE_5.7" -- sage mcp'
);
assert.deepEqual(
  claudeMcpAddArgs('sage-kale', {
    SAGE_PROJECT_ROOT: tempRoot,
    SAGE_REPO_ROOT: tempRoot,
    SAGE_UE_ROOT: 'C:\\UE_5.7',
  }),
  [
    'mcp',
    'add',
    'sage-kale',
    '--scope',
    'project',
    '-e',
    `SAGE_PROJECT_ROOT=${tempRoot}`,
    '-e',
    `SAGE_REPO_ROOT=${tempRoot}`,
    '-e',
    'SAGE_UE_ROOT=C:\\UE_5.7',
    '--',
    'sage',
    'mcp',
  ]
);
assert.equal(
  claudeCommandLine('claude', claudeMcpAddArgs('sage-kale', {
    SAGE_UE_ROOT: 'C:\\Program Files\\Epic Games\\UE_5.7',
  })),
  'claude mcp add sage-kale --scope project -e "SAGE_UE_ROOT=C:\\Program Files\\Epic Games\\UE_5.7" -- sage mcp'
);

const doctor = run(['doctor', projectPath, '--json']).stdout;
const report = JSON.parse(doctor);
assert.equal(report.ok, true);

const update = run(['update', '--plugin', projectPath, '--plugin-source', path.join(repoRoot, 'plugin')]);
assert.match(update.stderr, /SageBridge installed:/);
assert.equal(
  fs.readdirSync(path.join(tempRoot, 'Plugins')).some((entry) => entry.startsWith('SageBridge.backup-')),
  true
);

console.log('cli smoke ok');
