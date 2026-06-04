#!/usr/bin/env node
'use strict';

const { ensureServerBinary } = require('../lib/download');
const { ensureCodexMcp } = require('../lib/codex');

function shouldSkipCodexSetup() {
  const value = String(process.env.SAGE_SKIP_CODEX_SETUP || '').toLowerCase();
  return value === '1' || value === 'true' || value === 'yes';
}

function setupCodex() {
  if (shouldSkipCodexSetup()) {
    process.stderr.write('Sage postinstall: SAGE_SKIP_CODEX_SETUP set; skipping Codex MCP registration.\n');
    return;
  }
  try {
    const result = ensureCodexMcp({ name: 'sage', replace: true });
    if (result.ok) {
      process.stderr.write(`Sage postinstall: Codex MCP server ${result.status}: ${result.name}\n`);
      return;
    }
    process.stderr.write(`Sage postinstall warning: Codex MCP registration skipped (${result.status}${result.message ? `: ${result.message}` : ''}). Run \`sage setup codex\` after installing Codex.\n`);
  } catch (error) {
    if (process.env.SAGE_CODEX_SETUP_STRICT === '1') throw error;
    process.stderr.write(`Sage postinstall warning: Codex MCP registration skipped: ${error.message}. Run \`sage setup codex\` after installing Codex.\n`);
  }
}

async function main() {
  if (process.env.SAGE_SKIP_DOWNLOAD === '1' || process.env.SAGE_SKIP_DOWNLOAD === 'true') {
    process.stderr.write('Sage postinstall: SAGE_SKIP_DOWNLOAD set; skipping native binary download.\n');
    setupCodex();
    return;
  }
  try {
    const binary = await ensureServerBinary();
    process.stderr.write(`Sage postinstall: native server ready at ${binary}\n`);
    setupCodex();
  } catch (error) {
    if (process.env.SAGE_POSTINSTALL_STRICT === '0') {
      process.stderr.write(`Sage postinstall warning: ${error.message}\n`);
      setupCodex();
      return;
    }
    throw error;
  }
}

main().catch((error) => {
  process.stderr.write(`Sage postinstall failed: ${error.message}\n`);
  process.stderr.write('Set SAGE_SERVER_PATH to an existing sage-server binary or SAGE_SKIP_DOWNLOAD=1 for offline/dev installs.\n');
  process.exit(1);
});
