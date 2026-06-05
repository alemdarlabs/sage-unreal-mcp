'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { ensurePluginPackage } = require('./download');
const { backupExistingDir, copyRecursive, ensureDir, readJson, writeJson } = require('./file_ops');
const { packageVersion, resolvePluginSource } = require('./paths');

function normalizeUproject(projectArg) {
  if (!projectArg) throw new Error('Project .uproject path is required');
  const projectPath = path.resolve(projectArg);
  if (!projectPath.endsWith('.uproject')) {
    throw new Error(`Expected a .uproject file: ${projectPath}`);
  }
  if (!fs.existsSync(projectPath)) {
    throw new Error(`Project file not found: ${projectPath}`);
  }
  return projectPath;
}

function readProject(projectPath) {
  return readJson(projectPath);
}

function projectRoot(projectPath) {
  return path.dirname(projectPath);
}

function canonicalPath(value) {
  if (!value) return null;
  const resolved = path.resolve(value);
  try {
    return fs.realpathSync.native(resolved);
  } catch {
    return resolved;
  }
}

function samePath(left, right) {
  const a = canonicalPath(left);
  const b = canonicalPath(right);
  if (!a || !b) return false;
  return process.platform === 'win32'
    ? a.toLowerCase() === b.toLowerCase()
    : a === b;
}

function engineAssociation(project) {
  return project.EngineAssociation || '';
}

function pluginDescriptorPath(pluginDir) {
  return path.join(pluginDir, 'SageBridge.uplugin');
}

function readPluginDescriptor(pluginDir) {
  const descriptor = pluginDescriptorPath(pluginDir);
  if (!fs.existsSync(descriptor)) return null;
  return readJson(descriptor);
}

function pluginDescriptorVersion(descriptor) {
  if (!descriptor || typeof descriptor !== 'object') return null;
  return typeof descriptor.VersionName === 'string' && descriptor.VersionName.trim()
    ? descriptor.VersionName.trim()
    : null;
}

function parseComparableVersion(version) {
  if (!version || typeof version !== 'string') return null;
  const core = version.trim().split(/[+-]/, 1)[0];
  const parts = core.split('.');
  if (!parts.length || parts.length > 4) return null;
  const out = parts.map((part) => {
    if (!/^\d+$/.test(part)) return null;
    return Number.parseInt(part, 10);
  });
  if (out.some((part) => part === null || Number.isNaN(part))) return null;
  while (out.length < 3) out.push(0);
  return out;
}

function compareVersions(left, right) {
  const a = parseComparableVersion(left);
  const b = parseComparableVersion(right);
  if (!a || !b) return null;
  for (let i = 0; i < Math.max(a.length, b.length); i += 1) {
    const av = a[i] || 0;
    const bv = b[i] || 0;
    if (av < bv) return -1;
    if (av > bv) return 1;
  }
  return 0;
}

function pluginVersionStatus(pluginDir, expectedVersion = packageVersion()) {
  const descriptor = readPluginDescriptor(pluginDir);
  const installedVersion = pluginDescriptorVersion(descriptor);
  const comparison = compareVersions(installedVersion, expectedVersion);
  let status = 'missing';
  if (installedVersion && comparison === 0) status = 'current';
  else if (installedVersion && comparison !== null && comparison < 0) status = 'outdated';
  else if (installedVersion && comparison !== null && comparison > 0) status = 'newer_than_cli';
  else if (installedVersion) status = 'unknown';
  return {
    descriptor,
    installedVersion,
    expectedVersion,
    comparison,
    status,
    ok: status === 'current',
    updateAvailable: status === 'outdated',
  };
}

function findRunningEditorProcesses(projectPath) {
  if (process.platform !== 'win32') return [];
  const resolved = path.resolve(projectPath);
  const needles = [
    resolved.toLowerCase(),
    resolved.replace(/\\/g, '/').toLowerCase(),
  ];
  const result = spawnSync('powershell.exe', [
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-Command',
    "Get-CimInstance Win32_Process -Filter \"Name = 'UnrealEditor.exe'\" | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress",
  ], { encoding: 'utf8' });
  if (result.status !== 0 || !result.stdout.trim()) return [];
  let rows = [];
  try {
    const parsed = JSON.parse(result.stdout);
    rows = Array.isArray(parsed) ? parsed : [parsed];
  } catch {
    return [];
  }
  return rows
    .filter((row) => {
      const commandLine = String(row.CommandLine || '').toLowerCase();
      return needles.some((needle) => commandLine.includes(needle));
    })
    .map((row) => ({
      pid: Number(row.ProcessId),
      commandLine: String(row.CommandLine || ''),
    }));
}

function ensurePluginEnabled(projectPath) {
  const project = readProject(projectPath);
  if (!Array.isArray(project.Plugins)) project.Plugins = [];
  const existing = project.Plugins.find((plugin) => plugin && plugin.Name === 'SageBridge');
  if (existing) {
    existing.Enabled = true;
  } else {
    project.Plugins.push({ Name: 'SageBridge', Enabled: true });
  }
  writeJson(projectPath, project);
}

async function installPlugin(projectPath, options = {}) {
  if (options.requireEditorClosed) {
    const running = findRunningEditorProcesses(projectPath);
    if (running.length) {
      const pids = running.map((processInfo) => processInfo.pid).join(', ');
      throw new Error(`Unreal Editor is running for ${projectPath}. Close it before updating SageBridge. Running PID(s): ${pids}`);
    }
  }

  let source = resolvePluginSource(options.pluginSource);
  if (!source) {
    source = await ensurePluginPackage({ forceDownload: options.forceDownload });
  }

  const dest = path.join(projectRoot(projectPath), 'Plugins', 'SageBridge');
  const sourceVersion = pluginDescriptorVersion(readPluginDescriptor(source));
  ensureDir(path.dirname(dest));
  const backup = backupExistingDir(dest);
  copyRecursive(source, dest, {
    skipNames: new Set(['HostProject', 'Intermediate', 'Saved', 'DerivedDataCache']),
  });
  ensurePluginEnabled(projectPath);
  const installedVersion = pluginDescriptorVersion(readPluginDescriptor(dest));
  return { source, dest, backup, sourceVersion, installedVersion };
}

function mcpConfigPath(projectPath) {
  return path.join(projectRoot(projectPath), '.mcp.json');
}

function upsertProjectMcpConfig(projectPath, options = {}) {
  const configPath = options.mcpConfigPath ? path.resolve(options.mcpConfigPath) : mcpConfigPath(projectPath);
  let config = {};
  if (fs.existsSync(configPath)) config = readJson(configPath);
  if (!config.mcpServers || typeof config.mcpServers !== 'object') config.mcpServers = {};

  const root = projectRoot(projectPath);
  const env = {
    SAGE_PROJECT_ROOT: root,
    SAGE_REPO_ROOT: root,
  };
  if (options.ueRoot) env.SAGE_UE_ROOT = path.resolve(options.ueRoot);

  config.mcpServers.sage = {
    command: 'sage',
    args: ['mcp'],
    env,
  };
  writeJson(configPath, config);
  return configPath;
}

function doctor(projectArg) {
  const checks = [];
  let projectPath = null;
  if (projectArg) {
    projectPath = normalizeUproject(projectArg);
    const project = readProject(projectPath);
    const pluginDir = path.join(projectRoot(projectPath), 'Plugins', 'SageBridge');
    const pluginDescriptor = path.join(pluginDir, 'SageBridge.uplugin');
    const sourceDir = path.join(pluginDir, 'Source');
    const configPath = mcpConfigPath(projectPath);
    const config = fs.existsSync(configPath) ? readJson(configPath) : {};
    const sageConfig = config.mcpServers && config.mcpServers.sage;
    const sageEnv = sageConfig && sageConfig.env && typeof sageConfig.env === 'object' ? sageConfig.env : {};
    checks.push({ name: 'project', ok: true, path: projectPath, engine: engineAssociation(project) });
    checks.push({ name: 'plugin_dir', ok: fs.existsSync(pluginDir), path: pluginDir });
    checks.push({ name: 'plugin_descriptor', ok: fs.existsSync(pluginDescriptor), path: pluginDescriptor });
    checks.push({ name: 'plugin_source', ok: fs.existsSync(sourceDir), path: sourceDir });
    const version = pluginVersionStatus(pluginDir);
    checks.push({
      name: 'plugin_version',
      ok: version.ok,
      path: pluginDescriptor,
      installed_version: version.installedVersion,
      expected_version: version.expectedVersion,
      status: version.status,
      update_available: version.updateAvailable,
    });
    checks.push({
      name: 'plugin_enabled',
      ok: Array.isArray(project.Plugins) && project.Plugins.some((p) => p && p.Name === 'SageBridge' && p.Enabled === true),
      path: projectPath,
    });
    const configExists = fs.existsSync(configPath);
    checks.push({ name: 'mcp_config', ok: true, path: configPath, present: configExists, optional: true });
    if (configExists) {
      checks.push({
        name: 'mcp_server_sage',
        ok: !sageConfig || Boolean(sageConfig.command === 'sage' && Array.isArray(sageConfig.args) && sageConfig.args[0] === 'mcp'),
        path: configPath,
        present: Boolean(sageConfig),
        optional: true,
      });
      if (sageConfig) {
        checks.push({
          name: 'mcp_env_project_root',
          ok: samePath(sageEnv.SAGE_PROJECT_ROOT, projectRoot(projectPath)),
          path: configPath,
          value: sageEnv.SAGE_PROJECT_ROOT || null,
        });
        checks.push({
          name: 'mcp_env_repo_root',
          ok: samePath(sageEnv.SAGE_REPO_ROOT, projectRoot(projectPath)),
          path: configPath,
          value: sageEnv.SAGE_REPO_ROOT || null,
        });
      }
    }
  }
  return checks;
}

module.exports = {
  compareVersions,
  doctor,
  engineAssociation,
  ensurePluginEnabled,
  findRunningEditorProcesses,
  installPlugin,
  mcpConfigPath,
  normalizeUproject,
  pluginVersionStatus,
  projectRoot,
  readPluginDescriptor,
  readProject,
  samePath,
  upsertProjectMcpConfig,
};
