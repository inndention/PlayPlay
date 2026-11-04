import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

from server import Handler, Store, SAMPLE, validate

class StudioTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.tmp.name) / 'test.sqlite')
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.store = Store(self.path)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.host = '127.0.0.1:' + str(self.server.server_port)
    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()
        self.server.store.db.close(); self.tmp.cleanup()
    def request(self, method, path, data=None, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        body = json.dumps(data) if data is not None else None
        conn.request(method, path, body, headers or {})
        res = conn.getresponse(); payload = res.read(); conn.close()
        return res.status, payload
    def post(self, path, data):
        return self.request('POST', path, data, {'Origin': 'http://' + self.host, 'X-CG-Control': '1', 'Content-Type': 'application/json'})
    def test_preview_edit_snapshot_and_persistence(self):
        scene = dict(SAMPLE, headline='A long name', detail='Reporter', inMs=0, outMs=0)
        self.assertEqual(self.post('/api/project', scene)[0], 200)
        status, body = self.post('/api/command', {'channel': 'lower', 'command': 'take', 'scene': scene})
        self.assertEqual(status, 200); self.assertEqual(json.loads(body)['phase'], 'IDLE')
        self.post('/api/project', dict(scene, headline='Changed preview'))
        state = json.loads(self.request('GET', '/api/channel/lower')[1])
        self.assertEqual(state['scene']['headline'], 'A long name')
        self.assertEqual(self.server.store.command('lower', 'out')['phase'], 'CLEARED')
        self.server.store.command('bug', 'take', dict(scene, type='weather'))
        second = Store(self.path)
        try: self.assertEqual(second.state('bug')['scene']['type'], 'weather')
        finally: second.db.close()
    def test_isolated_channels_and_panic(self):
        for channel, kind in [('ticker', 'ticker'), ('full', 'starfield')]:
            self.post('/api/command', {'channel': channel, 'command': 'take', 'scene': dict(SAMPLE, type=kind)})
        self.assertNotEqual(self.server.store.state('ticker')['phase'], 'CLEARED')
        self.assertNotEqual(self.server.store.state('full')['phase'], 'CLEARED')
        self.assertEqual(self.post('/api/panic', {})[0], 200)
        self.assertEqual(self.server.store.state('ticker')['phase'], 'CLEARED')
        self.assertEqual(self.server.store.state('full')['phase'], 'CLEARED')
    def test_validation_and_origin(self):
        self.assertEqual(self.request('POST', '/api/project', SAMPLE)[0], 403)
        self.assertEqual(self.request('GET', '/', headers={'Host':'evil.example'})[0], 403)
        self.assertEqual(self.post('/api/project', dict(SAMPLE, headline='x' * 181))[0], 400)
        self.assertEqual(self.post('/api/command', [])[0], 400)
        self.assertEqual(self.post('/api/project', dict(SAMPLE, blue='url(evil)'))[0], 400)
        self.assertEqual(self.post('/api/command', {'channel':'unknown', 'command':'clear'})[0], 400)
        self.assertEqual(self.request('GET', '/output?channel=lower')[0], 200)
        with self.assertRaises(ValueError): validate(dict(SAMPLE, type='script'))

if __name__ == '__main__': unittest.main()
