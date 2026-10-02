#!/usr/bin/env python3
"""
psk_loopback.py — Over-the-air M-PSK loopback test.

Signal path:
  gen_psk (IQ generator) → Aaronia Spectran V6 TX (port 54664 IQ injection)
  → RF (cable or air) →
  Fobos SDR RX → demod_psk_file → compare TX vs RX symbols → SER table

Usage:
  python3 psk_loopback.py [options]

Options:
  --host HOST        Aaronia RTSA host   (default: 10.200.0.125)
  --tx-port PORT     Aaronia IQ TX port  (default: 54664)
  --center-freq F    Center frequency Hz (default: 433.92e6)
  --fobos-rate R     Fobos sample rate   (default: 2500000)
  --aaronia-rate R   Aaronia IQ TX rate  (default: 5714285)
  --lna N            Fobos LNA gain 0..3 (default: 2)
  --vga N            Fobos VGA gain 0..31 (default: 8)
  --m-list M...      PSK orders to test  (default: 2 4 8)
  --sym-rates R...   Symbol rates Hz     (default: 100 1000 10000 100000)
  --n-bursts N       Bursts per test     (default: 5)
  --data-syms N      Data symbols/burst  (default: 64)
  --seed N           RNG seed            (default: 42)
  --noise-sigma S    Gen AWGN sigma      (default: 0.01)
  --settle-ms T      Settle time ms after TX start (default: 100)
  --no-tx            Skip Aaronia TX
  --self-test        Pipe gen_psk → demod directly; no hardware needed
  --gen PATH         gen_psk binary
  --capture PATH     capture binary
  --demod PATH       demod_psk_file binary
  --out FILE         CSV output file (default: psk_loopback.csv)
"""

import argparse
import csv
import io
import json
import math
import os
import subprocess
import sys
import time
import urllib.request
import base64

import numpy as np

AUTH_HDR = 'Basic ' + base64.b64encode(b'a:').decode()
PREAMBLE_SYMS = 16  # must match gen_psk.c


# ─────────────────────────────────────────── Aaronia TX IQ injection

def build_rtsa_record(iq_f32: np.ndarray, center_freq: float, sample_rate: float) -> bytes:
    n = len(iq_f32) // 2
    now = time.time()
    header_fields = (
        f'"startTime":{now:.6f},'
        f'"endTime":{now + n/sample_rate:.6f},'
        f'"startFrequency":{int(center_freq - sample_rate/2)},'
        f'"endFrequency":{int(center_freq + sample_rate/2)},'
        f'"sampleFrequency":{sample_rate:.4f},'
        f'"sampleSize":16,'
        f'"sampleDepth":{n},'
        f'"payload":"iq",'
        f'"unit":"V"'
    )
    buf = io.BytesIO()
    np.savetxt(buf, iq_f32.reshape(1, -1), delimiter=',', fmt='%.6g', newline='')
    samples_csv = buf.getvalue()
    return b'\x1e{' + header_fields.encode() + b',"samples":[' + samples_csv + b']}\n'


def aaronia_tx_iq(host: str, port: int, iq_f32: np.ndarray,
                  center_freq: float, sample_rate: float) -> bool:
    payload = build_rtsa_record(iq_f32, center_freq, sample_rate)
    url = f'http://{host}:{port}/stream'
    req = urllib.request.Request(url, data=payload, method='POST', headers={
        'Authorization': AUTH_HDR,
        'Content-Type': 'application/octet-stream',
        'Content-Length': str(len(payload)),
    })
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status < 300
    except Exception as e:
        print(f'  WARNING aaronia TX failed: {e}', file=sys.stderr)
        return False


def aaronia_set_freq(host: str, port: int, center_freq: float, bw: float = 5e6):
    body = {
        'frequencyStart': int(center_freq - bw / 2),
        'frequencyEnd':   int(center_freq + bw / 2),
        'type': 'capture',
    }
    raw = json.dumps(body).encode()
    req = urllib.request.Request(
        f'http://{host}:{port}/control', data=raw, method='PUT',
        headers={'Authorization': AUTH_HDR, 'Content-Type': 'application/json',
                 'Content-Length': str(len(raw))})
    try:
        with urllib.request.urlopen(req, timeout=5):
            pass
    except Exception:
        pass


# ────────────────────────────────────────── Generate + capture + demod

def run_one(args, M: int, sym_rate: int, K: int) -> dict:
    fobos_rate = int(args.fobos_rate)
    center_freq = float(args.center_freq)
    n_bursts = args.n_bursts
    data_syms = args.data_syms
    seed = args.seed

    syms_per_burst = PREAMBLE_SYMS + data_syms + 24
    n_gen_syms = 24 + n_bursts * syms_per_burst
    n_gen_samps = n_gen_syms * K

    # ── Generate ─────────────────────────────────────────────────────────────
    gen_cmd = [
        args.gen, str(M), str(K), str(n_bursts), str(data_syms),
        str(seed), str(args.noise_sigma),
    ]
    gen_proc = subprocess.run(gen_cmd, capture_output=True, timeout=30)
    if gen_proc.returncode != 0:
        return {'error': f'gen_psk failed: {gen_proc.stderr.decode()[:200]}'}

    tx_iq_f32 = np.frombuffer(gen_proc.stdout, dtype=np.float32)
    tx_info = gen_proc.stderr.decode()

    tx_syms = {}
    for line in tx_info.splitlines():
        if line.startswith('TXSYM'):
            parts = line.split()
            b = int(parts[1].split('=')[1])
            tx_syms[b] = [int(x) for x in parts[3:]]

    if not tx_syms:
        return {'error': 'No TX symbols from gen_psk'}

    # ── Resample for Aaronia ─────────────────────────────────────────────────
    if not args.no_tx:
        aaronia_rate = float(args.aaronia_rate)
        if abs(aaronia_rate - fobos_rate) > 1:
            from scipy.signal import resample_poly
            g = math.gcd(int(aaronia_rate), fobos_rate)
            up = int(aaronia_rate) // g
            down = fobos_rate // g
            iq_c = tx_iq_f32[0::2] + 1j * tx_iq_f32[1::2]
            iq_rs = resample_poly(iq_c, up, down).astype(np.complex64)
            tx_iq_aa = np.empty(len(iq_rs) * 2, dtype=np.float32)
            tx_iq_aa[0::2] = iq_rs.real
            tx_iq_aa[1::2] = iq_rs.imag
        else:
            tx_iq_aa = tx_iq_f32
            aaronia_rate = fobos_rate

        aaronia_set_freq(args.host, args.tx_port, center_freq, max(5e6, fobos_rate * 1.2))

    # ── Capture ──────────────────────────────────────────────────────────────
    tx_duration_s = n_gen_samps / fobos_rate
    capture_samps = int((tx_duration_s + args.settle_ms / 1000.0 + 0.5) * fobos_rate)
    capture_samps = ((capture_samps + K - 1) // K) * K

    capture_cmd = [
        args.capture,
        str(int(center_freq)), str(fobos_rate), str(capture_samps),
        str(args.lna), str(args.vga),
    ]

    if args.self_test:
        demod_cmd2 = [
            args.demod, str(M), str(K), str(PREAMBLE_SYMS), str(data_syms),
        ]
        demod_proc2 = subprocess.run(demod_cmd2, input=gen_proc.stdout,
                                     capture_output=True, timeout=60)
        rx_syms = {}
        for line in demod_proc2.stdout.decode().splitlines():
            if line.startswith('RXSYM'):
                parts = line.split()
                b = int(parts[1].split('=')[1])
                rx_syms[b] = [int(x) for x in parts[3:]]
        total_syms = 0
        err_syms = 0
        matched_bursts = 0
        for b in range(n_bursts):
            if b not in rx_syms:
                continue
            matched_bursts += 1
            for t, r in zip(tx_syms.get(b, []), rx_syms[b]):
                total_syms += 1
                if t != r:
                    err_syms += 1
        ser = err_syms / total_syms if total_syms > 0 else float('nan')
        return {
            'M': M, 'sym_rate': sym_rate, 'K': K,
            'bursts_det': matched_bursts, 'bursts_tx': n_bursts,
            'sym_total': total_syms, 'sym_errors': err_syms, 'SER': ser,
        }

    if args.no_tx:
        cap_proc = subprocess.run(capture_cmd, capture_output=True, timeout=60)
    else:
        cap_proc_bg = subprocess.Popen(capture_cmd, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
        time.sleep(args.settle_ms / 1000.0)
        aaronia_tx_iq(args.host, args.tx_port, tx_iq_aa, center_freq, aaronia_rate)
        cap_proc_bg.wait(timeout=60)
        class _FakeProc:
            def __init__(self, p):
                self.stdout = p.stdout.read() if p.stdout else b''
                self.stderr = p.stderr.read() if p.stderr else b''
                self.returncode = p.returncode
        cap_proc = _FakeProc(cap_proc_bg)

    if cap_proc.returncode not in (0, None):
        return {'error': f'capture failed: {cap_proc.stderr[:200]}'}

    rx_iq_raw = cap_proc.stdout if isinstance(cap_proc.stdout, bytes) else cap_proc.stdout

    # ── Demodulate ───────────────────────────────────────────────────────────
    demod_cmd = [
        args.demod, str(M), str(K), str(PREAMBLE_SYMS), str(data_syms),
    ]
    demod_proc = subprocess.run(demod_cmd, input=rx_iq_raw,
                                capture_output=True, timeout=60)
    if demod_proc.returncode != 0:
        return {'error': f'demod failed: {demod_proc.stderr.decode()[:200]}'}

    rx_syms = {}
    for line in demod_proc.stdout.decode().splitlines():
        if line.startswith('RXSYM'):
            parts = line.split()
            b = int(parts[1].split('=')[1])
            rx_syms[b] = [int(x) for x in parts[3:]]

    total_syms = 0
    err_syms = 0
    matched_bursts = 0
    for b in range(n_bursts):
        if b not in rx_syms:
            continue
        matched_bursts += 1
        for t, r in zip(tx_syms.get(b, []), rx_syms[b]):
            total_syms += 1
            if t != r:
                err_syms += 1

    ser = err_syms / total_syms if total_syms > 0 else float('nan')
    return {
        'M': M, 'sym_rate': sym_rate, 'K': K,
        'bursts_det': matched_bursts, 'bursts_tx': n_bursts,
        'sym_total': total_syms, 'sym_errors': err_syms, 'SER': ser,
    }


# ──────────────────────────────────────────────────────────────────── Main

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host',         default='10.200.0.125')
    p.add_argument('--tx-port',      type=int, default=54664)
    p.add_argument('--center-freq',  type=float, default=433.92e6)
    p.add_argument('--fobos-rate',   type=int, default=2_500_000)
    p.add_argument('--aaronia-rate', type=float, default=5_714_285.0)
    p.add_argument('--lna',          type=int, default=2)
    p.add_argument('--vga',          type=int, default=8)
    p.add_argument('--m-list',       type=int, nargs='+', default=[2, 4, 8])
    p.add_argument('--sym-rates',    type=int, nargs='+',
                   default=[100, 1_000, 10_000, 100_000])
    p.add_argument('--n-bursts',     type=int, default=5)
    p.add_argument('--data-syms',    type=int, default=64)
    p.add_argument('--seed',         type=int, default=42)
    p.add_argument('--noise-sigma',  type=float, default=0.01)
    p.add_argument('--settle-ms',    type=float, default=100.0)
    p.add_argument('--no-tx',        action='store_true')
    p.add_argument('--self-test',    action='store_true',
                   help='Pipe gen → demod directly, no hardware')
    p.add_argument('--gen',     default=os.path.join(os.path.dirname(__file__), 'c', 'gen_psk'))
    p.add_argument('--capture', default=os.path.join(os.path.dirname(__file__), 'c', 'capture'))
    p.add_argument('--demod',   default=os.path.join(os.path.dirname(__file__), 'c', 'demod_psk_file'))
    p.add_argument('--out',          default='psk_loopback.csv')
    return p.parse_args()


def main():
    args = parse_args()

    for b in (args.gen, args.capture, args.demod):
        if not os.path.isfile(b):
            print(f'ERROR: binary not found: {b}', file=sys.stderr)
            print('Build first:  cd c && make', file=sys.stderr)
            sys.exit(1)

    fobos_rate = args.fobos_rate

    print(f'M-PSK over-the-air loopback test')
    print(f'Center: {args.center_freq/1e6:.3f} MHz  '
          f'Fobos rate: {fobos_rate/1e6:.3f} Msps')
    if args.no_tx:
        print('TX: disabled (--no-tx)')
    else:
        print(f'Aaronia TX: {args.host}:{args.tx_port}  '
              f'rate: {args.aaronia_rate/1e6:.3f} Msps')
    print()

    print(f'{"M":>2}  {"sym_rate":>9}  {"K":>6}  {"det/tx":>6}  '
          f'{"err/total":>12}  {"SER":>8}')
    print('-' * 58)

    with open(args.out, 'w', newline='') as csvf:
        writer = csv.writer(csvf)
        writer.writerow(['M', 'sym_rate_hz', 'K', 'bursts_det', 'bursts_tx',
                         'sym_errors', 'sym_total', 'SER'])

        for M in args.m_list:
            K_min = 4
            for sym_rate in args.sym_rates:
                K = fobos_rate // sym_rate
                if K < K_min:
                    print(f'  M={M} sym_rate={sym_rate:,}: K={K} < {K_min}, skip')
                    continue
                if K > 65536:
                    print(f'  M={M} sym_rate={sym_rate:,}: K={K} > 65536, skip')
                    continue

                actual_rate = fobos_rate // K
                res = run_one(args, M, actual_rate, K)

                if 'error' in res:
                    print(f'  M={M} sym_rate={sym_rate:,}: ERROR {res["error"]}')
                    continue

                print(f'{M:>2}  {actual_rate:>9,}  {K:>6}  '
                      f'{res["bursts_det"]:>3}/{res["bursts_tx"]:<2}  '
                      f'{res["sym_errors"]:>6}/{res["sym_total"]:<5}  '
                      f'{res["SER"]:>8.4f}')

                writer.writerow([
                    M, actual_rate, K,
                    res['bursts_det'], res['bursts_tx'],
                    res['sym_errors'], res['sym_total'], f'{res["SER"]:.6f}',
                ])
                csvf.flush()

    print(f'\nResults saved → {args.out}')


if __name__ == '__main__':
    main()
