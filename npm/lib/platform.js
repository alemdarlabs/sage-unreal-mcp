'use strict';

function platformKey() {
  return `${process.platform}-${process.arch}`;
}

function platformFromKey(key = platformKey()) {
  return String(key).split('-')[0];
}

function serverExeName(key = platformKey()) {
  return platformFromKey(key) === 'win32' ? 'sage-server.exe' : 'sage-server';
}

function archiveExt(key = platformKey()) {
  return platformFromKey(key) === 'win32' ? 'zip' : 'tar.gz';
}

function runtimeLibraryExtensions(key = platformKey()) {
  const platform = platformFromKey(key);
  if (platform === 'win32') return ['.dll'];
  if (platform === 'darwin') return ['.dylib'];
  if (platform === 'linux') return ['.so'];
  return [];
}

function pluginPlatformName(key = platformKey()) {
  const platform = platformFromKey(key);
  if (platform === 'win32') return 'Win64';
  if (platform === 'darwin') return 'Mac';
  if (platform === 'linux') return 'Linux';
  return key;
}

module.exports = {
  archiveExt,
  platformKey,
  platformFromKey,
  pluginPlatformName,
  runtimeLibraryExtensions,
  serverExeName,
};
