# FobosOne Web Spectrum Analyzer

A real-time, browser-based spectrum analyzer + waterfall for the **FobosOne SDR**, with
absolute-power (dBm) calibration. Ships with two interchangeable backends — a fast native
**C++** service and a **Python** (Flask/SocketIO) fallback — selectable at runtime from the
web UI.

![mode: FFT / SCAN / CAL / CFG](templates/index.html)

---

## Features

- **FFT mode** — live spectrum + waterfall at one center frequency.
- **SCAN mode** — wideband panorama (hardware scan, 70–6000 MHz) with max-hold.
- **CAL mode** — drive an absolute-power (dBm) calibration measurement (see `calibration/`).
- **CFG mode** — switch the running backend (C++ ↔ Python) in place.
- Mouse-wheel **frequency zoom** on both spectrum and waterfall; DC-notch; grid; auto-range.
- **CAL dBm** toggle applies the measured calibration so the trace/peak read in absolute dBm;
  a coherent **time-domain carrier meter** (dBm / dBmV) for the strongest tone.
- Compact binary WebSocket frames (1 byte/bin + `permessage-deflate`, ~90 KB/s at 8192).

---

## Architecture

```
Browser (templates/index.html + static/ws-shim.js)
   │  REST  (control: /api/attach,start,stop,params,cal/*,config/backend)
   │  WS    (push:    binary 'SPC2' spectrum frames + text status events)
   ▼
launch.sh  ── reads backend.conf ──►  bin/spectrumd   (C++)   ← default
                                 └►   python/app.pyc  (Python/Flask)
   │
   ▼  libfobos_sdr  →  FobosOne over USB
```

- **C++ backend** (`cpp/`): a from-scratch HTTP+WebSocket server on POSIX sockets (no web
  framework), FFTW DSP, libfobos, true multithreading. Split into documented modules — see
  `cpp/*.h`. Entry point `cpp/main.cpp` → `bin/spectrumd`.
- **Python backend** (`python/app.py`): the original Flask-SocketIO implementation, kept as a
  fallback; compiled to `app.pyc` at build time.
- **Frontend** (`templates/index.html`, `static/ws-shim.js`): single-page UI; the shim adapts
  a native WebSocket to a Socket.IO-style API and decodes the binary spectrum frames.
- **calibration/**: the CAL tab's measurement orchestrator + helpers (spawned by the backend).
- **siggen_sweep/**: a helper that sweeps the Aaronia generator so SCAN + Max-Hold paints a
  full-band coverage picture (manual test utility).

---

## Build & install

Prereqs (Raspberry Pi OS / Debian):

```sh
sudo apt install cmake g++ libfftw3-dev libssl-dev zlib1g-dev \
                 python3 python3-numpy python3-flask python3-flask-socketio
# plus the installed libfobos_sdr (libfobos-sdr-agile, sudo make install + ldconfig)
```

Build both backends and install the systemd service:

```sh
cd web_spectrum
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local/fobos/web_spectrum
cmake --build build -j            # builds bin/spectrumd + python/app.pyc
sudo cmake --install build        # installs + enables the service
sudo systemctl restart fobos-web-spectrum
```

Then open **http://<host>:8080/**.

### Debian package

```sh
cmake --build build --target package     # -> build/fobos-web-spectrum_<ver>_<arch>.deb
sudo dpkg -i build/fobos-web-spectrum_*.deb
```
The package installs to `/usr/local/fobos/web_spectrum`, drops the unit in
`/etc/systemd/system`, and its `postinst` seeds `backend.conf`, fixes ownership, and enables +
starts the service. `libfobos_sdr` is a runtime prerequisite (checked, not a declared Depends).

### Dev build (no CMake)

```sh
cd cpp && make          # -> cpp/spectrumd
./spectrumd --port 8080
```

---

## Using it

1. Open the page, click **Attach** (opens the FobosOne).
2. Pick **FFT** or **SCAN**, set frequency/rate/gain, click **Start**.
3. **CAL dBm** toggle (in Display) applies the stored calibration; wheel-zoom the trace/waterfall.
4. **CAL** tab runs a calibration measurement (needs the Aaronia signal generator — see
   `calibration/README`... i.e. `calibration/calib_orchestrator.py`).
5. **CFG** tab switches the backend between C++ and Python (the service restarts in place).

---

## REST API (control)

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/attach` / `/api/detach` | open / close the device |
| POST | `/api/start` / `/api/stop` | start / stop acquisition (FFT or scan) |
| POST | `/api/params` | live tune/gain/dc-reject change |
| GET  | `/api/status` / `/api/samplerates` | state / supported rates |
| GET  | `/api/spectrum` | last spectrum meta (peak/avg/carrier) |
| GET  | `/api/calibration` | the stored calibration JSON |
| POST | `/api/cal/test_gen` / `/api/cal/start` / `/api/cal/abort` | calibration control |
| GET/POST | `/api/config/backend` | read / switch the backend |
| GET  | `/api/log` | recent server log lines |

The WebSocket (`/ws`) is push-only: **text** status events (`connected`, `started`, `stopped`,
`params_updated`, `cal_progress`, `error`) and **binary** `SPC2` spectrum frames.

---

## Layout

```
web_spectrum/
├── cpp/            native C++ backend (modular; see cpp/*.h) + Makefile
├── python/         app.py  (Flask/SocketIO fallback; built to app.pyc)
├── templates/      index.html (single-page UI)
├── static/         ws-shim.js (WebSocket transport + binary decode)
├── calibration/    absolute-power (dBm) calibration orchestrator + helpers
├── siggen_sweep/   Aaronia generator sweep helper (SCAN/Max-Hold coverage)
├── debian/         .deb maintainer scripts (postinst/prerm/postrm)
├── systemd/        service unit template
├── launch.sh       backend dispatcher (reads backend.conf)
└── CMakeLists.txt  build + install + CPack packaging
```

---

## Notes

- The C++ backend pushes ~88× less traffic than the original JSON design (uint8 bins +
  compression) and uses ~⅓ the CPU.
- Switching backends writes `backend.conf` and lets systemd (`Restart=always`) relaunch the
  launcher into the selected backend — same service, same port, no sudo.
- This unit has a known RX gap ~2200–2600 MHz (hardware); calibration interpolates across it.
