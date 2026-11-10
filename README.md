# LAN Messenger

A local-network messenger prototype with an SFML desktop interface and a Python server.

## Current architecture

- `server.py` remains available as the legacy server. `messenger.cpp` now uses the v2 JSON Lines protocol.
- `server_v2.py` is the persistent v2 server (JSON Lines over TCP, UDP discovery, SQLite message storage, device pairing).
- `client_v2.py` is the v2 reference client. It provides LAN discovery, device registration, a durable local outbox, inbox synchronization, and acknowledgements after local persistence.

Both the Python reference client and the SFML desktop client now speak the v2 protocol. The SFML client discovers the v2 UDP service, pairs a device, loads contacts, sends direct messages, and periodically syncs/ACKs incoming messages. The GUI currently supports one-to-one text conversations.

## Run the v2 server

Python 3.10+; no third-party Python packages are required.

```bash
python server_v2.py --host 0.0.0.0 --port 5000 --pairing-code "choose-a-long-local-code"
```

The server stores its database in `data/messenger.sqlite3` by default. Keep the pairing code private. For traffic encryption, configure `--tls-cert` and `--tls-key`.

## Pair a client

Discover servers on the same broadcast domain:

```bash
python client_v2.py discover
```

Register a device (replace the address and values):

```bash
python client_v2.py --host 192.168.1.10 register --username alice --name "Alice" --pairing-code "choose-a-long-local-code"
```

The client stores its device token and local message database in `data/client.sqlite3`. Protect this file like a password. Subsequent commands reuse the registered device:

```bash
python client_v2.py --host 192.168.1.10 users
python client_v2.py --host 192.168.1.10 send bob "Hello over the LAN"
python client_v2.py --host 192.168.1.10 sync
python client_v2.py --host 192.168.1.10 profile "Alice Example"
```

For a TLS server signed by a private CA, add `--ca-file path/to/ca.pem` before the subcommand. Do not disable certificate verification on untrusted networks.

## Build the SFML desktop client

The desktop application uses **SFML 3** and CMake 3.20+.

```bash
cmake -S . -B build
cmake --build build --config Release
```

If SFML 3 is installed in a non-standard location, pass its prefix with
`-DCMAKE_PREFIX_PATH=/path/to/SFML`.

## SFML desktop client (v2)

The SFML application reads first-device enrollment settings from environment variables:

- `MESSENGER_USERNAME`: account username (letters, digits, underscore, dot, hyphen).
- `MESSENGER_DISPLAY_NAME`: initial display name.
- `MESSENGER_PAIRING_CODE`: the same pairing code configured on the server.

Example (Linux/macOS):

```bash
export MESSENGER_USERNAME=alice
export MESSENGER_DISPLAY_NAME="Alice Example"
export MESSENGER_PAIRING_CODE="choose-a-long-local-code"
./build/messenger 192.168.1.10 5000
```

On Windows PowerShell:

```powershell
$env:MESSENGER_USERNAME = "alice"
$env:MESSENGER_DISPLAY_NAME = "Alice Example"
$env:MESSENGER_PAIRING_CODE = "choose-a-long-local-code"
.\\build\\Release\\messenger.exe 192.168.1.10 5000
```

The first successful pairing stores the device ID and device token in
`.messenger_device` in the application's working directory. Protect this file like a password; do not commit or share it. The
`.messenger_outbox` file may contain message text that has not yet reached the server,
and `.messenger_received_ids` tracks already-saved server messages to prevent duplicates
when an ACK must be retried; keep these files private as well. Later launches reuse the stored device token.
If no server IP is supplied, the client broadcasts the v2 discovery request on the LAN.

Open **Settings → Contacts**, select a user, then type in the message field and press
Enter. Outgoing messages are written to the local `.messenger_outbox` queue and retried
with the same client message ID until the server accepts them. Incoming direct messages
are synchronized periodically and saved to `chat_history.txt` before the client
acknowledges them to the server.

The current native client uses plaintext TCP unless a future TLS transport is added.
Use it only on a trusted LAN; the Python reference client already supports verified
TLS with a configured CA file. Do not expose plaintext sessions to untrusted networks.

## Tests

```bash
python -m unittest discover -s tests -v
```

## Scope and limitations

This is an evolving LAN messenger, not yet a production-secure product. Pairing is not end-to-end encryption. The current v2 implementation supports direct text messages; group chats, attachments, calls, device revocation, and end-to-end encryption remain future work. The native SFML client currently supports direct text chats only; group chats, attachments, calls, device revocation, verified TLS transport in the native client, and end-to-end encryption remain future work.
