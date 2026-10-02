#!/usr/bin/env python3
"""Compile a .py to a specific .pyc path (used by CMake to produce app.pyc)."""
import sys, py_compile
py_compile.compile(sys.argv[1], cfile=sys.argv[2], optimize=2, doraise=True)
