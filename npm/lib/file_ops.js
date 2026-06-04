'use strict';

const fs = require('node:fs');
const path = require('node:path');

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function readJson(file) {
  const raw = fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, '');
  return JSON.parse(raw);
}

function writeJson(file, value) {
  fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

function copyRecursive(src, dest, options = {}) {
  const skipNames = new Set(options.skipNames || []);
  const stat = fs.statSync(src);
  if (stat.isDirectory()) {
    ensureDir(dest);
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
      if (skipNames.has(entry.name)) continue;
      copyRecursive(path.join(src, entry.name), path.join(dest, entry.name), options);
    }
    return;
  }
  ensureDir(path.dirname(dest));
  fs.copyFileSync(src, dest);
}

function timestamp() {
  return new Date().toISOString().replace(/[-:]/g, '').replace(/\..+$/, '').replace('T', '-');
}

function backupExistingDir(dir) {
  if (!fs.existsSync(dir)) return null;
  const parent = path.dirname(dir);
  const base = path.basename(dir);
  let backup = path.join(parent, `${base}.backup-${timestamp()}`);
  let suffix = 1;
  while (fs.existsSync(backup)) {
    backup = path.join(parent, `${base}.backup-${timestamp()}-${suffix++}`);
  }
  fs.renameSync(dir, backup);
  return backup;
}

function findUp(startDir, filename) {
  let current = path.resolve(startDir);
  while (true) {
    const candidate = path.join(current, filename);
    if (fs.existsSync(candidate)) return candidate;
    const parent = path.dirname(current);
    if (parent === current) return null;
    current = parent;
  }
}

module.exports = {
  backupExistingDir,
  copyRecursive,
  ensureDir,
  findUp,
  readJson,
  timestamp,
  writeJson,
};
