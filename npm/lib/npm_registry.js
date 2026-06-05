'use strict';

const https = require('node:https');

const registryUrl = 'https://registry.npmjs.org/%40alemdarlabs%2Fsage-mcp/latest';

function skipLatestCheck() {
  const value = String(process.env.SAGE_SKIP_NPM_LATEST || '').toLowerCase();
  return value === '1' || value === 'true' || value === 'yes';
}

function latestPackageVersion(options = {}) {
  if (options.offline || skipLatestCheck()) {
    return Promise.resolve({ ok: true, status: 'skipped' });
  }

  const timeoutMs = options.timeoutMs || 2000;
  return new Promise((resolve) => {
    const req = https.get(registryUrl, {
      headers: { Accept: 'application/json', 'User-Agent': 'sage-mcp-doctor' },
      timeout: timeoutMs,
    }, (res) => {
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => {
        body += chunk;
        if (body.length > 1024 * 1024) req.destroy(new Error('registry response too large'));
      });
      res.on('end', () => {
        if (res.statusCode !== 200) {
          resolve({ ok: false, status: 'unavailable', http_status: res.statusCode });
          return;
        }
        try {
          const parsed = JSON.parse(body);
          if (typeof parsed.version !== 'string' || !parsed.version.trim()) {
            resolve({ ok: false, status: 'unavailable', error: 'missing version in registry response' });
            return;
          }
          resolve({ ok: true, status: 'available', version: parsed.version.trim() });
        } catch (error) {
          resolve({ ok: false, status: 'unavailable', error: error.message });
        }
      });
    });
    req.on('timeout', () => {
      req.destroy(new Error(`registry check timed out after ${timeoutMs} ms`));
    });
    req.on('error', (error) => {
      resolve({ ok: false, status: 'unavailable', error: error.message });
    });
  });
}

module.exports = {
  latestPackageVersion,
};
