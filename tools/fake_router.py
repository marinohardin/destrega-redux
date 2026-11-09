#!/usr/bin/env python3
"""A fake home router for the port-mapping tests (runtime/portmap.c): PCP + NAT-PMP on one UDP
port and a UPnP IGD (device description + SOAP control) over HTTP, all bound to 127.0.0.1 only.
Nothing here talks to a real network; the game is pointed at it with
--net-portmap-gateway 127.0.0.1:UDP and --net-portmap-upnp http://127.0.0.1:HTTP/rootDesc.xml.

Behaviour per protocol (constructor arguments / command-line flags):
  pcp:    ok | unsupported (answers like a NAT-PMP-only router) | refuse (NOT_AUTHORIZED) | silent
  natpmp: ok | refuse (result 2) | silent
  upnp:   ok | permanent (error 725 unless the lease is 0) | refuse (error 606) | silent (accepts the
          connection, never answers)
  ext_ip: the router's internet address it reports (e.g. 100.72.1.9 for carrier-grade NAT)

Every request is recorded in `events` (dicts: t, proto, op = map | renew | delete | refused |
nonce_mismatch | addr | ext_ip | get_desc, port, ext, lifetime, nonce) and, with --log FILE, as
JSON lines. Standalone: fake_router.py [--pcp M] [--natpmp M] [--upnp M] [--ext-ip IP] prints
"fake_router: udp PORT http PORT" and runs until interrupted.
"""
import argparse, http.server, json, re, socket, socketserver, struct, threading, time

DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>Fake router (tests)</friendlyName>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl/IPConn</controlURL>
<eventSubURL>/evt/IPConn</eventSubURL><SCPDURL>/WANIPCn.xml</SCPDURL>
</service></serviceList></device></deviceList></device></deviceList></device></root>
"""


def soap_ok(action, inner=''):
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            f'<u:{action}Response xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">{inner}'
            f'</u:{action}Response></s:Body></s:Envelope>\r\n')


def soap_fault(code, desc):
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><s:Fault>'
            '<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>'
            f'<UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>{code}</errorCode>'
            f'<errorDescription>{desc}</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>\r\n')


class FakeRouter:
    def __init__(self, pcp='ok', natpmp='ok', upnp='ok', ext_ip='203.0.113.45', log=None):
        self.pcp, self.natpmp, self.upnp, self.ext_ip = pcp, natpmp, upnp, ext_ip
        self.events = []
        self.lock = threading.Lock()
        self.logf = open(log, 'w') if log else None
        self.maps = {}           # (proto, internal port) -> dict(ext, nonce, lifetime)
        self.t0 = time.time()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.udp.settimeout(0.2)
        self.udp_port = self.udp.getsockname()[1]
        router = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def reply(self, code, body, chunked=False):
                data = body.encode()
                self.send_response(code)
                self.send_header('Content-Type', 'text/xml; charset="utf-8"')
                self.send_header('Connection', 'close')
                if chunked:   # real routers do this too: exercises the client's chunked decoding
                    self.send_header('Transfer-Encoding', 'chunked')
                    self.end_headers()
                    for i in range(0, len(data), 200):
                        part = data[i:i + 200]
                        self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
                    self.wfile.write(b'0\r\n\r\n')
                else:
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                self.close_connection = True

            def do_GET(self):
                if router.upnp == 'silent':
                    time.sleep(10)
                    return
                router.event('upnp', 'get_desc')
                if self.path == '/rootDesc.xml':
                    self.reply(200, DESC, chunked=True)
                else:
                    self.reply(404, '')

            def do_POST(self):
                n = int(self.headers.get('Content-Length', '0'))
                body = self.rfile.read(n).decode('utf-8', 'replace')
                if router.upnp == 'silent':
                    time.sleep(10)
                    return
                action = (self.headers.get('SOAPAction', '').strip('"').split('#') + [''])[1]
                arg = lambda k: (re.search(rf'<{k}>(.*?)</{k}>', body, re.S) or [None, ''])[1].strip()
                if self.path != '/ctl/IPConn':
                    return self.reply(404, '')
                if action == 'GetExternalIPAddress':
                    router.event('upnp', 'ext_ip')
                    return self.reply(200, soap_ok(action, f'<NewExternalIPAddress>{router.ext_ip}</NewExternalIPAddress>'))
                if action == 'AddPortMapping':
                    ext, port, life = int(arg('NewExternalPort')), int(arg('NewInternalPort')), int(arg('NewLeaseDuration'))
                    proto, client = arg('NewProtocol'), arg('NewInternalClient')
                    if router.upnp == 'refuse':
                        router.event('upnp', 'refused', port=port, ext=ext, lifetime=life)
                        return self.reply(500, soap_fault(606, 'Action not authorized'))
                    if router.upnp == 'permanent' and life != 0:
                        router.event('upnp', 'refused', port=port, ext=ext, lifetime=life, why='725')
                        return self.reply(500, soap_fault(725, 'OnlyPermanentLeasesSupported'))
                    key = ('upnp', ext)
                    with router.lock:
                        op = 'renew' if key in router.maps else 'map'
                        router.maps[key] = dict(ext=ext, port=port, lifetime=life, client=client, proto=proto)
                    router.event('upnp', op, port=port, ext=ext, lifetime=life, client=client, protocol=proto)
                    return self.reply(200, soap_ok(action))
                if action == 'DeletePortMapping':
                    ext = int(arg('NewExternalPort'))
                    with router.lock:
                        had = router.maps.pop(('upnp', ext), None)
                    router.event('upnp', 'delete' if had else 'delete_unknown', ext=ext)
                    return self.reply(200 if had else 500, soap_ok(action) if had else soap_fault(714, 'NoSuchEntryInArray'))
                return self.reply(500, soap_fault(401, 'Invalid Action'))

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.http = Server(('127.0.0.1', 0), Handler)
        self.http_port = self.http.server_address[1]
        self.upnp_url = f'http://127.0.0.1:{self.http_port}/rootDesc.xml'
        self.gateway = f'127.0.0.1:{self.udp_port}'
        self.running = True
        self.threads = [threading.Thread(target=self.udp_loop, daemon=True),
                        threading.Thread(target=self.http.serve_forever, daemon=True)]
        for t in self.threads:
            t.start()

    def event(self, proto, op, **kw):
        e = dict(t=round(time.time() - self.t0, 3), proto=proto, op=op, **kw)
        with self.lock:
            self.events.append(e)
            if self.logf:
                self.logf.write(json.dumps(e) + '\n')
                self.logf.flush()

    def ops(self, proto=None):
        with self.lock:
            return [e['op'] for e in self.events if proto is None or e['proto'] == proto]

    def close(self):
        self.running = False
        self.http.shutdown()
        self.http.server_close()
        self.udp.close()
        if self.logf:
            self.logf.close()

    # ---- PCP / NAT-PMP --------------------------------------------------------------------------
    def udp_loop(self):
        while self.running:
            try:
                data, addr = self.udp.recvfrom(1100)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                continue
            out = self.pcp_packet(data) if data[0] == 2 else self.natpmp_packet(data) if data[0] == 0 else None
            if out:
                try:
                    self.udp.sendto(out, addr)
                except OSError:
                    pass

    def epoch(self):
        return int(time.time() - self.t0) + 100

    def pcp_packet(self, d):
        if self.pcp == 'silent':
            self.event('pcp', 'ignored')
            return None
        if self.pcp == 'unsupported':   # what a NAT-PMP-only router answers to version 2
            self.event('pcp', 'unsupported')
            return struct.pack('>BBHI', 0, 0x80 | (d[1] & 0x7f), 1, self.epoch())
        if len(d) < 60 or d[1] != 1:
            return None
        life = struct.unpack('>I', d[4:8])[0]
        nonce, proto = d[24:36], d[36]
        port, sugg = struct.unpack('>HH', d[40:44])
        key = ('pcp', port)
        res, ext = 0, sugg or port
        if self.pcp == 'refuse':
            res = 2
            self.event('pcp', 'refused', port=port, lifetime=life)
        else:
            with self.lock:
                cur = self.maps.get(key)
            if cur and cur['nonce'] != nonce:
                res = 2
                self.event('pcp', 'nonce_mismatch', port=port, lifetime=life, nonce=nonce.hex())
            elif life == 0:
                with self.lock:
                    self.maps.pop(key, None)
                self.event('pcp', 'delete' if cur else 'delete_unknown', port=port, nonce=nonce.hex())
                ext = cur['ext'] if cur else 0
            else:
                ext = cur['ext'] if cur else ext
                with self.lock:
                    self.maps[key] = dict(ext=ext, nonce=nonce, lifetime=life)
                self.event('pcp', 'renew' if cur else 'map', port=port, ext=ext, lifetime=life, nonce=nonce.hex(),
                           protocol=proto)
        ip = bytes(10) + b'\xff\xff' + socket.inet_aton(self.ext_ip)
        return (struct.pack('>BBBBII', 2, 0x81, 0, res, life if res == 0 else 0, self.epoch()) + bytes(12) +
                nonce + bytes([proto, 0, 0, 0]) + struct.pack('>HH', port, ext) + ip)

    def natpmp_packet(self, d):
        if self.natpmp == 'silent':
            self.event('natpmp', 'ignored')
            return None
        if len(d) >= 2 and d[1] == 0:
            self.event('natpmp', 'addr')
            res = 2 if self.natpmp == 'refuse' else 0
            return struct.pack('>BBHI', 0, 128, res, self.epoch()) + socket.inet_aton(self.ext_ip)
        if len(d) < 12 or d[1] not in (1, 2):
            return None
        port, sugg, life = struct.unpack('>HHI', d[4:12])
        key = ('natpmp', port)
        if self.natpmp == 'refuse':
            self.event('natpmp', 'refused', port=port, lifetime=life)
            return struct.pack('>BBHI', 0, 128 + d[1], 2, self.epoch()) + struct.pack('>HHI', port, 0, 0)
        with self.lock:
            cur = self.maps.get(key)
        if life == 0:
            with self.lock:
                self.maps.pop(key, None)
            self.event('natpmp', 'delete' if cur else 'delete_unknown', port=port)
            ext = 0
        else:
            ext = cur['ext'] if cur else (sugg or port)
            with self.lock:
                self.maps[key] = dict(ext=ext, lifetime=life)
            self.event('natpmp', 'renew' if cur else 'map', port=port, ext=ext, lifetime=life)
        return struct.pack('>BBHI', 0, 128 + d[1], 0, self.epoch()) + struct.pack('>HHI', port, ext, life)


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--pcp', default='ok')
    a.add_argument('--natpmp', default='ok')
    a.add_argument('--upnp', default='ok')
    a.add_argument('--ext-ip', default='203.0.113.45')
    a.add_argument('--log')
    o = a.parse_args()
    r = FakeRouter(o.pcp, o.natpmp, o.upnp, o.ext_ip, o.log)
    print(f'fake_router: udp {r.udp_port} http {r.http_port}  (--net-portmap-gateway {r.gateway} '
          f'--net-portmap-upnp {r.upnp_url})', flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        r.close()


if __name__ == '__main__':
    main()
