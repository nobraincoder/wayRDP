# wayRDP Server (`wayrdp`)

A high-performance, standalone Remote Desktop Protocol (RDP) server engineered specifically for **KDE Plasma 6 on Wayland**.

Unlike standard RDP implementations that simply mirror physical monitors, `wayrdp` spawns an isolated, on-demand virtual display (`VIRTUAL-1`) via `xdg-desktop-portal-kde`. This enables true headless remote sessions, dynamic client-side resolution adaptation, and zero-copy hardware-accelerated video streaming at 60 FPS without disturbing local monitors.

---

[Features](#key-features) • [Prerequisites & Install](#system-requirements) • [Configuration](#configuration-reference) • [Client Setup](#connecting-from-clients) • [Troubleshooting](#troubleshooting--faq) • [Known Limitations](#known-limitations) • [Acknowledgements](#acknowledgements--inspirations)

---

## Key Features

While official KDE KRdp provides screen-sharing and mirroring of physical monitors, `wayrdp` is built specifically for headless virtual sessions with features not supported by KRdp:

- **Isolated Headless Virtual Display (`VIRTUAL-1`):**
  - Spawns an on-demand, private virtual monitor in KWin via `xdg-desktop-portal-kde`.
  - Physical local monitors stay powered off or private; operates seamlessly with laptop lids closed or on completely headless mini PCs.
- **Dynamic Display Resizing & HiDPI Preservation (`[MS-RDPEDISP]`):**
  - Dynamically resizes the virtual display resolution in real-time via `kscreen-doctor` when resizing client windows without dropping the session.
  - Preserves fractional scaling (1.25x, 1.5x) on high-DPI and Retina client screens to eliminate blurriness and cursor distortion.
- **Bidirectional Clipboard File Transfer (`[MS-RDPECLIP]`):**
  - Supports true file copying and pasting between client and host alongside text clipboard:
    - **Client to Host:** Copy files on Windows/macOS and paste directly into KDE Dolphin via `KSystemClipboard`.
    - **Host to Client:** Copy files in Dolphin and paste directly into Windows Explorer or macOS Finder via `FileGroupDescriptorW` / `FileContents` virtual channels.
- **Zero-Prompt Remote Activation:**
  - Pre-seeds `org.freedesktop.impl.portal.PermissionStore` at startup, allowing headless connections without requiring physical confirmation at the host screen.
- **Session Auto-Lock on Disconnect:**
  - Automatically invokes KDE screen lock (`org.freedesktop.ScreenSaver.Lock` / `loginctl`) the instant the client disconnects to secure the workstation.
- **Idle Power Saver:**
  - Automatically throttles video capture and encoding to 5 FPS after 30 seconds of inactivity, instantly ramping back to 60 FPS upon mouse or keyboard input.
- **Dynamic Multi-Monitor Bounding Canvas (`/multimon`):**
  - Dynamically calculates a unified bounding canvas across all active client displays when connecting in multi-monitor mode.
- **KDE System Settings Live Sync:**
  - Synchronizes touchpad natural scrolling, scroll speed multiplier (`ScrollFactor`), and left-handed mouse mapping directly from `kcminputrc`, hot-reloading changes in real time via `QFileSystemWatcher`.
- **Orderly Teardown & Primary Display Recovery:**
  - Automatically restores primary physical display output (`eDP-1`) priorities upon disconnect or process termination (`SIGINT`/`SIGTERM`) to avoid orphaned virtual outputs.

---

## System Requirements

- **Operating System:** Linux with systemd
- **Desktop Environment:** KDE Plasma 6.x running on Wayland (`kwin_wayland`)
- **Core Services:** `pipewire`, `wireplumber`, `xdg-desktop-portal`, `xdg-desktop-portal-kde`, `kscreen-doctor`
- **Hardware Acceleration:** GPU supporting VA-API H.264 encoding (Intel QuickSync, AMD Radeon Mesa, or NVIDIA VA-API wrapper)

> [!NOTE]
> **Platform Testing & Compatibility:** `wayrdp` is designed for **KDE Plasma 6 on Wayland**. It is **actively tested on Arch Linux**, and **verified to compile on Debian 13 (Trixie) and Fedora 40/41+**.

<details>
<summary><b>Distribution Packages & Build Instructions (Arch / Fedora / Debian / Ubuntu / openSUSE)</b></summary>

#### 1. Arch Linux / EndeavourOS / Manjaro (Actively Tested)
```bash
sudo pacman -S --needed \
    base-devel cmake extra-cmake-modules pkgconf \
    qt6-base qt6-multimedia kguiaddons kpipewire \
    freerdp libxkbcommon libei pam openssl \
    libpulse libkscreen libva libva-utils
```

#### 2. Fedora 40 / 41+ (Verified Compilation)
```bash
sudo dnf install \
    cmake extra-cmake-modules gcc-c++ pkgconfig \
    qt6-qtbase-devel qt6-qtmultimedia-devel kf6-kguiaddons-devel \
    kpipewire-devel freerdp-devel libwinpr-devel libxkbcommon-devel \
    libei-devel pam-devel openssl-devel openssl pulseaudio-libs-devel \
    libkscreen-devel libkscreen libva-devel
```

#### 3. Debian 13 (Trixie) / Ubuntu 24.10+ (Verified Compilation)
```bash
sudo apt-get install \
    build-essential cmake extra-cmake-modules pkg-config \
    qt6-base-dev qt6-multimedia-dev libkf6guiaddons-dev \
    libkpipewire-dev freerdp3-dev libwinpr3-dev \
    libxkbcommon-dev libei-dev libpam0g-dev \
    libssl-dev openssl libpulse-dev libkscreen-dev libkscreen-bin va-driver-all
```

#### 4. openSUSE Tumbleweed & Slowroll (Package Reference)
```bash
sudo zypper install \
    cmake extra-cmake-modules gcc-c++ pkg-config \
    qt6-base-devel qt6-multimedia-devel kf6-kguiaddons-devel \
    libkpipewire-devel freerdp-devel libwinpr3-devel \
    libxkbcommon-devel libei-devel pam-devel libopenssl-devel \
    openssl libpulse-devel libkscreen6-devel libkscreen6-plugin
```

</details>

<details>
<summary><b>Unsupported Environments (Incompatible Systems)</b></summary>

The following systems are fundamentally incompatible with `wayrdp`:

1. **Distributions on KDE Plasma 5** (e.g. Kubuntu 22.04 / 24.04 LTS, Debian 12 Bookworm, RHEL 8 / 9, Rocky Linux, Linux Mint):
   - **Why:** Plasma 5 lacks the KWin Wayland virtual display portal (`types: 4u` was introduced in Plasma 6.0), has no built-in EIS (Emulated Input Server), and runs on Qt 5 / KF5 instead of Qt 6 / KF6.
2. **Non-KDE Desktop Environments & Compositors** (GNOME, Hyprland, Sway, COSMIC, Cinnamon, XFCE):
   - **Why:** `wayrdp` is built specifically to interact with KWin's internal Wayland display pipeline, `xdg-desktop-portal-kde`, and `kscreen-doctor`.
3. **X11 / Xorg Sessions**:
   - **Why:** `wayrdp` requires a Wayland compositor (`kwin_wayland`) for PipeWire DMA-BUF buffer capture and EIS virtual device emulation.

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
<summary><b>Manual Build & Installation (CMake)</b></summary>

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
# Strongly recommended for Windows mstsc.exe clients: automatically enables native
# Network Level Authentication (NLA) with Windows Security credential prompts.
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
When `RDP_PASSWORD` is configured in `~/.config/wayrdp.env`, `wayrdp` automatically enables **Network Level Authentication (NLA)**. Windows `mstsc.exe` will seamlessly display the native Windows Security credentials prompt upon connection:

1. Open Remote Desktop Connection (`mstsc.exe`).
2. Enter `<HOST_IP>:3390` and click **Connect**.
3. When prompted by Windows Security, enter your username and `RDP_PASSWORD`.

> [!TIP]
> If `RDP_PASSWORD` is left commented out, `wayrdp` falls back to Linux PAM authentication via TLS. In PAM mode, if your Windows client does not prompt for credentials locally, pass `/prompt` (`mstsc /v:<HOST_IP>:3390 /prompt`) or enter your username in "Show Options".

### macOS & iOS / iPadOS (Windows App / Microsoft Remote Desktop)
1. Add PC → PC Name: `<HOST_IP>:3390`.
2. User Account: Enter your Linux username and password.
3. Display: Enable native resolution / Retina display for crisp scaling.

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
- **Root Cause:** When `maxPendingFrames` is configured too low (e.g. ≤ 4), VA-API's internal GPU surface pool is exhausted during dynamic resolution changes before the client acknowledges preceding frames.
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

## Known Limitations

- **Microsoft Windows App on Android Incompatibility:** The official Microsoft "Windows App" (formerly Microsoft Remote Desktop) on Android explicitly disables H.264 video decoding (`RDPGFX_CAPS_FLAG_AVC_DISABLED`) across all capability sets and only supports legacy bitmap/GDI codecs. Because `wayrdp` operates exclusively on a zero-copy hardware-accelerated video streaming pipeline (`[MS-RDPEGFX]`), the Microsoft client on Android is currently incompatible.
- **Microsoft RDP Protocol Constraints:** Microsoft RDP specifications ([MS-RDPEGFX]) require H.264 (`AVC420` / `AVC444`) for official client applications (Windows `mstsc.exe`, macOS Microsoft Remote Desktop / Windows App, iOS/iPadOS). Modern codecs like AV1 or HEVC (H.265) are not supported by Microsoft RDP specifications.
- **KDE Plasma 6 Wayland Exclusivity:** Engineered specifically around KWin Wayland, `xdg-desktop-portal-kde`, and `libei`. Traditional X11 sessions, GNOME Mutter, and generic wlroots compositors are outside the scope of this architecture.
- **Personal Workstation Model:** Designed for single-user workstation remote access rather than multi-tenant or concurrent multi-seat enterprise terminal server hosting.
- **Hardware Silicon Limits at 4K / Retina:** At 4K or high-DPI Retina (2x scaling) resolutions, stream throughput is physically bounded by the host GPU's hardware video encoder (VPU). Older integrated GPUs (such as Intel Gen 9 Skylake / HD Graphics 520) cap throughput at 25–35 FPS under 4K workloads due to fixed-function silicon limits, whereas 1080p and 1440p run at a continuous 60 FPS.

---

## Acknowledgements & Inspirations

`wayrdp` builds directly on the shoulders of the open-source Linux display and remote desktop communities. The project is heavily inspired by and indebted to:

- **[KDE KRdp (`krdpserver`)](https://invent.kde.org/plasma/krdp):** Provided fundamental architectural references for FreeRDP integration within the modern KDE ecosystem, session security lifecycle, and KPipeWire video streaming pipelines.
- **[gnome-remote-desktop](https://gitlab.gnome.org/GNOME/gnome-remote-desktop):** Pioneered the headless virtual monitor remote desktop paradigm on Wayland compositors.
- **[FreeRDP](https://www.freerdp.org/):** The foundational open-source Remote Desktop Protocol core that powers wayRDP's dynamic virtual channels (`RDPGFX`, `RDPEDISP`, `CLIPRDR`), connection negotiation, and TLS cryptography.
- **[KPipeWire](https://invent.kde.org/plasma/kpipewire) & [libei](https://gitlab.freedesktop.org/libinput/libei):** Provided the low-latency building blocks for zero-copy DMA-BUF video capture and direct Wayland emulated input injection.
