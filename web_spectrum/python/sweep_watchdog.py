#!/usr/bin/env python3
"""Watchdog for the full LNA x VGA gain sweep (gain_sweep_browser.py).

Runs autonomously and self-heals the three failure modes we have actually hit:
  1. sweep process died          -> (re)start it (it resumes from disk)
  2. sweep alive but STALLED      -> no new PNG for STALL_S (e.g. dead headless
     browser hanging the script)  -> kill stuck sweep+chromium, restart
  3. web service down / unresponsive -> systemctl restart, then ensure sweep runs

Because gain_sweep_browser.py resumes (skips combos whose PNG already exists),
restarting is always safe and idempotent. Exits 0 once all N_COMBOS PNGs + the
PDF are present. Logs every decision to /tmp/sweep_watchdog.log.

Usage: sweep_watchdog.py    (run in background; supervises an already-running or
                             not-yet-started sweep)
"""
import time, os, glob, subprocess, json, urllib.request

OUT        = "/home/pi5/dev/fobosone/reports/sweep"
SCRIPT_DIR = "/home/pi5/dev/fobosone/examples/web_spectrum"
WEB        = "http://localhost:8080"
LOG        = "/tmp/sweep_watchdog.log"
CHECK_S    = 60                      # poll cadence
STALL_S    = 26 * 60                 # >3 full combo retries (~24 min) => genuinely hung
WEB_SVC    = "fobos-web-spectrum.service"

# Gain grid, per fobos_sdr.h: LNA 0..3, VGA 0..31.
N_LNA, N_VGA = 4, 32
N_COMBOS     = N_LNA * N_VGA
ALL        = [(l, v) for l in range(N_LNA) for v in range(N_VGA)]


def log(m):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {m}"
    print(line, flush=True)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    except Exception:
        pass


def done_count():
    return sum(1 for l, v in ALL if glob.glob(f"{OUT}/sweep_LNA_{l}_VGA_{v}_*.png"))


def newest_png_mtime():
    fs = glob.glob(f"{OUT}/sweep_LNA_*_VGA_*.png")
    return max((os.path.getmtime(f) for f in fs), default=0.0)


def sweep_alive():
    try:
        out = subprocess.check_output(["pgrep", "-f", "gain_sweep_browser.py"],
                                      stderr=subprocess.DEVNULL).strip()
        return bool(out)
    except subprocess.CalledProcessError:
        return False


def web_ok():
    try:
        urllib.request.urlopen(WEB + "/api/status", timeout=5).read()
        return True
    except Exception:
        return False


def pdf_current():
    """A PDF that is at least as new as the last screenshot (so it includes the
    final combo). A stale PDF from a prior run does not count as complete."""
    pdfs = glob.glob(f"{OUT}/gain_sweep_*.pdf")
    if not pdfs:
        return False
    return max(os.path.getmtime(p) for p in pdfs) >= newest_png_mtime()


def kill_sweep_and_browser():
    for pat in ("gain_sweep_browser.py", "headless_shell", "chromium"):
        subprocess.run(["pkill", "-9", "-f", pat], stderr=subprocess.DEVNULL)
    time.sleep(2)


def restart_web():
    log("ACTION: restarting web service")
    subprocess.run(["sudo", "systemctl", "restart", WEB_SVC], stderr=subprocess.DEVNULL)
    for _ in range(15):
        time.sleep(2)
        if web_ok():
            log("web service back up")
            return True
    log("WARN: web service still not responding after restart")
    return False


def start_sweep():
    log("ACTION: starting gain_sweep_browser.py (resumes from disk)")
    out = open("/tmp/gain_browser_stdout.log", "a")
    subprocess.Popen(["python3", "gain_sweep_browser.py"], cwd=SCRIPT_DIR,
                     stdout=out, stderr=subprocess.STDOUT)
    time.sleep(5)


def main():
    log(f"watchdog start: {done_count()}/{N_COMBOS} done, sweep_alive={sweep_alive()}, "
        f"web_ok={web_ok()}, stall_threshold={STALL_S//60}min")
    last_progress = time.time()      # give the running sweep a full STALL window from now
    last_mtime = newest_png_mtime()
    restarts = 0
    while True:
        try:
            d = done_count()
            if d >= len(ALL) and pdf_current():
                log(f"COMPLETE: {d}/{N_COMBOS} + fresh PDF present. watchdog exiting.")
                return
            if d >= len(ALL) and not pdf_current():
                # all combos done but no PDF -> sweep likely crashed before build_pdf
                if not sweep_alive():
                    log(f"all {N_COMBOS} done but no PDF; restarting to build PDF")
                    start_sweep()
                time.sleep(CHECK_S); continue

            m = newest_png_mtime()
            if m > last_mtime:
                last_mtime = m
                last_progress = time.time()
                log(f"progress: {d}/{N_COMBOS} done")

            if not web_ok():
                log("DETECT: web service down/unresponsive")
                restart_web()
                # web restart drops device; ensure a fresh sweep re-attaches
                if sweep_alive():
                    kill_sweep_and_browser()
                start_sweep()
                last_progress = time.time()
                restarts += 1
            elif not sweep_alive():
                log(f"DETECT: sweep process not running ({d}/{N_COMBOS} done)")
                kill_sweep_and_browser()
                start_sweep()
                last_progress = time.time()
                restarts += 1
            elif time.time() - last_progress > STALL_S:
                log(f"DETECT: STALLED — no new PNG in {int((time.time()-last_progress)/60)}min "
                    f"({d}/{N_COMBOS} done). Killing + restarting.")
                kill_sweep_and_browser()
                if not web_ok():
                    restart_web()
                start_sweep()
                last_progress = time.time()
                restarts += 1
        except Exception as e:
            log(f"watchdog loop error (continuing): {str(e)[:120]}")
        time.sleep(CHECK_S)


if __name__ == "__main__":
    main()
