'use strict';

function platformKey() {
  return `${process.platform}-${process.arch}`;
}

function serverExeName() {
  return process.platform === 'win32' ? 'sage-server.exe' : 'sage-server';
}

function archiveExt() {
  return process.platform === 'win32' ? 'zip' : 'tar.gz';
}

function pluginPlatformName() {
  if (process.platform === 'win32') return 'Win64';
  if (process.platform === 'darwin') return 'Mac';
  if (process.platform === 'linux') return 'Linux';
  return platformKey();
}

module.exports = {
  archiveExt,
  platformKey,
  pluginPlatformName,
  serverExeName,
};
