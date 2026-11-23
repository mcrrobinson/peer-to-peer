# WebRTC loopback A/B server: serves page.html, gives the page CLOCK_MONOTONIC
# (= p2p::nowUs, steady_clock) and writes its flash switches in the probe's
# --source-log format ("<us> <0|1>") plus per-second getStats to stats.jsonl.
import http.server, sys, time, os
OUT = sys.argv[2] if len(sys.argv) > 2 else '.'
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, body=b'', ctype='text/plain'):
        self.send_response(code); self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith('/now'):
            return self._send(200, str(time.monotonic_ns() // 1000).encode())
        if self.path.startswith('/page.html'):
            return self._send(200, open(os.path.join(os.path.dirname(__file__), 'page.html'), 'rb').read(), 'text/html')
        self._send(404)
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0)); body = self.rfile.read(n)
        name = {'/log': 'flash.txt', '/stats': 'stats.jsonl', '/info': 'info.txt'}.get(self.path)
        if not name: return self._send(404)
        with open(os.path.join(OUT, name), 'ab') as f: f.write(body)
        self._send(200)
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
