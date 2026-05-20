#!/usr/bin/env python3
"""Simple LAN messenger server with broadcast discovery."""

from __future__ import annotations

import argparse
import socket
import threading
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, Optional

DISCOVERY_PORT = 5001
DISCOVERY_QUERY = b"MESSENGER_DISCOVER"


@dataclass
class Client:
    sock: socket.socket
    addr: tuple[str, int]
    name: str


class ChatServer:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        self._clients_by_sock: Dict[socket.socket, Client] = {}
        self._lock = threading.Lock()
        self._running = threading.Event()
        self._running.set()
        
        self._discovery_sock: Optional[socket.socket] = None
        self._server_ip = self._get_local_ip()

    def _get_local_ip(self) -> str:
        """Get the actual local IP address of the server."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            print(f"[INFO] Server IP detected: {ip}")
            return ip
        except Exception:
            return "127.0.0.1"

    def start_discovery(self) -> None:
        """Starts UDP broadcast listener for server discovery."""
        try:
            self._discovery_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._discovery_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            
            # Android requires this for broadcast
            self._discovery_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            
            # Bind to all interfaces
            self._discovery_sock.bind(('', DISCOVERY_PORT))
            
            thread = threading.Thread(target=self._discovery_listener, daemon=True)
            thread.start()
            print(f"[INFO] Discovery listener started on UDP port {DISCOVERY_PORT}")
            print(f"[INFO] Server will respond with IP: {self._server_ip}")
        except Exception as e:
            print(f"[WARN] Could not start discovery listener: {e}")

    def _discovery_listener(self) -> None:
        """Listens for discovery requests and responds."""
        while self._running.is_set():
            try:
                self._discovery_sock.settimeout(1.0)
                data, addr = self._discovery_sock.recvfrom(1024)
                
                if data == DISCOVERY_QUERY:
                    response = f"MESSENGER_SERVER {self._server_ip}:{self.port}".encode('utf-8')
                    self._discovery_sock.sendto(response, addr)
                    print(f"[INFO] Discovery response sent to {addr[0]}:{addr[1]} -> {self._server_ip}:{self.port}")
            except socket.timeout:
                continue
            except Exception as e:
                if self._running.is_set():
                    print(f"[WARN] Discovery error: {e}")

    def start(self) -> None:
        self.server_sock.bind((self.host, self.port))
        self.server_sock.listen()
        self.start_discovery()
        print(f"[INFO] Server started on {self._server_ip}:{self.port}")
        print(f"[INFO] Connect with: ./messenger {self._server_ip} {self.port}")

        discovery_thread = threading.Thread(target=self._discovery_loop, daemon=True)
        discovery_thread.start()

        try:
            while self._running.is_set():
                self.server_sock.settimeout(1.0)
                try:
                    client_sock, addr = self.server_sock.accept()
                    print(f"[INFO] Client connected from {addr[0]}:{addr[1]}")
                    thread = threading.Thread(
                        target=self._handle_client,
                        args=(client_sock, addr),
                        daemon=True,
                    )
                    thread.start()
                except socket.timeout:
                    continue
        except KeyboardInterrupt:
            print("\n[INFO] Keyboard interrupt received, shutting down...")
        finally:
            self.stop()

    def stop(self) -> None:
        if not self._running.is_set():
            return

        self._running.clear()

        if self._discovery_sock:
            try:
                self._discovery_sock.close()
            except OSError:
                pass

        with self._lock:
            clients = list(self._clients_by_sock.values())
            self._clients_by_sock.clear()

        for client in clients:
            try:
                client.sock.sendall(b"[SERVER] Server is shutting down.\n")
            except OSError:
                pass
            try:
                client.sock.close()
            except OSError:
                pass

        try:
            self.server_sock.close()
        except OSError:
            pass

        print("[INFO] Server stopped")


    def _discovery_loop(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((self.host, DISCOVERY_PORT))
        except OSError:
            # fallback for hosts like 0.0.0.0 / mobile runtimes
            sock.bind(("", DISCOVERY_PORT))

        while self._running.is_set():
            try:
                data, addr = sock.recvfrom(1024)
            except OSError:
                break

            if data != DISCOVERY_QUERY:
                continue

            response = f"MESSENGER_SERVER {self.port}".encode("utf-8")
            try:
                sock.sendto(response, addr)
            except OSError:
                pass

        try:
            sock.close()
        except OSError:
            pass

    def _register_client(self, client_sock: socket.socket, addr: tuple[str, int], name: str) -> Client:
        client = Client(sock=client_sock, addr=addr, name=name)
        with self._lock:
            self._clients_by_sock[client_sock] = client
        return client

    def _remove_client(self, client_sock: socket.socket) -> Optional[Client]:
        with self._lock:
            return self._clients_by_sock.pop(client_sock, None)

    def _broadcast(self, message: str, *, exclude: Optional[socket.socket] = None) -> None:
        data = (message + "\n").encode("utf-8", errors="replace")

        with self._lock:
            clients = list(self._clients_by_sock.values())

        for client in clients:
            if exclude is not None and client.sock is exclude:
                continue
            try:
                client.sock.sendall(data)
            except OSError:
                self._remove_client(client.sock)

    def _recv_line(self, sock_file) -> Optional[str]:
        line = sock_file.readline()
        if not line:
            return None
        return line.rstrip("\r\n")

    def _format_chat_message(self, name: str, message: str) -> str:
        ts = datetime.now().strftime("%H:%M:%S")
        return f"[{ts}] {name}: {message}"

    def _handle_client(self, client_sock: socket.socket, addr: tuple[str, int]) -> None:
        print(f"[INFO] Connection from {addr[0]}:{addr[1]}")
        sock_file = client_sock.makefile("r", encoding="utf-8", newline="\n")

        try:
            client_sock.sendall(b"Enter your username:\n")
            first_line = self._recv_line(sock_file)
            if first_line is None:
                client_sock.close()
                return

            name = self._sanitize_name(first_line, fallback=f"user_{addr[1]}")
            client = self._register_client(client_sock, addr, name)

            client_sock.sendall(f"OK {client.name}\n".encode("utf-8"))
            self._broadcast(f"[SERVER] {client.name} joined the chat", exclude=client_sock)

            while self._running.is_set():
                line = self._recv_line(sock_file)
                if line is None:
                    break

                if not line.strip():
                    continue

                if line.startswith("/quit"):
                    break

                if line.startswith("/name "):
                    new_name = self._sanitize_name(line[6:], fallback=client.name)
                    old_name = client.name
                    client.name = new_name
                    client_sock.sendall(f"[SERVER] Your new name: {new_name}\n".encode("utf-8"))
                    if old_name != new_name:
                        self._broadcast(f"[SERVER] {old_name} is now known as {new_name}", exclude=client_sock)
                    continue

                self._broadcast(self._format_chat_message(client.name, line), exclude=client_sock)
        except (ConnectionError, OSError):
            pass
        finally:
            removed = self._remove_client(client_sock)
            try:
                sock_file.close()
            except OSError:
                pass
            try:
                client_sock.close()
            except OSError:
                pass

            if removed is not None:
                self._broadcast(f"[SERVER] {removed.name} left the chat")
                print(f"[INFO] Disconnected {removed.name} ({removed.addr[0]}:{removed.addr[1]})")

    @staticmethod
    def _sanitize_name(raw_name: str, fallback: str) -> str:
        cleaned = raw_name.strip()
        if not cleaned:
            return fallback
        cleaned = cleaned.replace("\n", " ").replace("\r", " ")
        return cleaned[:24]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LAN messenger server with auto-discovery")
    parser.add_argument("--host", default="0.0.0.0", help="Host/IP to bind (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=5000, help="Port to bind (default: 5000)")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = ChatServer(args.host, args.port)
    server.start()


if __name__ == "__main__":
    main()
