# LAN Messenger v2 protocol

The original line-based protocol remains in `server.py`. The new persistent server is
`server_v2.py`; it uses UTF-8 JSON objects separated by newline (JSON Lines).

## Discovery

Send UDP payload `MESSENGER_DISCOVER_V2` to broadcast address port `5001`.
The server replies with JSON: `{"service":"lan-messenger","version":2,"port":5000,"tls":false}`.

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

## Security and rollout notes

Use `--pairing-code` to control device enrollment. For traffic confidentiality and
integrity, configure `--tls-cert` and `--tls-key`; the current SFML client still speaks
the legacy protocol and must be migrated before using this server. A shared pairing
code is an enrollment mechanism, not end-to-end encryption. Do not expose this server
to untrusted networks. Device-token revocation, per-chat authorization, group chats,
and E2EE are planned follow-up work.
