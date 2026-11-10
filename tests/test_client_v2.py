import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

from client_v2 import LocalStore, MessengerClient, MessengerError, _encode_frame


class FakeServer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.received = []

    def start(self):
        self.thread.start()

    def run(self):
        conn, _ = self.listener.accept()
        with conn:
            stream = conn.makefile("rb")
            hello = json.loads(stream.readline())
            self.received.append(hello)
            conn.sendall(_encode_frame({"type": "welcome", "user_id": 7, "device_token": "token-123"}))
            while True:
                line = stream.readline()
                if not line:
                    break
                request = json.loads(line)
                self.received.append(request)
                action = request.get("action")
                if action == "message.send":
                    response = {"type": "message.accepted", "client_message_id": request["client_message_id"]}
                elif action == "messages.sync":
                    response = {"type": "messages.sync", "messages": [{
                        "id": 42, "sender": "alice", "body": "hello", "created_at": "2026-01-01T00:00:00Z"
                    }]}
                elif action == "messages.ack":
                    response = {"type": "messages.acknowledged", "count": len(request["message_ids"])}
                else:
                    response = {"type": "users.list", "users": []}
                conn.sendall(_encode_frame(response))
        self.listener.close()


class ClientTests(unittest.TestCase):
    def test_registration_token_outbox_and_ack_after_persist(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = LocalStore(Path(tmp) / "client.sqlite3")
            server = FakeServer()
            server.start()
            with MessengerClient("127.0.0.1", server.port, store) as client:
                client.connect("bob", "Bob", "pair-secret")
                self.assertEqual(client.user_id, 7)
                self.assertEqual(store.get("device_token"), "token-123")
                message_id = store.enqueue("alice", "offline-safe")
                self.assertEqual(client.flush_outbox(), 1)
                self.assertEqual(store.pending_outbox(), [])
                received = client.sync()
                self.assertEqual(len(received), 1)
                self.assertEqual(store.inbox()[0]["body"], "hello")
            server.thread.join(timeout=2)
            actions = [item.get("action") for item in server.received]
            self.assertIn("messages.ack", actions)
            self.assertEqual(server.received[0]["pairing_code"], "pair-secret")

    def test_frame_limit(self):
        with self.assertRaises(MessengerError):
            _encode_frame({"body": "x" * (64 * 1024)})


if __name__ == "__main__":
    unittest.main()
