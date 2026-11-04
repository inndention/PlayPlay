"""Loopback-only Star News CG Studio. No external services are required."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent
CHANNELS = ('lower', 'bug', 'ticker', 'full', 'custom')
TYPES = ('lower', 'ticker', 'weather', 'starfield')
SAMPLE = {'version': 1, 'type': 'lower', 'headline': 'Your headline here',
          'detail': 'Location or secondary line', 'label': 'STAR NEWS',
          'accent': '#c8001e', 'blue': '#123e70', 'inMs': 450, 'outMs': 350}
lock = threading.RLock()

def validate(value):
    if not isinstance(value, dict) or value.get('version') != 1 or value.get('type') not in TYPES:
        raise ValueError('Invalid scene version or template')
    result = {'version': 1, 'type': value['type']}
    for name in ('headline', 'detail', 'label'):
        text = value.get(name, '')
        if not isinstance(text, str) or len(text) > 180:
            raise ValueError(f'{name} must be text of at most 180 characters')
        result[name] = text
    for name in ('accent', 'blue'):
        color = value.get(name, '')
        if not isinstance(color, str) or len(color) != 7 or color[0] != '#' or any(c not in '0123456789abcdefABCDEF' for c in color[1:]):
            raise ValueError('Colors must be hex RGB values')
        result[name] = color
    for name in ('inMs', 'outMs'):
        n = value.get(name)
        if type(n) is not int or not 0 <= n <= 10000:
            raise ValueError('Animation durations must be 0–10000 ms')
        result[name] = n
    return result

class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute('CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT NOT NULL)')
        self.db.commit()
    def get(self, key, default):
        with lock:
            row = self.db.execute('SELECT value FROM kv WHERE key=?', (key,)).fetchone()
            return json.loads(row[0]) if row else default
    def put(self, key, value):
        with lock, self.db:
            self.db.execute('INSERT OR REPLACE INTO kv VALUES (?, ?)', (key, json.dumps(value)))
    def state(self, channel):
        state = self.get('channel:' + channel, {'phase': 'CLEARED', 'scene': None, 'since': 0})
        if state['phase'] in ('IN', 'OUT'):
            duration = state['scene']['inMs' if state['phase'] == 'IN' else 'outMs']
            if time.time() * 1000 >= state['since'] + duration:
                state['phase'] = 'IDLE' if state['phase'] == 'IN' else 'CLEARED'
                self.put('channel:' + channel, state)
        return state
    def command(self, channel, command, scene=None):
        with lock:
            state = self.state(channel)
            if command == 'take':
                state = {'phase': 'IN', 'scene': validate(scene), 'since': time.time() * 1000}
            elif command == 'out' and state['phase'] != 'CLEARED':
                state.update(phase='OUT', since=time.time() * 1000)
            elif command == 'clear':
                state.update(phase='CLEARED', since=time.time() * 1000)
            elif command != 'out':
                raise ValueError('Unknown command')
            self.put('channel:' + channel, state)
            return self.state(channel)

class Handler(BaseHTTPRequestHandler):
    server_version = 'StarNewsCG/1'
    def log_message(self, format, *args):
        pass
    def send(self, status, body, content_type='application/json; charset=utf-8'):
        body = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'self'")
        self.end_headers()
        self.wfile.write(body)
    def local_host(self):
        return self.headers.get('Host', '') in (f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}')
    def do_GET(self):
        if not self.local_host():
            return self.send(403, {'error': 'Invalid host'})
        path = urlsplit(self.path).path
        if path == '/api/project':
            return self.send(200, self.server.store.get('project', SAMPLE))
        if path.startswith('/api/channel/'):
            name = path.removeprefix('/api/channel/')
            return self.send(200, self.server.store.state(name)) if name in CHANNELS else self.send(404, {'error': 'Unknown channel'})
        files = {'/': ('index.html', 'text/html'), '/output': ('output.html', 'text/html'),
                 '/app.js': ('app.js', 'text/javascript'), '/renderer.js': ('renderer.js', 'text/javascript'),
                 '/output.js': ('output.js', 'text/javascript'), '/style.css': ('style.css', 'text/css')}
        if path not in files:
            return self.send(404, {'error': 'Not found'})
        filename, kind = files[path]
        self.send(200, (ROOT / filename).read_bytes(), kind + '; charset=utf-8')
    def do_POST(self):
        if not self.local_host() or self.headers.get('Origin') != f'http://{self.headers.get("Host")}' or self.headers.get('X-CG-Control') != '1':
            return self.send(403, {'error': 'Control requests require the local studio origin'})
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if length < 1 or length > 16384:
                raise ValueError('Invalid request size')
            data = json.loads(self.rfile.read(length))
            if not isinstance(data, dict):
                raise ValueError('Request body must be a JSON object')
            path = urlsplit(self.path).path
            if path == '/api/project':
                scene = validate(data)
                self.server.store.put('project', scene)
                return self.send(200, scene)
            if path == '/api/command':
                if data.get('channel') not in CHANNELS:
                    raise ValueError('Unknown channel')
                return self.send(200, self.server.store.command(data['channel'], data.get('command'), data.get('scene')))
            if path == '/api/panic':
                for channel in CHANNELS:
                    self.server.store.command(channel, 'clear')
                return self.send(200, {'ok': True})
            return self.send(404, {'error': 'Not found'})
        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            return self.send(400, {'error': str(exc)})

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=7866)
    parser.add_argument('--data', default=os.path.join(os.environ.get('LOCALAPPDATA', str(Path.home())), 'StarNewsCG'))
    args = parser.parse_args()
    Path(args.data).mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.store = Store(Path(args.data) / 'studio.sqlite3')
    print(f'Star News CG Studio: http://127.0.0.1:{server.server_port}/', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == '__main__':
    main()
