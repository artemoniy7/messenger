#!/usr/bin/env python3
"""Reliable command-line client for the LAN Messenger v2 protocol.

The local SQLite database is the source of truth for the outbox and inbox.
A server message is acknowledged only after it has been committed locally.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import socket
import sqlite3
import ssl
import sys
import time
import uuid
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = 2
DISCOVERY_PORT = 5001
DISCOVERY_QUERY = b"MESSENGER_DISCOVER_V2"
MAX_FRAME_BYTES = 64 * 1024


class MessengerError(RuntimeError):
    pass


def _encode_frame(payload: dict[str, Any]) -> bytes:
    data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    if len(data) > MAX_FRAME_BYTES:
        raise MessengerError("Request exceeds the protocol frame limit")
    return data


def discover_servers(timeout: float = 1.5) -> list[dict[str, Any]]:
    """Broadcast a v2 discovery request and return unique server endpoints."""
    found: dict[tuple[str, int], dict[str, Any]] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(timeout)
        sock.sendto(DISCOVERY_QUERY, ("255.255.255.255", DISCOVERY_PORT))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                raw, address = sock.recvfrom(2048)
            except socket.timeout:
                break
            try:
                response = json.loads(raw.decode("utf-8"))
                if response.get("service") != "lan-messenger" or response.get("version") != PROTOCOL_VERSION:
                    continue
                port = int(response["port"])
                if not 1 <= port <= 65535:
                    continue
                item = {"host": address[0], "port": port, "tls": bool(response.get("tls", False))}
                found[(item["host"], port)] = item
            except (ValueError, TypeError, KeyError, UnicodeError):
                continue
    return list(found.values())


class LocalStore:
    """Durable local account, outbox, and inbox store."""

    def __init__(self, path: str | Path):
        self.path = str(path)
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS settings (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS outbox (
                    client_message_id TEXT PRIMARY KEY,
                    recipient TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                    sent INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS inbox (
                    server_message_id INTEGER PRIMARY KEY,
                    sender TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    received_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );
            """)

    def connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        return db

    def get(self, key: str, default: str = "") -> str:
        with self.connect() as db:
            row = db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
            return str(row["value"]) if row else default

    def set(self, key: str, value: str) -> None:
        with self.connect() as db:
            db.execute(
                "INSERT INTO settings(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, value),
            )

    def enqueue(self, recipient: str, body: str) -> str:
        message_id = str(uuid.uuid4())
        with self.connect() as db:
            db.execute(
                "INSERT INTO outbox(client_message_id,recipient,body) VALUES(?,?,?)",
                (message_id, recipient, body),
            )
        return message_id

    def pending_outbox(self) -> list[dict[str, Any]]:
        with self.connect() as db:
            return [dict(row) for row in db.execute(
                "SELECT client_message_id,recipient,body FROM outbox WHERE sent=0 ORDER BY created_at"
            ).fetchall()]

    def mark_sent(self, client_message_id: str) -> None:
        with self.connect() as db:
            db.execute("UPDATE outbox SET sent=1 WHERE client_message_id=?", (client_message_id,))

    def store_inbox(self, messages: list[dict[str, Any]]) -> list[int]:
        ids: list[int] = []
        with self.connect() as db:
            for message in messages:
                message_id = int(message["id"])
                db.execute(
                    "INSERT OR IGNORE INTO inbox(server_message_id,sender,body,created_at) VALUES(?,?,?,?)",
                    (message_id, str(message["sender"]), str(message["body"]), str(message["created_at"])),
                )
                ids.append(message_id)
        return ids

    def inbox(self, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            return [dict(row) for row in db.execute(
                "SELECT server_message_id,sender,body,created_at FROM inbox "
                "ORDER BY server_message_id DESC LIMIT ?", (max(1, min(limit, 500)),)
            ).fetchall()]


class MessengerClient:
    def __init__(
        self, host: str, port: int, store: LocalStore, timeout: float = 8.0,
        ca_file: str | None = None,
    ):
        self.host, self.port, self.store, self.timeout = host, port, store, timeout
        self.ca_file = ca_file
        self.sock: socket.socket | None = None
        self.reader = None
        self.user_id: int | None = None

    def connect(self, username: str = "", display_name: str = "", pairing_code: str = "") -> None:
        raw = socket.create_connection((self.host, self.port), timeout=self.timeout)
        raw.settimeout(self.timeout)
        if self.ca_file:
            context = ssl.create_default_context(cafile=self.ca_file)
            self.sock = context.wrap_socket(raw, server_hostname=self.host)
        else:
            self.sock = raw
        self.reader = self.sock.makefile("rb")
        device_id = self.store.get("device_id")
        if not device_id:
            device_id = str(uuid.uuid4())
            self.store.set("device_id", device_id)

        hello: dict[str, Any] = {
            "action": "hello",
            "version": PROTOCOL_VERSION,
            "device_id": device_id,
        }
        token = self.store.get("device_token")
        if token:
            hello["device_token"] = token
        else:
            if not username or not display_name or not pairing_code:
                raise MessengerError("First connection requires username, display name and pairing code")
            hello.update(username=username, display_name=display_name, pairing_code=pairing_code)

        response = self.request(hello)
        if response.get("type") == "error":
            raise MessengerError(str(response.get("code", "server_error")))
        if response.get("type") != "welcome":
            raise MessengerError("Unexpected handshake response")
        self.user_id = int(response["user_id"])
        if response.get("device_token"):
            self.store.set("device_token", str(response["device_token"]))
        if username:
            self.store.set("username", username)

    def request(self, payload: dict[str, Any]) -> dict[str, Any]:
        if self.sock is None or self.reader is None:
            raise MessengerError("Not connected")
        self.sock.sendall(_encode_frame(payload))
        line = self.reader.readline(MAX_FRAME_BYTES + 1)
        if not line:
            raise MessengerError("Server closed the connection")
        if len(line) > MAX_FRAME_BYTES or not line.endswith(b"\n"):
            raise MessengerError("Invalid or oversized server frame")
        response = json.loads(line.decode("utf-8"))
        if not isinstance(response, dict):
            raise MessengerError("Server response must be a JSON object")
        return response

    def users(self) -> list[dict[str, Any]]:
        response = self.request({"action": "users.list"})
        self._raise_server_error(response)
        return list(response.get("users", []))

    def update_profile(self, display_name: str) -> dict[str, Any]:
        response = self.request({"action": "profile.update", "display_name": display_name})
        self._raise_server_error(response)
        return response

    def flush_outbox(self) -> int:
        sent = 0
        for item in self.store.pending_outbox():
            response = self.request({
                "action": "message.send",
                "recipient": item["recipient"],
                "body": item["body"],
                "client_message_id": item["client_message_id"],
            })
            self._raise_server_error(response)
            if response.get("type") != "message.accepted":
                raise MessengerError("Unexpected send response")
            self.store.mark_sent(item["client_message_id"])
            sent += 1
        return sent

    def sync(self, limit: int = 100) -> list[dict[str, Any]]:
        response = self.request({"action": "messages.sync", "limit": limit})
        self._raise_server_error(response)
        messages = response.get("messages", [])
        if not isinstance(messages, list):
            raise MessengerError("Invalid inbox response")
        # Commit locally before ACK: reconnects can safely replay unacknowledged messages.
        ids = self.store.store_inbox(messages)
        if ids:
            ack = self.request({"action": "messages.ack", "message_ids": ids})
            self._raise_server_error(ack)
        return messages

    @staticmethod
    def _raise_server_error(response: dict[str, Any]) -> None:
        if response.get("type") == "error":
            raise MessengerError(str(response.get("code", "server_error")))

    def close(self) -> None:
        if self.reader:
            self.reader.close()
            self.reader = None
        if self.sock:
            self.sock.close()
            self.sock = None

    def __enter__(self) -> "MessengerClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="LAN Messenger v2 client")
    parser.add_argument("--host", default="")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--state", default="data/client.sqlite3")
    parser.add_argument("--ca-file", default=None, help="CA certificate used to verify the server TLS certificate")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("discover")
    register = sub.add_parser("register", help="Pair this device with a server")
    register.add_argument("--username", required=True)
    register.add_argument("--name", required=True)
    register.add_argument("--pairing-code", required=True)
    sub.add_parser("users")
    send = sub.add_parser("send")
    send.add_argument("recipient")
    send.add_argument("message")
    sync = sub.add_parser("sync")
    sync.add_argument("--limit", type=int, default=100)
    profile = sub.add_parser("profile")
    profile.add_argument("display_name")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "discover":
        for server in discover_servers():
            print(f'{server["host"]}:{server["port"]} TLS={server["tls"]}')
        return 0

    if not args.host:
        print("Specify --host or run the discover command first.", file=sys.stderr)
        return 2

    store = LocalStore(args.state)
    client = MessengerClient(args.host, args.port, store, ca_file=args.ca_file)
    try:
        if args.command == "register":
            client.connect(args.username, args.name, args.pairing_code)
        else:
            client.connect()
        if args.command == "users":
            print(json.dumps(client.users(), ensure_ascii=False, indent=2))
        elif args.command == "send":
            message_id = store.enqueue(args.recipient.lstrip("@"), args.message)
            count = client.flush_outbox()
            print(f"Accepted {count} message(s); local id {message_id}")
        elif args.command == "sync":
            client.flush_outbox()
            messages = client.sync(args.limit)
            for message in messages:
                print(f'[{message["created_at"]}] @{message["sender"]}: {message["body"]}')
            if not messages:
                print("No new messages.")
        elif args.command == "profile":
            print(json.dumps(client.update_profile(args.display_name), ensure_ascii=False))
        return 0
    except (OSError, ValueError, MessengerError, ssl.SSLError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
