'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { pathToFileURL } = require('node:url');

const { resolveServerBinary } = require('../lib/paths');

const repoRoot = path.resolve(__dirname, '..', '..');
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-global-smoke-'));
const dataDir = path.join(tempRoot, 'data');
const prefix = path.join(tempRoot, 'prefix');
const releaseDir = path.join(tempRoot, 'release-assets');
const packDir = path.join(tempRoot, 'npm-pack');
const npmCli = path.join(path.dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js');
const sageCommand = process.platform === 'win32'
  ? path.join(prefix, 'sage.cmd')
  : path.join(prefix, 'bin', 'sage');
const installedCli = path.join(prefix, 'node_modules', '@alemdarlabs', 'sage-mcp', 'npm', 'bin', 'sage.js');
const globalBinDir = process.platform === 'win32' ? prefix : path.join(prefix, 'bin');

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd || repoRoot,
    encoding: 'utf8',
    env: {
      ...process.env,
      SAGE_DATA_DIR: dataDir,
      SAGE_BINARY_BASE_URL: pathToFileURL(releaseDir).toString(),
      SAGE_PLUGIN_BASE_URL: pathToFileURL(releaseDir).toString(),
      PATH: `${globalBinDir}${path.delimiter}${process.env.PATH || ''}`,
      ...(options.env || {}),
    },
  });
  if (options.allowFailure) return result;
  assert.equal(result.status, 0, `${command} ${args.join(' ')}\nSTDERR:\n${result.stderr}\nSTDOUT:\n${result.stdout}`);
  return result;
}

function runNpm(args) {
  return run(process.execPath, [npmCli, ...args]);
}

function runSage(args) {
  return run(process.execPath, [installedCli, ...args]);
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function makeFakeCodex(root) {
  fs.mkdirSync(root, { recursive: true });
  const log = path.join(root, 'codex-calls.jsonl');
  const script = path.join(root, 'fake-codex.js');
  fs.writeFileSync(script, `
const fs = require('node:fs');
const args = process.argv.slice(2);
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify(args) + '\\n', 'utf8');
if (args[0] === 'mcp' && args[1] === 'get') {
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

fs.mkdirSync(packDir, { recursive: true });
const fakeCodex = makeFakeCodex(globalBinDir);
const serverBinary = resolveServerBinary();
assert.ok(serverBinary, 'sage-server binary must be built before global install smoke');

run(process.execPath, [
  path.join(repoRoot, 'npm', 'scripts', 'package-assets.js'),
  '--out', releaseDir,
  '--server', serverBinary,
  '--plugin-source', path.join(repoRoot, 'plugin'),
]);

const pack = runNpm(['pack', '--pack-destination', packDir]);
const tarballName = pack.stdout.trim().split(/\r?\n/).filter(Boolean).pop();
assert.ok(tarballName, pack.stdout);
const tarball = path.join(packDir, tarballName);
assert.equal(fs.existsSync(tarball), true);

runNpm(['install', '-g', '--prefix', prefix, tarball]);
assert.equal(fs.existsSync(sageCommand), true);
assert.equal(fs.existsSync(installedCli), true);
const codexCalls = fs.readFileSync(fakeCodex.log, 'utf8').trim().split(/\r?\n/).map((line) => JSON.parse(line));
assert.deepEqual(codexCalls, [
  ['mcp', 'get', 'sage'],
  ['mcp', 'remove', 'sage'],
  ['mcp', 'add', 'sage', '--', 'sage', 'mcp'],
]);

const projectRoot = path.join(tempRoot, 'Project');
fs.mkdirSync(projectRoot, { recursive: true });
const fakeUeRoot = path.join(tempRoot, 'UE_5.7');
fs.mkdirSync(fakeUeRoot, { recursive: true });
const projectPath = path.join(projectRoot, 'GlobalSmoke.uproject');
fs.writeFileSync(
  projectPath,
  `${JSON.stringify({ FileVersion: 3, EngineAssociation: '5.7', Category: '', Description: '' }, null, 2)}\n`,
  'utf8'
);

runSage(['init', projectPath, '--ue-root', fakeUeRoot]);

const pluginRoot = path.join(projectRoot, 'Plugins', 'SageBridge');
assert.equal(fs.existsSync(path.join(pluginRoot, 'SageBridge.uplugin')), true);
assert.equal(fs.existsSync(path.join(pluginRoot, 'Source')), true);

const project = readJson(projectPath);
assert.equal(project.Plugins.some((plugin) => plugin.Name === 'SageBridge' && plugin.Enabled === true), true);

const mcpConfig = readJson(path.join(projectRoot, '.mcp.json'));
assert.equal(mcpConfig.mcpServers.sage.command, 'sage');
assert.deepEqual(mcpConfig.mcpServers.sage.args, ['mcp']);
assert.equal(mcpConfig.mcpServers.sage.env.SAGE_PROJECT_ROOT, projectRoot);
assert.equal(mcpConfig.mcpServers.sage.env.SAGE_REPO_ROOT, projectRoot);
assert.equal(mcpConfig.mcpServers.sage.env.SAGE_UE_ROOT, fakeUeRoot);

const doctor = runSage(['doctor', projectPath, '--json']);
const report = JSON.parse(doctor.stdout);
assert.equal(report.ok, true);

console.log('global install smoke ok');
