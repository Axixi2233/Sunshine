/** @file Browser authentication navigation regression tests. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loginDestination, handleAuthentication } from '../src_assets/common/assets/web/web_auth.js';

test('login redirects only to known management pages', () => {
  assert.equal(loginDestination('?next=/config'), '/config');
  for (const next of ['https://evil.test', '//evil.test', '/\\evil.test', '/login', '/api/restart', '/config%0d%0a']) {
    assert.equal(loginDestination('?next=' + encodeURIComponent(next)), '/');
  }
  assert.equal(loginDestination(''), '/');
});

test('expired API session redirects while login failures stay in the form', () => {
  const destinations = [];
  const location = { pathname: '/config', replace: value => destinations.push(value) };
  handleAuthentication({ status: 401 }, location);
  assert.deepEqual(destinations, ['/login?next=%2Fconfig']);
  for (const pathname of ['/login', '/welcome', '/logout', '/login/']) {
    handleAuthentication({ status: 401 }, { ...location, pathname });
  }
  handleAuthentication({ status: 200 }, location);
  assert.equal(destinations.length, 1);
});
