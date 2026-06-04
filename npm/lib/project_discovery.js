'use strict';

const fs = require('node:fs');
const path = require('node:path');

function toDirectory(startPath) {
  const resolved = path.resolve(startPath || process.cwd());
  try {
    const stat = fs.statSync(resolved);
    return stat.isDirectory() ? resolved : path.dirname(resolved);
  } catch {
    return path.dirname(resolved);
  }
}

function uprojectFilesIn(dir) {
  try {
    return fs.readdirSync(dir, { withFileTypes: true })
      .filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.uproject'))
      .map((entry) => path.join(dir, entry.name));
  } catch {
    return [];
  }
}

function discoverUnrealProject(startPath = process.cwd()) {
  let current = toDirectory(startPath);
  while (true) {
    const matches = uprojectFilesIn(current);
    if (matches.length === 1) {
      return {
        ok: true,
        projectPath: matches[0],
        projectRoot: current,
        source: 'cwd',
      };
    }
    if (matches.length > 1) {
      return {
        ok: false,
        projectPath: null,
        projectRoot: current,
        source: 'cwd',
        reason: 'multiple_uproject_files',
        matches,
      };
    }

    const parent = path.dirname(current);
    if (parent === current) {
      return {
        ok: false,
        projectPath: null,
        projectRoot: null,
        source: 'cwd',
        reason: 'not_found',
        matches: [],
      };
    }
    current = parent;
  }
}

function resolveProjectArgOrDiscover(projectArg, startPath = process.cwd()) {
  if (projectArg) return path.resolve(projectArg);
  const discovered = discoverUnrealProject(startPath);
  if (discovered.ok) return discovered.projectPath;
  if (discovered.reason === 'multiple_uproject_files') {
    throw new Error(`Multiple .uproject files found in ${discovered.projectRoot}; pass one explicitly.`);
  }
  throw new Error('No .uproject found from the current directory upward; pass a project path explicitly.');
}

module.exports = {
  discoverUnrealProject,
  resolveProjectArgOrDiscover,
};
