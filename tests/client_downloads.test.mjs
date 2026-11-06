/** @file Regression checks for client downloads and home-page placement. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { streamingClients } from '../src_assets/common/assets/web/client_downloads.js';

const web = new URL('../src_assets/common/assets/web/', import.meta.url);
const read = path => readFileSync(new URL(path, web), 'utf8');

test('home offers the same three client destinations as macOS, above resources', () => {
  assert.deepEqual(streamingClients.map(client => client.id), ['ios', 'android', 'harmonyos']);
  for (const client of streamingClients) {
    assert.equal(new URL(client.url).protocol, 'https:');
  }
  const home = read('index.html');
  assert.ok(home.indexOf('<Client-Downloads>') < home.indexOf('<Resource-Card'));
  assert.doesNotMatch(read('Navbar.vue'), /href="\.\/featured"/);
});

test('client images resolve under the WebUI static asset route', () => {
  const component = read('ClientDownloads.vue');
  const logo = component.match(/src="(\/assets\/[^"\s]+\.png)"/)?.[1];
  const qrPattern = component.match(/:src="`(\/assets\/[^`]+\.svg)`"/)?.[1];
  assert.ok(logo && qrPattern);
  assert.ok(existsSync(new URL(`public${logo}`, web)), logo);
  for (const client of streamingClients) {
    const path = qrPattern.replace('${client.id}', client.id);
    assert.ok(existsSync(new URL(`public${path}`, web)), path);
  }
});

test('client cards have complete English and Simplified Chinese messages', () => {
  const en = JSON.parse(read('public/assets/locale/en.json')).client_downloads;
  const zh = JSON.parse(read('public/assets/locale/zh.json')).client_downloads;
  assert.deepEqual(Object.keys(zh).sort(), Object.keys(en).sort());
  for (const [key, text] of Object.entries(zh)) {
    assert.match(text, /[\u3400-\u9fff]/, key);
  }
  for (const client of streamingClients) {
    for (const suffix of ['name', 'detail', 'action']) {
      assert.ok(en[`${client.id}_${suffix}`]);
    }
  }
});
