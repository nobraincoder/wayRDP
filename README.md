# wayRDP Server (`wayrdp`)

A high-performance, standalone Remote Desktop Protocol (RDP) server engineered specifically for **KDE Plasma 6 on Wayland**.

Unlike standard RDP implementations that simply mirror physical monitors, `wayrdp` spawns an isolated, on-demand virtual display (`VIRTUAL-1`) via `xdg-desktop-portal-kde`. This enables true headless remote sessions, dynamic client-side resolution adaptation, and zero-copy hardware-accelerated video streaming at 60 FPS without disturbing local monitors.

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

### Platform Prerequisites
- **Operating System:** Linux with systemd
- **Desktop Environment:** KDE Plasma 6.x running on Wayland (`kwin_wayland`)
- **Core Services:**
  - `pipewire` and `wireplumber`
  - `xdg-desktop-portal` and `xdg-desktop-portal-kde`
  - `libkscreen` (`kscreen-doctor`)
- **Hardware Acceleration:** GPU supporting VA-API H.264 encoding (Intel QuickSync, AMD Radeon via Mesa VA-API, or NVIDIA via VA-API driver wrapper).

### Distribution Packages

#### 1. Arch Linux / EndeavourOS / Manjaro
```bash
sudo pacman -S --needed \
    base-devel \
    cmake \
    extra-cmake-modules \
    pkgconf \
    qt6-base \
    qt6-multimedia \
    kguiaddons \
    kpipewire \
    freerdp \
    libxkbcommon \
    libei \
    pam \
    openssl \
    libpulse \
    libkscreen \
    libva \
    libva-utils
```

#### 2. Fedora 40 / 41+
```bash
sudo dnf install \
    cmake \
    extra-cmake-modules \
    gcc-c++ \
    pkgconfig \
    qt6-qtbase-devel \
    qt6-qtmultimedia-devel \
    kf6-kguiaddons-devel \
    kpipewire-devel \
    freerdp-devel \
    libxkbcommon-devel \
    libei-devel \
    pam-devel \
    openssl-devel \
    pulseaudio-libs-devel \
    libkscreen-devel \
    libva-devel
```

#### 3. Ubuntu 24.10+ / Debian Trixie (Plasma 6 & Qt6)
```bash
sudo apt-get install \
    build-essential \
    cmake \
    extra-cmake-modules \
    pkg-config \
    qt6-base-dev \
    qt6-multimedia-dev \
    libkf6guiaddons-dev \
    libkpipewire-dev \
    libfreerdp-server3-dev \
    libfreerdp3-dev \
    libwinpr3-dev \
    libxkbcommon-dev \
    libei-dev \
    libpam0g-dev \
    libssl-dev \
    libpulse-dev \
    libkscreen-dev \
    va-driver-all
```

---

## Quick Installation

Run the automated installer script from the repository directory:

```bash
git clone https://github.com/your-repo/wayrdp.git
cd wayrdp
./install.sh
```

The script will:
1. Validate required build tools and libraries.
2. Build the project using Release optimizations.
3. Install the executable to `~/.local/bin/wayrdp`.
4. Install the systemd user service unit to `~/.config/systemd/user/wayrdp.service`.
5. Create a default configuration at `~/.config/wayrdp.env`.
6. Reload the user systemd daemon.

---

## Manual Build & Installation

If you prefer building manually with CMake:

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

---

## Configuration Reference

Edit `~/.config/wayrdp.env` to customize runtime settings:

```ini
# Server listening port (default: 3390)
# Note: Defaults to 3390 to avoid conflicting with official KRdp service on 3389.
RDP_PORT=3390

# Fixed RDP password authentication.
# Leave commented out to authenticate using your system PAM Linux user credentials.
# RDP_PASSWORD=your_secure_password

# Hardware Video Encoder Backend (vaapi, nvenc, x264)
RDP_ENCODER=vaapi

# Video Codec Profile (h264_main, h264_baseline, vp8, vp9)
RDP_CODEC=h264_main

# Video Encoding Quality: 30 to 100 (default: 95 for razor-sharp text)
RDP_QUALITY=95

# Direction Inversion: 1 to explicitly invert scroll direction
# RDP_INVERT_SCROLL=1

# Scroll Speed Multiplier (e.g. 0.125)
# RDP_SCROLL_SCALE=0.125

# Idle Power Saver timeout in seconds (throttles to 5 FPS on inactivity; 0 to disable)
RDP_IDLE_TIMEOUT_SEC=30

# Session Auto-Lock on Disconnect (1 to lock KDE desktop session via KScreenLocker)
RDP_LOCK_ON_DISCONNECT=1

# Left-Handed Mouse Mapping (1 to swap left and right buttons)
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

### Port 3389 vs Port 3390
If official KDE Remote Desktop (`krdpserver`) is enabled in KDE System Settings, it binds to port `3389`. `wayrdp` defaults to port `3390` so both services can coexist without port collision.

### Verifying Hardware Acceleration
To confirm your system has working VA-API encoding:
```bash
vainfo
```
Look for `VAProfileH264... : VAEntrypointEncSlice` in the output. If missing, verify your graphics driver (`intel-media-driver` or `mesa`).

### Virtual Display Does Not Appear
Ensure `xdg-desktop-portal-kde` is running in your session:
```bash
ps aux | grep xdg-desktop-portal-kde
```
Also verify that `kwin_wayland` is your active compositor (`echo $XDG_SESSION_TYPE` should return `wayland`).
