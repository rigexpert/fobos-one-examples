#!/usr/bin/env python3
"""
diag_iq_sign.py — Diagnose V6B negative-IQ handling via RF loopback.

Test A: +200 kHz IQ  →  expected RF at LO − 3376 kHz  (confirms calibration)
Test B: −200 kHz IQ  →  expected RF at LO + 3376 kHz  (tests negative-IQ support)

If Test B fails (no +3376 kHz power) V6B cannot handle negative IQ frequencies.
Redesign: use positive-only IQ tones (sym=0 at +200 kHz, sym=1 at +400 kHz),
  which map to RF at −3376 kHz and −6752 kHz respectively.

Usage:
  python3 diag_iq_sign.py [options]

Options:
  --aaronia-host H   SSH host (default: snedosiekov@10.200.0.125)
  --center-freq F    RF center Hz (default: 433920000)
  --dji-template P   DJI .rtsa template (default: /tmp/DJI-Mini4-10ms.rtsa)
  --remote-dir D     Aaronia remote dir (default: /home/snedosiekov/rtsa_capture)
  --transattn V      V6B TX attenuation dB (default: -50)
  --fobos-rate R     FobosOne sample rate Hz (default: 10000000)
  --capture-s T      Capture duration seconds (default: 3)
  --lna N            FobosOne LNA 0-3 (default: 2)
  --vga N            FobosOne VGA 0-31 (default: 8)
  --rtsa-wait N      Seconds to wait after RTSA restart (default: 18)
  --v6b-scale S      IQ→RF scale factor (default: 16.88)
  --iq-offset-hz F   IQ offset to test Hz (default: 200000)
  --cap-bin P        Path to capture binary (default: <this-dir>/c/capture)
  --skip-upload      Skip SSH/SCP steps (capture only, for re-running analysis)
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

AARONIA_RATE    = 40_000_000 / 7   # 5,714,285.714... Hz
HEADER_SIZE     = 736
SAMP_HDR_SZ     = 72
SAMP_PAYLOAD    = 348020
SAMPLES_PER_SAMP = 32768
STRM_CF_OFF     = 40
SSTR_BASE       = 432
SSTR_CF_OFF     = SSTR_BASE + 24
SSTR_SPAN_OFF   = SSTR_BASE + 32
SSTR_STEP_OFF   = SSTR_BASE + 40
SSTR_TS_OFF     = SSTR_BASE + 64
SAMP_MAGIC      = b'SAMP'
DJI_CHUNK_DUR   = SAMPLES_PER_SAMP / AARONIA_RATE


def find_samp_positions(data: bytes) -> list:
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


def build_rtsa(iq_f32: np.ndarray, center_hz: float, dji_template: bytes) -> bytes:
    span  = AARONIA_RATE
    tstep = SAMPLES_PER_SAMP / AARONIA_RATE
    data  = bytearray(dji_template)
    struct.pack_into('<d', data, STRM_CF_OFF,   center_hz)
    struct.pack_into('<d', data, SSTR_CF_OFF,   center_hz)
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
                src = 0
        data[pos + SAMP_HDR_SZ : pos + SAMP_HDR_SZ + SAMP_PAYLOAD] = payload
        struct.pack_into('<d', data, pos + 32, chunk_idx * DJI_CHUNK_DUR)
        struct.pack_into('<d', data, pos + 40, (chunk_idx + 1) * DJI_CHUNK_DUR)
    return bytes(data)


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


def ssh(host: str, cmd: str, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(['ssh', '-o', 'ConnectTimeout=10', host, cmd],
                          capture_output=True, text=True, timeout=timeout)


def scp_to(host: str, local: str, remote: str):
    subprocess.run(['scp', '-q', local, f'{host}:{remote}'], check=True, timeout=60)


def restart_rtsa(host: str, mission_path: str, wait_s: int = 18):
    ssh(host, 'sudo pkill -f Aaronia-RTSA-Suite-PRO 2>/dev/null; true')
    time.sleep(3)
    r = ssh(host, f'bash /tmp/go.sh {mission_path} /tmp/rtsa_diag.log')
    pid = r.stdout.strip()
    print(f'  RTSA pid={pid}, waiting {wait_s}s for startup...', flush=True)
    time.sleep(wait_s)


def capture_fobos(cap_bin: str, center_hz: float, rate: int, n_samps: int,
                  lna: int, vga: int, timeout_s: float) -> np.ndarray:
    cmd = [cap_bin, str(int(center_hz)), str(rate), str(n_samps), str(lna), str(vga)]
    cp = subprocess.run(cmd, capture_output=True, timeout=int(timeout_s) + 15)
    if cp.returncode != 0:
        print(f'  capture error: {cp.stderr.decode()[:300]}', file=sys.stderr)
        return None
    raw = np.frombuffer(cp.stdout, dtype=np.float32)
    n = len(raw) // 2
    if n == 0:
        print('  capture returned 0 samples', file=sys.stderr)
        return None
    return (raw[:n*2:2] + 1j * raw[1:n*2:2]).astype(np.complex64)


def power_at_hz(iq: np.ndarray, f_hz: float, fs: float) -> float:
    n = len(iq)
    t = np.arange(n, dtype=np.float64)
    phasor = np.exp(-1j * 2.0 * np.pi * f_hz / fs * t).astype(np.complex64)
    return float(np.abs(np.dot(phasor, iq))**2 / n)


def fft_peak_hz(iq: np.ndarray, fs: float, f_min: float, f_max: float) -> float:
    n = len(iq)
    spec = np.abs(np.fft.fft(iq))**2
    freqs = np.fft.fftfreq(n, 1.0 / fs)
    mask = (freqs >= f_min) & (freqs <= f_max)
    idx_in_band = np.where(mask)[0]
    if len(idx_in_band) == 0:
        return float('nan')
    best = idx_in_band[np.argmax(spec[idx_in_band])]
    return float(freqs[best])


def run_test(label: str, iq_f32: np.ndarray, args,
             dji_template: bytes, rtsa_remote: str, mission_remote: str) -> dict:
    print(f'\n=== {label} ===', flush=True)

    if not args.skip_upload:
        rtsa_bytes = build_rtsa(iq_f32, args.center_freq, dji_template)
        with tempfile.NamedTemporaryFile(suffix='.rtsa', delete=False) as tf:
            tf.write(rtsa_bytes)
            local_rtsa = tf.name
        print(f'  Uploading {len(rtsa_bytes)//1024} KB...', flush=True)
        scp_to(args.aaronia_host, local_rtsa, rtsa_remote)
        os.unlink(local_rtsa)

        xml = mission_xml(rtsa_remote, int(args.center_freq), args.transattn)
        with tempfile.NamedTemporaryFile(mode='w', suffix='.rmix', delete=False) as tf:
            tf.write(xml)
            local_rmix = tf.name
        scp_to(args.aaronia_host, local_rmix, mission_remote)
        os.unlink(local_rmix)

        restart_rtsa(args.aaronia_host, mission_remote, wait_s=args.rtsa_wait)

    n_samps = int(args.capture_s * args.fobos_rate)
    print(f'  Capturing {n_samps} samples @ {args.fobos_rate/1e6:.1f} Msps...', flush=True)
    iq = capture_fobos(args.cap_bin, args.center_freq, args.fobos_rate, n_samps,
                       args.lna, args.vga, args.capture_s)
    if iq is None:
        return {}

    rf_offset = args.v6b_scale * args.iq_offset_hz
    p_neg = power_at_hz(iq, -rf_offset, args.fobos_rate)
    p_pos = power_at_hz(iq,  rf_offset, args.fobos_rate)
    p_neg_db = 10 * math.log10(max(p_neg, 1e-30))
    p_pos_db = 10 * math.log10(max(p_pos, 1e-30))
    ratio_db  = p_pos_db - p_neg_db

    # Find actual spectral peak near expected location (within ±500 kHz)
    peak_neg = fft_peak_hz(iq, args.fobos_rate, -rf_offset - 500e3, -rf_offset + 500e3)
    peak_pos = fft_peak_hz(iq, args.fobos_rate,  rf_offset - 500e3,  rf_offset + 500e3)

    print(f'  P(−{rf_offset/1e3:.0f} kHz) = {p_neg_db:.1f} dBfs  '
          f'[FFT peak near: {peak_neg/1e3:+.1f} kHz]')
    print(f'  P(+{rf_offset/1e3:.0f} kHz) = {p_pos_db:.1f} dBfs  '
          f'[FFT peak near: {peak_pos/1e3:+.1f} kHz]')
    print(f'  P(+) / P(−) = {ratio_db:+.1f} dB', flush=True)
    return {'p_neg_db': p_neg_db, 'p_pos_db': p_pos_db, 'ratio_db': ratio_db,
            'peak_neg_hz': peak_neg, 'peak_pos_hz': peak_pos}


def gen_cw_f32(freq_hz: float, n_samples: int) -> np.ndarray:
    t = np.arange(n_samples, dtype=np.float64) / AARONIA_RATE
    iq = (0.7 * np.exp(1j * 2.0 * np.pi * freq_hz * t)).astype(np.complex64)
    out = np.empty(n_samples * 2, dtype=np.float32)
    out[0::2] = iq.real
    out[1::2] = iq.imag
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--aaronia-host',   default='snedosiekov@10.200.0.125')
    ap.add_argument('--center-freq',    type=float, default=433_920_000)
    ap.add_argument('--dji-template',   default='/tmp/DJI-Mini4-10ms.rtsa')
    ap.add_argument('--remote-dir',     default='/home/snedosiekov/rtsa_capture')
    ap.add_argument('--transattn',      type=float, default=-50.0)
    ap.add_argument('--fobos-rate',     type=int,   default=10_000_000)
    ap.add_argument('--capture-s',      type=float, default=3.0)
    ap.add_argument('--lna',            type=int,   default=2)
    ap.add_argument('--vga',            type=int,   default=8)
    ap.add_argument('--rtsa-wait',      type=int,   default=18)
    ap.add_argument('--v6b-scale',      type=float, default=16.88,
                    help='V6B IQ→RF upconversion scale factor')
    ap.add_argument('--iq-offset-hz',   type=float, default=200_000,
                    help='IQ tone offset to test Hz (default 200000)')
    ap.add_argument('--cap-bin',        default=os.path.join(here, 'c', 'capture'))
    ap.add_argument('--skip-upload',    action='store_true',
                    help='Skip SSH/SCP/RTSA restart; capture only')
    args = ap.parse_args()

    dji_template = open(args.dji_template, 'rb').read()
    n_chunks = len(find_samp_positions(dji_template))
    print(f'DJI template: {len(dji_template)} bytes, {n_chunks} SAMP chunks')

    rtsa_remote    = f'{args.remote_dir}/diag_cw.rtsa'
    mission_remote = '/tmp/diag_cw.rmix'

    n_iq = SAMPLES_PER_SAMP * n_chunks

    rf_offset = args.v6b_scale * args.iq_offset_hz
    print(f'\nExpected RF offsets:')
    print(f'  +{args.iq_offset_hz/1e3:.0f} kHz IQ → −{rf_offset/1e3:.0f} kHz RF '
          f'({(args.center_freq - rf_offset)/1e6:.3f} MHz)')
    print(f'  −{args.iq_offset_hz/1e3:.0f} kHz IQ → +{rf_offset/1e3:.0f} kHz RF '
          f'({(args.center_freq + rf_offset)/1e6:.3f} MHz)')

    res_a = run_test(
        f'Test A: +{args.iq_offset_hz/1e3:.0f} kHz IQ (reference, expect RF at '
        f'−{rf_offset/1e3:.0f} kHz)',
        gen_cw_f32(+args.iq_offset_hz, n_iq),
        args, dji_template, rtsa_remote, mission_remote)

    res_b = run_test(
        f'Test B: −{args.iq_offset_hz/1e3:.0f} kHz IQ (test, expect RF at '
        f'+{rf_offset/1e3:.0f} kHz if V6B handles negative IQ)',
        gen_cw_f32(-args.iq_offset_hz, n_iq),
        args, dji_template, rtsa_remote, mission_remote)

    print('\n=== Diagnosis ===')
    if res_a:
        ra = res_a['ratio_db']
        if ra < -6:
            print(f'Test A: PASS  P(+)/P(−) = {ra:+.1f} dB  '
                  f'(−{rf_offset/1e3:.0f} kHz dominant, calibration OK)')
        else:
            print(f'Test A: WARN  P(+)/P(−) = {ra:+.1f} dB  (expected < −6 dB)')
    else:
        print('Test A: NO DATA')

    v6b_ok = False
    if res_b:
        rb = res_b['ratio_db']
        if rb > +6:
            v6b_ok = True
            print(f'Test B: PASS  V6B handles negative IQ  '
                  f'P(+)/P(−) = {rb:+.1f} dB ✓')
        else:
            print(f'Test B: FAIL  V6B does NOT handle negative IQ  '
                  f'P(+)/P(−) = {rb:+.1f} dB')
    else:
        print('Test B: NO DATA')

    print()
    if v6b_ok:
        print('RECOMMENDATION: V6B handles negative IQ.')
        print('  Use gen_fsk_py.py (conjugate mode) for FSK OTA TX.')
        print(f'  Demod tones: −{rf_offset/1e3:.0f} kHz and +{rf_offset/1e3:.0f} kHz in FobosOne baseband.')
        print(f'  Command:')
        print(f'    ./demod_fsk_file 2 K 8 data_syms 6.0 0.75 {args.fobos_rate}'
              f' {-int(rf_offset)} {int(rf_offset)} < capture.bin')
    else:
        rf2 = args.v6b_scale * 2 * args.iq_offset_hz
        print('RECOMMENDATION: V6B does NOT handle negative IQ.')
        print(f'  Use positive-only IQ tones:')
        print(f'    sym=0 at +{args.iq_offset_hz/1e3:.0f} kHz IQ  → RF at −{rf_offset/1e3:.0f} kHz')
        print(f'    sym=1 at +{2*args.iq_offset_hz/1e3:.0f} kHz IQ → RF at −{rf2/1e3:.0f} kHz')
        print()
        print(f'  TX (on Aaronia host, pipe to IQTransmitterStdin, or via gen_fsk_py.py --iq-hz):')
        print(f'    python3 gen_fsk_py.py 2 K N_BURSTS DATA_SYMS SEED'
              f' --fs {AARONIA_RATE:.1f}'
              f' --iq-hz {int(args.iq_offset_hz)} {int(2*args.iq_offset_hz)}')
        print()
        print(f'  Demod:')
        print(f'    ./demod_fsk_file 2 K 8 data_syms 6.0 0.75 {args.fobos_rate}'
              f' {-int(rf_offset)} {-int(rf2)} < capture.bin')


if __name__ == '__main__':
    main()
