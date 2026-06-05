'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { claudeMcpAddArgs, commandLine: claudeCommandLine } = require('../lib/claude');
const { codexMcpAddArgs, windowsCommandLine } = require('../lib/codex');
const { samePath } = require('../lib/install');

const repoRoot = path.resolve(__dirname, '..', '..');
const cli = path.join(repoRoot, 'npm', 'bin', 'sage.js');

function run(args, options = {}) {
  const result = spawnSync(process.execPath, [cli, ...args], {
    cwd: options.cwd || repoRoot,
    encoding: 'utf8',
    env: {
      ...process.env,
      SAGE_SKIP_DOWNLOAD: '1',
      SAGE_SKIP_NPM_LATEST: '1',
      SAGE_DATA_DIR: options.dataDir || path.join(os.tmpdir(), 'sage-mcp-test-data'),
      ...(options.env || {}),
    },
  });
  if (options.allowFailure) return result;
  assert.equal(result.status, 0, `${result.stderr}\n${result.stdout}`);
  return result;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function makeFakeCodex(root, mode = 'missing') {
  const log = path.join(root, 'codex-calls.jsonl');
  const script = path.join(root, 'fake-codex.js');
  fs.writeFileSync(script, `
const fs = require('node:fs');
const args = process.argv.slice(2);
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify(args) + '\\n', 'utf8');
if (args[0] === 'mcp' && args[1] === 'get') {
  if (${JSON.stringify(mode)} === 'configured') {
    process.stdout.write('sage\\n  enabled: true\\n  transport: stdio\\n  command: sage\\n  args: mcp\\n');
    process.exit(0);
  }
  if (${JSON.stringify(mode)} === 'http') {
    process.stdout.write('sage\\n  enabled: true\\n  transport: streamable_http\\n  url: http://127.0.0.1:7777/mcp\\n');
    process.exit(0);
  }
  process.stderr.write('not found\\n');
  process.exit(1);
}
if (args[0] === 'mcp' && (args[1] === 'remove' || args[1] === 'add')) {
  process.exit(0);
}
process.stderr.write('unexpected fake codex args: ' + args.join(' ') + '\\n');
process.exit(2);
`, 'utf8');
  const command = process.platform === 'win32' ? path.join(root, 'codex.cmd') : path.join(root, 'codex');
  if (process.platform === 'win32') {
    fs.writeFileSync(command, `@echo off\r\n"${process.execPath}" "${script}" %*\r\n`, 'utf8');
  } else {
    fs.writeFileSync(command, `#!/bin/sh\n"${process.execPath}" "${script}" "$@"\n`, { mode: 0o755 });
  }
  return { command, log };
}

const version = run(['--version']).stdout.trim();
assert.match(version, /^\d+\.\d+\.\d+$/);

const codexGuide = run(['guide', 'codex', '--json']);
const codexGuideJson = JSON.parse(codexGuide.stdout);
assert.equal(codexGuideJson.agent, 'codex');
assert.match(codexGuideJson.content, /sage\.doctor/);

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
assert.equal(readJson(path.join(installedPlugin, 'SageBridge.uplugin')).VersionName, version);

const mcp = readJson(mcpPath);
assert.deepEqual(mcp.mcpServers.sage.command, 'sage');
assert.deepEqual(mcp.mcpServers.sage.args, ['mcp']);
assert.equal(mcp.mcpServers.sage.env.SAGE_PROJECT_ROOT, tempRoot);
assert.equal(mcp.mcpServers.sage.env.SAGE_REPO_ROOT, tempRoot);

assert.deepEqual(
  codexMcpAddArgs('sage-kale', {
    SAGE_UE_ROOT: 'C:\\UE_5.7',
  }),
  [
    'mcp',
    'add',
    'sage-kale',
    '--env',
    'SAGE_UE_ROOT=C:\\UE_5.7',
    '--',
    'sage',
    'mcp',
  ]
);

const fakeCodex = makeFakeCodex(tempRoot, 'http');
run(['setup', 'codex', '--codex-command', fakeCodex.command]);
const codexCalls = fs.readFileSync(fakeCodex.log, 'utf8').trim().split(/\r?\n/).map((line) => JSON.parse(line));
assert.deepEqual(codexCalls[0], ['mcp', 'get', 'sage']);
assert.deepEqual(codexCalls[1], ['mcp', 'remove', 'sage']);
assert.deepEqual(codexCalls[2], ['mcp', 'add', 'sage', '--', 'sage', 'mcp']);
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

const descriptorPath = path.join(installedPlugin, 'SageBridge.uplugin');
const oldDescriptor = readJson(descriptorPath);
oldDescriptor.VersionName = '0.0.1';
fs.writeFileSync(descriptorPath, `${JSON.stringify(oldDescriptor, null, 2)}\n`, 'utf8');
const outdatedDoctor = run(['doctor', projectPath, '--json'], { allowFailure: true });
assert.equal(outdatedDoctor.status, 1);
const outdatedReport = JSON.parse(outdatedDoctor.stdout);
assert.equal(outdatedReport.ok, false);
assert.equal(
  outdatedReport.checks.some((check) => (
    check.name === 'plugin_version'
    && check.installed_version === '0.0.1'
    && check.expected_version === version
    && check.status === 'outdated'
  )),
  true
);
assert.equal(
  outdatedReport.suggestions.some((suggestion) => (
    suggestion.id === 'update_project_plugin'
    && suggestion.command === `sage update "${projectPath}"`
  )),
  true
);

const projectUpdate = run(['update', projectPath, '--plugin-source', path.join(repoRoot, 'plugin')], {
  env: { SAGE_SERVER_PATH: process.execPath },
});
assert.match(projectUpdate.stderr, /sage-server ready:/);
assert.match(projectUpdate.stderr, /SageBridge version:/);
assert.equal(readJson(descriptorPath).VersionName, version);
assert.equal(JSON.parse(run(['doctor', projectPath, '--json']).stdout).ok, true);

const nestedDir = path.join(tempRoot, 'Content', 'Maps');
fs.mkdirSync(nestedDir, { recursive: true });
const discoveredDoctor = JSON.parse(run(['doctor', '--json'], { cwd: nestedDir }).stdout);
assert.equal(discoveredDoctor.ok, true);
assert.equal(discoveredDoctor.checks.some((check) => check.name === 'project_discovery' && samePath(check.path, projectPath)), true);

const bootstrapRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-bootstrap-smoke-'));
const bootstrapProjectPath = path.join(bootstrapRoot, 'BootstrapSmoke.uproject');
fs.writeFileSync(
  bootstrapProjectPath,
  `${JSON.stringify({ FileVersion: 3, EngineAssociation: '5.7', Category: '', Description: '' }, null, 2)}\n`,
  'utf8'
);
const bootstrapNested = path.join(bootstrapRoot, 'Source');
fs.mkdirSync(bootstrapNested, { recursive: true });
run(['bootstrap', '--plugin-source', path.join(repoRoot, 'plugin')], { cwd: bootstrapNested });
assert.equal(fs.existsSync(path.join(bootstrapRoot, 'Plugins', 'SageBridge', 'SageBridge.uplugin')), true);
assert.equal(fs.existsSync(path.join(bootstrapRoot, '.mcp.json')), false);
assert.equal(JSON.parse(run(['doctor', '--json'], { cwd: bootstrapNested }).stdout).ok, true);

const update = run(['update', '--plugin', projectPath, '--plugin-source', path.join(repoRoot, 'plugin')]);
assert.match(update.stderr, /SageBridge installed:/);
assert.equal(
  fs.readdirSync(path.join(tempRoot, 'Plugins')).some((entry) => entry.startsWith('SageBridge.backup-')),
  true
);

console.log('cli smoke ok');
