# -*- coding: utf-8 -*-
"""A print() can no longer corrupt the SAPI protocol.

Every byte the serve script writes on stdout is protocol: frame counts and
PCM.  Panthera's sibling learned what one stray line of chatter in that
stream does -- the engine on the other side reads text as a frame count,
asks for billions of frames, and the client application dies of the failed
allocation.  outSPOKEN's serve runs a whole Python driver, where a print()
is one contributor-moment away, so the defense is structural: serve mode
takes a private duplicate of the pipe and rebinds stdout to stderr before
the first request is read.

This test states the contract: after `_claim_stdout`, a print(), a
sys.stdout.write and a raw os.write to fd 1 all land on stderr, and the
claimed handle alone reaches the real stdout pipe.  The parity test in
test_sapi_serve.py then proves the whole serve loop still runs over the
claimed handle, byte-identical to the driver.
"""
import os
import subprocess
import sys

SAPI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "sapi")

CHILD = r"""
import os, sys
sys.path.insert(0, %r)
import osp_serve
proto = osp_serve._claim_stdout()
print("a print after the claim")
sys.stdout.write("a sys.stdout.write after the claim\n")
sys.stdout.flush()
os.write(1, b"a raw fd-1 write after the claim\n")
proto.write(b"PROTOCOL-BYTES-ONLY")
proto.flush()
"""


def test_the_claimed_stdout_carries_only_protocol():
    done = subprocess.run(
        [sys.executable, "-c", CHILD % SAPI],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    assert done.returncode == 0, done.stderr
    assert done.stdout == b"PROTOCOL-BYTES-ONLY", done.stdout


def test_the_chatter_goes_to_stderr_instead_of_vanishing():
    """Diverted, not discarded: with diagnostics on, the SAPI engine sends
    the serve's stderr to a log file, and chatter that still exists is
    chatter that can convict."""
    done = subprocess.run(
        [sys.executable, "-c", CHILD % SAPI],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    assert b"a print after the claim" in done.stderr
    assert b"a raw fd-1 write after the claim" in done.stderr
