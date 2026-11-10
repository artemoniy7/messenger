#!/usr/bin/env python3
"""Persistent LAN messenger server (JSON-lines protocol, SQLite, UDP discovery).

This is the v2 protocol server. The legacy server.py remains available while the
C++ client is migrated, so existing installations are not broken.
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import secrets
import socket
import sqlite3
import ssl
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

DISCOVERY_PORT = 5001
DISCOVERY_QUERY = b"MESSENGER_DISCOVER_V2"
PROTOCOL_VERSION = 2
MAX_FRAME_BYTES = 64 * 1024
MAX_MESSAGE_CHARS = 16_000


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


class MessengerStore:
    """Small SQLite persistence layer; each operation owns its connection."""

    def __init__(self, database: str | Path) -> None:
        self.database = str(database)
        Path(self.database).parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            db.executescript(
                """
                PRAGMA journal_mode=WAL;
                PRAGMA foreign_keys=ON;
                CREATE TABLE IF NOT EXISTS users (
                    id INTEGER PRIMARY KEY,
                    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
                    display_name TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS devices (
                    id TEXT PRIMARY KEY,
                    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
                    token_hash TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    last_seen_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    client_message_id TEXT NOT NULL,
                    sender_id INTEGER NOT NULL REFERENCES users(id),
                    recipient_id INTEGER NOT NULL REFERENCES users(id),
                    body TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    delivered_at TEXT,
                    UNIQUE(sender_id, client_message_id)
                );
                CREATE INDEX IF NOT EXISTS idx_messages_inbox
                    ON messages(recipient_id, delivered_at, id);
                """
            )

    def _connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.database, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        return db

    def register_device(
        self, device_id: str, username: str, display_name: str, token: str
    ) -> int:
        now = utc_now()
        with self._connect() as db:
            row = db.execute("SELECT id FROM users WHERE username=?", (username,)).fetchone()
            if row is None:
                cur = db.execute(
                    "INSERT INTO users(username, display_name, created_at) VALUES(?,?,?)",
                    (username, display_name, now),
                )
                user_id = int(cur.lastrowid)
            else:
                user_id = int(row["id"])
            db.execute(
                "INSERT INTO devices(id,user_id,token_hash,created_at,last_seen_at) VALUES(?,?,?,?,?)",
                (device_id, user_id, token_hash(token), now, now),
            )
            return user_id

    def authenticate_device(self, device_id: str, token: str) -> int | None:
        with self._connect() as db:
            row = db.execute(
                "SELECT id,user_id,token_hash FROM devices WHERE id=?", (device_id,)
            ).fetchone()
            if row is None or not hmac.compare_digest(row["token_hash"], token_hash(token)):
                return None
            db.execute("UPDATE devices SET last_seen_at=? WHERE id=?", (utc_now(), device_id))
            return int(row["user_id"])

    def update_profile(self, user_id: int, display_name: str) -> None:
        with self._connect() as db:
            db.execute("UPDATE users SET display_name=? WHERE id=?", (display_name, user_id))

    def users(self) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute(
                "SELECT username,display_name FROM users ORDER BY username COLLATE NOCASE"
            ).fetchall()
            return [dict(row) for row in rows]

    def send_message(
        self, sender_id: int, recipient: str, client_message_id: str, body: str
    ) -> dict[str, Any]:
        with self._connect() as db:
            target = db.execute(
                "SELECT id FROM users WHERE username=?", (recipient,)
            ).fetchone()
            if target is None:
                raise ValueError("recipient_not_found")
            existing = db.execute(
                """SELECT id,created_at FROM messages
                   WHERE sender_id=? AND client_message_id=?""",
                (sender_id, client_message_id),
            ).fetchone()
            if existing:
                return {"message_id": existing["id"], "created_at": existing["created_at"], "duplicate": True}
            created = utc_now()
            cur = db.execute(
                """INSERT INTO messages(client_message_id,sender_id,recipient_id,body,created_at)
                   VALUES(?,?,?,?,?)""",
                (client_message_id, sender_id, int(target["id"]), body, created),
            )
            return {"message_id": int(cur.lastrowid), "created_at": created, "duplicate": False}

    def pending_messages(self, user_id: int, limit: int = 100) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute(
                """SELECT m.id,m.client_message_id,m.body,m.created_at,
                          u.username AS sender
                   FROM messages m JOIN users u ON u.id=m.sender_id
                   WHERE m.recipient_id=? AND m.delivered_at IS NULL
                   ORDER BY m.id LIMIT ?""",
                (user_id, min(max(limit, 1), 200)),
            ).fetchall()
            return [dict(row) for row in rows]

    def acknowledge(self, user_id: int, message_ids: list[int]) -> int:
        safe_ids = [int(value) for value in message_ids[:200] if isinstance(value, int) and value > 0]
        if not safe_ids:
            return 0
        placeholders = ",".join("?" for _ in safe_ids)
        with self._connect() as db:
            cur = db.execute(
                f"""UPDATE messages SET delivered_at=?
                    WHERE recipient_id=? AND delivered_at IS NULL AND id IN ({placeholders})""",
                [utc_now(), user_id, *safe_ids],
            )
            return cur.rowcount


class MessengerServer:
    def __init__(
        self, host: str, port: int, database: str, pairing_code: str,
        certfile: str | None = None, keyfile: str | None = None,
    ) -> None:
        self.host, self.port = host, port
        self.store = MessengerStore(database)
        self.pairing_code = pairing_code
        self.ssl_context: ssl.SSLContext | None = None
        if certfile or keyfile:
            if not certfile or not keyfile:
                raise ValueError("Both --tls-cert and --tls-key are required")
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certfile, keyfile)
            self.ssl_context = context
        self.server_socket: socket.socket | None = None
        self.discovery_socket: socket.socket | None = None
        self.running = threading.Event()
        self.running.set()
        self.clients_lock = threading.Lock()
        self.online: dict[int, set[socket.socket]] = {}

    def start(self) -> None:
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind((self.host, self.port))
        self.server_socket.listen(64)
        self.port = int(self.server_socket.getsockname()[1])
        self._start_discovery()
        scheme = "TLS" if self.ssl_context else "plaintext"
        print(f"[LAN-MESSENGER] listening on {self.host}:{self.port} ({scheme})")
        try:
            while self.running.is_set():
                self.server_socket.settimeout(1.0)
                try:
                    conn, address = self.server_socket.accept()
                except socket.timeout:
                    continue
                threading.Thread(target=self._handle_client, args=(conn, address), daemon=True).start()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def _start_discovery(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", DISCOVERY_PORT))
        sock.settimeout(1.0)
        self.discovery_socket = sock
        threading.Thread(target=self._discovery_loop, daemon=True).start()

    def _discovery_loop(self) -> None:
        assert self.discovery_socket is not None
        while self.running.is_set():
            try:
                payload, address = self.discovery_socket.recvfrom(1024)
                if payload == DISCOVERY_QUERY:
                    response = json.dumps({
                        "service": "lan-messenger", "version": PROTOCOL_VERSION,
                        "port": self.port, "tls": self.ssl_context is not None,
                    }).encode("utf-8")
                    self.discovery_socket.sendto(response, address)
            except socket.timeout:
                continue
            except OSError:
                break

    @staticmethod
    def _send(conn: socket.socket, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        conn.sendall(data)

    @staticmethod
    def _receive(conn: socket.socket, buffer: bytearray) -> dict[str, Any] | None:
        while b"\n" not in buffer:
            chunk = conn.recv(4096)
            if not chunk:
                return None
            buffer.extend(chunk)
            if len(buffer) > MAX_FRAME_BYTES:
                raise ValueError("frame_too_large")
        line, _, remainder = buffer.partition(b"\n")
        buffer[:] = remainder
        value = json.loads(line.decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("json_object_required")
        return value

    def _attach(self, user_id: int, conn: socket.socket) -> None:
        with self.clients_lock:
            self.online.setdefault(user_id, set()).add(conn)

    def _detach(self, user_id: int | None, conn: socket.socket) -> None:
        if user_id is None:
            return
        with self.clients_lock:
            sockets = self.online.get(user_id)
            if sockets:
                sockets.discard(conn)
                if not sockets:
                    self.online.pop(user_id, None)

    def _handle_client(self, raw_conn: socket.socket, address: tuple[str, int]) -> None:
        conn: socket.socket = raw_conn
        user_id: int | None = None
        buffer = bytearray()
        try:
            conn.settimeout(60)
            if self.ssl_context:
                conn = self.ssl_context.wrap_socket(conn, server_side=True)
            hello = self._receive(conn, buffer)
            if hello is None or hello.get("action") != "hello":
                self._send(conn, {"type": "error", "code": "hello_required"})
                return
            if hello.get("version") != PROTOCOL_VERSION:
                self._send(conn, {"type": "error", "code": "unsupported_version"})
                return
            device_id = str(hello.get("device_id", ""))
            if not device_id or len(device_id) > 80:
                self._send(conn, {"type": "error", "code": "invalid_device_id"})
                return

            device_token = str(hello.get("device_token", ""))
            user_id = self.store.authenticate_device(device_id, device_token) if device_token else None
            if user_id is None:
                supplied_code = str(hello.get("pairing_code", ""))
                if not self.pairing_code or not hmac.compare_digest(supplied_code, self.pairing_code):
                    self._send(conn, {"type": "error", "code": "pairing_required"})
                    return
                username = str(hello.get("username", "")).strip()
                display_name = str(hello.get("display_name", username)).strip()
                if not username or len(username) > 32 or not all(c.isalnum() or c in "_.-" for c in username):
                    self._send(conn, {"type": "error", "code": "invalid_username"})
                    return
                if not display_name or len(display_name) > 64:
                    self._send(conn, {"type": "error", "code": "invalid_display_name"})
                    return
                issued_token = secrets.token_urlsafe(32)
                try:
                    user_id = self.store.register_device(device_id, username, display_name, issued_token)
                except sqlite3.IntegrityError:
                    self._send(conn, {"type": "error", "code": "username_or_device_exists"})
                    return
                self._send(conn, {"type": "welcome", "user_id": user_id, "device_token": issued_token,
                                  "protocol_version": PROTOCOL_VERSION})
            else:
                self._send(conn, {"type": "welcome", "user_id": user_id,
                                  "protocol_version": PROTOCOL_VERSION})

            self._attach(user_id, conn)
            while self.running.is_set():
                request = self._receive(conn, buffer)
                if request is None:
                    break
                action = request.get("action")
                try:
                    if action == "profile.update":
                        name = str(request.get("display_name", "")).strip()
                        if not name or len(name) > 64:
                            raise ValueError("invalid_display_name")
                        self.store.update_profile(user_id, name)
                        self._send(conn, {"type": "profile.updated", "display_name": name})
                    elif action == "users.list":
                        self._send(conn, {"type": "users.list", "users": self.store.users()})
                    elif action == "message.send":
                        recipient = str(request.get("recipient", "")).strip()
                        body = str(request.get("body", ""))
                        client_id = str(request.get("client_message_id", ""))
                        if not body or len(body) > MAX_MESSAGE_CHARS or not client_id or len(client_id) > 80:
                            raise ValueError("invalid_message")
                        result = self.store.send_message(user_id, recipient, client_id, body)
                        self._send(conn, {"type": "message.accepted", **result,
                                          "client_message_id": client_id})
                    elif action == "messages.sync":
                        messages = self.store.pending_messages(user_id, int(request.get("limit", 100)))
                        self._send(conn, {"type": "messages.sync", "messages": messages})
                    elif action == "messages.ack":
                        ids = request.get("message_ids", [])
                        if not isinstance(ids, list):
                            raise ValueError("message_ids_must_be_array")
                        count = self.store.acknowledge(user_id, ids)
                        self._send(conn, {"type": "messages.acknowledged", "count": count})
                    elif action == "ping":
                        self._send(conn, {"type": "pong", "server_time": utc_now()})
                    else:
                        self._send(conn, {"type": "error", "code": "unknown_action"})
                except (ValueError, TypeError) as exc:
                    self._send(conn, {"type": "error", "code": str(exc) or "invalid_request"})
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError, ssl.SSLError):
            pass
        finally:
            self._detach(user_id, conn)
            try:
                conn.close()
            except OSError:
                pass
            if user_id is not None:
                print(f"[INFO] user_id={user_id} disconnected from {address[0]}")

    def stop(self) -> None:
        if not self.running.is_set():
            return
        self.running.clear()
        for sock in (self.server_socket, self.discovery_socket):
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LAN Messenger v2 server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--database", default="data/messenger.sqlite3")
    parser.add_argument("--pairing-code", default="", help="Required to register a new device")
    parser.add_argument("--tls-cert", default=None)
    parser.add_argument("--tls-key", default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.pairing_code:
        raise SystemExit("Set --pairing-code before exposing the server to your LAN.")
    MessengerServer(args.host, args.port, args.database, args.pairing_code,
                    args.tls_cert, args.tls_key).start()


if __name__ == "__main__":
    main()
