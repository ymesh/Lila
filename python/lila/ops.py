"""Every Lila command as a function: lila.ops.name(*objects, flag=value).

Flags take long or short names. Generated from the live command registry, so extension commands appear
here too after refresh().
"""

import _lila

from ._stubgen import command_doc

__all__ = []


def _make(spec):
    name = spec["name"]

    def command(*args, **flags):
        call = dict(flags)
        if args:
            call["args"] = list(args)
        return _lila.execute(name, call)

    command.__name__ = name
    command.__qualname__ = name
    command.__doc__ = command_doc(spec)
    return command


def refresh():
    """Rebuild from the registry. Called at startup and when extensions load."""
    g = globals()
    for name in list(__all__):
        g.pop(name, None)
    __all__.clear()
    for spec in _lila.commands():
        g[spec["name"]] = _make(spec)
        __all__.append(spec["name"])
    try:
        from . import _startup

        _startup.expose_commands()
    except ImportError:
        pass
    try:
        from . import _stubgen

        _stubgen.write_extensions()
    except Exception:
        pass


def __getattr__(name):
    """A command registered after the last refresh (the UI registers some after Python starts): picks it up on first use."""
    if not name.startswith("_"):
        for spec in _lila.commands():
            if spec["name"] == name:
                command = _make(spec)
                globals()[name] = command
                __all__.append(name)
                return command
    raise AttributeError(f"module 'lila.ops' has no command {name!r}")


def __dir__():
    return sorted(set(globals()) | {spec["name"] for spec in _lila.commands()})


refresh()
