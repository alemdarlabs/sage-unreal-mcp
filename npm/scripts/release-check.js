#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { pluginArchiveName, serverArchiveName } = require('../lib/download');
const { npmInvocation } = require('../lib/npm_command');
const { packageVersion } = require('../lib/paths');
const { platformKey, serverExeName } = require('../lib/platform');

const repoRoot = path.resolve(__dirname, '..', '..');

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd || repoRoot,
    encoding: 'utf8',
    env: { ...process.env, ...(options.env || {}) },
    stdio: options.stdio || 'pipe',
  });
  if (result.status !== 0) {
    process.stderr.write(`release-check failed: ${command} ${args.join(' ')}\n`);
    if (result.stdout) process.stderr.write(`STDOUT:\n${result.stdout}\n`);
    if (result.stderr) process.stderr.write(`STDERR:\n${result.stderr}\n`);
    if (result.error) process.stderr.write(`ERROR: ${result.error.message}\n`);
    process.exit(result.status || 1);
  }
  if (!options.quiet) {
    if (result.stdout) process.stdout.write(result.stdout);
    if (result.stderr) process.stderr.write(result.stderr);
  }
  return result;
}

function npm(args, options = {}) {
  const invocation = npmInvocation();
  return run(invocation.command, [...invocation.args, ...args], options);
}

function listJsFiles(dir) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const child = path.join(dir, entry.name);
    if (entry.isDirectory()) out.push(...listJsFiles(child));
    else if (entry.isFile() && entry.name.endsWith('.js')) out.push(child);
  }
  return out;
}

function extractArchive(archive, dest) {
  fs.mkdirSync(dest, { recursive: true });
  const lower = archive.toLowerCase();
  if (lower.endsWith('.zip') && process.platform === 'win32') {
    run('powershell.exe', [
      '-NoProfile',
      '-ExecutionPolicy',
      'Bypass',
      '-Command',
      `Expand-Archive -LiteralPath ${JSON.stringify(archive)} -DestinationPath ${JSON.stringify(dest)} -Force`,
    ], { quiet: true });
    return;
  }

  if (lower.endsWith('.zip')) {
    run('unzip', ['-q', archive, '-d', dest], { quiet: true });
    return;
  }

  run('tar', ['-xzf', archive, '-C', dest], { quiet: true });
}

function assertFile(file) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    throw new Error(`missing file: ${file}`);
  }
}

function assertDir(dir) {
  if (!fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) {
    throw new Error(`missing directory: ${dir}`);
  }
}

function main() {
  const version = packageVersion();
  const key = platformKey();
  process.stdout.write(`release-check: version=${version} platform=${key}\n`);

  for (const file of listJsFiles(path.join(repoRoot, 'npm'))) {
    run(process.execPath, ['--check', file], { quiet: true });
  }
  npm(['test']);
  npm(['run', 'test:global']);

  const outDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-release-check-assets-'));
  npm(['run', 'package:assets', '--', '--out', outDir]);

  const serverZip = path.join(outDir, serverArchiveName(version, key));
  const pluginZip = path.join(outDir, pluginArchiveName(version));
  assertFile(serverZip);
  assertFile(pluginZip);
  assertFile(path.join(outDir, 'checksums.txt'));

  const serverExtract = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-release-check-server-'));
  const pluginExtract = fs.mkdtempSync(path.join(os.tmpdir(), 'sage-release-check-plugin-'));
  extractArchive(serverZip, serverExtract);
  extractArchive(pluginZip, pluginExtract);
  assertFile(path.join(serverExtract, serverExeName(key)));
  assertFile(path.join(pluginExtract, 'SageBridge', 'SageBridge.uplugin'));
  assertDir(path.join(pluginExtract, 'SageBridge', 'Source'));

  npm(['publish', '--dry-run', '--access', 'public']);
  process.stdout.write('release-check ok\n');
}

try {
  main();
} catch (error) {
  process.stderr.write(`release-check failed: ${error.message}\n`);
  process.exit(1);
}
