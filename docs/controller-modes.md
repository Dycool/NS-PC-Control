# Controller Emulation Modes

The Raspberry Pi server can emulate several controller profiles, allowing you to choose the layout and features that best match your setup.

---

## Supported Controller Profiles

You can choose which controller type the server emulates for your connection:

### 1. Pro Controller
- The default profile.
- Emulates a standard Switch Pro Controller.
- Supports **gyroscope** and **rumble** (requires client-side support and hardware support).

### 2. Hori Controller
- Emulates a wired Hori Pad.
- Does **not** support gyroscope or rumble.

### 3. Joy-Con (L)
- Emulates a standalone left Joy-Con.
- Useful for single-Joy-Con multiplayer games.

### 4. Joy-Con (R)
- Emulates a standalone right Joy-Con.
- Useful for standalone right-hand Joy-Con controls.

### 5. Joy-Con L + R Pair
- Emulates a combined Joy-Con pair (Left + Right).
- Exposes the connection as two separate virtual USB devices (Joy-Con L and Joy-Con R) connected to the Switch, which can be grouped as a pair. This allocates **two virtual ports** on the server.

