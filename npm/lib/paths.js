'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { platformKey, serverExeName } = require('./platform');

function packageRoot() {
  return path.resolve(__dirname, '..', '..');
}

function packageJson() {
  const raw = fs.readFileSync(path.join(packageRoot(), 'package.json'), 'utf8');
  return JSON.parse(raw);
}

function packageVersion() {
  return packageJson().version;
}

function dataDir() {
  return process.env.SAGE_DATA_DIR || path.join(os.homedir(), '.sage-mcp');
}

function binInstallDir(version = packageVersion(), key = platformKey()) {
  return path.join(dataDir(), 'bin', version, key);
}

function installedServerPath(version = packageVersion(), key = platformKey()) {
  return path.join(binInstallDir(version, key), serverExeName());
}

function pluginInstallDir(version = packageVersion(), key = platformKey()) {
  return path.join(dataDir(), 'plugins', version, key, 'SageBridge');
}

function devServerCandidates() {
  const root = packageRoot();
  return [
    path.join(root, 'build', 'release', 'bin', serverExeName()),
    path.join(root, 'build', 'RelWithDebInfo', 'bin', serverExeName()),
    path.join(root, 'build', 'debug', 'bin', serverExeName()),
  ];
}

function resolveServerBinary() {
  const candidates = [];
  if (process.env.SAGE_SERVER_PATH) candidates.push(process.env.SAGE_SERVER_PATH);
  candidates.push(...devServerCandidates());
  candidates.push(installedServerPath());
  for (const candidate of candidates) {
    if (candidate && fs.existsSync(candidate)) return path.resolve(candidate);
  }
  return null;
}

function pluginSourceCandidates() {
  const root = packageRoot();
  const candidates = [];
  if (process.env.SAGE_PLUGIN_SOURCE) candidates.push(process.env.SAGE_PLUGIN_SOURCE);
  candidates.push(pluginInstallDir());
  candidates.push(path.join(root, 'build', 'plugin'));
  candidates.push(path.join(root, 'plugin'));
  return candidates;
}

function resolvePluginSource(explicitPath) {
  const candidates = explicitPath ? [explicitPath] : pluginSourceCandidates();
  for (const candidate of candidates) {
    if (!candidate) continue;
    const abs = path.resolve(candidate);
    if (fs.existsSync(path.join(abs, 'SageBridge.uplugin'))) return abs;
  }
  return null;
}

module.exports = {
  binInstallDir,
  dataDir,
  devServerCandidates,
  installedServerPath,
  packageJson,
  packageRoot,
  packageVersion,
  pluginInstallDir,
  pluginSourceCandidates,
  resolvePluginSource,
  resolveServerBinary,
};
