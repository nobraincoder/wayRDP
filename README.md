# wayRDP Server (`wayrdp`)

A high-performance, standalone Remote Desktop Protocol (RDP) server engineered specifically for **KDE Plasma 6 on Wayland**.

Unlike standard RDP implementations that simply mirror physical monitors, `wayrdp` spawns an isolated, on-demand virtual display (`VIRTUAL-1`) via `xdg-desktop-portal-kde`. This enables true headless remote sessions, dynamic client-side resolution adaptation, and zero-copy hardware-accelerated video streaming at 60 FPS without disturbing local monitors.

---

[Features](#key-features) • [Prerequisites & Install](#system-requirements) • [Configuration](#configuration-reference) • [Client Setup](#connecting-from-clients) • [Troubleshooting](#troubleshooting--faq) • [Known Limitations](#known-limitations) • [Roadmap](#whats-next-roadmap) • [Acknowledgements](#acknowledgements--inspirations)

---

## Key Features

- **Hardware-Accelerated Zero-Latency Video (VA-API / H.264):**
  - Streams desktop frames with zero-copy DMA-BUF capture via PipeWire and KPipeWire at up to 60 FPS.
  - Zero-latency GPU encoding (`async_depth = 1` / `tune = zerolatency`) eliminating trapped frames or trailing ghost artifacts.
  - Support for H.264 Main (CABAC & 8x8 transform for crisp text), H.264 Baseline, VP8, and VP9.
- **Dynamic Framerate & Bitrate Adaptation:**
  - Real-time RTT latency measurement via FreeRDP RDPGFX frame acknowledgments.
  - Exponentially Weighted Moving Average (EWMA) network smoothing dynamically scaling framerate (20–60 FPS) and visual quality (50–95%) without tearing down the stream.
- **Bidirectional Clipboard File Transfer (`[MS-RDPECLIP]`):**
  - **Client to Host:** Copy files on Windows/macOS and paste directly into KDE Dolphin via `text/uri-list` on `KSystemClipboard` (streamed to `$XDG_RUNTIME_DIR/rdp-clipboard/`).
  - **Host to Client:** Copy files in Dolphin and paste directly into Windows Explorer or macOS Finder via `FileGroupDescriptorW` and `FileContents` virtual channels.
- **Dynamic Display Resizing & HiDPI Preservation (`[MS-RDPEDISP]`):**
  - Automatically negotiates client window resizing on the fly via `kscreen-doctor` without tearing down the session.
  - Preserves client high-DPI fractional scaling (e.g., 1.25x / 1.5x) preventing desktop blurriness and cursor distortion.
- **Direct Low-Latency EIS Input:** Direct `libei` socket integration with KWin bypassing D-Bus IPC for near-zero input latency.
- **KDE System Input Sync & Natural Scrolling:**
  - Automatically parses and synchronizes touchpad natural scrolling, scroll speed multiplier (`ScrollFactor`), and left-handed mouse mapping from KDE Plasma settings (`kcminputrc`).
- **Host Cursor Theme & Size Sync:**
  - Automatically matches the host cursor theme and size with clean 32-bit ARGB alpha transparency and LRU shape caching.
- **Session Lifecycle & Security Integration:**
  - **Sleep Inhibit:** Prevents system suspend while an active RDP session is connected (`org.freedesktop.PowerManagement.Inhibit`).
  - **Auto-Lock on Disconnect:** Automatically triggers KDE screen lock (`org.freedesktop.ScreenSaver.Lock`) when the client disconnects.
- **Idle Power Saver:**
  - Dynamically throttles stream capture to 5 FPS after 30 seconds of user inactivity, resuming 60 FPS instantly upon input.
- **Multi-Monitor Support:**
  - Full support for multi-monitor client layouts (`/multimon`) by calculating a unified bounding canvas across all active client displays.
- **Zero-Prompt Remote Activation:** Automatically pre-seeds `org.freedesktop.impl.portal.PermissionStore` to start virtual screen sessions completely headless without GUI desktop prompts.
- **Orderly Teardown & Display Recovery:** Traps POSIX termination signals (`SIGINT`/`SIGTERM`) and client disconnects to destroy virtual displays and restore physical output (`eDP-1`) priority cleanly.
- **Systemd User Service Integration:** Managed as an unprivileged user service (`systemctl --user`) with automated restart on failure.

---

## System Requirements

- **Operating System:** Linux with systemd
- **Desktop Environment:** KDE Plasma 6.x running on Wayland (`kwin_wayland`)
- **Core Services:** `pipewire`, `wireplumber`, `xdg-desktop-portal`, `xdg-desktop-portal-kde`, `kscreen-doctor`
- **Hardware Acceleration:** GPU supporting VA-API H.264 encoding (Intel QuickSync, AMD Radeon Mesa, or NVIDIA VA-API wrapper)

<details>
<summary><b>📦 Distribution Packages (Arch / Fedora / Ubuntu)</b></summary>

#### 1. Arch Linux / EndeavourOS / Manjaro
```bash
sudo pacman -S --needed \
    base-devel cmake extra-cmake-modules pkgconf \
    qt6-base qt6-multimedia kguiaddons kpipewire \
    freerdp libxkbcommon libei pam openssl \
    libpulse libkscreen libva libva-utils
```

#### 2. Fedora 40 / 41+
```bash
sudo dnf install \
    cmake extra-cmake-modules gcc-c++ pkgconfig \
    qt6-qtbase-devel qt6-qtmultimedia-devel kf6-kguiaddons-devel \
    kpipewire-devel freerdp-devel libxkbcommon-devel \
    libei-devel pam-devel openssl-devel pulseaudio-libs-devel \
    libkscreen-devel libva-devel
```

#### 3. Ubuntu 24.10+ / Debian Trixie (Plasma 6 & Qt6)
```bash
sudo apt-get install \
    build-essential cmake extra-cmake-modules pkg-config \
    qt6-base-dev qt6-multimedia-dev libkf6guiaddons-dev \
    libkpipewire-dev libfreerdp-server3-dev libfreerdp3-dev \
    libwinpr3-dev libxkbcommon-dev libei-dev libpam0g-dev \
    libssl-dev libpulse-dev libkscreen-dev va-driver-all
```

</details>

---

## Quick Installation

Run the automated installer script from the repository directory:

```bash
git clone https://github.com/nobraincoder/wayRDP.git
cd wayRDP
./install.sh
```

The script automatically checks dependencies, compiles with Release optimizations, installs to `~/.local/bin/wayrdp`, configures `~/.config/systemd/user/wayrdp.service`, creates `~/.config/wayrdp.env`, and reloads systemd.

<details>
<summary><b>🛠️ Manual Build & Installation (CMake)</b></summary>

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build . -j$(nproc)

# Install binary to ~/.local/bin
cp wayrdp ~/.local/bin/

# Install systemd user service
mkdir -p ~/.config/systemd/user
cp ../wayrdp.service ~/.config/systemd/user/

# Create configuration file
cp ../wayrdp.env.example ~/.config/wayrdp.env
chmod 600 ~/.config/wayrdp.env

# Reload systemd
systemctl --user daemon-reload
```

</details>

---

## Configuration Reference

### Automatic KDE System Settings Synchronization

`wayrdp` automatically reads and synchronizes user input preferences directly from your KDE Plasma session:
- **Touchpad Natural Scrolling:** Detected via `~/.config/kcminputrc` (`[Touchpad] NaturalScroll`) and live KWin D-Bus (`org.kde.KWin.InputDeviceManager`).
- **Touchpad & Mouse Scroll Speed:** Synchronized from KDE's `ScrollFactor` setting.
- **Left-Handed Mouse Mapping:** Synchronized from `[Mouse] LeftHanded` in `kcminputrc`.
- **Cursor Theme & Size:** Extracted from `kcminputrc` and `kdeglobals`, dynamically matching your local desktop cursor appearance.

> [!NOTE]
> An integrated file system watcher (`QFileSystemWatcher`) monitors `~/.config/kcminputrc` and `~/.config/kdeglobals`. Whenever you change mouse, touchpad, or cursor preferences in KDE System Settings GUI, `wayrdp` detects the change and reloads configuration instantly in real-time without restarting the service or disconnecting active sessions.

---

### Environment Overrides (`~/.config/wayrdp.env`)

If you want your remote RDP session to diverge from your local KDE desktop settings, edit `~/.config/wayrdp.env` to specify runtime overrides:

```ini
# Server listening port (default: 3390)
# Defaults to 3390 so it can run alongside official KRdp (port 3389) without conflicts.
RDP_PORT=3390

# Fixed RDP password authentication.
# Leave commented out to authenticate using your standard system PAM Linux user credentials.
# RDP_PASSWORD=your_secure_password

# Hardware Video Encoder Backend (vaapi, nvenc, x264)
RDP_ENCODER=vaapi

# Video Codec Profile (h264_main, h264_baseline, vp8, vp9)
# Default: h264_main (enables CABAC and 8x8 transforms for razor-sharp text on modern clients)
RDP_CODEC=h264_main

# Video Encoding Quality: 30 to 100 (default: 95 for visually lossless text and UI clarity)
RDP_QUALITY=95

# Hardware Encoder Latency & Pipeline Preference (speed, quality, size)
# Default: speed (enforces async_depth=1 in VA-API and -tune zerolatency in software)
# CRITICAL: Setting this to quality/size will cause closing window frames to linger on static desktops.
RDP_ENCODER_PREFERENCE=speed

# Idle Power Saver timeout in seconds (default: 30)
# Automatically throttles video stream to 5 FPS after N seconds of no client input.
# Resumes full 60 FPS instantly upon mouse movement or keypress. Set to 0 to disable.
RDP_IDLE_TIMEOUT_SEC=30

# Session Auto-Lock on Disconnect (default: 1)
# Locks the KDE desktop session via KScreenLocker / loginctl when the RDP client disconnects.
RDP_LOCK_ON_DISCONNECT=1

# --- Optional Input Overrides (Takes precedence over KDE System Settings) ---

# Explicitly override touchpad natural scrolling (1 = natural/inverted, 0 = traditional)
# RDP_NATURAL_SCROLL=1

# Explicitly override scroll multiplier (e.g. 0.125; default scales with KDE ScrollFactor)
# RDP_SCROLL_SCALE=0.125

# Directional Inversions:
# Invert both axes:
# RDP_INVERT_SCROLL=1
# Invert horizontal wheel/trackpad scrolling only:
# RDP_INVERT_HSCROLL=1
# Invert vertical wheel/trackpad scrolling only:
# RDP_INVERT_VSCROLL=1

# Explicitly swap left and right mouse buttons (1 = left-handed, 0 = right-handed)
# RDP_LEFT_HANDED=1
```

---

## Known Limitations

- **Session Lifecycle & Cold-Boot (No SDDM / Login Screen Support):** `wayrdp` operates as an unprivileged user service (`systemctl --user`) attaching to an existing `kwin_wayland` session. It cannot initiate logins from a cold-boot SDDM display manager screen; the host user must already be logged in (or have auto-login enabled).
- **Single Active Client Session:** Designed for personal workstation access. Supports one active client connection at a time (sequential reconnects); concurrent multi-seat or multi-tenant desktop sessions are not supported.
- **Audio Output & Microphone Redirection (`[MS-RDPSND]`, `[MS-RDPEAI]`):** Remote audio playback and microphone capture redirection are currently in active development. Audio generated in the session plays on the host machine's physical audio devices.
- **Drive Redirection (`[MS-RDPEFS]`):** Mounting client local drives or folders as virtual drives inside Dolphin is not yet implemented; file transfers are handled via bidirectional clipboard copy/paste.
- **Multi-Monitor Canvas Model:** Multi-monitor client setups (`/multimon`) project a single unified bounding canvas across displays rather than instantiating discrete separate virtual displays in KWin.
- **Touch & Stylus Gestures:** Touch and pen inputs map to standard absolute mouse pointer events; native multi-touch gestures (pinch-to-zoom) and pen pressure/tilt sensitivity are not forwarded.
- **Network Transport (TCP Only):** `wayrdp` communicates exclusively over TCP; high-loss UDP transport (`[MS-RDPEUDP]`) is not implemented.
- **Video Pipeline Limits:** Streams strictly in 8-bit SDR H.264 (`AVC420`) capped at 60 FPS. HDR10, wide color gamuts (10-bit), and 120+ Hz refresh rates are not supported.
- **H.264 Protocol Constraint:** Microsoft RDP specifications ([MS-RDPEGFX]) strictly require H.264 (`AVC420` / `AVC444`) for official client applications (Windows `mstsc.exe`, macOS Microsoft Remote Desktop, iOS/Android apps). Neither HEVC (H.265) nor AV1 are supported by official Microsoft RDP clients.
- **TLS Authentication (Non-NLA):** FreeRDP on Linux uses standard TLS encryption with PAM validation rather than Windows CredSSP/NLA. When connecting from Windows `mstsc.exe`, the `/prompt` switch or credentials saved via `cmdkey` must be used so Windows presents the credential entry dialog.
- **Desktop Environment & Compositor Requirement:** Engineered exclusively for KDE Plasma 6 running on Wayland (`kwin_wayland`) using `xdg-desktop-portal-kde` and `libei`. X11 sessions, GNOME Mutter, and generic wlroots compositors are not supported.
- **Hardware Encoder Throughput at 4K / Retina on Older Silicon:** At 4K or high-DPI Retina (2x scaling) resolutions, video stream throughput is limited by the host GPU's hardware video encoder (VPU). Older integrated GPUs (e.g., Intel Gen 9 Skylake / HD Graphics 520) may cap throughput around 25–35 FPS under 4K workloads due to fixed-function silicon limits, whereas 1080p and 1440p run at a continuous 60 FPS.

---

## Managing the Service

Use standard `systemctl --user` commands:

| Action | Command |
| :--- | :--- |
| **Start server** | `systemctl --user start wayrdp` |
| **Stop server** | `systemctl --user stop wayrdp` |
| **Restart server** | `systemctl --user restart wayrdp` |
| **Enable autostart on login** | `systemctl --user enable wayrdp` |
| **Check status** | `systemctl --user status wayrdp` |
| **View live logs** | `journalctl --user -u wayrdp -f` |

---

## Connecting from Clients

Connect to your host machine's IP on port **3390** (e.g. `192.168.1.100:3390`):

### Windows (Remote Desktop Connection / `mstsc.exe`)
Because FreeRDP on Linux uses standard TLS encryption without CredSSP/NLA, Windows `mstsc.exe` does not show a login prompt by default unless prompted:

1. **Option A (Recommended):** Press `Win + R` and run:
   ```cmd
   mstsc /v:<HOST_IP>:3390 /prompt
   ```
   This tells Windows to display the Windows Security prompt to enter your Linux username and password.

2. **Option B (Save credentials in Windows Credential Manager):**
   ```cmd
   cmdkey /generic:TERMSRV/<HOST_IP>:3390 /user:<USER> /pass:<PASSWORD>
   ```
   Then connect normally via `mstsc.exe <HOST_IP>:3390`.

### macOS (Windows App / Microsoft Remote Desktop)
1. Add PC $\rightarrow$ PC Name: `<HOST_IP>:3390`.
2. User Account: Enter your Linux username and password.
3. Display: Enable "Optimize for Retina display" for native high-DPI scaling.

### Linux (`xfreerdp`)
```bash
xfreerdp /v:<HOST_IP>:3390 /u:<USER> /p:<PASSWORD> /gfx:AVC420 /network:auto /clipboard +dynamic-resolution
```

---

## Troubleshooting & FAQ

<details>
<summary><b>1. Port 3389 vs Port 3390 (Coexisting with official KRdp)</b></summary>

If official KDE Remote Desktop (`krdpserver`) is enabled in KDE System Settings, it binds to port `3389`. `wayrdp` defaults to port `3390` so both services can coexist simultaneously without port collision.

</details>

<details>
<summary><b>2. Virtual Display Does Not Appear (Portal Check)</b></summary>

Ensure `xdg-desktop-portal-kde` is running in your active session:
```bash
ps aux | grep xdg-desktop-portal-kde
```
Also verify that `kwin_wayland` is your active compositor (`echo $XDG_SESSION_TYPE` should return `wayland`).

</details>

<details>
<summary><b>3. Buffer Starvation (Failed receiving filtered frame: Cannot allocate memory)</b></summary>

- **Symptom:** During rapid client window resizing or bursty 60 FPS motion, the server log displays `Failed receiving filtered frame: Cannot allocate memory` and the video stream freezes or drops frames.
- **Root Cause:** When `maxPendingFrames` is configured too low (e.g. $\le 4$), VA-API's internal GPU surface pool is exhausted during dynamic resolution changes before the client acknowledges preceding frames.
- **Fix:** `wayrdp` allocates a bounded queue of **25 pending frames** (`m_stream->setMaxPendingFrames(25)`). This guarantees sufficient buffer headroom for the hardware encoder pipeline without introducing measurable latency.

</details>

<details>
<summary><b>4. Trailing Ghost Windows / Lagging Fade-out Animations</b></summary>

- **Symptom:** When a window is closed or an animation fades out, a ghost image of the window lingers on the client screen until the mouse is moved or a key is pressed.
- **Root Cause:** By default in KPipeWire, non-speed encoding modes configure VA-API with `async_depth = 2`. Because Wayland PipeWire capture only generates frames upon screen damage, the final clean frame rendered by KWin gets trapped inside the GPU's internal asynchronous buffer queue waiting for a subsequent frame to push it out.
- **Fix:** Always ensure `RDP_ENCODER_PREFERENCE=speed` (default in `wayrdp`). This configures `async_depth = 1` in `h264vaapiencoder` and `-tune zerolatency` in software encoders, flushing every completed frame to the network immediately.

</details>

<details>
<summary><b>5. DRM Render Node Permissions (/dev/dri/renderD128)</b></summary>

- **Symptom:** `kpipewire_vaapi_logging: Failed to initialize VA-API display` or falling back to CPU software encoding.
- **Root Cause:** The unprivileged user session lacks read/write permissions to the DRM render node.
- **Verification & Fix:**
  ```bash
  ls -l /dev/dri/renderD128
  groups $USER
  ```
  Ensure your user belongs to both `video` and `render` groups:
  ```bash
  sudo usermod -aG video,render $USER
  ```
  Log out and log back in for group membership to take effect.

</details>

<details>
<summary><b>6. Intel Driver Selection (intel-media-driver vs i965)</b></summary>

- **Symptom:** `vainfo` reports driver errors or H.264 encode entrypoints (`VAEntrypointEncSlice`) are missing.
- **Root Cause:** On modern Intel GPUs (Broadwell / Skylake / Gen 8+ and newer), the older legacy driver `libva-intel-driver` (`i965`) lacks support for Wayland DMA-BUF modifiers.
- **Fix:** Install `intel-media-driver` (`iHD`):
  - **Arch Linux:** `sudo pacman -S intel-media-driver`
  - **Fedora:** `sudo dnf install intel-media-driver`
  - **Ubuntu/Debian:** `sudo apt-get install intel-media-va-driver`
  Confirm entrypoint availability:
  ```bash
  vainfo --display drm --device /dev/dri/renderD128
  ```
  Ensure `VAProfileH264Main : VAEntrypointEncSlice` or `VAProfileH264ConstrainedBaseline : VAEntrypointEncSlice` appears in the list.

</details>

<details>
<summary><b>7. Forcing the Hardware VA-API Encoder (KPIPEWIRE_FORCE_ENCODER)</b></summary>

If KPipeWire's automatic encoder negotiation selects software `x264` instead of GPU acceleration, verify that `wayrdp.service` contains:
```ini
Environment=KPIPEWIRE_FORCE_ENCODER=h264_vaapi
```
This is included by default in the provided `wayrdp.service` unit.

</details>

---

## What's Next (Roadmap)

Active engineering and planned milestones for upcoming `wayRDP` releases:

- **Audio Output & Microphone Redirection (`[MS-RDPSND]`, `[MS-RDPEAI]`):**
  - Implement remote desktop audio streaming via a dedicated PipeWire virtual sink, routing host system audio directly over RDP to the client.
  - Add remote microphone capture redirection (`[MS-RDPEAI]`) to pass client voice inputs back into the host system.
- **Session Auto-Unlock on Connect:**
  - When credentials are authenticated during RDP handshake, automatically unlock the locked KDE Plasma session via KScreenLocker / loginctl so the desktop is instantly usable without a secondary unlock prompt.
- **Remote Drive Redirection (`[MS-RDPEFS]`):**
  - Seamlessly mount client-side folders and local disk drives directly into the host user's KDE Dolphin filesystem hierarchy alongside existing clipboard copy/paste.
- **Discrete Multi-Monitor Virtual Displays:**
  - Evolve from the current unified bounding-canvas model to provisioning discrete, independent virtual outputs (`VIRTUAL-1`, `VIRTUAL-2`, etc.) in KWin for true physical multi-monitor client setups.
- **Windows Authentication & NLA Support:**
  - Implement CredSSP / Network Level Authentication (NLA) support alongside standard Linux PAM, enabling seamless native authentication workflows for Windows RDP clients.
- **System-Level Startup Daemon & Cold-Boot Login:**
  - Extend beyond the current user-session daemon to support pre-session system daemons capable of attaching to display managers (like SDDM) or initializing virtual sessions prior to local user login.
- **Loss-Tolerant UDP Transport (`[MS-RDPEUDP]`):**
  - Implement RDP-UDP transport channels to deliver smooth framerates over high-latency, packet-loss-prone cellular or public internet connections.
- **Advanced Touch & Stylus Gestures (`[MS-RDPEI]`):**
  - Forward native multi-touch gestures (pinch-to-zoom, two-finger pan) and active pen/stylus pressure and tilt sensitivity into KWin.
- **High Refresh Rates (120/144 Hz) & HDR Color Pipelines:**
  - Add high-refresh-rate stream pacing and investigate 10-bit / HDR color pipelines for high-end client hardware.
- **Compositor Architecture Expansion:**
  - Implement native GNOME Mutter and generic wlroots backends for `IVirtualDisplayBackend`.

---

## Acknowledgements & Inspirations

`wayrdp` builds directly on the shoulders of the open-source Linux display and remote desktop communities. The project is heavily inspired by and indebted to:

- **[KDE KRdp (`krdpserver`)](https://invent.kde.org/plasma/krdp):** Provided fundamental architectural references for FreeRDP integration within the modern KDE ecosystem, session security lifecycle, and KPipeWire video streaming pipelines.
- **[gnome-remote-desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop):** Pioneered the headless virtual monitor remote desktop paradigm on Wayland compositors.
- **[FreeRDP](https://www.freerdp.org/):** The foundational open-source Remote Desktop Protocol core that powers wayRDP's dynamic virtual channels (`RDPGFX`, `RDPEDISP`, `CLIPRDR`), connection negotiation, and TLS cryptography.
- **[KPipeWire](https://invent.kde.org/plasma/kpipewire) & [libei](https://gitlab.freedesktop.org/libinput/libei):** Provided the low-latency building blocks for zero-copy DMA-BUF video capture and direct Wayland emulated input injection.
