# LAN Messenger

A local-network messenger prototype with an SFML desktop interface and a Python server.

## Current architecture

- `server.py` and the original `messenger.cpp` keep the legacy protocol for backward compatibility.
- `server_v2.py` is the persistent v2 server (JSON Lines over TCP, UDP discovery, SQLite message storage, device pairing).
- `client_v2.py` is the v2 reference client. It provides LAN discovery, device registration, a durable local outbox, inbox synchronization, and acknowledgements after local persistence.

The v2 command-line client is the first client implementation for the new protocol. The SFML interface has not yet been migrated and still speaks the legacy protocol.

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

## Tests

```bash
python -m unittest discover -s tests -v
```

## Scope and limitations

This is an evolving LAN messenger, not yet a production-secure product. Pairing is not end-to-end encryption. The current v2 implementation supports direct text messages; group chats, attachments, calls, device revocation, and end-to-end encryption remain future work. The SFML desktop UI migration is a separate next step.
