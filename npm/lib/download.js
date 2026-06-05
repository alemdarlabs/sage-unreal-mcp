'use strict';

const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { fileURLToPath } = require('node:url');

const { archiveExt, platformKey, serverExeName } = require('./platform');
const {
  binInstallDir,
  installedServerPath,
  packageVersion,
  packageRoot,
  pluginInstallDir,
} = require('./paths');
const { copyRecursive, ensureDir } = require('./file_ops');

function defaultReleaseBaseUrl(version = packageVersion()) {
  return `https://github.com/alemdarlabs/sage-unreal-mcp/releases/download/v${version}`;
}

function serverArchiveName(version = packageVersion(), key = platformKey()) {
  return `sage-server-${version}-${key}.${archiveExt(key)}`;
}

function serverArchiveUrl(version = packageVersion(), key = platformKey()) {
  const base = process.env.SAGE_BINARY_BASE_URL || defaultReleaseBaseUrl(version);
  return `${base.replace(/\/$/, '')}/${serverArchiveName(version, key)}`;
}

function pluginArchiveName(version = packageVersion()) {
  return `sagebridge-plugin-${version}-source.tar.gz`;
}

function legacyPluginArchiveName(version = packageVersion(), key = platformKey()) {
  return `sagebridge-plugin-${version}-${key}.${archiveExt(key)}`;
}

function pluginArchiveUrl(version = packageVersion()) {
  const base = process.env.SAGE_PLUGIN_BASE_URL
    || process.env.SAGE_BINARY_BASE_URL
    || defaultReleaseBaseUrl(version);
  return `${base.replace(/\/$/, '')}/${pluginArchiveName(version)}`;
}

function legacyPluginArchiveUrl(version = packageVersion(), key = platformKey()) {
  const base = process.env.SAGE_PLUGIN_BASE_URL
    || process.env.SAGE_BINARY_BASE_URL
    || defaultReleaseBaseUrl(version);
  return `${base.replace(/\/$/, '')}/${legacyPluginArchiveName(version, key)}`;
}

function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    ensureDir(path.dirname(dest));
    let parsed = null;
    try {
      parsed = new URL(url);
    } catch {
      parsed = null;
    }
    if (!parsed || parsed.protocol === 'file:') {
      const source = parsed ? fileURLToPath(parsed) : path.resolve(url);
      try {
        fs.copyFileSync(source, dest);
        resolve();
      } catch (error) {
        reject(error);
      }
      return;
    }
    const transport = parsed.protocol === 'http:' ? http : https;
    const file = fs.createWriteStream(dest);
    const request = transport.get(url, (response) => {
      if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
        file.close();
        fs.rmSync(dest, { force: true });
        const redirect = new URL(response.headers.location, url).toString();
        downloadFile(redirect, dest).then(resolve, reject);
        return;
      }
      if (response.statusCode !== 200) {
        file.close();
        fs.rmSync(dest, { force: true });
        reject(new Error(`Download failed ${response.statusCode}: ${url}`));
        return;
      }
      response.pipe(file);
      file.on('finish', () => {
        file.close(resolve);
      });
    });
    request.on('error', (error) => {
      file.close();
      fs.rmSync(dest, { force: true });
      reject(error);
    });
  });
}

function archiveFormat(archivePath) {
  const lower = archivePath.toLowerCase();
  if (lower.endsWith('.zip')) return 'zip';
  if (lower.endsWith('.tar.gz') || lower.endsWith('.tgz')) return 'tar.gz';
  throw new Error(`Unsupported archive format: ${archivePath}`);
}

function extractZipArchive(archivePath, destDir) {
  if (process.platform === 'win32') {
    const ps = spawnSync('powershell.exe', [
      '-NoProfile',
      '-ExecutionPolicy',
      'Bypass',
      '-Command',
      `Expand-Archive -LiteralPath ${JSON.stringify(archivePath)} -DestinationPath ${JSON.stringify(destDir)} -Force`,
    ], { stdio: 'inherit' });
    if (ps.status !== 0) throw new Error(`Expand-Archive failed with exit ${ps.status}`);
    return;
  }

  const unzip = spawnSync('unzip', ['-q', archivePath, '-d', destDir], { stdio: 'inherit' });
  if (unzip.status !== 0) throw new Error(`unzip extraction failed with exit ${unzip.status}`);
}

function extractArchive(archivePath, destDir) {
  ensureDir(destDir);
  if (archiveFormat(archivePath) === 'zip') {
    extractZipArchive(archivePath, destDir);
    return;
  }

  const tar = spawnSync('tar', ['-xzf', archivePath, '-C', destDir], { stdio: 'inherit' });
  if (tar.status !== 0) throw new Error(`tar extraction failed with exit ${tar.status}`);
}

function findExtractedServer(destDir, key = platformKey()) {
  const expectedName = serverExeName(key);
  const direct = path.join(destDir, expectedName);
  if (fs.existsSync(direct)) return direct;
  const stack = [destDir];
  while (stack.length) {
    const current = stack.pop();
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const child = path.join(current, entry.name);
      if (entry.isDirectory()) stack.push(child);
      else if (entry.name === expectedName) return child;
    }
  }
  return null;
}

async function downloadFirstAvailable(candidates, tmpDir) {
  let lastError = null;
  for (const candidate of candidates) {
    const archivePath = path.join(tmpDir, candidate.name);
    try {
      await downloadFile(candidate.url, archivePath);
      return archivePath;
    } catch (error) {
      fs.rmSync(archivePath, { force: true });
      lastError = error;
    }
  }

  throw lastError || new Error('No archive candidates were available.');
}

function archiveNameFromUrlOrPath(value, fallback) {
  try {
    const parsed = new URL(value);
    const name = path.basename(parsed.pathname);
    if (name) return name;
  } catch {
    const name = path.basename(value);
    if (name) return name;
  }

  return fallback;
}

function findExtractedPlugin(destDir) {
  const direct = path.join(destDir, 'SageBridge.uplugin');
  if (fs.existsSync(direct)) return destDir;
  const stack = [destDir];
  while (stack.length) {
    const current = stack.pop();
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const child = path.join(current, entry.name);
      if (!entry.isDirectory()) continue;
      if (fs.existsSync(path.join(child, 'SageBridge.uplugin'))) return child;
      stack.push(child);
    }
  }
  return null;
}

async function ensureServerBinary(options = {}) {
  const version = options.version || packageVersion();
  const key = options.platformKey || platformKey();
  const target = installedServerPath(version, key);
  if (fs.existsSync(target)) return target;

  if (process.env.SAGE_SERVER_PATH && fs.existsSync(process.env.SAGE_SERVER_PATH)) {
    return process.env.SAGE_SERVER_PATH;
  }

  const devBinary = path.join(packageRoot(), 'build', 'debug', 'bin', serverExeName());
  if (fs.existsSync(devBinary) && !options.forceDownload) return devBinary;

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-server-'));
  const archivePath = path.join(tmpDir, serverArchiveName(version, key));
  const url = process.env.SAGE_BINARY_URL || serverArchiveUrl(version, key);
  await downloadFile(url, archivePath);
  const extractDir = path.join(tmpDir, 'extract');
  extractArchive(archivePath, extractDir);
  const extracted = findExtractedServer(extractDir, key);
  if (!extracted) throw new Error(`Archive did not contain ${serverExeName(key)}`);
  const installDir = binInstallDir(version, key);
  ensureDir(installDir);
  for (const entry of fs.readdirSync(path.dirname(extracted), { withFileTypes: true })) {
    if (entry.isFile()) {
      fs.copyFileSync(path.join(path.dirname(extracted), entry.name), path.join(installDir, entry.name));
    }
  }
  if (process.platform !== 'win32') fs.chmodSync(target, 0o755);
  return target;
}

async function ensurePluginPackage(options = {}) {
  const version = options.version || packageVersion();
  const key = options.platformKey || platformKey();
  const target = pluginInstallDir(version, 'source');
  if (fs.existsSync(path.join(target, 'SageBridge.uplugin')) && !options.forceDownload) return target;

  if (process.env.SAGE_PLUGIN_SOURCE && fs.existsSync(path.join(process.env.SAGE_PLUGIN_SOURCE, 'SageBridge.uplugin'))) {
    return process.env.SAGE_PLUGIN_SOURCE;
  }

  const devPlugin = path.join(packageRoot(), 'build', 'plugin');
  if (fs.existsSync(path.join(devPlugin, 'SageBridge.uplugin')) && !options.forceDownload) return devPlugin;

  const sourcePlugin = path.join(packageRoot(), 'plugin');
  if (fs.existsSync(path.join(sourcePlugin, 'SageBridge.uplugin')) && !options.forceDownload) return sourcePlugin;

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-plugin-'));
  const archivePath = process.env.SAGE_PLUGIN_URL
    ? path.join(tmpDir, archiveNameFromUrlOrPath(process.env.SAGE_PLUGIN_URL, pluginArchiveName(version)))
    : await downloadFirstAvailable([
      { name: pluginArchiveName(version), url: pluginArchiveUrl(version) },
      { name: legacyPluginArchiveName(version, key), url: legacyPluginArchiveUrl(version, key) },
    ], tmpDir);
  if (process.env.SAGE_PLUGIN_URL) {
    await downloadFile(process.env.SAGE_PLUGIN_URL, archivePath);
  }
  const extractDir = path.join(tmpDir, 'extract');
  extractArchive(archivePath, extractDir);
  const extracted = findExtractedPlugin(extractDir);
  if (!extracted) throw new Error('Plugin archive did not contain SageBridge.uplugin');
  fs.rmSync(target, { recursive: true, force: true });
  ensureDir(path.dirname(target));
  copyRecursive(extracted, target, {
    skipNames: new Set(['HostProject', 'Intermediate', 'Saved', 'DerivedDataCache']),
  });
  return target;
}

module.exports = {
  defaultReleaseBaseUrl,
  downloadFile,
  ensurePluginPackage,
  ensureServerBinary,
  extractArchive,
  legacyPluginArchiveName,
  legacyPluginArchiveUrl,
  pluginArchiveName,
  pluginArchiveUrl,
  serverArchiveName,
  serverArchiveUrl,
};
