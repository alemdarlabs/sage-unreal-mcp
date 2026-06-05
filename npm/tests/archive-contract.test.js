'use strict';

const assert = require('node:assert/strict');

const {
  legacyPluginArchiveName,
  pluginArchiveName,
  pluginArchiveUrl,
  serverArchiveName,
} = require('../lib/download');
const {
  archiveExt,
  platformFromKey,
  pluginPlatformName,
  runtimeLibraryExtensions,
  serverExeName,
} = require('../lib/platform');
const { pluginInstallDir } = require('../lib/paths');

assert.equal(platformFromKey('win32-x64'), 'win32');
assert.equal(platformFromKey('darwin-arm64'), 'darwin');
assert.equal(platformFromKey('linux-x64'), 'linux');

assert.equal(archiveExt('win32-x64'), 'zip');
assert.equal(archiveExt('darwin-arm64'), 'tar.gz');
assert.equal(archiveExt('linux-x64'), 'tar.gz');

assert.equal(serverExeName('win32-x64'), 'sage-server.exe');
assert.equal(serverExeName('darwin-arm64'), 'sage-server');
assert.equal(serverExeName('linux-x64'), 'sage-server');

assert.deepEqual(runtimeLibraryExtensions('win32-x64'), ['.dll']);
assert.deepEqual(runtimeLibraryExtensions('darwin-arm64'), ['.dylib']);
assert.deepEqual(runtimeLibraryExtensions('linux-x64'), ['.so']);

assert.equal(pluginPlatformName('win32-x64'), 'Win64');
assert.equal(pluginPlatformName('darwin-arm64'), 'Mac');
assert.equal(pluginPlatformName('linux-x64'), 'Linux');

assert.equal(serverArchiveName('1.2.3', 'win32-x64'), 'sage-server-1.2.3-win32-x64.zip');
assert.equal(serverArchiveName('1.2.3', 'darwin-arm64'), 'sage-server-1.2.3-darwin-arm64.tar.gz');
assert.equal(serverArchiveName('1.2.3', 'linux-x64'), 'sage-server-1.2.3-linux-x64.tar.gz');

assert.equal(pluginArchiveName('1.2.3'), 'sagebridge-plugin-1.2.3-source.tar.gz');
assert.equal(pluginArchiveName('1.2.3', 'win32-x64'), 'sagebridge-plugin-1.2.3-source.tar.gz');
assert.equal(pluginArchiveName('1.2.3', 'darwin-arm64'), 'sagebridge-plugin-1.2.3-source.tar.gz');
assert.equal(
  pluginArchiveUrl('1.2.3'),
  'https://github.com/alemdarlabs/sage-unreal-mcp/releases/download/v1.2.3/sagebridge-plugin-1.2.3-source.tar.gz'
);

assert.equal(legacyPluginArchiveName('1.2.3', 'win32-x64'), 'sagebridge-plugin-1.2.3-win32-x64.zip');
assert.equal(legacyPluginArchiveName('1.2.3', 'darwin-arm64'), 'sagebridge-plugin-1.2.3-darwin-arm64.tar.gz');

assert.match(pluginInstallDir('1.2.3'), /[\\/]plugins[\\/]1\.2\.3[\\/]source[\\/]SageBridge$/);

console.log('archive contract ok');
