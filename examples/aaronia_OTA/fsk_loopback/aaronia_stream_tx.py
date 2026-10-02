#!/usr/bin/env python3
"""
aaronia_stream_tx.py — Stream IQ data to Aaronia RTSA-Suite for OTA TX.

Format discovered empirically from GET /stream on port 54666:
  Content-Type: application/json-seq
  Transfer-Encoding: chunked
  Each chunk: JSON object with "samples" as CSV float array (I0,Q0,I1,Q1,...)

POST /sample on port 54664 uses the same JSON structure (without chunked encoding
overhead — the POST body is a single JSON object).
"""

import json
import math
import socket
import time
import urllib.request
import base64
import numpy as np

HOST = '10.200.0.125'
PORT = 54664
AUTH = base64.b64encode(b'a:').decode()

# Aaronia native IQ sample rates (Hz)
AARONIA_RATES = [
    5_714_285.7143,
    11_428_571.429,
    12_571_428.571,
    17_142_857.143,
    22_857_142.857,
    25_142_857.143,
]
AARONIA_DEFAULT_RATE = AARONIA_RATES[0]  # 5.714 Msps


def make_iq_record(samples_iq, center_hz, sample_rate, start_time=None):
    """
    Build the JSON record for POST /sample.

    samples_iq: numpy complex array or flat float32 [I0,Q0,I1,Q1,...]
    center_hz:  RF center frequency Hz
    sample_rate: IQ sample rate Hz
    start_time: POSIX timestamp (default: now)
    """
    if start_time is None:
        start_time = time.time()

    if np.iscomplexobj(samples_iq):
        flat = np.empty(len(samples_iq) * 2, dtype=np.float32)
        flat[0::2] = samples_iq.real
        flat[1::2] = samples_iq.imag
    else:
        flat = np.asarray(samples_iq, dtype=np.float32)

    n = len(flat) // 2
    dt = n / sample_rate
    bw = sample_rate
    half_bw = bw / 2

    record = {
        "startTime": start_time,
        "endTime":   start_time + dt,
        "startFrequency": int(center_hz - half_bw),
        "endFrequency":   int(center_hz + half_bw),
        "sampleFrequency": sample_rate,
        "sampleSize": 2,
        "sampleDepth": n,
        "payload": "iq",
        "unit": "volt",
        "samples": flat.tolist()
    }
    return json.dumps(record)


def post_sample(record_json, timeout=5):
    """POST one JSON record to port 54664 /sample."""
    body = record_json.encode()
    req = urllib.request.Request(
        f'http://{HOST}:{PORT}/sample',
        data=body,
        method='POST',
        headers={
            'Authorization': f'Basic {AUTH}',
            'Content-Type': 'application/json',
            'Content-Length': str(len(body)),
        }
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except Exception as e:
        return None, str(e)


def stream_iq_chunked(iq_data, center_hz, sample_rate, chunk_size=32768):
    """
    Stream a long IQ array to Aaronia via persistent chunked POST.

    iq_data: flat float32 array [I0,Q0,I1,Q1,...] at sample_rate sps
    center_hz: RF center frequency Hz
    sample_rate: IQ sample rate Hz
    chunk_size: IQ samples per POST chunk
    """
    n_total = len(iq_data) // 2
    pos = 0
    t = time.time()
    chunk_idx = 0

    while pos < n_total:
        end = min(pos + chunk_size, n_total)
        chunk = iq_data[pos*2 : end*2]
        rec = make_iq_record(chunk, center_hz, sample_rate, start_time=t)
        status, resp = post_sample(rec)
        if status is None:
            print(f'  Chunk {chunk_idx}: ERROR {resp}')
        else:
            print(f'  Chunk {chunk_idx}: {status} ({end-pos} samples)')
        t += (end - pos) / sample_rate
        pos = end
        chunk_idx += 1


def gen_cw_iq(freq_offset_hz, n_samples, sample_rate):
    """Generate a CW tone at freq_offset_hz relative to center."""
    t = np.arange(n_samples) / sample_rate
    sig = np.exp(1j * 2 * np.pi * freq_offset_hz * t).astype(np.complex64)
    return sig


if __name__ == '__main__':
    import sys

    center_hz = float(sys.argv[1]) if len(sys.argv) > 1 else 433_920_000
    rate = AARONIA_DEFAULT_RATE
    n = 57143  # ~10 ms at 5.714 Msps

    print(f'Sending CW tone: center={center_hz/1e6:.3f} MHz, rate={rate/1e6:.3f} Msps, {n} samples')

    iq = gen_cw_iq(freq_offset_hz=0, n_samples=n, sample_rate=rate)
    flat = np.empty(n * 2, dtype=np.float32)
    flat[0::2] = iq.real * 0.1
    flat[1::2] = iq.imag * 0.1

    status, resp = post_sample(make_iq_record(flat, center_hz, rate))
    print(f'POST /sample: status={status}, resp={resp[:200]}')
