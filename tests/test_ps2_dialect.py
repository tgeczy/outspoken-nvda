# -*- coding: utf-8 -*-
"""The two PowerShell scripts stay inside PowerShell 2.0's dialect.

`register.ps1` and `settings.ps1` say in their own headers why: stock
Windows 7 has no newer engine, this community runs Windows 7, and the
scripts run from the installer, where "update PowerShell first" is not an
answer.

The dialect cannot be proven on this machine -- there is no PowerShell 2.0
here to run them under, only 5.1, which accepts everything -- so the
constructs that are known to be missing are banned mechanically instead.
Every one of these was reached for while porting Panthera's (5.1-dialect)
settings tool, which is exactly how a later session would break Windows 7
without noticing: the port works perfectly on every machine that can test
it.
"""
import os
import re

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAPI = os.path.join(os.path.dirname(_HERE), "sapi")

SCRIPTS = ("register.ps1", "settings.ps1")

#: Construct -> the 2.0-dialect replacement, as used in these files already.
BANNED = (
    (r"OpenBaseKey", "the Wow6432Node path written directly"),
    (r"\[pscustomobject\]", "a plain hashtable or parallel arrays"),
    (r"\[ordered\]", "parallel arrays"),
    (r"\$PSScriptRoot", "Split-Path -Parent $MyInvocation.MyCommand.Path"),
    (r"(?<![\w-])-(?:not)?in\b", "-contains, reversed"),
    (r"::new\(", "New-Object"),
    (r"Get-ChildItem[^\n]*-Depth", "a narrowed path, then -Recurse"),
    (r"-shr\b|-shl\b", "arithmetic"),
)


@pytest.mark.parametrize("script", SCRIPTS)
@pytest.mark.parametrize("pattern,instead", BANNED)
def test_no_post_20_construct(script, pattern, instead):
    with open(os.path.join(_SAPI, script), encoding="utf-8-sig") as f:
        text = f.read()
    hits = [
        "%s:%d: %s" % (script, number, line.strip())
        for number, line in enumerate(text.splitlines(), 1)
        # The headers *document* these bans, which is not the same as using
        # them; only code lines count.
        if not line.lstrip().startswith("#")
        and re.search(pattern, line, re.IGNORECASE)
    ]
    assert not hits, (
        "PowerShell 2.0 (stock Windows 7) does not have this; use %s "
        "instead:\n%s" % (instead, "\n".join(hits)))
