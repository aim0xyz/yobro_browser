#!/usr/bin/env python3
"""Regression audit against YOBRO_SOCKET, using only temporary agent tabs and localhost."""
import http.server
import json
import os
from pathlib import Path
import sys
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'plugins/yobro-browser/scripts'))
import yobro_mcp as bridge

PAGE = b'''<!doctype html><title>YOBRO Agent Audit</title><body><h1>Agent function test</h1>
<form onsubmit="event.preventDefault();document.querySelector('#result').textContent='Submitted'">
<label>Name<input aria-label="Name" role="textbox" value="Original"></label>
<label>Choice<select aria-label="Choice"><option value="one">One</option><option value="two">Two</option></select></label>
<label><input type="checkbox">Remember</label><button>Submit</button></form>
<button aria-disabled="true" onclick="document.querySelector('#result').textContent='Disabled clicked'">Unavailable</button>
<button onclick="document.querySelector('#result').textContent='Clicked'">Apply</button>
<input aria-label="Password" type="password" value="audit-secret">
<div style="height:1600px"></div><p id="result">Ready</p><a href="/second">Second page</a></body>'''

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'<title>Second</title><h1>Second page</h1><button>Second action</button>' if self.path == '/second' else PAGE
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def call(name, **arguments):
    result = bridge.call_tool(name, arguments)
    assert not result.get('isError'), result
    return result['structuredContent']


def main():
    assert os.environ.get('YOBRO_SOCKET'), 'Set YOBRO_SOCKET explicitly to the browser under test.'
    assert call('status')['enabled'], 'Enable Agent Access first.'
    assert not any(t['owner'] == 'agent' for t in call('tabs')['tabs']), 'Finish the existing agent session before running this audit.'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    created = []
    def read(tab):
        return call('read_page', tab=tab)['page']
    def element(page, label):
        return next(e for e in page['elements'] if e['label'] == label)
    def action(name, page, label, **args):
        return bridge.call_tool(name, dict(tab=tab, ref=element(page, label)['ref'], document=page['document'], **args))
    try:
        base = f'http://127.0.0.1:{server.server_port}'
        tab = call('new_page', url=base)['id']; created.append(tab)
        page = read(tab)
        assert 'audit-secret' not in json.dumps(page)
        assert page['title'] == 'YOBRO Agent Audit'
        result = action('fill', page, 'Name', value='Laura Test')
        assert not result.get('isError'), result
        page = read(tab)
        assert element(page, 'Name')['value'] == 'Laura Test'
        print('PASS native text input with textbox role and password redaction', flush=True)

        stale = dict(page, document='not-the-current-document')
        assert action('click', stale, 'Apply').get('isError')
        page = read(tab)
        assert 'Clicked' not in page['text']
        assert element(page, 'Unavailable')['disabled']
        assert action('click', page, 'Unavailable').get('isError')
        page = read(tab)
        assert 'Disabled clicked' not in page['text']
        assert action('fill', page, 'Choice', value='missing').get('isError')
        page = read(tab)
        assert element(page, 'Choice')['value'] == 'one'
        assert not action('fill', page, 'Choice', value='two').get('isError')
        page = read(tab)
        assert element(page, 'Choice')['value'] == 'two'
        print('PASS stale-document, disabled-control, and dropdown protection', flush=True)

        assert not action('click', page, 'Remember').get('isError')
        page = read(tab)
        assert element(page, 'Remember')['checked']
        assert not action('press_key', page, 'Name', key='Enter').get('isError')
        page = read(tab)
        assert 'Submitted' in page['text']
        before = page['viewport']['scrollY']
        call('scroll', tab=tab, amount=600)
        page = read(tab)
        assert page['viewport']['scrollY'] > before
        assert call('find_on_page', tab=tab, query='Agent function test')['found']
        page = read(tab)
        print('PASS checkbox, keyboard submission, measurable scrolling, and find', flush=True)

        call('open_page', tab=tab, url=base + '/second'); page = read(tab)
        assert page['title'] == 'Second'
        for direction, title in [('back', 'YOBRO Agent Audit'), ('forward', 'Second'), ('reload', 'Second')]:
            call('navigate', tab=tab, direction=direction)
            assert read(tab)['title'] == title
        duplicate = call('duplicate_page', tab=tab)['id']; created.append(duplicate)
        assert read(duplicate)['title'] == 'Second'
        call('close_page', tab=duplicate); created.remove(duplicate)
        assert all(t['id'] != duplicate for t in call('tabs')['tabs'])
        print('PASS navigation, back/forward, reload, duplicate, close', flush=True)
    finally:
        try:
            for tab in created:
                call('close_page', tab=tab)
        finally:
            call('end_session')
            server.shutdown()
            server.server_close()
    assert not any(t['owner'] == 'agent' for t in call('tabs')['tabs'])
    print('PASS agent session cleanup', flush=True)

if __name__ == '__main__':
    main()
