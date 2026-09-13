"""Loopback-only TLS fixture; its ephemeral self-signed cert must NOT be trusted."""
import argparse
import socket
import ssl

parser = argparse.ArgumentParser()
parser.add_argument("--cert", required=True)
parser.add_argument("--key", required=True)
args = parser.parse_args()
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(args.cert, args.key)
with socket.socket() as server:
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    server.settimeout(20)
    print(server.getsockname()[1], flush=True)
    client, _ = server.accept()
    client.settimeout(10)
    try:
        with context.wrap_socket(client, server_side=True) as connection:
            connection.recv(8192)
            connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nunsafe")
    except (ssl.SSLError, ConnectionResetError):
        # Expected: WinHTTP rejects our certificate during the handshake.
        client.close()
