#!/usr/bin/env python3
"""
ota_loopback.py — FSK and PSK OTA loopback via Aaronia V6B TX + FobosOne RX.

Signal path:
  gen_fsk/gen_psk → resample 2.5→5.714 Msps → .rtsa file
  → scp to Aaronia → FileReader → V6B TX
  → RF cable → FobosOne RX capture → demod → SER table

Usage:
  python3 ota_loopback.py [options]

Options:
  --mode fsk|psk       Modulation (default: fsk)
  --m-list M...        Orders to test (default: 2 4)
  --sym-rates R...     Symbol rates Hz (default: 10000 50000 100000)
  --n-bursts N         Bursts per .rtsa file (default: 10)
  --data-syms N        Data symbols per burst (default: 64)
  --seed N             RNG seed (default: 42)
  --noise-sigma S      Gen AWGN (default: 0.01)
  --center-freq F      RF center Hz (default: 433920000)
  --fobos-rate R       FobosOne sample rate (default: 5000000)
  --lna N              FobosOne LNA gain 0..3 (default: 2)
  --vga N              FobosOne VGA gain 0..31 (default: 8)
  --capture-s T        Capture duration seconds (default: 3)
  --aaronia-host H     Aaronia SSH host (default: snedosiekov@10.200.0.125)
  --remote-rtsa-dir D  Remote dir for .rtsa files (default: /home/snedosiekov/rtsa_capture)
  --dji-template P     Local DJI template path (default: /tmp/DJI-Mini4-10ms.rtsa)
  --transattn V        V6B TX attenuation dB (default: -50)
  --out FILE           CSV output (default: ota_loopback_results.csv)
  --fsk-dir D          Path to fsk_loopback/c dir
  --psk-dir D          Path to psk_loopback/c dir
  --iq-hz F0 F1...     IQ tone frequencies Hz per symbol for V6B TX (one per M).
                       When given, uses gen_fsk_py.py (positive IQ, no conjugate)
                       instead of gen_fsk.c, and passes explicit RF tones to the
                       DFT demodulator. Example for positive-only IQ (V6B cannot
                       handle negative IQ): --iq-hz 200000 400000
                       RF tones = -f_iq * v6b_scale = −3376kHz, −6752kHz.
  --v6b-scale S        IQ→RF scale factor (default: 16.88)
"""

import argparse
import csv
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from scipy.signal import resample_poly

# ── Constants ────────────────────────────────────────────────────────────────

AARONIA_RATE  = 40_000_000 / 7   # 5,714,285.714... Hz — V6B native IQ rate
# Resample ratio is computed per-run from fobos_rate; constants here for reference.
# 2.5 Msps → 5.714 Msps: up=16, down=7
# 5.0 Msps → 5.714 Msps: up=8,  down=7
RESAMPLE_UP   = 8
RESAMPLE_DOWN = 7

FSK_PREAMBLE_SYMS = 8   # must match gen_fsk.c
PSK_PREAMBLE_SYMS = 16  # must match gen_psk.c (longer for carrier phase acquisition)
PREAMBLE_SYMS = FSK_PREAMBLE_SYMS  # default; overridden per-mode in run_one()

# DJI template geometry (from empirical dissection of DJI-Mini4-10ms.rtsa)
HEADER_SIZE  = 736   # DSFH+STRM+ANTA+SSTR total
SAMP_HDR_SZ  = 72    # per SAMP: 16-byte chunk hdr + 56-byte specific fields
SAMP_PAYLOAD = 348020  # bytes per SAMP payload (348092 total − 72 hdr)
SAMPLES_PER_SAMP = 32768   # float32 IQ pairs used per SAMP

# Byte offsets into the template
STRM_CF_OFF  = 40    # STRM center_freq (double)
SSTR_BASE    = 432   # SSTR specific fields start
SSTR_CF_OFF  = SSTR_BASE + 24   # center_freq
SSTR_SPAN_OFF = SSTR_BASE + 32  # span
SSTR_STEP_OFF = SSTR_BASE + 40  # step_freq
SSTR_TS_OFF  = SSTR_BASE + 64   # time_step

SAMP_MAGIC   = b'SAMP'
SAMP_TOTAL_SZ = 348092   # bytes (fixed in DJI template)

DJI_CHUNK_DUR = SAMPLES_PER_SAMP / AARONIA_RATE   # s per SAMP chunk


# ── .rtsa file builder ───────────────────────────────────────────────────────

def find_samp_positions(data: bytes) -> list[int]:
    positions = []
    i = HEADER_SIZE
    while i < len(data) - 4:
        if data[i:i+4] == SAMP_MAGIC:
            positions.append(i)
            total = struct.unpack_from('<I', data, i + 4)[0]
            i += total
        else:
            i += 4
    return positions


def build_rtsa(iq_f32: np.ndarray, center_hz: float,
               dji_template: bytes) -> bytes:
    """
    Pack iq_f32 (float32, interleaved I/Q) into a .rtsa file using the
    DJI template structure.  iq_f32 is at AARONIA_RATE sps.

    The IQ is distributed across the 8 DJI SAMP chunks.  If iq_f32 is
    shorter than 8×SAMPLES_PER_SAMP pairs, remaining space is silence.
    If longer, it wraps (the file will loop anyway).
    """
    span    = AARONIA_RATE   # use full-rate span so V6B stays at native rate
    tstep   = SAMPLES_PER_SAMP / AARONIA_RATE

    data = bytearray(dji_template)

    # Patch STRM center
    struct.pack_into('<d', data, STRM_CF_OFF, center_hz)

    # Patch SSTR
    struct.pack_into('<d', data, SSTR_CF_OFF,  center_hz)
    struct.pack_into('<d', data, SSTR_SPAN_OFF, span)
    struct.pack_into('<d', data, SSTR_STEP_OFF, span)
    struct.pack_into('<d', data, SSTR_TS_OFF,   tstep)

    samp_positions = find_samp_positions(dji_template)
    n_total_pairs  = len(iq_f32) // 2

    for chunk_idx, pos in enumerate(samp_positions):
        src_start = (chunk_idx * SAMPLES_PER_SAMP) % max(n_total_pairs, 1)
        payload   = bytearray(SAMP_PAYLOAD)

        pairs_written = 0
        src = src_start
        while pairs_written < SAMPLES_PER_SAMP and src < n_total_pairs:
            n = min(SAMPLES_PER_SAMP - pairs_written, n_total_pairs - src)
            payload[pairs_written*8 : pairs_written*8 + n*8] = \
                iq_f32[src*2 : src*2 + n*2].tobytes()
            pairs_written += n
            src += n
            if src >= n_total_pairs:
                src = 0   # wrap: signal repeats within the chunk

        data[pos + SAMP_HDR_SZ : pos + SAMP_HDR_SZ + SAMP_PAYLOAD] = payload

        t_start = chunk_idx * DJI_CHUNK_DUR
        t_end   = (chunk_idx + 1) * DJI_CHUNK_DUR
        struct.pack_into('<d', data, pos + 32, t_start)
        struct.pack_into('<d', data, pos + 40, t_end)

    return bytes(data)


# ── RTSA mission XML ─────────────────────────────────────────────────────────

def mission_xml(rtsa_path: str, center_hz: int, transattn: float) -> str:
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<Mission>
  <General GUID="796c3c55-d84b-4638-8297-6c2e1399ddb7" Version="RMIX_2016-09-30-1" Build="16454">
    <Index>
      <DateCreatedUTC Val="1781400000"/>
      <BuildCreated Val="3.0.3.16454"/>
      <DateSavedUTC Val="1781400000"/>
      <BuildSaved Val="3.0.3.16454"/>
      <SaveCompressed Val="0"/>
      <IsTXMission Val="1"/>
    </Index>
  </General>
  <Data>
    <Bases>
      <Base GUID="b4185570-c9a5-4b10-8c1b-c4a762024d00" Factory="Factory_BlockEditor" Name="Base_BlockEditor" Title="Blockgraph Editor" AppBlock="1"/>
      <Base GUID="6d1c50e3-9e74-45da-bd61-e426c07d8c00" Factory="Factory_MainControl" Name="Base_MainControl" Title="Main Control" AppBlock="1"/>
      <Base GUID="abc12345-0001-0001-0001-000000000010" Factory="Factory_FileReader" Name="Block_FileReader_0" Title="File Reader">
        <ConfigItem Name="Block_FileReader_0_config" Class="group" Type="0">
          <ConfigItem Name="main" Class="group" Type="0">
            <ConfigItem Name="filename" Class="file" Type="4" Val="{rtsa_path}" RVal="{rtsa_path}"/>
            <ConfigItem Name="autostart" Class="bool" Type="6" Val="1"/>
            <ConfigItem Name="loop" Class="bool" Type="6" Val="1"/>
          </ConfigItem>
        </ConfigItem>
        <Inputs>
          <Input Name="Input_FileReader_Sync_0" Title="Sync" Owner="Block_FileReader_0"/>
        </Inputs>
        <Outputs>
          <Output Name="Input_FileReader_Spectra_0" Title="Stream" Owner="Block_FileReader_0">
            <Connections>
              <Connection Name="Input_Spectran_V6B_IQ_0" Title="IQStream1" Owner="Block_Spectran_V6B_0"/>
            </Connections>
          </Output>
        </Outputs>
      </Base>
      <Base GUID="abc12345-0001-0001-0001-000000000011" Factory="Factory_Spectran_V6B" Name="Block_Spectran_V6B_0" Title="SPECTRAN V6 PLUS">
        <ConfigItem Name="Block_Spectran_V6B_0_config" Class="group" Type="0">
          <ConfigItem Name="main" Class="group" Type="0">
            <ConfigItem Name="centerfreq" Class="float" Type="2" Val="{center_hz}"/>
            <ConfigItem Name="spanfreq" Class="float" Type="2" Val="5714285"/>
            <ConfigItem Name="transattn" Class="float" Type="2" Val="{transattn}"/>
            <ConfigItem Name="connect" Class="bool" Type="6" Val="1"/>
            <ConfigItem Name="run" Class="bool" Type="6" Val="1"/>
          </ConfigItem>
          <ConfigItem Name="device" Class="group" Type="0">
            <ConfigItem Name="transmittermode" Class="enum" Type="7" Val="2" SVal="Stream"
              Names="Off,Test,Stream,Reactive,Signal Generator,Pattern Generator"/>
          </ConfigItem>
        </ConfigItem>
        <Inputs>
          <Input Name="Input_Antenna_0" Title="RF1" Owner="Block_Spectran_V6B_0"/>
          <Input Name="Input_Antenna2_0" Title="RF2" Owner="Block_Spectran_V6B_0"/>
          <Input Name="Input_Spectran_V6B_IQ_0" Title="IQStream1" Owner="Block_Spectran_V6B_0"/>
        </Inputs>
        <Outputs>
          <Output Name="Output_Spectran_V6B_Spectra1_0" Title="Spectra1" Owner="Block_Spectran_V6B_0"/>
          <Output Name="Output_Spectran_V6B_IQ_0" Title="IQStream1" Owner="Block_Spectran_V6B_0"/>
          <Output Name="Output_Spectran_V6B_TX_0" Title="Tx" Owner="Block_Spectran_V6B_0"/>
        </Outputs>
      </Base>
    </Bases>
  </Data>
</Mission>'''


# ── Aaronia control helpers ──────────────────────────────────────────────────

def ssh(host: str, cmd: str, check: bool = False, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(
        ['ssh', '-o', 'ConnectTimeout=10', host, cmd],
        capture_output=True, text=True, timeout=timeout, check=check)


def scp_to(host: str, local: str, remote: str):
    subprocess.run(['scp', '-q', local, f'{host}:{remote}'],
                   check=True, timeout=60)


def restart_rtsa(host: str, mission_path: str, log_path: str, wait_s: int = 18):
    ssh(host, 'sudo pkill -f Aaronia-RTSA-Suite-PRO 2>/dev/null; true')
    time.sleep(3)
    r = ssh(host, f'bash /tmp/go.sh {mission_path} {log_path}')
    pid = r.stdout.strip()
    print(f'  RTSA pid={pid}, waiting {wait_s}s for startup...', flush=True)
    time.sleep(wait_s)
    return pid


# ── Per-test run ─────────────────────────────────────────────────────────────

def run_one(args, mode: str, M: int, K: int,
            dji_template: bytes, rtsa_remote: str,
            mission_remote: str) -> dict:

    fobos_rate  = args.fobos_rate
    center_hz   = args.center_freq
    data_syms   = args.data_syms
    use_py_gen  = (mode == 'fsk' and args.iq_hz)
    preamble_syms = FSK_PREAMBLE_SYMS if mode == 'fsk' else PSK_PREAMBLE_SYMS

    # K is always in the generator's sample-rate domain.
    # use_py_gen: generator runs at AARONIA_RATE → K_gen = K, K_cap = fobos_rate // sym_rate
    # default:    generator runs at fobos_rate  → K_gen = K_cap = K
    if use_py_gen:
        sym_rate = int(AARONIA_RATE) // K          # actual sym rate from gen at AARONIA_RATE
        K_cap    = fobos_rate // sym_rate           # samples/sym at FobosOne rate
        samples_capacity = 8 * SAMPLES_PER_SAMP    # generator output already at AARONIA_RATE
    else:
        sym_rate = fobos_rate // K
        K_cap    = K
        rs_up_local   = 8 if fobos_rate == 5_000_000 else 16
        rs_down_local = 7
        samples_capacity = int(8 * SAMPLES_PER_SAMP * rs_down_local / rs_up_local)
    leading_gap_samps = 24 * K
    samples_per_burst = (preamble_syms + data_syms + 24) * K
    max_bursts = max(1, (samples_capacity - leading_gap_samps) // samples_per_burst)
    n_bursts = min(args.n_bursts, max_bursts)
    if n_bursts < args.n_bursts:
        print(f'  (capping n_bursts {args.n_bursts}→{n_bursts} to fit .rtsa file at {sym_rate} sym/s)')

    demod_bin = os.path.join(args.fsk_dir if mode == 'fsk' else args.psk_dir, f'demod_{mode}_file')
    cap_bin   = os.path.join(args.fsk_dir, 'capture')

    # ── Generate baseband IQ ──────────────────────────────────────────────────
    if use_py_gen:
        gen_py = os.path.join(os.path.dirname(__file__), 'gen_fsk_py.py')
        gen_cmd = [sys.executable, gen_py,
                   str(M), str(K), str(n_bursts), str(data_syms), str(args.seed),
                   '--fs', str(AARONIA_RATE),
                   '--iq-hz'] + [str(f) for f in args.iq_hz]
    else:
        gen_bin = os.path.join(args.fsk_dir if mode == 'fsk' else args.psk_dir, f'gen_{mode}')
        gen_cmd = [gen_bin, str(M), str(K), str(n_bursts), str(data_syms),
                   str(args.seed), str(args.noise_sigma)]

    gp = subprocess.run(gen_cmd, capture_output=True, timeout=30)
    if gp.returncode != 0:
        return {'error': f'gen_{mode} failed: {gp.stderr.decode()[:200]}'}

    tx_iq_f32 = np.frombuffer(gp.stdout, dtype=np.float32).copy()

    # Parse TX symbols
    tx_syms = {}
    for line in gp.stderr.decode().splitlines():
        if line.startswith('TXSYM'):
            parts = line.split()
            b = int(parts[1].split('=')[1])
            tx_syms[b] = [int(x) for x in parts[3:]]
    if not tx_syms:
        return {'error': 'No TXSYM lines from generator'}

    # ── Resample to Aaronia rate ──────────────────────────────────────────────
    if use_py_gen:
        # gen_fsk_py.py already outputs at AARONIA_RATE; no resample needed.
        iq_aa = tx_iq_f32
    else:
        # Resample: fobos_rate → AARONIA_RATE
        # For 5 Msps: 5e6 × 8/7 = 5.714 Msps; for 2.5 Msps: 2.5e6 × 16/7 = 5.714 Msps
        rs_up   = int(round(AARONIA_RATE / math.gcd(int(AARONIA_RATE * 7), fobos_rate * 7)))
        rs_down = fobos_rate // math.gcd(int(AARONIA_RATE * 7) // 7, fobos_rate)
        if fobos_rate == 5_000_000:
            rs_up, rs_down = 8, 7
        elif fobos_rate == 2_500_000:
            rs_up, rs_down = 16, 7
        iq_c  = tx_iq_f32[0::2] + 1j * tx_iq_f32[1::2]
        iq_rs = resample_poly(iq_c, rs_up, rs_down).astype(np.complex64)
        iq_aa = np.empty(len(iq_rs) * 2, dtype=np.float32)
        iq_aa[0::2] = iq_rs.real
        iq_aa[1::2] = iq_rs.imag

    # ── Build .rtsa file ──────────────────────────────────────────────────────
    rtsa_bytes = build_rtsa(iq_aa, center_hz, dji_template)

    with tempfile.NamedTemporaryFile(suffix='.rtsa', delete=False) as tf:
        tf.write(rtsa_bytes)
        local_rtsa = tf.name

    # ── Upload .rtsa and optionally restart RTSA ─────────────────────────────
    print(f'  Uploading {len(rtsa_bytes)//1024} KB .rtsa...', flush=True)
    scp_to(args.aaronia_host, local_rtsa, rtsa_remote)
    os.unlink(local_rtsa)

    if args.no_restart:
        # Hot-swap: overwrite the .rtsa file the running FileReader is looping.
        # FileReader picks up the new content on the next loop iteration.
        # Wait briefly for FileReader to complete its current loop pass.
        wait_s = max(2, args.rtsa_wait // 8)
        print(f'  (no-restart mode) waiting {wait_s}s for FileReader to re-read...', flush=True)
        time.sleep(wait_s)
    else:
        xml = mission_xml(rtsa_remote, int(center_hz), args.transattn)
        with tempfile.NamedTemporaryFile(mode='w', suffix='.rmix', delete=False) as tf:
            tf.write(xml)
            local_rmix = tf.name
        scp_to(args.aaronia_host, local_rmix, mission_remote)
        os.unlink(local_rmix)

        restart_rtsa(args.aaronia_host, mission_remote, '/tmp/rtsa_ota.log',
                     wait_s=args.rtsa_wait)

    # ── Capture with FobosOne ─────────────────────────────────────────────────
    cap_samps = int(args.capture_s * fobos_rate)
    cap_samps = ((cap_samps + K_cap - 1) // K_cap) * K_cap
    cap_cmd = [cap_bin, str(int(center_hz)), str(fobos_rate), str(cap_samps),
               str(args.lna), str(args.vga)]

    print(f'  Capturing {cap_samps} samples ({args.capture_s}s)...', flush=True)
    cp = subprocess.run(cap_cmd, capture_output=True, timeout=args.capture_s + 15)
    if cp.returncode != 0:
        return {'error': f'capture failed: {cp.stderr.decode()[:200]}'}

    # ── Demodulate ────────────────────────────────────────────────────────────
    if use_py_gen and mode == 'fsk':
        # DFT mode: RF tones = -f_iq * v6b_scale (V6B LSB inversion)
        rf_tones = [-f * args.v6b_scale for f in args.iq_hz]
        demod_cmd = [demod_bin, str(M), str(K_cap), str(preamble_syms), str(data_syms),
                     '6.0', '0.75', str(fobos_rate)] + [str(int(f)) for f in rf_tones]
    else:
        demod_cmd = [demod_bin, str(M), str(K_cap), str(preamble_syms), str(data_syms)]
    dp = subprocess.run(demod_cmd, input=cp.stdout, capture_output=True, timeout=300)
    if dp.returncode != 0:
        return {'error': f'demod failed: {dp.stderr.decode()[:200]}'}

    rx_syms = {}
    for line in dp.stdout.decode().splitlines():
        if line.startswith('RXSYM'):
            parts = line.split()
            b = int(parts[1].split('=')[1])
            rx_syms[b] = [int(x) for x in parts[3:]]

    # ── Score ─────────────────────────────────────────────────────────────────
    total_syms = err_syms = matched_bursts = 0
    for b_rx_idx, rx_b in rx_syms.items():
        tx_b = tx_syms.get(b_rx_idx % n_bursts, [])
        if not tx_b:
            continue
        matched_bursts += 1
        for t, r in zip(tx_b, rx_b):
            total_syms += 1
            if t != r:
                err_syms += 1

    ser = err_syms / total_syms if total_syms > 0 else float('nan')
    return {
        'mode': mode, 'M': M, 'sym_rate': sym_rate, 'K': K,
        'bursts_det': matched_bursts,
        'sym_total': total_syms, 'sym_errors': err_syms, 'SER': ser,
    }


# ── Main ─────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--mode',          default='fsk', choices=['fsk', 'psk', 'both'])
    p.add_argument('--m-list',        type=int, nargs='+', default=[2, 4])
    p.add_argument('--sym-rates',     type=int, nargs='+',
                   default=[10_000, 50_000, 100_000])
    p.add_argument('--n-bursts',      type=int, default=10)
    p.add_argument('--data-syms',     type=int, default=64)
    p.add_argument('--seed',          type=int, default=42)
    p.add_argument('--noise-sigma',   type=float, default=0.0)
    p.add_argument('--center-freq',   type=float, default=433_920_000)
    p.add_argument('--fobos-rate',    type=int, default=5_000_000)
    p.add_argument('--lna',           type=int, default=2)
    p.add_argument('--vga',           type=int, default=8)
    p.add_argument('--capture-s',     type=float, default=3.0)
    p.add_argument('--aaronia-host',  default='snedosiekov@10.200.0.125')
    p.add_argument('--remote-rtsa-dir', default='/home/snedosiekov/rtsa_capture')
    p.add_argument('--dji-template',  default='/tmp/DJI-Mini4-10ms.rtsa')
    p.add_argument('--transattn',     type=float, default=-50.0)
    p.add_argument('--rtsa-wait',     type=int, default=18)
    p.add_argument('--out',           default='ota_loopback_results.csv')
    p.add_argument('--fsk-dir', default=os.path.join(os.path.dirname(__file__), 'c'))
    p.add_argument('--psk-dir', default=os.path.join(os.path.dirname(os.path.dirname(__file__)),
                                                      'psk_loopback', 'c'))
    p.add_argument('--iq-hz', type=float, nargs='+', default=None,
                   help='IQ tone frequencies Hz per symbol (uses gen_fsk_py.py + DFT demod)')
    p.add_argument('--v6b-scale', type=float, default=16.88,
                   help='V6B IQ→RF scale factor (default 16.88)')
    p.add_argument('--no-restart', action='store_true',
                   help='Skip RTSA restart and mission upload; hot-swap .rtsa file only. '
                        'Use when V6B is already connected manually and FileReader is looping.')
    return p.parse_args()


def main():
    args = parse_args()

    dji_template = open(args.dji_template, 'rb').read()
    print(f'DJI template: {len(dji_template)} bytes, '
          f'{len(find_samp_positions(dji_template))} SAMP chunks')

    rtsa_remote    = f'{args.remote_rtsa_dir}/ota_loopback.rtsa'
    mission_remote = '/tmp/ota_loopback.rmix'

    if args.no_restart:
        print('*** --no-restart mode ***')
        print(f'  FileReader must already be running on Aaronia host, looping:')
        print(f'    {rtsa_remote}')
        print(f'  V6B must already be connected and streaming in the RTSA GUI.')
        print(f'  Each test case: upload new .rtsa → wait → capture.')
        print()

    modes = ['fsk', 'psk'] if args.mode == 'both' else [args.mode]

    print(f'\n{"mode":>4}  {"M":>2}  {"sym_rate":>9}  {"K":>5}  '
          f'{"bursts":>6}  {"err/total":>12}  {"SER":>8}')
    print('-' * 62)

    with open(args.out, 'w', newline='') as csvf:
        writer = csv.writer(csvf)
        writer.writerow(['mode', 'M', 'sym_rate_hz', 'K', 'bursts_det',
                         'sym_errors', 'sym_total', 'SER'])

        for mode in modes:
            for M in args.m_list:
                K_min = 128 if M >= 8 else 16
                # When --iq-hz, K is in Aaronia rate domain; otherwise fobos_rate.
                base_rate = int(AARONIA_RATE) if (mode == 'fsk' and args.iq_hz) else args.fobos_rate
                for sym_rate in args.sym_rates:
                    K = base_rate // sym_rate
                    if K < K_min:
                        print(f'  {mode} M={M} sym_rate={sym_rate:,}: K={K} < {K_min}, skip')
                        continue
                    if K > 2048:
                        print(f'  {mode} M={M} sym_rate={sym_rate:,}: K={K} > 2048, skip')
                        continue

                    actual_rate = base_rate // K
                    print(f'\n[{mode.upper()} M={M} sym_rate={actual_rate:,} K={K}]', flush=True)

                    res = run_one(args, mode, M, K, dji_template,
                                  rtsa_remote, mission_remote)

                    if 'error' in res:
                        print(f'  ERROR: {res["error"]}')
                        continue

                    ser_str = f'{res["SER"]:.4f}' if not math.isnan(res["SER"]) else 'NaN'
                    print(f'{mode:>4}  {M:>2}  {actual_rate:>9,}  {K:>5}  '
                          f'{res["bursts_det"]:>6}  '
                          f'{res["sym_errors"]:>6}/{res["sym_total"]:<5}  '
                          f'{ser_str:>8}')

                    writer.writerow([mode, M, actual_rate, K,
                                     res['bursts_det'], res['sym_errors'],
                                     res['sym_total'], ser_str])
                    csvf.flush()

    print(f'\nResults → {args.out}')


if __name__ == '__main__':
    main()
