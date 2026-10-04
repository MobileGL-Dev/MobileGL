#!/usr/bin/env python3
# cdpmedia.py [port] [seconds] - ask a Chrome started with --remote-debugging-port=<port> (and a
# non-default --user-data-dir) which video decoder its first page's players use, and evaluate
# `window.__mglStats` / the element `#t` text of the page. Standard library only (no websocket
# module in the container): a minimal RFC 6455 client.
#   python3 cdpmedia.py 9333 6
# Prints kVideoDecoderName / kIsPlatformVideoDecoder / kVideoTracks properties and media errors as
# they arrive, then the page's stats text.
import base64, json, os, socket, struct, sys, time, urllib.request

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9333
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
page = next(t for t in targets if t.get("type") == "page")
url = page["webSocketDebuggerUrl"]
path = url.split(f"127.0.0.1:{port}", 1)[1]
sock = socket.create_connection(("127.0.0.1", port))
key = base64.b64encode(os.urandom(16)).decode()
sock.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
response = b""
while b"\r\n\r\n" not in response:
    response += sock.recv(4096)
buffer = response.split(b"\r\n\r\n", 1)[1]


def send(message):
    data = json.dumps(message).encode()
    header = bytes([0x81])
    mask = os.urandom(4)
    if len(data) < 126:
        header += bytes([0x80 | len(data)])
    elif len(data) < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", len(data))
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", len(data))
    sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_exact(n):
    global buffer
    while len(buffer) < n:
        chunk = sock.recv(65536)
        if not chunk:
            raise EOFError
        buffer += chunk
    out, buffer = buffer[:n], buffer[n:]
    return out


def recv():
    first, second = recv_exact(2)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(8))[0]
    payload = recv_exact(length)
    return json.loads(payload) if first & 0x0F == 1 else None


send({"id": 1, "method": "Media.enable"})
# Players created before the domain was enabled report nothing: reload so the page makes new ones.
if os.environ.get("CDP_NAVIGATE"):
    send({"id": 3, "method": "Page.navigate", "params": {"url": os.environ["CDP_NAVIGATE"]}})
elif os.environ.get("CDP_RELOAD", "1") == "1":
    send({"id": 3, "method": "Page.reload"})
deadline = time.time() + seconds
sock.settimeout(0.5)
seen = set()
while time.time() < deadline:
    try:
        message = recv()
    except socket.timeout:
        continue
    if not message or "method" not in message:
        continue
    if message["method"] == "Media.playerPropertiesChanged":
        for prop in message["params"]["properties"]:
            if prop["name"] in ("kVideoDecoderName", "kIsPlatformVideoDecoder", "kVideoTracks", "kResolution"):
                line = f"{prop['name']} = {prop['value']}"
                if line not in seen:
                    seen.add(line)
                    print(line)
    elif message["method"] in ("Media.playerErrorsRaised",):
        print("media error:", json.dumps(message["params"])[:300])
send({"id": 2, "method": "Runtime.evaluate",
      "params": {"expression": os.environ.get("CDP_EXPR", "(document.getElementById('t')||{}).textContent"), "returnByValue": True}})
sock.settimeout(3)
while True:
    message = recv()
    if message and message.get("id") == 2:
        print("page:", message.get("result", {}).get("result", {}).get("value"))
        break
