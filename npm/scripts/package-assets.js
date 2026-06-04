#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const crypto = require('node:crypto');

const {
  pluginArchiveName,
  serverArchiveName,
} = require('../lib/download');
const {
  packageVersion,
  resolvePluginSource,
  resolveServerBinary,
} = require('../lib/paths');
const { platformKey, serverExeName } = require('../lib/platform');
const { copyRecursive, ensureDir } = require('../lib/file_ops');

function usage() {
  return `Usage:
  node npm/scripts/package-assets.js [options]

Options:
  --out <dir>             Output directory. Default: dist/release-assets/v<version>
  --server <path>         Native sage-server binary to package.
  --plugin-source <path>  SageBridge package/source directory to archive.
  --version <version>     Release version. Default: package.json version.
  --platform <key>        Platform key. Default: process platform key.
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

function archiveDir(sourceDir, archivePath) {
  ensureDir(path.dirname(archivePath));
  fs.rmSync(archivePath, { force: true });
  if (process.platform === 'win32') {
    const ps = spawnSync('powershell.exe', [
      '-NoProfile',
      '-ExecutionPolicy',
      'Bypass',
      '-Command',
      `$ErrorActionPreference='Stop'; Compress-Archive -Path ${JSON.stringify(path.join(sourceDir, '*'))} -DestinationPath ${JSON.stringify(archivePath)} -Force`,
    ], { stdio: 'inherit' });
    if (ps.status !== 0) throw new Error(`Compress-Archive failed with exit ${ps.status}`);
  } else {
    const tar = spawnSync('tar', ['-czf', archivePath, '-C', sourceDir, '.'], { stdio: 'inherit' });
    if (tar.status !== 0) throw new Error(`tar failed with exit ${tar.status}`);
  }
}

function sha256(file) {
  const hash = crypto.createHash('sha256');
  hash.update(fs.readFileSync(file));
  return hash.digest('hex');
}

function stageServer(tempRoot, serverPath) {
  const stage = path.join(tempRoot, 'server');
  ensureDir(stage);
  fs.copyFileSync(serverPath, path.join(stage, serverExeName()));
  if (process.platform !== 'win32') fs.chmodSync(path.join(stage, serverExeName()), 0o755);
  return stage;
}

function stagePlugin(tempRoot, pluginSource) {
  const stage = path.join(tempRoot, 'plugin');
  const pluginDest = path.join(stage, 'SageBridge');
  copyRecursive(pluginSource, pluginDest, {
    skipNames: new Set(['HostProject', 'Intermediate', 'Saved', 'DerivedDataCache']),
  });
  return stage;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes('-h') || args.includes('--help')) {
    process.stdout.write(usage());
    return;
  }

  const version = parseOption(args, '--version') || packageVersion();
  const key = parseOption(args, '--platform') || platformKey();
  const outDir = path.resolve(parseOption(args, '--out') || path.join('dist', 'release-assets', `v${version}`));
  const serverInput = parseOption(args, '--server') || resolveServerBinary();
  const pluginInput = parseOption(args, '--plugin-source') || resolvePluginSource();

  if (args.length) throw new Error(`Unexpected arguments: ${args.join(' ')}`);
  if (!serverInput) {
    throw new Error('sage-server binary not found. Build it first or pass --server <path>.');
  }
  const serverPath = path.resolve(serverInput);
  if (!fs.existsSync(serverPath) || !fs.statSync(serverPath).isFile()) {
    throw new Error(`sage-server binary is not a file: ${serverPath}`);
  }
  if (!pluginInput) {
    throw new Error('SageBridge plugin source/package not found. Build it or pass --plugin-source <path>.');
  }
  const pluginSource = path.resolve(pluginInput);
  if (!fs.existsSync(path.join(pluginSource, 'SageBridge.uplugin'))) {
    throw new Error(`SageBridge.uplugin not found under: ${pluginSource}`);
  }

  ensureDir(outDir);
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-release-assets-'));
  const serverArchive = path.join(outDir, serverArchiveName(version, key));
  const pluginArchive = path.join(outDir, pluginArchiveName(version, key));

  archiveDir(stageServer(tempRoot, serverPath), serverArchive);
  archiveDir(stagePlugin(tempRoot, pluginSource), pluginArchive);

  const checksums = [
    `${sha256(serverArchive)}  ${path.basename(serverArchive)}`,
    `${sha256(pluginArchive)}  ${path.basename(pluginArchive)}`,
  ];
  fs.writeFileSync(path.join(outDir, 'checksums.txt'), `${checksums.join('\n')}\n`, 'utf8');
  fs.rmSync(tempRoot, { recursive: true, force: true });

  process.stdout.write(JSON.stringify({
    out_dir: outDir,
    server_archive: serverArchive,
    plugin_archive: pluginArchive,
    checksums: path.join(outDir, 'checksums.txt'),
  }, null, 2));
  process.stdout.write('\n');
}

try {
  main();
} catch (error) {
  process.stderr.write(`package-assets failed: ${error.message}\n`);
  process.exit(1);
}
