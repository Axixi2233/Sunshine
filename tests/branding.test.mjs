/** @file Product branding regression tests. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { brandText, productName } from '../src_assets/common/assets/web/branding.js';

test('product name replaces both original and fork names in translated text', () => {
  assert.equal(productName, 'StarCore星核');
  assert.equal(brandText('你好，Sunshine·阿西西！'), '你好，StarCore星核！');
  assert.equal(brandText('Sign in to Sunshine'), 'Sign in to StarCore星核');
});

test('branding preserves repository links, executable names and service identifiers', () => {
  const text = 'https://github.com/LizardByte/Sunshine Sunshine.exe SunshineService /Sunshine';
  assert.equal(brandText(text), text);
});

test('Windows and web icons share valid multi-resolution ICO data', () => {
  const rootIcon = readFileSync(new URL('../sunshine.ico', import.meta.url));
  const webIcon = readFileSync(new URL('../src_assets/common/assets/web/public/images/sunshine.ico', import.meta.url));
  assert.deepEqual(rootIcon, webIcon);
  assert.equal(rootIcon.readUInt16LE(2), 1);
  const count = rootIcon.readUInt16LE(4);
  assert.equal(count, 8);
  for (let index = 0; index < count; index++) {
    const entry = 6 + index * 16;
    const length = rootIcon.readUInt32LE(entry + 8);
    const offset = rootIcon.readUInt32LE(entry + 12);
    assert.ok(offset + length <= rootIcon.length);
    assert.equal(rootIcon.subarray(offset, offset + 8).toString('hex'), '89504e470d0a1a0a');
  }
});

test('release version and visible Windows identity are updated without renaming the executable', () => {
  const cmake = readFileSync(new URL('../CMakeLists.txt', import.meta.url), 'utf8');
  const resource = readFileSync(new URL('../src/platform/windows/windows.rc', import.meta.url), 'utf8');
  assert.match(cmake, /project\(Sunshine VERSION 1\.2\.0/);
  assert.match(resource, /VALUE "ProductName",\s+L"StarCore星核"/);
});
