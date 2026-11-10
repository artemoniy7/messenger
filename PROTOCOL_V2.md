# LAN Messenger v2 protocol

The original line-based protocol remains in `server.py`. The persistent v2 server is
`server_v2.py`; it uses UTF-8 JSON objects separated by newline (JSON Lines).

## Discovery

Send UDP payload `MESSENGER_DISCOVER_V2` to broadcast address port `5001`.
The server replies with JSON:
`{"service":"lan-messenger","version":2,"port":5000,"tls":false}`.

## TCP session

Each frame is one JSON object followed by newline. Maximum frame size is 64 KiB.
The first frame must be:

```json
{"action":"hello","version":2,"device_id":"stable-install-id","username":"alice","display_name":"Alice","pairing_code":"one-time-admin-code"}
```

A newly paired device receives a random `device_token`. Store it locally and protect it
like a password. Subsequent connections send `device_token` instead of `pairing_code`.
The server stores only a SHA-256 hash of the token.

## Actions

- `users.list`: retrieve registered accounts.
- `profile.update`: update the display name.
- `message.send`: submit a direct message with a unique `client_message_id`.
- `messages.sync`: retrieve up to 200 unacknowledged inbox messages.
- `messages.ack`: acknowledge message IDs after the client has persisted them.
- `ping`: keepalive and server clock.

Messages are persisted before the server returns `message.accepted`. A repeated
`client_message_id` from the same sender is idempotent. Offline messages remain in
SQLite until acknowledged by the recipient.

## Reference client behavior

`client_v2.py` stores the device token and a stable device ID in a local SQLite database.
Outgoing messages are first inserted into a durable local outbox. The client marks an
outbox row as sent only after receiving `message.accepted`; retries reuse the same
client-generated ID, making reconnect/retry safe. Incoming messages are committed to
the local inbox before the client sends `messages.ack`. If the connection fails before
the ACK, the server may replay the message and the local primary key prevents duplicates.

The client verifies TLS certificates when `--ca-file` is supplied. Do not send pairing
codes or device tokens over an untrusted plaintext network.

## Security and rollout notes

Use `--pairing-code` to control device enrollment. For traffic confidentiality and
integrity, configure `--tls-cert` and `--tls-key`; the current SFML client still speaks
the legacy protocol and must be migrated before using this server. A shared pairing
code is an enrollment mechanism, not end-to-end encryption. Do not expose this server
to untrusted networks. Device-token revocation, per-chat authorization, group chats,
and E2EE are planned follow-up work.
