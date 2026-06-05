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
  packageRoot,
  resolvePluginSource,
  resolveServerBinary,
} = require('../lib/paths');
const { archiveExt, platformKey, runtimeLibraryExtensions, serverExeName } = require('../lib/platform');
const { copyRecursive, ensureDir, readJson, writeJson } = require('../lib/file_ops');

function usage() {
  return `Usage:
  node npm/scripts/package-assets.js [options]

Options:
  --out <dir>             Output directory. Default: dist/release-assets/v<version>
  --server <path>         Native sage-server binary to package.
  --plugin-source <path>  SageBridge package/source directory to archive.
  --version <version>     Release version. Default: package.json version.
  --platform <key>        Platform key. Default: process platform key.
  --skip-server           Do not produce a sage-server archive.
  --skip-plugin           Do not produce a SageBridge source archive.
  --no-checksums          Do not write checksums.txt.
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

function parseFlag(args, name) {
  const index = args.indexOf(name);
  if (index === -1) return false;
  args.splice(index, 1);
  return true;
}

function archiveFormat(archivePath) {
  const lower = archivePath.toLowerCase();
  if (lower.endsWith('.zip')) return 'zip';
  if (lower.endsWith('.tar.gz') || lower.endsWith('.tgz')) return 'tar.gz';
  throw new Error(`Unsupported archive format: ${archivePath}`);
}

function archiveDir(sourceDir, archivePath) {
  ensureDir(path.dirname(archivePath));
  fs.rmSync(archivePath, { force: true });
  if (archiveFormat(archivePath) === 'zip') {
    if (process.platform !== 'win32') {
      const zip = spawnSync('zip', ['-qry', archivePath, '.'], { cwd: sourceDir, stdio: 'inherit' });
      if (zip.status !== 0) throw new Error(`zip failed with exit ${zip.status}`);
      return;
    }

    const ps = spawnSync('powershell.exe', [
      '-NoProfile',
      '-ExecutionPolicy',
      'Bypass',
      '-Command',
      `$ErrorActionPreference='Stop'; Compress-Archive -Path ${JSON.stringify(path.join(sourceDir, '*'))} -DestinationPath ${JSON.stringify(archivePath)} -Force`,
    ], { stdio: 'inherit' });
    if (ps.status !== 0) throw new Error(`Compress-Archive failed with exit ${ps.status}`);
    return;
  }

  const tar = spawnSync('tar', ['-czf', archivePath, '-C', sourceDir, '.'], { stdio: 'inherit' });
  if (tar.status !== 0) throw new Error(`tar failed with exit ${tar.status}`);
}

function sha256(file) {
  const hash = crypto.createHash('sha256');
  hash.update(fs.readFileSync(file));
  return hash.digest('hex');
}

function isRuntimeLibrary(name, key) {
  const lower = name.toLowerCase();
  if (key.startsWith('linux-')) return lower.includes('.so');
  return runtimeLibraryExtensions(key).some((ext) => lower.endsWith(ext));
}

function stageServer(tempRoot, serverPath, key) {
  const stage = path.join(tempRoot, 'server');
  ensureDir(stage);
  fs.copyFileSync(serverPath, path.join(stage, serverExeName(key)));
  for (const entry of fs.readdirSync(path.dirname(serverPath), { withFileTypes: true })) {
    if (entry.isFile() && isRuntimeLibrary(entry.name, key)) {
      fs.copyFileSync(path.join(path.dirname(serverPath), entry.name), path.join(stage, entry.name));
    }
  }
  if (archiveExt(key) !== 'zip') fs.chmodSync(path.join(stage, serverExeName(key)), 0o755);
  return stage;
}

function patchPluginDescriptorVersion(pluginDest, version) {
  const descriptorPath = path.join(pluginDest, 'SageBridge.uplugin');
  const descriptor = readJson(descriptorPath);
  descriptor.VersionName = version;
  writeJson(descriptorPath, descriptor);
}

function stagePlugin(tempRoot, pluginSource, version) {
  const stage = path.join(tempRoot, 'plugin');
  const pluginDest = path.join(stage, 'SageBridge');
  copyRecursive(pluginSource, pluginDest, {
    skipNames: new Set(['HostProject', 'Intermediate', 'Saved', 'DerivedDataCache']),
  });
  patchPluginDescriptorVersion(pluginDest, version);
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
  const skipServer = parseFlag(args, '--skip-server');
  const skipPlugin = parseFlag(args, '--skip-plugin');
  const skipChecksums = parseFlag(args, '--no-checksums');
  const serverInput = skipServer ? null : (parseOption(args, '--server') || resolveServerBinary());
  const pluginSourceOption = parseOption(args, '--plugin-source');
  const defaultPluginSource = path.join(packageRoot(), 'plugin');
  const pluginInput = skipPlugin
    ? null
    : (pluginSourceOption
      || (fs.existsSync(path.join(defaultPluginSource, 'SageBridge.uplugin')) ? defaultPluginSource : resolvePluginSource()));

  if (args.length) throw new Error(`Unexpected arguments: ${args.join(' ')}`);
  if (skipServer && skipPlugin) {
    throw new Error('Nothing to package: --skip-server and --skip-plugin cannot both be set.');
  }
  if (!skipServer && !serverInput) {
    throw new Error('sage-server binary not found. Build it first or pass --server <path>.');
  }
  const serverPath = serverInput ? path.resolve(serverInput) : null;
  if (serverPath && (!fs.existsSync(serverPath) || !fs.statSync(serverPath).isFile())) {
    throw new Error(`sage-server binary is not a file: ${serverPath}`);
  }
  if (!skipPlugin && !pluginInput) {
    throw new Error('SageBridge plugin source/package not found. Build it or pass --plugin-source <path>.');
  }
  const pluginSource = pluginInput ? path.resolve(pluginInput) : null;
  if (pluginSource && !fs.existsSync(path.join(pluginSource, 'SageBridge.uplugin'))) {
    throw new Error(`SageBridge.uplugin not found under: ${pluginSource}`);
  }

  ensureDir(outDir);
  const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-release-assets-'));
  const serverArchive = skipServer ? null : path.join(outDir, serverArchiveName(version, key));
  const pluginArchive = skipPlugin ? null : path.join(outDir, pluginArchiveName(version));
  const checksums = [];

  if (serverArchive) {
    archiveDir(stageServer(tempRoot, serverPath, key), serverArchive);
    checksums.push(`${sha256(serverArchive)}  ${path.basename(serverArchive)}`);
  }

  if (pluginArchive) {
    archiveDir(stagePlugin(tempRoot, pluginSource, version), pluginArchive);
    checksums.push(`${sha256(pluginArchive)}  ${path.basename(pluginArchive)}`);
  }

  const checksumsPath = path.join(outDir, 'checksums.txt');
  if (!skipChecksums) {
    fs.writeFileSync(checksumsPath, `${checksums.join('\n')}\n`, 'utf8');
  }
  fs.rmSync(tempRoot, { recursive: true, force: true });

  process.stdout.write(JSON.stringify({
    out_dir: outDir,
    server_archive: serverArchive,
    plugin_archive: pluginArchive,
    checksums: skipChecksums ? null : checksumsPath,
  }, null, 2));
  process.stdout.write('\n');
}

try {
  main();
} catch (error) {
  process.stderr.write(`package-assets failed: ${error.message}\n`);
  process.exit(1);
}
