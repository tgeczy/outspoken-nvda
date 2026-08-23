# -*- coding: utf-8 -*-
"""The half of machfs's dependency that this add-on does not use.

`machfs` imports `macresources` at module scope, and would not load without
it -- but every call is in a path that **writes** a volume or exports one to a
host directory: `Volume.write`, `read_folder`, `write_folder`, and the alias
handling that goes with them. This add-on only ever mounts an image the user
already has and reads two forks out of it.

Vendoring a second library for code that never runs would be a poor trade, and
so would deleting the lines: the next time machfs is updated, a diff against
upstream should show the import change and nothing else.

**A module actually named `macresources` was the wrong answer**, and the reason
is written down in [[module-names-must-not-shadow-nvda]]: everything in this
folder ends up on a `sys.path` shared with every other NVDA add-on, and a
generic name there is a collision waiting to happen. The one that already
happened cost two engines that were listed and silent. So this is private to
`_machfs`, reached by a relative import, and cannot be picked up by anyone.

Each name raises rather than returning something plausible. If a future change
does reach one of these, it should say so at once.
"""


def _unavailable(what):
    def fn(*args, **kwargs):
        raise NotImplementedError(
            "machfs.%s needs the macresources package, which this add-on does "
            "not vendor because it only reads volumes.  See "
            "_machfs/_macresources.py." % what)
    return fn


make_rez_code = _unavailable("make_rez_code")
parse_rez_code = _unavailable("parse_rez_code")
make_file = _unavailable("make_file")


def parse_file(fork):
    """The one that is genuinely reached while reading. -> []

    **The raising version above found this, which is the argument for writing
    stubs that raise.**  Every other call really is on a write path; this one
    is not.  `Volume.read` ends by calling `_link_aliases`, which parses each
    object's resource fork looking for an `alis` resource so that a Finder
    alias resolves to what it points at.

    That whole block is wrapped in `except (AttributeError, KeyError,
    StopIteration, ValueError): pass`, so an empty list is a supported answer
    and means only that aliases are left as they are.  Which suits this add-on:
    it is looking for `Extensions/MacinTalk 2` and its neighbours, and an alias
    to one of those has no resources worth taking anyway.

    A stub that returned something plausible instead of raising would have hid
    this behind a wrong answer somewhere further down.
    """
    return []


class Resource(object):
    def __init__(self, *args, **kwargs):
        _unavailable("Resource")()
