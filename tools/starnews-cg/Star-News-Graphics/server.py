"""Star News local graphics relay. Python 3.8+, standard library only.
Run: python server.py --port 8765 --open
Localhost only. Serves this folder and relays graphics cues to OBS/vMix.
"""
import argparse
import json
import threading
import webbrowser
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent
STATE = {}
LOCK = threading.Lock()


class Handler(SimpleHTTPRequestHandler):
    def valid_host(self):
        return self.headers.get('Host') in {
            'localhost:' + str(self.server.server_port),
            '127.0.0.1:' + str(self.server.server_port),
        }

    def reply(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self.valid_host():
            return self.reply({'error': 'Invalid host'}, 403)
        path = urlsplit(self.path).path
        if path == '/api/health':
            return self.reply({'starNewsRelay': True})
        if path == '/api/state':
            with LOCK:
                state = dict(STATE)
            return self.reply(state)
        return super().do_GET()

    def do_POST(self):
        if not self.valid_host():
            return self.reply({'error': 'Invalid host'}, 403)
        origin = self.headers.get('Origin')
        allowed = {'http://127.0.0.1:' + str(self.server.server_port),
                   'http://localhost:' + str(self.server.server_port)}
        if origin and origin not in allowed:
            return self.reply({'error': 'Origin not allowed'}, 403)
        if urlsplit(self.path).path != '/api/state':
            return self.reply({'error': 'Not found'}, 404)
        if self.headers.get_content_type() != 'application/json':
            return self.reply({'error': 'Expected JSON'}, 415)
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 32768:
                return self.reply({'error': 'Invalid request size'}, 413)
            data = json.loads(self.rfile.read(size))
            if not isinstance(data, dict) or data.get('type') != 'star-news-control':
                raise ValueError('Invalid command')
            if not isinstance(data.get('seq'), (int, float)) or not isinstance(data.get('config'), dict):
                raise ValueError('Invalid state')
            if data.get('action') not in (None, 'in', 'out', 'replay'):
                raise ValueError('Invalid action')
            with LOCK:
                if data['seq'] > STATE.get('seq', -1):
                    STATE.clear()
                    STATE.update(data)
            return self.reply({'ok': True})
        except (ValueError, UnicodeError):
            return self.reply({'error': 'Invalid JSON state'}, 400)

    def log_message(self, fmt, *args):
        if '/api/' not in self.path:
            super().log_message(fmt, *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--open', action='store_true')
    args = parser.parse_args()
    handler = partial(Handler, directory=str(ROOT))
    try:
        server = ThreadingHTTPServer(('127.0.0.1', args.port), handler)
    except OSError as exc:
        print('Cannot start local studio:', exc)
        print('Close the existing server, or choose another port with --port 8766.')
        return
    url = 'http://127.0.0.1:' + str(args.port)
    print('\nStar News Motion Studio\nStudio: ' + url + '/\nOBS output: ' + url + '/output.html?live=1')
    print('Keep this window open while on air. Ctrl+C stops the server.\n')
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
