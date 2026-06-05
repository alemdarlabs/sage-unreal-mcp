'use strict';

const fs = require('node:fs');
const path = require('node:path');

function existingFile(file) {
  return file && fs.existsSync(file) && fs.statSync(file).isFile();
}

function npmInvocation() {
  const candidates = [
    process.env.npm_execpath,
    path.join(path.dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npm-cli.js'),
    path.join(path.dirname(path.dirname(process.execPath)), 'lib', 'node_modules', 'npm', 'bin', 'npm-cli.js'),
  ];

  for (const candidate of candidates) {
    if (existingFile(candidate)) {
      return { command: process.execPath, args: [candidate] };
    }
  }

  if (process.platform === 'win32') {
    return { command: 'cmd.exe', args: ['/d', '/s', '/c', 'npm'] };
  }

  return { command: 'npm', args: [] };
}

module.exports = {
  npmInvocation,
};
