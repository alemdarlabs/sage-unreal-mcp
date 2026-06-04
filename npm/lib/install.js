'use strict';

const fs = require('node:fs');
const path = require('node:path');

const { ensurePluginPackage } = require('./download');
const { backupExistingDir, copyRecursive, ensureDir, readJson, writeJson } = require('./file_ops');
const { resolvePluginSource } = require('./paths');

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

function engineAssociation(project) {
  return project.EngineAssociation || '';
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
  let source = resolvePluginSource(options.pluginSource);
  if (!source) {
    source = await ensurePluginPackage({ forceDownload: options.forceDownload });
  }

  const dest = path.join(projectRoot(projectPath), 'Plugins', 'SageBridge');
  ensureDir(path.dirname(dest));
  const backup = backupExistingDir(dest);
  copyRecursive(source, dest, {
    skipNames: new Set(['HostProject', 'Intermediate', 'Saved', 'DerivedDataCache']),
  });
  ensurePluginEnabled(projectPath);
  return { source, dest, backup };
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
    checks.push({
      name: 'plugin_enabled',
      ok: Array.isArray(project.Plugins) && project.Plugins.some((p) => p && p.Name === 'SageBridge' && p.Enabled === true),
      path: projectPath,
    });
    checks.push({ name: 'mcp_config', ok: fs.existsSync(configPath), path: configPath });
    checks.push({
      name: 'mcp_server_sage',
      ok: Boolean(sageConfig && sageConfig.command === 'sage' && Array.isArray(sageConfig.args) && sageConfig.args[0] === 'mcp'),
      path: configPath,
    });
    checks.push({
      name: 'mcp_env_project_root',
      ok: sageEnv.SAGE_PROJECT_ROOT === projectRoot(projectPath),
      path: configPath,
      value: sageEnv.SAGE_PROJECT_ROOT || null,
    });
    checks.push({
      name: 'mcp_env_repo_root',
      ok: sageEnv.SAGE_REPO_ROOT === projectRoot(projectPath),
      path: configPath,
      value: sageEnv.SAGE_REPO_ROOT || null,
    });
  }
  return checks;
}

module.exports = {
  doctor,
  engineAssociation,
  ensurePluginEnabled,
  installPlugin,
  mcpConfigPath,
  normalizeUproject,
  projectRoot,
  readProject,
  upsertProjectMcpConfig,
};
