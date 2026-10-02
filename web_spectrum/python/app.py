#!/usr/bin/env python3
"""
FobosOne Web Spectrum Analyzer
Real-time spectrum + waterfall via Flask-SocketIO + REST API.

FFT mode  — uses fobos_sdr_read_async (async callback)
Scan mode — uses fobos_sdr_start_scan + fobos_sdr_read_async (async callback per step)
"""
import ctypes, threading, time, os, json, logging
from collections import deque
import numpy as np
from flask import Flask, render_template, jsonify, request
from flask_socketio import SocketIO, emit

# ── Logging ───────────────────────────────────────────────────────────────────

_LOG_RING   = deque(maxlen=1000)
_LOG_RING_LOCK = threading.Lock()

class _RingHandler(logging.Handler):
    def emit(self, record):
        with _LOG_RING_LOCK:
            _LOG_RING.append({
                'ts'   : record.created,
                'level': record.levelname,
                'msg'  : self.format(record),
            })

log = logging.getLogger('web_spectrum')
log.setLevel(logging.DEBUG)
_fmt = logging.Formatter('%(asctime)s %(levelname)-5s %(message)s')
_fh  = logging.FileHandler('/tmp/web_spectrum.log')
_fh.setFormatter(_fmt)
log.addHandler(_fh)
_rh = _RingHandler()
_rh.setFormatter(_fmt)
log.addHandler(_rh)
# Mirror to stderr so systemd journal captures it too
_sh = logging.StreamHandler()
_sh.setFormatter(_fmt)
log.addHandler(_sh)

# ── Library ───────────────────────────────────────────────────────────────────
# Prefer the system-installed lib (ldconfig, /usr/local/lib) so this works from the
# installed location; fall back to the in-tree build for dev checkouts.
def _load_libfobos():
    _here = os.path.dirname(__file__)
    for cand in ('libfobos_sdr.so',
                 os.path.join(_here, '../../libfobos-sdr-agile/build/libfobos_sdr.so'),
                 os.path.join(_here, '../../../libfobos-sdr-agile/build/libfobos_sdr.so')):
        try:
            return ctypes.CDLL(cand)
        except OSError:
            continue
    raise OSError('libfobos_sdr.so not found (build libfobos-sdr-agile or install it)')
lib = _load_libfobos()

lib.fobos_sdr_get_device_count.restype = ctypes.c_int
lib.fobos_sdr_open.restype             = ctypes.c_int
lib.fobos_sdr_close.restype            = ctypes.c_int
lib.fobos_sdr_reset.restype            = ctypes.c_int
lib.fobos_sdr_set_frequency.restype    = ctypes.c_int
lib.fobos_sdr_set_samplerate.restype   = ctypes.c_int
lib.fobos_sdr_set_lna_gain.restype     = ctypes.c_int
lib.fobos_sdr_set_vga_gain.restype     = ctypes.c_int
lib.fobos_sdr_set_auto_bandwidth.restype= ctypes.c_int
lib.fobos_sdr_start_sync.restype       = ctypes.c_int
lib.fobos_sdr_read_sync.restype        = ctypes.c_int
lib.fobos_sdr_stop_sync.restype        = ctypes.c_int
lib.fobos_sdr_read_async.restype       = ctypes.c_int
lib.fobos_sdr_cancel_async.restype     = ctypes.c_int
lib.fobos_sdr_start_scan.restype       = ctypes.c_int
lib.fobos_sdr_stop_scan.restype        = ctypes.c_int
lib.fobos_sdr_get_scan_index.restype   = ctypes.c_int
lib.fobos_sdr_is_scanning.restype      = ctypes.c_int
lib.fobos_sdr_get_samplerates.restype  = ctypes.c_int

# void(*cb)(float *buf, uint32_t buf_length, fobos_sdr_dev_t *sender, void *user)
CB_TYPE = ctypes.CFUNCTYPE(None,
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.c_void_p)

# ── Flask / SocketIO ──────────────────────────────────────────────────────────
app = Flask(__name__, template_folder='../templates', static_folder='../static')
app.config['SECRET_KEY'] = 'fobos-web-spectrum'
app.config['TEMPLATES_AUTO_RELOAD'] = True   # pick up index.html edits without a restart
app.jinja_env.auto_reload = True
sio = SocketIO(app, cors_allowed_origins='*', async_mode='threading')

# ── Global state ──────────────────────────────────────────────────────────────
SCAN_BUF_LEN = 65536   # min required by scan mode
FFT_BUF_LEN  = 65536
EMIT_MAX_FPS = 25      # cap spectrum emits to keep the socket from flooding
EMIT_MIN_INTERVAL = 1.0 / EMIT_MAX_FPS
STATE_FILE   = os.path.join(os.path.dirname(__file__), 'state.json')

# Gain step ranges accepted by libfobos_sdr (see fobos_sdr.h):
#   LNA 0..3  — 0,1: 0 dB, 2: +16 dB, 3: +33 dB
#   VGA 0..31 — 0..+62 dB in 2 dB steps
LNA_MAX = 3
VGA_MAX = 31

_DEFAULT_STATE = {
    'mode'        : 'fft',
    'freq'        : 433.92e6,
    'rate'        : 5e6,
    'lna'         : 2,
    'vga'         : 8,
    'fft_size'    : 4096,
    'overlap'     : 0,
    'scan_freqs'  : [],
    'scan_overlap': 0,
    'accum_n'     : 4,
    'scan_from'   : 430e6,
    'scan_to'     : 440e6,
    'dc_reject'   : True,        # interpolate across the receiver DC/LO spike at center
    'dc_reject_hz': 100e3,       # width of the DC notch (Hz), ~100 kHz
}

def _load_state():
    try:
        with open(STATE_FILE) as f:
            saved = json.load(f)
        d = dict(_DEFAULT_STATE)
        d.update({k: saved[k] for k in _DEFAULT_STATE if k in saved})
        return d
    except Exception:
        return dict(_DEFAULT_STATE)

def _save_state():
    try:
        with open(STATE_FILE, 'w') as f:
            json.dump({k: state[k] for k in _DEFAULT_STATE}, f, indent=2)
    except Exception:
        pass

state = {'dev': ctypes.c_void_p(None), 'running': False, 'hw_dirty': False}
state.update(_load_state())
state_lock  = threading.Lock()
rx_thread   = None
stop_event  = threading.Event()
hw_apply    = threading.Event()  # signals worker to pick up new params

# ── Last-spectrum store (for REST /api/spectrum) ──────────────────────────────
_spec_cond    = threading.Condition()  # protects _last_spectrum
_last_spectrum = None                  # dict: ts, freqs, power, center, rate, peak_db, avg_db

# ── Fobos helpers ─────────────────────────────────────────────────────────────

def dev_open():
    log.info('dev_open: attempt')
    with state_lock:
        if state['dev']:
            log.info('dev_open: already open')
            return True, 'ok'  # already open
    count = lib.fobos_sdr_get_device_count()
    if count < 1:
        return False, 'No Fobos device found'
    dev_p = ctypes.c_void_p()
    r = lib.fobos_sdr_open(ctypes.byref(dev_p), 0)
    if r != 0:
        log.error('dev_open: fobos_sdr_open error %d', r)
        return False, f'fobos_sdr_open error {r}'
    with state_lock:
        state['dev'] = dev_p
    log.info('dev_open: ok')
    return True, 'ok'

def dev_close():
    log.info('dev_close: start')
    with state_lock:
        dev = state['dev']
    if dev:
        lib.fobos_sdr_cancel_async(dev)
        lib.fobos_sdr_stop_scan(dev)
        lib.fobos_sdr_close(dev)
        with state_lock:
            state['dev'] = ctypes.c_void_p(None)
    log.info('dev_close: done')

def _force_gain(dev, lna, vga):
    """The library skips set_lna/vga_gain when value == its CACHED copy (rx_lna_gain/
    rx_vga_gain), but that cache can be stale vs the real hardware — notably on the first
    set after open (cache=0, hardware=power-on default), so set(0) is a no-op and the gain
    is never applied (~15 dB off). Force the USB command by toggling to a different value
    first, then the target."""
    lna = int(lna); vga = int(vga)
    lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint((lna + 1) % (LNA_MAX + 1)))
    lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(lna))
    lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint((vga + 1) % (VGA_MAX + 1)))
    lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(vga))

def _hw_apply_locked(dev, freq, rate, lna, vga):
    """Apply all hardware settings. Must be called from the USB-owning thread."""
    _force_gain(dev, lna, vga)
    lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(rate))
    lib.fobos_sdr_set_frequency(dev, ctypes.c_double(freq))

def apply_settings():
    with state_lock:
        dev  = state['dev']
        freq = state['freq']
        rate = state['rate']
        lna  = state['lna']
        vga  = state['vga']
    if not dev:
        return
    _hw_apply_locked(dev, freq, rate, lna, vga)

def _reapply_gain_soon():
    """The FobosOne doesn't reliably apply LNA/VGA set before the first start_scan/
    start_rx_async (stays at device default -> readings ~15 dB high). A real gain CHANGE
    while streaming DOES apply. So shortly after start, nudge VGA to a different value and
    back — two real changes force the hardware to the requested gain."""
    time.sleep(1.5)
    with state_lock:
        v = state['vga']; state['vga'] = (v + 1) % (VGA_MAX + 1)
    hw_apply.set()
    time.sleep(0.9)
    with state_lock:
        state['vga'] = v
    hw_apply.set()

def get_samplerates():
    with state_lock:
        dev = state['dev']
    if not dev:
        return []
    buf = (ctypes.c_double * 64)()
    cnt = ctypes.c_uint(64)
    r   = lib.fobos_sdr_get_samplerates(dev, buf, ctypes.byref(cnt))
    if r != 0:
        return []
    return [buf[i] for i in range(cnt.value)]

# ── DSP helpers ───────────────────────────────────────────────────────────────

def _hamming(n):
    return (0.54 - 0.46 * np.cos(2 * np.pi * np.arange(n) / (n - 1))).astype(np.float32)

def _process_chunk(chunk, win, fft_size, hop, psd_acc, leftover):
    """Apply windowed FFT to chunk, accumulate into psd_acc, return leftover."""
    buf = np.concatenate([leftover, chunk])
    pos = 0
    n_frames = 0
    while pos + fft_size <= len(buf):
        seg = buf[pos:pos + fft_size]
        spec = np.fft.fftshift(np.fft.fft(seg * win))
        psd_acc += np.abs(spec) ** 2
        n_frames += 1
        pos += hop
    return buf[pos:], n_frames

# ── FFT worker (async mode) ───────────────────────────────────────────────────
# Uses fobos_sdr_read_async instead of sync mode.
# Stopping is via fobos_sdr_cancel_async → libusb_cancel_transfer → FX3 stalls
# → CMD_STOP succeeds.  Sync-mode stop_sync sends CMD_STOP while FX3 is still
# streaming, which hangs indefinitely on Raspberry Pi 5 (RP1 USB controller
# does not honour CTRL_TIMEOUT for control transfers during bulk streaming).

def _notch_dc(psd_db, fft_size, rate, width_hz):
    """Linearly interpolate across the receiver DC/LO bins (~width_hz wide,
    centred on the bin at fft_size//2) so the center spike doesn't dominate the
    trace. Edits psd_db in place and returns it."""
    if width_hz <= 0 or rate <= 0:
        return psd_db
    dc   = fft_size // 2
    half = int(round((width_hz / 2.0) / (rate / fft_size)))
    lo   = max(1, dc - half)
    hi   = min(fft_size - 2, dc + half)
    if hi <= lo:                      # too narrow to span -> flatten just the DC bin
        if 1 <= dc <= fft_size - 2:
            psd_db[dc] = 0.5 * (psd_db[dc-1] + psd_db[dc+1])
        return psd_db
    psd_db[lo:hi+1] = np.linspace(psd_db[lo-1], psd_db[hi+1], hi - lo + 3)[1:-1]
    return psd_db


def fft_worker():
    with state_lock:
        dev      = state['dev']
        fft_size = state['fft_size']
        overlap  = state['overlap']
        accum_n  = state['accum_n']

    hop       = max(1, fft_size - int(fft_size * overlap / 100))
    win       = _hamming(fft_size)
    win_power = float(np.sum(win ** 2))

    # Mutable accumulator state visible inside the closure
    acc = {
        'leftover'   : np.zeros(0, dtype=np.complex64),
        'psd_acc'    : np.zeros(fft_size, dtype=np.float64),
        'frames'     : 0,
        'last_emit'  : 0.0,
    }

    def fft_cb(buf_ptr, buf_length, dev_ptr, user_ptr):
        # Cancel immediately on stop or param-change request
        if stop_event.is_set() or hw_apply.is_set():
            lib.fobos_sdr_cancel_async(dev)
            return

        # buf_length = complex sample count; buf_ptr → float32 I,Q,I,Q,...
        n    = buf_length
        addr = ctypes.cast(buf_ptr, ctypes.c_void_p).value
        raw  = np.frombuffer(
            (ctypes.c_float * (n * 2)).from_address(addr),
            dtype=np.float32, count=n * 2).copy()
        chunk = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)

        acc['leftover'], n_frames = _process_chunk(
            chunk, win, fft_size, hop, acc['psd_acc'], acc['leftover'])
        acc['frames'] += n_frames

        if acc['frames'] >= accum_n:
            # Throttle: cap emit rate to EMIT_MAX_FPS. Without this the worker
            # produces ~600 frames/s of ~100 KB JSON each, flooding the socket
            # and delaying everything (the user's apply/stop lag). Excess frames
            # are dropped so the display stays fresh rather than backlogged.
            now = time.monotonic()
            if now - acc['last_emit'] >= EMIT_MIN_INTERVAL:
                acc['last_emit'] = now
                psd_db = 10 * np.log10(
                    acc['psd_acc'] / (acc['frames'] * win_power) + 1e-30)
                with state_lock:
                    cur_freq = state['freq']
                    cur_rate = state['rate']
                    dc_reject    = state.get('dc_reject', True)
                    dc_reject_hz = state.get('dc_reject_hz', 100e3)
                if dc_reject:
                    psd_db = _notch_dc(psd_db, fft_size, cur_rate, dc_reject_hz)
                freqs = cur_freq + np.fft.fftshift(
                    np.fft.fftfreq(fft_size, 1.0 / cur_rate))
                # ── Time-domain power meter: coherent single-bin power of the strongest
                # carrier. The carrier FREQUENCY is found from a HIGH-RES FFT of the raw
                # chunk (independent of the display fft_size), then a coherent DFT at that
                # exact (sub-bin-refined) frequency gives the amplitude — no scalloping,
                # fft_size-INDEPENDENT absolute power. ──
                carrier_dbfs = None; carrier_freq = None
                Nc = len(chunk)
                Xr = np.abs(np.fft.fft(chunk)); cfr = np.fft.fftfreq(Nc, 1.0/cur_rate)
                dmask = np.abs(cfr) > 80e3                  # ignore DC/LO region
                if dmask.any():
                    kk = int(np.argmax(np.where(dmask, Xr, -1.0)))
                    if Xr[kk] > 8.0 * float(np.median(Xr)):   # a real carrier
                        l0 = np.log(Xr[(kk-1) % Nc] + 1e-30)
                        l1 = np.log(Xr[kk] + 1e-30)
                        l2 = np.log(Xr[(kk+1) % Nc] + 1e-30)
                        den = l0 - 2*l1 + l2
                        d = max(-0.5, min(0.5, 0.5*(l0-l2)/den)) if abs(den) > 1e-9 else 0.0
                        f_off = cfr[kk] + d*(cur_rate/Nc)
                        nn = np.arange(Nc, dtype=np.float64)
                        amp = np.mean(chunk * np.exp(-1j*2*np.pi*f_off*nn/cur_rate))
                        carrier_dbfs = float(20*np.log10(abs(amp) + 1e-30))
                        carrier_freq = float(cur_freq + f_off)
                payload = {
                    'mode'  : 'fft',
                    'freqs' : freqs.tolist(),
                    'power' : psd_db.tolist(),
                    'center': cur_freq,
                    'rate'  : cur_rate,
                    'fft_size': fft_size,
                    'carrier_dbfs': carrier_dbfs,
                    'carrier_freq': carrier_freq,
                }
                sio.emit('spectrum', payload)
                # update last_spectrum for REST /api/spectrum
                with _spec_cond:
                    global _last_spectrum
                    _last_spectrum = {
                        'ts'      : time.time(),
                        'center'  : cur_freq,
                        'rate'    : cur_rate,
                        'freqs'   : payload['freqs'],
                        'power'   : payload['power'],
                        'peak_db' : float(psd_db.max()),
                        'avg_db'  : float(psd_db.mean()),
                        'carrier_dbfs': carrier_dbfs,
                        'carrier_freq': carrier_freq,
                    }
                    _spec_cond.notify_all()
            # reset accumulator whether or not we emitted (drop excess)
            acc['psd_acc'][:] = 0
            acc['frames'] = 0

    cb_func = CB_TYPE(fft_cb)   # keep alive for duration of async loop

    while not stop_event.is_set():
        hw_apply.clear()
        acc['leftover'] = np.zeros(0, dtype=np.complex64)
        acc['psd_acc'][:] = 0
        acc['frames'] = 0

        # Apply current settings (device is stopped between iterations)
        with state_lock:
            cur_freq = state['freq']
            cur_rate = state['rate']
            cur_lna  = state['lna']
            cur_vga  = state['vga']
        log.info('fft_worker: apply freq=%.3fMHz rate=%.1fM lna=%d vga=%d',
                 cur_freq/1e6, cur_rate/1e6, cur_lna, cur_vga)
        _force_gain(dev, cur_lna, cur_vga)
        lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(cur_rate))
        lib.fobos_sdr_set_frequency(dev, ctypes.c_double(cur_freq))

        log.info('fft_worker: read_async start')
        t0 = time.monotonic()
        # Blocks: sends CMD_START, fires callbacks, then CMD_STOP on cancel
        r = lib.fobos_sdr_read_async(dev, cb_func, None,
                                     ctypes.c_uint32(16),
                                     ctypes.c_uint32(FFT_BUF_LEN))
        log.info('fft_worker: read_async returned %d in %.3fs', r, time.monotonic() - t0)
        if r != 0 and not stop_event.is_set() and not hw_apply.is_set():
            sio.emit('error', {'msg': f'fft async error {r}'})
            break
        # If hw_apply → loop back, apply new params, restart
        # If stop_event → while condition fails → exit

    log.info('fft_worker: exit')

# ── Scan worker (async callback mode) ────────────────────────────────────────

def scan_worker():
    with state_lock:
        dev          = state['dev']
        freqs_list   = list(state['scan_freqs'])
        fft_size     = state['fft_size']
        scan_overlap = state['scan_overlap']
        accum_n      = state['accum_n']

    n_steps = len(freqs_list)
    if n_steps < 2:
        sio.emit('error', {'msg': 'scan needs at least 2 frequencies'})
        return
    if n_steps > 256:                       # FOBOS_MAX_FREQS_CNT; start_scan would return -8
        sio.emit('error', {'msg': f'scan needs {n_steps} tiles but the hardware limit is 256 — '
                                  f'raise the sample rate (wider tiles) or narrow the scan range'})
        return

    hop       = max(1, fft_size - int(fft_size * scan_overlap / 100))
    win       = _hamming(fft_size)
    win_power = float(np.sum(win ** 2))

    # Per-step accumulators
    psd_acc   = [np.zeros(fft_size, dtype=np.float64) for _ in range(n_steps)]
    fc_count  = [0] * n_steps
    leftovers = [np.zeros(0, dtype=np.complex64) for _ in range(n_steps)]
    rate_box  = [state['rate']]   # current sample rate (re-read on each (re)start)

    def scan_cb(buf_ptr, buf_length, dev_ptr, user_ptr):
        # cancel on stop OR on a param change (gain/rate) so the loop re-applies
        if stop_event.is_set() or hw_apply.is_set():
            lib.fobos_sdr_cancel_async(dev)
            return
        if lib.fobos_sdr_is_scanning(dev) != 1:
            lib.fobos_sdr_cancel_async(dev)
            return

        idx = lib.fobos_sdr_get_scan_index(dev)
        if idx < 0 or idx >= n_steps:
            return  # still tuning

        n = buf_length  # complex samples; buf has 2*n floats
        addr = ctypes.cast(buf_ptr, ctypes.c_void_p).value
        raw  = np.frombuffer(
            (ctypes.c_float * (n * 2)).from_address(addr),
            dtype=np.float32, count=n * 2).copy()
        chunk = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)

        leftovers[idx], n_frames = _process_chunk(
            chunk, win, fft_size, hop, psd_acc[idx], leftovers[idx])
        fc_count[idx] += n_frames

        if all(c >= accum_n for c in fc_count):
            rate = rate_box[0]
            # DC-centred crop (like the fft-scan example): keep only the central
            # bins that span one step's spacing, so adjacent steps tile contiguously
            # and the rolloff/out-of-band edges (which alias strong nearby signals
            # to the wrong place) are discarded. Without this, wide steps leave gaps
            # and strong out-of-band signals smear into the wrong frequency bins.
            step_spacing = (freqs_list[1] - freqs_list[0]) if n_steps > 1 else rate
            keep = int(round(abs(step_spacing) / rate * fft_size))
            keep = max(1, min(fft_size, keep))
            lo   = (fft_size - keep) // 2
            hi   = lo + keep
            # Use only each step's flat-passband centre (cropped per step overlap);
            # flatness comes from physical step overlap, not from subtracting a shape.
            # Just interpolate over the localized LO/DC spike at each step centre.
            f_off = np.fft.fftshift(np.fft.fftfreq(fft_size, 1.0 / rate))
            with state_lock:
                dc_reject    = state.get('dc_reject', True)
                dc_reject_hz = state.get('dc_reject_hz', 100e3)
            all_freqs = []
            all_power = []
            for i, fc in enumerate(freqs_list):
                avg    = psd_acc[i] / fc_count[i]
                psd_db = 10 * np.log10(avg / win_power + 1e-30)
                if dc_reject:                      # reject each step's center DC/LO spike
                    _notch_dc(psd_db, fft_size, rate, dc_reject_hz)
                all_freqs.extend((fc + f_off[lo:hi]).tolist())
                all_power.extend(psd_db[lo:hi].tolist())

            sio.emit('spectrum', {
                'mode'  : 'scan',
                'freqs' : all_freqs,
                'power' : all_power,
                'fft_size': fft_size,   # actual FFT size of THIS scan (for the cal fft correction)
            })
            for i in range(n_steps):
                psd_acc[i][:] = 0
                fc_count[i]   = 0

    cb_func = CB_TYPE(scan_cb)  # keep alive for duration of async read

    # Outer loop: (re)apply gains/rate then scan. A gain/rate change sets hw_apply,
    # which cancels the async read so we loop back and re-apply — the scan worker
    # previously ignored gain changes while running (LNA/VGA had no effect in scan).
    while not stop_event.is_set():
        hw_apply.clear()
        with state_lock:
            cur_lna  = state['lna']
            cur_vga  = state['vga']
            cur_rate = state['rate']
        rate_box[0] = cur_rate
        _force_gain(dev, cur_lna, cur_vga)
        lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(cur_rate))
        lib.fobos_sdr_set_auto_bandwidth(dev, ctypes.c_double(0.8))
        for i in range(n_steps):           # fresh accumulation after a settings change
            psd_acc[i][:] = 0; fc_count[i] = 0
            leftovers[i] = np.zeros(0, dtype=np.complex64)
        log.info('scan_worker: apply lna=%d vga=%d rate=%.1fM', cur_lna, cur_vga, cur_rate/1e6)
        freq_arr = (ctypes.c_double * n_steps)(*freqs_list)
        r = lib.fobos_sdr_start_scan(dev, freq_arr, ctypes.c_uint(n_steps))
        if r != 0:
            sio.emit('error', {'msg': f'start_scan error {r}'}); break
        r = lib.fobos_sdr_read_async(dev, cb_func, None, 16, ctypes.c_uint32(SCAN_BUF_LEN))
        lib.fobos_sdr_stop_scan(dev)
        if r != 0 and not stop_event.is_set() and not hw_apply.is_set():
            sio.emit('error', {'msg': f'read_async error {r}'}); break
        # hw_apply set -> loop and re-apply; stop_event -> exit
    log.info('scan_worker: exit')

def _signal_stop():
    """Signal worker to stop. Returns immediately."""
    log.info('_signal_stop: signalling')
    stop_event.set()
    with state_lock:
        dev = state['dev']
    if dev:
        lib.fobos_sdr_cancel_async(dev)

def _join_worker(timeout=2.0):
    """Wait for the worker thread to finish."""
    global rx_thread
    t = rx_thread
    if t and t.is_alive() and t is not threading.current_thread():
        t0 = time.monotonic()
        t.join(timeout=timeout)
        elapsed = time.monotonic() - t0
        if t.is_alive():
            log.warning('_join_worker: thread still alive after %.1fs', elapsed)
        else:
            log.info('_join_worker: thread joined in %.3fs', elapsed)
    rx_thread = None

# ── HTTP routes ───────────────────────────────────────────────────────────────

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/samplerates')
def api_samplerates():
    return jsonify({'rates': get_samplerates()})

@app.route('/api/status')
def api_status():
    with state_lock:
        s = {k: v for k, v in state.items() if k != 'dev'}
        s['attached'] = bool(state['dev'])
    return jsonify(s)

def _cal_dir():
    """The calibration tooling sits next to the app root, i.e. ../calibration
    relative to python/ (both installed and in the dev tree)."""
    return os.path.join(os.path.dirname(__file__), '../calibration')
CAL_DIR  = _cal_dir()
CAL_FILE = os.path.join(CAL_DIR, 'fobos_calibration.json')

@app.route('/api/calibration')
def api_calibration():
    """Serve the absolute-power calibration (if measured). The browser applies it as a
    per-frequency dB offset: displayed_dBm = raw_dBFS + C(f,lna,vga)."""
    try:
        with open(CAL_FILE) as f:
            return jsonify({'available': True, 'cal': json.load(f)})
    except Exception:
        return jsonify({'available': False})

# ── Backend selection (cpp vs python) — matches the C++ backend's endpoints ──
BACKEND_CONF = 'backend.conf'   # relative to CWD (the app root, set by launch.sh)

def _read_backend():
    try:
        with open(BACKEND_CONF) as f:
            return (f.read().strip() or 'cpp')
    except Exception:
        return 'cpp'

def _schedule_reexec():
    # backend.conf already rewritten; exit so systemd (Restart=always) re-runs the launcher.
    def _go():
        time.sleep(0.5)                       # let the HTTP reply flush
        try: dev_close()
        except Exception: pass
        os._exit(0)
    threading.Thread(target=_go, daemon=True).start()

@app.route('/api/config/backend', methods=['GET'])
def api_get_backend():
    return jsonify({'running': 'python', 'selected': _read_backend()})

@app.route('/api/config/backend', methods=['POST'])
def api_set_backend():
    data = request.get_json(force=True, silent=True) or {}
    be = data.get('backend')
    if be not in ('cpp', 'python'):
        return jsonify({'ok': False, 'msg': 'backend must be cpp or python'}), 400
    try:
        with open(BACKEND_CONF, 'w') as f: f.write(be + '\n')
    except Exception as e:
        return jsonify({'ok': False, 'msg': str(e)}), 500
    if be == 'python':
        return jsonify({'ok': True, 'switching': False, 'msg': 'already python'})
    _schedule_reexec()
    return jsonify({'ok': True, 'switching': True, 'backend': 'cpp'})

@app.route('/api/attach', methods=['POST'])
def rest_attach():
    ok, msg = dev_open()
    if not ok:
        return jsonify({'ok': False, 'msg': msg}), 500
    rates = get_samplerates()
    return jsonify({'ok': True, 'rates': rates})

@app.route('/api/detach', methods=['POST'])
def rest_detach():
    _signal_stop()
    _join_worker(timeout=5.0)
    dev_close()
    with state_lock:
        state['running'] = False      # was left True after detach (stale "occupied" state)
    return jsonify({'ok': True})

@app.route('/api/start', methods=['POST'])
def rest_start():
    global rx_thread
    data = request.get_json(force=True, silent=True) or {}
    log.info('REST /api/start data=%s', data)
    _signal_stop()
    _join_worker(timeout=5.0)
    with state_lock:
        for k in ('freq', 'rate', 'scan_from', 'scan_to'):
            if k in data: state[k] = float(data[k])
        for k in ('lna', 'vga', 'fft_size', 'overlap', 'scan_overlap', 'accum_n'):
            if k in data: state[k] = max(1 if k == 'accum_n' else 0, int(data[k]))
        if 'mode'       in data: state['mode']       = data['mode']
        if 'scan_freqs' in data: state['scan_freqs'] = [float(f) for f in data['scan_freqs']]
        if 'dc_reject'    in data: state['dc_reject']    = bool(data['dc_reject'])
        if 'dc_reject_hz' in data: state['dc_reject_hz'] = max(0.0, float(data['dc_reject_hz']))
        mode = state['mode']
        state['running'] = True
    apply_settings()
    _save_state()
    stop_event.clear()
    worker    = fft_worker if mode == 'fft' else scan_worker
    rx_thread = threading.Thread(target=worker, daemon=True, name='rx_worker')
    rx_thread.start()
    # Gain set before the first start_scan/start_rx_async doesn't reliably stick on the
    # FobosOne (stays at device default -> readings ~15 dB high). Re-apply once the device
    # is warm (this is the same path a live param change uses, which DOES apply correctly).
    log.info('REST /api/start: worker started mode=%s', mode)
    return jsonify({'ok': True, 'mode': mode})

@app.route('/api/stop', methods=['POST'])
def rest_stop():
    t0 = time.monotonic()
    _signal_stop()
    with state_lock:
        state['running'] = False
    wait = (request.args.get('wait', 'true').lower() != 'false')
    if wait:
        _join_worker(timeout=5.0)
    log.info('REST /api/stop: done in %.3fs (wait=%s)', time.monotonic() - t0, wait)
    return jsonify({'ok': True, 'elapsed_ms': round((time.monotonic() - t0) * 1000)})

@app.route('/api/params', methods=['POST'])
def rest_params():
    data = request.get_json(force=True, silent=True) or {}
    log.info('REST /api/params data=%s', data)
    with state_lock:
        if 'freq' in data: state['freq'] = float(data['freq'])
        if 'rate' in data: state['rate'] = float(data['rate'])
        if 'lna'  in data: state['lna']  = min(LNA_MAX, max(0, int(data['lna'])))
        if 'vga'  in data: state['vga']  = min(VGA_MAX, max(0, int(data['vga'])))
        if 'dc_reject'    in data: state['dc_reject']    = bool(data['dc_reject'])
        if 'dc_reject_hz' in data: state['dc_reject_hz'] = max(0.0, float(data['dc_reject_hz']))
        dev     = state['dev']
        running = state['running']
        freq = state['freq']; rate = state['rate']
        lna  = state['lna'];  vga  = state['vga']
    if dev:
        if running:
            hw_apply.set()
        else:
            if 'freq' in data: lib.fobos_sdr_set_frequency(dev, ctypes.c_double(freq))
            if 'rate' in data: lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(rate))
            if 'lna'  in data: lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(lna))
            if 'vga'  in data: lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(vga))
    _save_state()
    return jsonify({'ok': True, 'freq': freq, 'rate': rate, 'lna': lna, 'vga': vga})

@app.route('/api/spectrum')
def rest_spectrum():
    """Return last spectrum. With ?wait=true&timeout=5 blocks for next fresh frame."""
    global _last_spectrum
    wait    = request.args.get('wait', 'false').lower() == 'true'
    timeout = float(request.args.get('timeout', '5'))
    after   = float(request.args.get('after', '0'))  # only accept ts > after

    with _spec_cond:
        if wait:
            deadline = time.time() + timeout
            while _last_spectrum is None or _last_spectrum['ts'] <= after:
                remaining = deadline - time.time()
                if remaining <= 0:
                    return jsonify({'ok': False, 'msg': 'timeout'}), 408
                _spec_cond.wait(timeout=remaining)
        if _last_spectrum is None:
            return jsonify({'ok': False, 'msg': 'no data yet'}), 404
        return jsonify({'ok': True, **_last_spectrum})

@app.route('/api/log')
def rest_log():
    n = int(request.args.get('n', '100'))
    level = request.args.get('level', '').upper()
    with _LOG_RING_LOCK:
        entries = list(_LOG_RING)
    if level:
        entries = [e for e in entries if e['level'] == level]
    return jsonify({'entries': entries[-n:]})

# ── SocketIO events ───────────────────────────────────────────────────────────

@sio.on('connect')
def on_connect():
    # Send initial state without opening the device yet
    with state_lock:
        s = {k: v for k, v in state.items() if k != 'dev'}
    emit('connected', {'rates': [], 'state': s})

@sio.on('attach')
def on_attach():
    ok, msg = dev_open()
    if not ok:
        emit('error', {'msg': msg})
        return
    rates = get_samplerates()
    log.info('WS attach: ok, rates=%s', rates)
    emit('attached', {'rates': rates})

@sio.on('detach')
def on_detach():
    log.info('WS detach')
    _signal_stop()
    _join_worker(timeout=2.0)
    dev_close()
    with state_lock:
        state['running'] = False
    emit('detached', {})

@sio.on('disconnect')
def on_disconnect():
    # A viewer leaving must NOT stop acquisition: this is a persistent service and
    # the controller page's socket may transiently reconnect during a long run.
    # Stopping here froze the scan (and max-hold) mid-run with no auto-restart.
    # Acquisition stops only on explicit /api/stop, toggleRx(stop), or detach.
    log.info('WS disconnect (acquisition left running)')

@sio.on('start')
def on_start(data):
    global rx_thread
    log.info('WS start data=%s', data)
    t0 = time.monotonic()
    _signal_stop()
    _join_worker(timeout=2.0)
    log.info('WS start: previous worker stopped in %.3fs', time.monotonic() - t0)

    with state_lock:
        for k in ('freq', 'rate', 'scan_from', 'scan_to'):
            if k in data: state[k] = float(data[k])
        for k in ('lna', 'vga', 'fft_size', 'overlap', 'scan_overlap', 'accum_n'):
            if k in data: state[k] = max(1 if k == 'accum_n' else 0, int(data[k]))
        if 'mode'       in data: state['mode']       = data['mode']
        if 'scan_freqs' in data: state['scan_freqs'] = [float(f) for f in data['scan_freqs']]
        if 'dc_reject'    in data: state['dc_reject']    = bool(data['dc_reject'])
        if 'dc_reject_hz' in data: state['dc_reject_hz'] = max(0.0, float(data['dc_reject_hz']))
        mode = state['mode']
        state['running'] = True

    apply_settings()
    _save_state()
    stop_event.clear()
    worker    = fft_worker if mode == 'fft' else scan_worker
    rx_thread = threading.Thread(target=worker, daemon=True, name='rx_worker')
    rx_thread.start()
    log.info('WS start: worker started mode=%s total=%.3fs', mode, time.monotonic() - t0)
    emit('started', {'mode': mode})

@sio.on('stop')
def on_stop():
    log.info('WS stop')
    _signal_stop()       # returns immediately
    with state_lock:
        state['running'] = False
    emit('stopped', {})  # UI updates right away; worker exits in background

@sio.on('set_params')
def on_set_params(data):
    log.info('WS set_params data=%s', data)
    with state_lock:
        if 'freq' in data: state['freq'] = float(data['freq'])
        if 'rate' in data: state['rate'] = float(data['rate'])
        if 'lna'  in data: state['lna']  = min(LNA_MAX, max(0, int(data['lna'])))
        if 'vga'  in data: state['vga']  = min(VGA_MAX, max(0, int(data['vga'])))
        if 'dc_reject'    in data: state['dc_reject']    = bool(data['dc_reject'])
        if 'dc_reject_hz' in data: state['dc_reject_hz'] = max(0.0, float(data['dc_reject_hz']))
        dev     = state['dev']
        running = state['running']
        freq = state['freq']; rate = state['rate']
        lna  = state['lna'];  vga  = state['vga']
    if dev:
        if running:
            # Streaming: signal worker to apply from its own thread (safe, no contention)
            hw_apply.set()
        else:
            # Not streaming: no concurrent bulk transfer, safe to call directly
            if 'freq' in data: lib.fobos_sdr_set_frequency(dev, ctypes.c_double(freq))
            if 'rate' in data: lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(rate))
            if 'lna'  in data: lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(lna))
            if 'vga'  in data: lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(vga))
    _save_state()
    emit('params_updated', {'freq': freq, 'rate': rate, 'lna': lna, 'vga': vga})

@sio.on('set_gain')
def on_set_gain(data):
    on_set_params(data)

# ── Main ──────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=int(os.environ.get('FOBOS_PORT', '8080')))
    args = ap.parse_args()
    n = lib.fobos_sdr_get_device_count()
    print(f'Fobos devices found: {n}')
    if n < 1:
        print('WARNING: No Fobos device — UI will still start but RX will fail')
    sio.run(app, host='0.0.0.0', port=args.port, debug=False,
            use_reloader=False, allow_unsafe_werkzeug=True)
