# -*- coding: utf-8 -*-
"""The host binding, for the tools -- a re-export of the add-on's `osp.py`.

Until 2.0 this was a second copy, and the two had drifted: one grew a method
the other never got, and when the add-on's copy learned the Linux library
name and the `OSP_HOST_DLL` override, this one knew neither -- which is how
a render on the Linux box failed looking for a `.dll`.  `voices.py` exists
twice for the same historical reason and a test keeps those two identical;
here the simpler fix is to have one.

The add-on's module is loaded from its file and installed under this name,
so every probe's `import osp` keeps working and gets the real thing.  Its
library search already looks in the repository's `build/`.
"""
import importlib.util
import os
import sys

_ADDON = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "addon", "synthDrivers", "_outspoken", "osp.py")
_spec = importlib.util.spec_from_file_location(__name__, _ADDON)
_module = importlib.util.module_from_spec(_spec)
sys.modules[__name__] = _module
_spec.loader.exec_module(_module)
