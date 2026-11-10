import tempfile
import unittest
from pathlib import Path

from server_v2 import MessengerStore


class MessengerStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = MessengerStore(Path(self.temp.name) / "test.sqlite3")
        self.alice_token = "alice-secret"
        self.bob_token = "bob-secret"
        self.alice = self.store.register_device("alice-device", "alice", "Alice", self.alice_token)
        self.bob = self.store.register_device("bob-device", "bob", "Bob", self.bob_token)

    def tearDown(self):
        self.temp.cleanup()

    def test_device_token_authentication(self):
        self.assertEqual(self.store.authenticate_device("alice-device", self.alice_token), self.alice)
        self.assertIsNone(self.store.authenticate_device("alice-device", "wrong-token"))

    def test_offline_message_is_persisted_and_acknowledged(self):
        result = self.store.send_message(self.alice, "bob", "local-1", "Hello over LAN")
        self.assertFalse(result["duplicate"])
        pending = self.store.pending_messages(self.bob)
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0]["body"], "Hello over LAN")
        self.assertEqual(pending[0]["sender"], "alice")
        self.assertEqual(self.store.acknowledge(self.bob, [result["message_id"]]), 1)
        self.assertEqual(self.store.pending_messages(self.bob), [])

    def test_send_is_idempotent(self):
        first = self.store.send_message(self.alice, "bob", "same-id", "Hello")
        second = self.store.send_message(self.alice, "bob", "same-id", "Hello")
        self.assertEqual(first["message_id"], second["message_id"])
        self.assertTrue(second["duplicate"])

    def test_unknown_recipient_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "recipient_not_found"):
            self.store.send_message(self.alice, "missing", "m-1", "Hello")


if __name__ == "__main__":
    unittest.main()
