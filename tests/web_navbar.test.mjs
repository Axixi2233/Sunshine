/** @file Navigation highlighting regression tests for local pages and external app links. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { isCurrentNavigationLink } from '../src_assets/common/assets/web/navigation.js';

test('highlights the current local page including its trailing-slash form', () => {
  assert.equal(isCurrentNavigationLink('./config', 'https://localhost:47990/config'), true);
  assert.equal(isCurrentNavigationLink('/config', 'https://localhost:47990/config/'), true);
  assert.equal(isCurrentNavigationLink('./', 'https://localhost:47990/'), true);
  assert.equal(isCurrentNavigationLink('./pin', 'https://localhost:47990/config'), false);
});

test('external app links stay inactive when their pathname matches the home page', () => {
  assert.equal(isCurrentNavigationLink('https://www.axixi.top/', 'https://localhost:47990/'), false);
  assert.equal(isCurrentNavigationLink('https://localhost:47991/', 'https://localhost:47990/'), false);
});

test('missing and placeholder destinations stay inactive', () => {
  for (const href of [null, '', '#']) {
    assert.equal(isCurrentNavigationLink(href, 'https://localhost:47990/'), false);
  }
});
