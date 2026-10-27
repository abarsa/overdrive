"""Integration smoke test: runs POSIX HOST_SIM with a fake BRC1 ELF loader.
No PS5, WSL, paid service, or network connection is required.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request

BINARY = Path(os.environ.get('LINK_CAPTURE_HOST_EXE', '/tmp/ps5_link_v08_host'))
EXPECTED_ELF_LEN = int(os.environ.get('LINK_CAPTURE_MOCK_ELF_SIZE', '128816'))
PAGE = 'https://vik1ngfile.site/f/jTxcioO1oV'
CANDIDATE = 'https://vikingfile.com/d/d8D9ZZzdQo/example-package.rargdu6m'


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class FakeLoader:
    def __init__(self, port):
        self.port = port
        self.ready = threading.Event()
        self.errors = []
        self.received = None
        self.release = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(('127.0.0.1', self.port))
                listener.listen(1)
                self.ready.set()
                with listener.accept()[0] as sock:
                    sock.settimeout(10)
                    payload = bytearray()
                    while len(payload) < EXPECTED_ELF_LEN:
                        piece = sock.recv(min(32768, EXPECTED_ELF_LEN - len(payload)))
                        if not piece:
                            raise RuntimeError('Payload truncated')
                        payload.extend(piece)
                    self.received = bytes(payload[:4])
                    while b'\nfresh\n' not in payload[EXPECTED_ELF_LEN:]:
                        payload.extend(sock.recv(1024))
                    request = bytes(payload[EXPECTED_ELF_LEN:])
                    if b'BRC1\nhttps://vikingfile.com/d/\n/\n180\n0\nfresh\n' not in request:
                        raise RuntimeError('Invalid BRC1 request: ' + repr(request))
                    sock.sendall(b'preparing\n')
                    if not self.release.wait(timeout=7):
                        raise RuntimeError('Host test did not release ARMED')
                    sock.sendall(b'armed 2 known links\nwaiting\n')
                    time.sleep(0.5)
                    sock.sendall(b'found ' + CANDIDATE.encode() + b'\n')
        except Exception as e:
            self.errors.append(str(e))
            self.ready.set()


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.http_port = free_port()
        self.loader_port = free_port()
        self.loader = FakeLoader(self.loader_port)
        self.loader.thread.start()
        assert self.loader.ready.wait(5)
        self.proc = subprocess.Popen([str(BINARY)], env={**os.environ,
            'LINK_CAPTURE_TEST_PORT': str(self.http_port),
            'LINK_CAPTURE_TEST_LOADER_PORT': str(self.loader_port)},
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.base = f'http://127.0.0.1:{self.http_port}'
        for _ in range(50):
            try:
                self.get('/api/status')
                break
            except OSError:
                time.sleep(.05)
        else:
            self.fail('server did not launch')

    def tearDown(self):
        self.loader.release.set()
        self.proc.terminate()
        try:
            self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=3)
        self.loader.thread.join(timeout=5)
        self.assertEqual(self.loader.errors, [])

    def get(self, path):
        with urllib.request.urlopen(self.base+path, timeout=3) as resp:
            return json.load(resp) if path.endswith('status') else resp.read().decode()

    def post(self, page, filename='', **headers):
        data=json.dumps({'url': page, 'filename': filename}).encode()
        req=urllib.request.Request(self.base+'/api/start',data=data,
            headers={'Content-Type': 'application/json', **headers},method='POST')
        try:
            with urllib.request.urlopen(req,timeout=5) as r:
                return r.status,r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code,e.read().decode()

    def test_capture_flow(self):
        self.assertIn('PS5 Link Capture v0.8', self.get('/'))
        self.assertEqual('idle',self.get('/api/status')['stage'])
        code, message = self.post(PAGE)
        self.assertEqual(code, 202, message)
        code,_ = self.post(PAGE)
        self.assertEqual(code, 409)
        self.assertEqual('', self.get('/api/status')['candidate'])
        self.loader.release.set()
        for _ in range(100):
            state = self.get('/api/status')
            if state['stage']=='captured' and not state['active']:
                break
            time.sleep(.05)
        else:
            self.fail('never captured: ' + repr(state))
        self.assertEqual(state['candidate'], CANDIDATE)
        self.assertEqual(state['browser_return'], 0)
        self.assertEqual(self.loader.received, b'\x7fELF')

    def test_reject_origin_after_full_body(self):
        """Reject a cross-site request cleanly even when headers/body arrive separately."""
        payload = json.dumps({'url': PAGE, 'filename': ''}).encode()
        headers = (
            f'POST /api/start HTTP/1.1\r\n'
            f'Host: 127.0.0.1:{self.http_port}\r\n'
            'Origin: http://evil.localhost\r\n'
            'Content-Type: application/json\r\n'
            f'Content-Length: {len(payload)}\r\n'
            'Connection: close\r\n\r\n'
        ).encode()
        with socket.create_connection(('127.0.0.1', self.http_port), timeout=5) as client:
            client.settimeout(3)
            client.sendall(headers)
            time.sleep(.08)  # force separate reads on many systems
            client.sendall(payload)
            data = bytearray()
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                data.extend(chunk)
        self.assertIn(b'HTTP/1.1 403 Forbidden', data)
        self.assertTrue(data.endswith(b'Cross-site request denied'), data[-100:])
        self.assertEqual('idle', self.get('/api/status')['stage'])

    def test_invalid_input(self):
        for candidate in ('https://vik1ngfile.site.evil/f/jTxcioO1oV',
                          'http://vik1ngfile.site/f/jTxcioO1oV',
                          'https://vik1ngfile.site/f/jTxcioO1oV?redirect=x',
                          'https://vik1ngfile.site/f/abc',
                          'https://vikingfile.com@evil.test/f/jTxcioO1oV'):
            code, msg = self.post(candidate)
            self.assertEqual(code, 400, (candidate, msg))
        self.assertEqual(self.post(PAGE, filename='../escape.txt')[0],400)
        self.assertEqual(self.post(PAGE, Origin='http://evil.localhost')[0],403)


if __name__=='__main__':
    unittest.main()
