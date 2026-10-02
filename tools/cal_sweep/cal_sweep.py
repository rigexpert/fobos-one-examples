#!/usr/bin/env python3
"""
cal_sweep.py — Receive-calibration sweep for Fobos SDR.

Controls the Aaronia RTSA-Suite PRO to emit a CW tone that steps across a
frequency range, while the Fobos SDR measures received peak power for every
(LNA, VGA) combination.  Results go to a CSV file and a matplotlib plot
showing all 128 gain curves on one figure.

Signal path:
  Aaronia Spectran V6 PLUS TX (CW generator) --(RF cable or air)-->
  Fobos SDR RX  -->  measure binary (FFT peak dBFS)

Usage:
  python3 cal_sweep.py [options]

Options:
  --host HOST        Aaronia RTSA host  (default: 10.200.0.125)
  --port PORT        Aaronia FFT HTTP port (default: 54665)
  --freq-start F     Start frequency Hz   (default: 70e6)
  --freq-stop  F     Stop  frequency Hz   (default: 6000e6)
  --freq-step  F     Step  frequency Hz   (default: 10e6)
  --tx-power   P     Generator output dBm (default: -20)
  --rate       R     Fobos sample rate Hz (default: 2500000)
  --n-fft      N     FFT size for measure (default: 65536)
  --settle-ms  T     Settle time ms after Aaronia retune (default: 200)
  --out        FILE  Output CSV filename  (default: cal_sweep.csv)
  --plot       FILE  Output PNG filename  (default: cal_sweep.png)
  --measure    PATH  Path to measure binary (default: ./c/measure)
  --no-tx            Skip Aaronia TX control (use if already set up manually)
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import time
import urllib.request
import zlib
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# Gain grid — mirrors FOBOS_LNA_GAIN_MAX / FOBOS_VGA_GAIN_MAX in c/fobos.h.
# LNA steps: 0,1 = 0 dB, 2 = +16 dB, 3 = +33 dB.  VGA steps: 0..+62 dB, 2 dB each.
N_LNA = 4
N_VGA = 32
N_GAIN_COMBOS = N_LNA * N_VGA

# ──────────────────────────────────────────────────────────── Aaronia control

AUTH = b'a:'
AUTH_HDR = 'Basic ' + __import__('base64').b64encode(AUTH).decode()
_req_id = 0

def _next_id():
    global _req_id
    _req_id += 1
    return _req_id


def aaronia_info(host, port):
    url = f'http://{host}:{port}/info'
    req = urllib.request.Request(url, headers={'Authorization': AUTH_HDR})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def aaronia_get_config(host, port):
    url = f'http://{host}:{port}/remoteconfig?request={_next_id()}'
    req = urllib.request.Request(url, headers={
        'Authorization': AUTH_HDR,
        'Accept-Encoding': 'deflate',
    })
    with urllib.request.urlopen(req, timeout=5) as r:
        data = r.read()
    try:
        return json.loads(data)
    except Exception:
        return json.loads(zlib.decompress(data))


def aaronia_put_config(host, port, body_dict):
    """PUT a remoteconfig subtree.  Returns HTTP status code."""
    raw = json.dumps(body_dict).encode()
    url = f'http://{host}:{port}/remoteconfig?request={_next_id()}'
    req = urllib.request.Request(url, data=raw, method='PUT', headers={
        'Authorization': AUTH_HDR,
        'Content-Type': 'application/json',
        'Content-Length': str(len(raw)),
    })
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def aaronia_put_control(host, port, body_dict):
    """PUT /control — the reliable Aaronia command path."""
    raw = json.dumps(body_dict).encode()
    url = f'http://{host}:{port}/control'
    req = urllib.request.Request(url, data=raw, method='PUT', headers={
        'Authorization': AUTH_HDR,
        'Content-Type': 'application/json',
        'Content-Length': str(len(raw)),
    })
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


TONE_OFFSET_HZ = 1_000_000  # Relative Tone offset: tone = V6B_center + 1 MHz


def aaronia_set_generator(host, port, tone_hz, power_dbm=-20):
    """
    Set the Spectran V6 built-in CW generator (Relative Tone mode) to
    produce a tone at tone_hz.

    The V6B is configured in Signal Generator / Relative Tone mode (offset=1 MHz),
    so the tone appears at V6B_centerfreq + TONE_OFFSET_HZ.  We therefore
    set V6B center = tone_hz - TONE_OFFSET_HZ via PUT /control.
    """
    center_hz = int(tone_hz - TONE_OFFSET_HZ)
    half_bw = 2_500_000
    ctrl = {
        "frequencyStart": center_hz - half_bw,
        "frequencyEnd":   center_hz + half_bw,
        "type": "capture",
    }
    aaronia_put_control(host, port, ctrl)
    return True


def aaronia_set_generator_off(host, port):
    # Generator is always-on while mission is loaded with transmittermode=4.
    # Nothing to do here — the V6B stays in Signal Generator mode until
    # the mission is unloaded.
    pass


# ──────────────────────────────────────────────────────── Fobos measurement

def measure_at_freq(measure_bin, freq_hz, rate_hz, n_fft):
    """
    Call the measure binary for one frequency.
    Returns a list of (lna, vga, power_dbfs) tuples (N_GAIN_COMBOS entries).
    """
    cmd = [measure_bin, str(int(freq_hz)), str(int(rate_hz)), str(n_fft)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        raise RuntimeError(f'measure failed: {result.stderr.strip()}')
    rows = []
    for line in result.stdout.strip().splitlines():
        parts = line.split(',')
        if len(parts) != 4:
            continue
        _f, lna, vga, pdb = parts
        rows.append((int(lna), int(vga), float(pdb)))
    return rows


# ──────────────────────────────────────────────────────────────────── Plotting

LNA_COLORS = ['#2196F3', '#FF9800', '#4CAF50', '#9C27B0']  # blue, orange, green, purple

def plot_results(csv_path, out_png):
    """Read CSV and produce a multi-curve calibration plot."""
    data = defaultdict(lambda: defaultdict(list))  # [lna][vga] → [(freq, power)]
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            lna = int(row['lna'])
            vga = int(row['vga'])
            freq_mhz = float(row['freq_hz']) / 1e6
            pdb = float(row['peak_dbfs'])
            data[lna][vga].append((freq_mhz, pdb))

    fig, axes = plt.subplots(N_LNA, 1, figsize=(14, 5 * N_LNA), sharex=True)
    fig.suptitle('Fobos SDR receive calibration — peak power vs frequency\n'
                 'Aaronia CW TX, all LNA × VGA combinations', fontsize=13)

    for lna, ax in enumerate(axes):
        ax.set_title(f'LNA = {lna}', fontweight='bold')
        ax.set_ylabel('Peak power (dBFS)')
        ax.set_ylim(-100, 0)
        ax.grid(True, alpha=0.3)
        ax.axhline(-3, color='red', lw=0.5, ls='--', label='-3 dBFS (clipping zone)')

        cmap = plt.cm.get_cmap('RdYlBu', N_VGA)
        for vga in range(N_VGA):
            pts = data[lna].get(vga, [])
            if not pts:
                continue
            freqs, powers = zip(*sorted(pts))
            ax.plot(freqs, powers, lw=0.8, alpha=0.85,
                    color=cmap(vga / (N_VGA - 1)),
                    label=f'VGA={vga}')

        ax.legend(loc='lower right', fontsize=6, ncol=4)

    axes[-1].set_xlabel('Frequency (MHz)')
    plt.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f'Saved plot → {out_png}')


def plot_gain_linearity(csv_path, out_png):
    """
    For each (LNA, frequency), plot received power vs VGA setting to check
    linearity.  Uses median across all frequencies per (LNA, VGA).
    """
    data = defaultdict(list)  # [lna][vga] → list of powers
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            lna = int(row['lna'])
            vga = int(row['vga'])
            pdb = float(row['peak_dbfs'])
            data[(lna, vga)].append(pdb)

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.set_title('Fobos SDR gain linearity: median received power vs VGA\n'
                 '(median over all measured frequencies)')
    ax.set_xlabel('VGA setting')
    ax.set_ylabel('Median peak power (dBFS)')
    ax.set_xticks(range(0, N_VGA, 2))
    ax.grid(True, alpha=0.3)

    for lna in range(N_LNA):
        vgas = list(range(N_VGA))
        medians = [float(np.median(data[(lna, v)])) if data[(lna, v)] else float('nan')
                   for v in vgas]
        ax.plot(vgas, medians, marker='o', lw=2,
                color=LNA_COLORS[lna], label=f'LNA={lna}')

    ax.legend()
    plt.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f'Saved linearity plot → {out_png}')


# ─────────────────────────────────────────────────────────────────────── Main

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host',       default='10.200.0.125')
    p.add_argument('--port',       type=int, default=54665)
    p.add_argument('--freq-start', type=float, default=200e6)  # V6B min centerfreq ~193 MHz
    p.add_argument('--freq-stop',  type=float, default=6000e6)
    p.add_argument('--freq-step',  type=float, default=10e6)
    p.add_argument('--tx-power',   type=float, default=-20.0)
    p.add_argument('--rate',       type=float, default=2500000.0)
    p.add_argument('--n-fft',      type=int,   default=65536)
    p.add_argument('--settle-ms',  type=float, default=200.0)
    p.add_argument('--out',        default='cal_sweep.csv')
    p.add_argument('--plot',       default='cal_sweep.png')
    p.add_argument('--measure',    default=os.path.join(os.path.dirname(__file__), 'c', 'measure'))
    p.add_argument('--no-tx',      action='store_true',
                   help='Skip Aaronia TX control (set up generator manually)')
    return p.parse_args()


def main():
    args = parse_args()

    if not os.path.isfile(args.measure):
        print(f'ERROR: measure binary not found: {args.measure}', file=sys.stderr)
        print('Build it first:  cd c && make', file=sys.stderr)
        sys.exit(1)

    freqs = np.arange(args.freq_start, args.freq_stop + args.freq_step * 0.5,
                      args.freq_step)
    n_total = len(freqs) * N_GAIN_COMBOS

    print(f'Calibration sweep: {len(freqs)} frequencies × {N_GAIN_COMBOS} gain combos '
          f'= {n_total} measurements')
    print(f'Frequency range: {args.freq_start/1e6:.1f} – {args.freq_stop/1e6:.1f} MHz'
          f' step {args.freq_step/1e6:.2f} MHz')

    if not args.no_tx:
        try:
            info = aaronia_info(args.host, args.port)
            print(f'Aaronia: {info.get("title")} on {args.host}:{args.port}')
        except Exception as e:
            print(f'WARNING: Cannot reach Aaronia at {args.host}:{args.port}: {e}')
            print('Continuing without TX control — ensure signal source is set up manually.')
            args.no_tx = True

    csv_exists = os.path.exists(args.out)
    # Resume: collect already-measured frequencies from existing CSV
    done_freqs = set()
    if csv_exists:
        with open(args.out) as f:
            for row in csv.DictReader(f):
                done_freqs.add(float(row['freq_hz']))
        print(f'Resuming: {len(done_freqs)} frequencies already in {args.out}')

    with open(args.out, 'a', newline='') as csvf:
        writer = csv.writer(csvf)
        if not csv_exists:
            writer.writerow(['freq_hz', 'lna', 'vga', 'peak_dbfs'])

        for i, freq in enumerate(freqs):
            if freq in done_freqs:
                continue

            # ── Aaronia TX: set generator to this frequency ──
            if not args.no_tx:
                try:
                    aaronia_set_generator(args.host, args.port, freq,
                                         power_dbm=args.tx_power)
                except Exception as e:
                    print(f'  WARNING aaronia retune failed: {e}')

            time.sleep(args.settle_ms / 1000.0)

            # ── Fobos: measure all N_GAIN_COMBOS LNA×VGA combos ──
            try:
                rows = measure_at_freq(args.measure, freq, args.rate, args.n_fft)
            except Exception as e:
                print(f'  ERROR measure failed at {freq/1e6:.1f} MHz: {e}', file=sys.stderr)
                continue

            for lna, vga, pdb in rows:
                writer.writerow([int(freq), lna, vga, f'{pdb:.3f}'])
            csvf.flush()

            pct = 100 * (i + 1) / len(freqs)
            print(f'  [{pct:5.1f}%] {freq/1e6:8.2f} MHz — '
                  f'{len(rows)} measurements  '
                  f'(VGA15/LNA2={rows[-1][2]:.1f} dBFS)', flush=True)

    if not args.no_tx:
        try:
            aaronia_set_generator_off(args.host, args.port)
        except Exception:
            pass

    print(f'\nSweep complete. CSV: {args.out}')
    plot_results(args.out, args.plot)
    lin_png = args.plot.replace('.png', '_linearity.png')
    plot_gain_linearity(args.out, lin_png)


if __name__ == '__main__':
    main()
