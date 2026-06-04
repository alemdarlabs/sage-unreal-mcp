#!/usr/bin/env node
'use strict';

const { ensureServerBinary } = require('../lib/download');

async function main() {
  if (process.env.SAGE_SKIP_DOWNLOAD === '1' || process.env.SAGE_SKIP_DOWNLOAD === 'true') {
    process.stderr.write('Sage postinstall: SAGE_SKIP_DOWNLOAD set; skipping native binary download.\n');
    return;
  }
  try {
    const binary = await ensureServerBinary();
    process.stderr.write(`Sage postinstall: native server ready at ${binary}\n`);
  } catch (error) {
    if (process.env.SAGE_POSTINSTALL_STRICT === '0') {
      process.stderr.write(`Sage postinstall warning: ${error.message}\n`);
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
