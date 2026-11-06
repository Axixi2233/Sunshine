/** @file Driver warning visibility and safe repair controls. */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRenderer, h, resolveComponent } from 'vue';
import { driverView } from '../src_assets/common/assets/web/usbip_driver.js';

test('browser-normalized page tags resolve the registered USB/IP component', () => {
  const renderer = createRenderer({
    createElement: tag => ({ tag, children: [] }),
    createText: text => ({ text }), createComment: text => ({ text }),
    insert: (child, parent) => parent.children.push(child),
    setElementText: (node, text) => { node.text = text; }, setText: () => {},
    patchProp: () => {}, remove: () => {}, parentNode: () => null, nextSibling: () => null,
  });
  for (const page of ['index.html', 'troubleshooting.html']) {
    const html = readFileSync(new URL(`../src_assets/common/assets/web/${page}`, import.meta.url), 'utf8');
    // In-DOM templates are parsed by HTML first, which lowercases component tag names.
    const tag = html.match(/<([a-z-]*usb[a-z-]*)\b/i)?.[1].toLowerCase();
    assert.ok(tag, `${page}: missing USB/IP component`);
    const root = { children: [] };
    const app = renderer.createApp({
      components: { UsbIpDriver: { render: () => h('section') } },
      render: () => h(resolveComponent(tag)),
    });
    app.config.warnHandler = () => {};
    app.mount(root);
    assert.equal(root.children[0]?.tag, 'section', `${page}: HTML tag does not resolve to UsbIpDriver`);
    app.unmount();
  }
});

test('home warns only for detected failures, not unused or uninstalled USB/IP', () => {
  for (const state of ['healthy', 'missing', 'unknown', 'checking']) assert.equal(driverView({ state }).warning, false);
  for (const state of ['stale', 'error', 'helper_missing']) assert.equal(driverView({ state }).warning, true);
});
test('repair is disabled during streaming, repair, healthy state or unavailable inspection', () => {
  assert.equal(driverView(null).disabled, true);
  for (const state of ['healthy', 'helper_missing', 'unknown', 'checking']) assert.equal(driverView({ state }).disabled, true);
  for (const state of ['missing', 'stale', 'error']) {
    assert.equal(driverView({ state }).disabled, false);
    assert.equal(driverView({ state, streaming: true }).disabled, true);
    assert.equal(driverView({ state, repairing: true }).disabled, true);
  }
});
