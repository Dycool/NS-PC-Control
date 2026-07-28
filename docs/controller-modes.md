# Controller Emulation Modes

The Raspberry Pi server now owns the USB controller family. The client only chooses the logical controller shape for input mapping; it no longer chooses HORI or Switch 2 mode directly.

---

## Server USB family flags

By default, the server starts as a Switch 1 Pro/Joy-Con-capable USB gadget.

- `--s2` starts the server with the Switch 2 USB identity. Clients detect this during the server-info handshake and automatically request the Switch 2 protocol variants. The desktop client shows **Scan Amiibo** only while connected to an `--s2` server.
- `--hori` starts the server with the legacy HORI USB identity.
- `--hori` and `--s2` are mutually exclusive.

Examples:

```bash
sudo ns-backend --s2
sudo ns-backend --hori
```

---

## Client logical controller shapes

The desktop/CLI client can choose these logical shapes:

### 1. Pro Controller
- The default shape.
- Supports gyroscope and rumble when the selected server USB family supports them.

### 2. Joy-Con (L)
- Maps input as a standalone left Joy-Con.
- Useful for single-Joy-Con multiplayer games.

### 3. Joy-Con (R)
- Maps input as a standalone right Joy-Con.
- Useful for standalone right-hand Joy-Con controls.

### 4. Joy-Con L + R Pair
- Maps one source controller as a combined Joy-Con pair.
- The server expands it into two virtual USB devices, Joy-Con L and Joy-Con R, which allocates **two virtual ports**.
