#!/usr/bin/env python3
"""
REST API test suite for the Fobos web spectrum analyzer.

Scenarios:
  1. start/stop cycle × 3  (must complete each cycle ≤ 1 s)
  2. start → freq +1 MHz → freq −2 MHz → verify each → stop
  3. start → cycle LNA 0‥3 × VGA 0‥31 → monitor signal level → stop

Usage:
  python3 test_api.py [--base http://HOST:PORT] [--freq BASE_MHZ]
"""
import argparse, sys, time, json
import urllib.request, urllib.error

# ─────────────────────────────────────────────────────────────────────────────

def http(method, url, body=None, timeout=10):
    data = json.dumps(body).encode() if body is not None else None
    headers = {'Content-Type': 'application/json'} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        t0 = time.monotonic()
        with urllib.request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
            elapsed = time.monotonic() - t0
            return resp, elapsed, r.status
    except urllib.error.HTTPError as e:
        body_txt = e.read().decode(errors='replace')
        elapsed  = time.monotonic() - t0
        try:
            resp = json.loads(body_txt)
        except Exception:
            resp = {'ok': False, 'msg': body_txt}
        return resp, elapsed, e.code

def get(base, path, params='', timeout=10):
    url = f'{base}{path}{"?" + params if params else ""}'
    return http('GET', url, timeout=timeout)

def post(base, path, body=None, timeout=10):
    url = f'{base}{path}'
    return http('POST', url, body=body, timeout=timeout)

# ─────────────────────────────────────────────────────────────────────────────

PASS = '\033[32mPASS\033[0m'
FAIL = '\033[31mFAIL\033[0m'
INFO = '\033[36mINFO\033[0m'

def ok(label, cond, detail=''):
    tag = PASS if cond else FAIL
    print(f'  [{tag}] {label}' + (f' — {detail}' if detail else ''))
    return cond

def info(msg):
    print(f'  [{INFO}] {msg}')

# ─────────────────────────────────────────────────────────────────────────────

def wait_spectrum(base, after_ts, timeout_s=5.0, center_hz=None, tol_hz=5e3):
    """Block until a spectrum newer than after_ts arrives (and optionally matches center_hz).

    If center_hz is given, keeps consuming spectrums until one with center≈center_hz arrives
    (or timeout). Use this to confirm a freq change actually propagated through.
    """
    deadline = time.monotonic() + timeout_s
    cur_after = after_ts
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None, timeout_s
        params = f'wait=true&timeout={min(remaining, 2.0)}&after={cur_after}'
        resp, elapsed, status = get(base, '/api/spectrum', params, timeout=min(remaining, 2.0) + 2)
        if status != 200 or not resp.get('ok'):
            time.sleep(0.05)
            continue
        if center_hz is None or abs(resp['center'] - center_hz) <= tol_hz:
            return resp, time.monotonic() - (deadline - timeout_s)
        # spectrum arrived but for wrong freq — consume it and wait for next
        cur_after = resp['ts']

def attach(base):
    resp, elapsed, status = post(base, '/api/attach', timeout=10)
    ok('attach', status == 200 and resp.get('ok'), f'{elapsed*1000:.0f} ms')
    return resp.get('ok', False)

def detach(base):
    resp, elapsed, status = post(base, '/api/detach', timeout=10)
    ok('detach', status == 200 and resp.get('ok'), f'{elapsed*1000:.0f} ms')

def start(base, **kw):
    resp, elapsed, status = post(base, '/api/start', body=kw, timeout=10)
    ok(f'start({kw})', status == 200 and resp.get('ok'), f'{elapsed*1000:.0f} ms')
    return resp.get('ok', False)

def stop_wait(base):
    resp, elapsed, status = post(base, '/api/stop?wait=true', timeout=10)
    ok(f'stop(wait)', status == 200, f'{elapsed*1000:.0f} ms')
    return elapsed

def set_params(base, **kw):
    resp, elapsed, status = post(base, '/api/params', body=kw, timeout=5)
    ok(f'params({kw})', status == 200 and resp.get('ok'), f'{elapsed*1000:.0f} ms')
    return resp

def status(base):
    resp, elapsed, status_code = get(base, '/api/status')
    return resp

def print_log_tail(base, n=20):
    resp, _, _ = get(base, '/api/log', f'n={n}')
    entries = resp.get('entries', [])
    if entries:
        print(f'\n  --- last {len(entries)} log lines ---')
        for e in entries:
            ts = time.strftime('%H:%M:%S', time.localtime(e['ts']))
            ms = int((e['ts'] % 1) * 1000)
            print(f'  {ts}.{ms:03d} {e["level"]:5s} {e["msg"]}')
        print()

# ─────────────────────────────────────────────────────────────────────────────

def scenario_1_start_stop_cycles(base, base_freq_hz):
    print('\n=== Scenario 1: start/stop cycles ×3 (target ≤ 1 s each) ===')
    all_ok = True
    for i in range(3):
        t0 = time.monotonic()
        r1 = post(base, '/api/start', body={'mode': 'fft', 'freq': base_freq_hz}, timeout=10)
        t1 = time.monotonic()
        r2 = post(base, '/api/stop?wait=true', timeout=10)
        t2 = time.monotonic()
        cycle_ms = (t2 - t0) * 1000
        start_ms = (t1 - t0) * 1000
        stop_ms  = (t2 - t1) * 1000
        passed   = cycle_ms <= 1000
        all_ok   = all_ok and passed
        ok(f'cycle {i+1}: total={cycle_ms:.0f}ms (start={start_ms:.0f}ms stop={stop_ms:.0f}ms)',
           passed)
    return all_ok

def scenario_2_freq_change(base, base_freq_hz):
    print('\n=== Scenario 2: start → freq+1MHz → freq−2MHz → verify each → stop ===')
    all_ok = True
    freq_a = base_freq_hz
    freq_b = base_freq_hz + 1e6
    freq_c = base_freq_hz - 1e6   # = base_freq_hz + 1e6 - 2e6

    all_ok &= start(base, mode='fft', freq=freq_a, accum_n=2)

    # Wait for first spectrum (baseline)
    info(f'waiting for baseline spectrum at {freq_a/1e6:.3f} MHz…')
    spec, _ = wait_spectrum(base, after_ts=0, timeout_s=8)
    all_ok &= ok('baseline spectrum received', spec is not None,
                 f'center={spec["center"]/1e6:.3f} MHz' if spec else 'timeout')

    ts_before = spec['ts'] if spec else 0

    # Change to freq_b — loop until we see a spectrum centered at freq_b
    info(f'changing to {freq_b/1e6:.3f} MHz')
    t0 = time.monotonic()
    set_params(base, freq=freq_b)
    spec_b, _ = wait_spectrum(base, after_ts=ts_before, timeout_s=8, center_hz=freq_b)
    change_ms = (time.monotonic() - t0) * 1000
    passed_b  = spec_b is not None
    all_ok   &= ok(f'freq→{freq_b/1e6:.3f}MHz confirmed in {change_ms:.0f}ms',
                   passed_b,
                   f'center={spec_b["center"]/1e6:.3f}MHz' if spec_b else 'timeout')

    ts_before = spec_b['ts'] if spec_b else ts_before

    # Change to freq_c — loop until we see a spectrum centered at freq_c
    info(f'changing to {freq_c/1e6:.3f} MHz')
    t0 = time.monotonic()
    set_params(base, freq=freq_c)
    spec_c, _ = wait_spectrum(base, after_ts=ts_before, timeout_s=8, center_hz=freq_c)
    change_ms = (time.monotonic() - t0) * 1000
    passed_c  = spec_c is not None
    all_ok   &= ok(f'freq→{freq_c/1e6:.3f}MHz confirmed in {change_ms:.0f}ms',
                   passed_c,
                   f'center={spec_c["center"]/1e6:.3f}MHz' if spec_c else 'timeout')

    stop_wait(base)
    return all_ok

# Gain grid, per fobos_sdr.h: LNA 0..3, VGA 0..31. The VGA is sampled (not swept
# exhaustively) to keep the scenario's runtime reasonable.
LNA_STEPS = [0, 1, 2, 3]
VGA_STEPS = [0, 8, 16, 24, 31]

def scenario_3_gain_sweep(base, base_freq_hz):
    print('\n=== Scenario 3: start → cycle LNA 0‥3 × VGA 0‥31 → signal level → stop ===')
    all_ok = True

    all_ok &= start(base, mode='fft', freq=base_freq_hz, accum_n=2)

    # Wait for initial spectrum
    spec0, _ = wait_spectrum(base, after_ts=0, timeout_s=8)
    all_ok  &= ok('initial spectrum', spec0 is not None)

    results = {}  # (lna, vga) → peak_db
    prev_ts = spec0['ts'] if spec0 else 0

    for lna in LNA_STEPS:
        for vga in VGA_STEPS:
            t0 = time.monotonic()
            set_params(base, lna=lna, vga=vga)
            # Skip the first spectrum (may be from before the restart completes)
            spec1, _ = wait_spectrum(base, after_ts=prev_ts, timeout_s=3)
            if spec1 is None:
                ok(f'LNA={lna} VGA={vga}: got spectrum', False, 'timeout on first')
                all_ok = False
                break
            # Get second spectrum (guaranteed post-restart)
            spec2, _ = wait_spectrum(base, after_ts=spec1['ts'], timeout_s=3)
            spec = spec2 if spec2 else spec1
            elapsed = (time.monotonic() - t0) * 1000
            if spec:
                results[(lna, vga)] = spec['peak_db']
                prev_ts = spec['ts']
                info(f'LNA={lna} VGA={vga:2d}: peak_db={spec["peak_db"]:+.1f} avg_db={spec["avg_db"]:+.1f}  ({elapsed:.0f}ms)')
            else:
                ok(f'LNA={lna} VGA={vga}: got spectrum', False, 'timeout')
                all_ok = False
                break

    # Verify that max gain > min gain (more gain → higher signal floor)
    hi_combo = (LNA_STEPS[-1], VGA_STEPS[-1])
    if (0, 0) in results and hi_combo in results:
        low  = results[(0, 0)]
        high = results[hi_combo]
        margin = high - low
        passed = margin > 3   # at least 3 dB difference expected
        all_ok &= ok(f'gain range: LNA={hi_combo[0]},VGA={hi_combo[1]} ({high:+.1f} dB) > '
                     f'LNA=0,VGA=0 ({low:+.1f} dB)',
                     passed, f'margin={margin:.1f} dB')
        if not passed:
            info('WARNING: less than 3 dB gain range — check antenna/signal source')

    # Verify monotonic trend: increasing VGA should increase signal for fixed LNA
    for lna in LNA_STEPS:
        vals = [results.get((lna, v)) for v in VGA_STEPS]
        if all(v is not None for v in vals):
            monotonic = all(vals[i] <= vals[i+1] + 1.0 for i in range(len(vals)-1))
            ok(f'VGA sweep monotonic for LNA={lna}', monotonic,
               ' '.join(f'{v:+.0f}' for v in vals))

    stop_wait(base)
    return all_ok

# ─────────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--base', default='http://localhost:8888',
                    help='Base URL of the web spectrum app (default: http://localhost:8888)')
    ap.add_argument('--freq', type=float, default=433.92,
                    help='Base frequency in MHz (default: 433.92)')
    ap.add_argument('--scenario', type=int, default=0,
                    help='Run only this scenario number (1/2/3), 0=all (default)')
    args = ap.parse_args()

    base      = args.base.rstrip('/')
    base_freq = args.freq * 1e6

    print(f'Target: {base}   base_freq={args.freq} MHz')

    # Check connectivity
    resp, elapsed, code = get(base, '/api/status', timeout=3)
    if code != 200:
        print(f'ERROR: cannot reach {base}/api/status  (HTTP {code})')
        sys.exit(1)
    info(f'status: {resp}')

    # Ensure device is attached
    st = status(base)
    if not st.get('attached'):
        if not attach(base):
            print('ERROR: failed to attach device')
            sys.exit(1)
    else:
        info('device already attached')

    # Ensure stopped before starting tests
    if st.get('running'):
        info('stopping running worker before tests…')
        stop_wait(base)

    results = {}
    try:
        if args.scenario in (0, 1):
            results[1] = scenario_1_start_stop_cycles(base, base_freq)
        if args.scenario in (0, 2):
            results[2] = scenario_2_freq_change(base, base_freq)
        if args.scenario in (0, 3):
            results[3] = scenario_3_gain_sweep(base, base_freq)
    finally:
        # Always stop cleanly
        st = status(base)
        if st.get('running'):
            info('stopping worker after tests…')
            stop_wait(base)
        # Print log tail
        print_log_tail(base, n=40)

    print('\n=== Summary ===')
    all_pass = True
    for s, r in sorted(results.items()):
        tag = PASS if r else FAIL
        print(f'  Scenario {s}: [{tag}]')
        all_pass = all_pass and r
    sys.exit(0 if all_pass else 1)

if __name__ == '__main__':
    main()
