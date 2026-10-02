#!/usr/bin/env python3
"""Autonomously restart RTSA on the Aaronia host with the File Source pointed at
test.iq (the swap file). Kill RTSA first so it can't overwrite the mission on exit,
rewrite the File Source filename in the .rmix, relaunch, connect V6B, start playback.

Usage: restart_rtsa.py            # just restart with current test.iq
The signal is swapped by overwriting ~/Aaronia/test.iq (+ test.iq.xml) beforehand.
"""
import sys, time, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'fsk_loopback'))
import ota_sweep as fk

# RTSA may reopen either of these on launch; point both File Sources at test.iq.
MISSIONS = ["C:/Users/vertical/Aaronia/missions/file_Reader_tx.rmix",
            "C:/Users/vertical/Aaronia/missions/file_Reader_tx_wo_iq_mod.rmix"]

def restart():
    print("kill RTSA ...");  print(fk.ssh('Stop-Process -Name "*Aaronia*" -Force -EA SilentlyContinue; Start-Sleep 5; echo killed', t=40).stdout.strip())
    # point every File Source -> test.iq (RTSA down, edit is safe from overwrite)
    for MP in MISSIONS:
        ps = ("$p='%s'; if(Test-Path $p){ (Get-Content $p -Raw) "
              "-replace 'Val=\"C:/Users/vertical/Aaronia/[^\"]*\\.iq\"','Val=\"C:/Users/vertical/Aaronia/test.iq\"' "
              "-replace 'RVal=\"\\.\\./\\.\\./[^\"]*\\.iq\"','RVal=\"../../test.iq\"' | Set-Content $p -NoNewline; "
              "(Select-String -Path $p -Pattern 'Name=\"filename\"').Line }" ) % MP
        r = fk.ssh(ps, t=40); print(MP.split('/')[-1], "->", (r.stdout.strip()[-90:] or r.stderr.strip()[:90]))
    print("launch RTSA ...");  print(fk.ssh('schtasks /run /tn AaroniaGUI', t=30).stdout.strip())
    print("wait + connect V6B + play ...")
    r = fk.ssh(f"Start-Sleep 45; [IO.File]::WriteAllBytes($env:TEMP+'\\v6.json',[Convert]::FromBase64String('{fk.PUT_V6}')); "
               f"curl.exe -s --max-time 8 -X PUT --data ('@'+$env:TEMP+'\\v6.json') http://localhost:54666/remoteconfig | Out-Null; "
               f"Start-Sleep 13; [IO.File]::WriteAllBytes($env:TEMP+'\\pb.json',[Convert]::FromBase64String('{fk.PUT_PB}')); "
               f"curl.exe -s --max-time 6 -X PUT --data ('@'+$env:TEMP+'\\pb.json') http://localhost:54666/remoteconfig | Out-Null; "
               f"Start-Sleep 3; echo started", t=100)
    print(r.stdout.strip() or r.stderr.strip())

if __name__ == '__main__':
    restart()
