#!/usr/bin/env python3
"""Insertion loss of the Mini-Circuits CBL-1M-SMSM+ test cable vs frequency.

Typical-performance data straight from the official datasheet (REV. C, page 3):
  https://www.minicircuits.com/pdfs/CBL-1M-SMSM+.pdf
Loss follows ~sqrt(f) (skin effect), so we interpolate the datasheet points in
sqrt(frequency) space for an accurate value at any frequency in band.

Use: expected power at the Fobos RX port = Aaronia TX power - cable_loss_db(f).
"""
import numpy as np

# (frequency MHz, typical insertion loss dB) from the datasheet table
_F_MHZ = np.array([0.30, 100, 1000, 2000, 2500, 3000, 4000, 5000, 6000,
                   8000, 10000, 12000, 14000, 16000, 18000])
_IL_DB = np.array([0.00, 0.12, 0.41, 0.59, 0.67, 0.74, 0.87, 0.99, 1.10,
                   1.31, 1.51, 1.68, 1.82, 1.99, 2.09])
_SQRT_F = np.sqrt(_F_MHZ)


def cable_loss_db(freq_hz):
    """Insertion loss (dB) at freq_hz (scalar or array), sqrt-f interpolated."""
    f_mhz = np.asarray(freq_hz, dtype=float) / 1e6
    return np.interp(np.sqrt(np.clip(f_mhz, _F_MHZ[0], _F_MHZ[-1])), _SQRT_F, _IL_DB)


def datasheet_points():
    """Return the raw datasheet (freq_hz, loss_db) arrays (for storing in the cal file)."""
    return (_F_MHZ * 1e6).tolist(), _IL_DB.tolist()


if __name__ == "__main__":
    print("CBL-1M-SMSM+ insertion loss (sqrt-f interpolation):")
    for f in [70e6, 100e6, 433e6, 1e9, 2.4e9, 3e9, 5e9, 5.8e9, 6e9]:
        print(f"  {f/1e6:7.1f} MHz : {cable_loss_db(f):.3f} dB")
