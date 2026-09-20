const {test} = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync('Sources/YOBRO/Resources/WebAppearance.js', 'utf8');
function page(color = 'rgb(255, 255, 255)') {
    let active = false, starts = 0, stops = 0, mutation;
    const listeners = {};
    const media = {matches: true, addEventListener: (_, fn) => listeners.media = fn};
    const body = {color, parentElement: null};
    const context = {
        matchMedia: () => media, location: {hostname: 'example.com', protocol: 'https:'},
        document: {body, documentElement: body, querySelector: () => null, elementFromPoint: () => body},
        getComputedStyle: node => ({backgroundColor: node.color}), innerWidth: 900, innerHeight: 700,
        frames: [], fetch() {}, setTimeout: fn => { mutation = fn; return 1; }, clearTimeout() {},
        MutationObserver: class {observe() {}},
        DarkReader: {setFetchMethod() {}, isEnabled: () => active,
            enable() {active = true; starts++;}, disable() {active = false; stops++;}},
        addEventListener: (name, fn) => listeners[name] = fn,
    };
    context.window = context; context.parent = context;
    vm.runInNewContext(source, context);
    return {api: context.__yobroAppearance, body, media, listeners,
        counts: () => ({starts, stops}), settle: () => mutation?.()};
}
test('repeated configuration preserves the running engine', () => {
    const p = page();
    p.api.configure({enabled: true, systemDark: true});
    p.api.configure({enabled: true, systemDark: true});
    assert.deepEqual(p.counts(), {starts: 1, stops: 0});
    p.api.configure({enabled: true, systemDark: true, excludedHosts: ['example.com']});
    assert.equal(p.api.status().state, 'original');
    assert.deepEqual(p.counts(), {starts: 1, stops: 1});
});
test('live media changes replace a stale system snapshot', () => {
    const p = page();
    p.api.configure({enabled: true, systemDark: true});
    p.media.matches = false; p.listeners.media();
    assert.equal(p.api.status().state, 'original');
    assert.equal(p.api.status().systemDark, false);
    p.media.matches = true; p.listeners.media();
    assert.equal(p.api.status().state, 'converted');
});
test('translucent dark surfaces are composited over their light parent', () => {
    const p = page('rgba(0, 0, 0, 0.7)');
    p.body.parentElement = {color: 'rgb(255, 255, 255)', parentElement: null};
    p.api.configure({enabled: true});
    assert.equal(p.api.status().state, 'native');
    assert.equal(p.counts().starts, 0);
});
test('page lifecycle rechecks native theme after conversion', () => {
    const p = page();
    p.api.configure({enabled: true});
    p.body.color = 'rgb(21, 21, 21)';
    p.listeners.pageshow(); p.settle();
    assert.equal(p.api.status().state, 'native');
    assert.equal(p.api.status().enabled, false);
});
