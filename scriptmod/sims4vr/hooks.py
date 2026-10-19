"""One shared place that wraps the game's Python functions.

Every script module that changes how a game function behaves registers its
wrapper here instead of assigning over the function itself. Each game function
is replaced once, by a chain of the registered wrappers, so two modules wrapping
the same function no longer depend on who happened to import first.

    hooks.hook('follow', InteractionQueue, 'append', append, first_person_only=False)

    def append(original, self, interaction, *args, **kwargs):
        ...
        return original(self, interaction, *args, **kwargs)

* tag: the module registering (log lines, ini switch).
* target: the class or module that owns the function; name: its attribute.
* wrapper(original, *the game's arguments): call original(...) to continue down
  the chain (the next wrapper, then the game's own function); not calling it
  replaces the game's behaviour. Generator functions work the same way
  (yield from original(...)).
* first_person_only=True: the wrapper is skipped (straight to the next one)
  unless you are in first person (body.following()). Use it only when the
  wrapper already does nothing outside first person; the first-person-only
  rule means nothing first-person-specific may touch god view.
* order: wrappers run outermost = highest order; equal order = the later
  registration is outer (what stacked assignments did before this module).
* catch=True (default, not for generator wrappers): an exception raised by the
  wrapper's own code (not by the game's function below it) is logged once per
  wrapper and the game's function runs as if the wrapper was absent.

Switches (sims4vr.ini [hooks]): disable = a comma list of tags, "Class.name"
labels or "tag:Class.name"; matching wrappers are skipped, and a target with
no wrapper left is given back to the game untouched.

Only plain functions and staticmethods are supported (not flexmethods, class
methods, properties, per-object patches or gc-cell swaps; see
docs/SCRIPT-HOOKS.md for those).
"""
import sys
import types

from sims4vr import native

_targets = {}    # (id(owner), name) -> _Target
_started = False
_disabled = None  # parsed [hooks] disable list, once the DLL (and its ini) is loaded
_seq = 0
_body = None


def _first_person():
    global _body
    try:
        if _body is None:
            from sims4vr import body
            _body = body
        return bool(_body.following())
    except Exception:
        return False


in_first_person = _first_person


class _Entry(object):
    __slots__ = ('tag', 'wrapper', 'first_person_only', 'order', 'seq', 'catch', 'enabled', 'errors')

    def __init__(self, tag, wrapper, first_person_only, order, seq, catch):
        self.tag = tag
        self.wrapper = wrapper
        self.first_person_only = first_person_only
        self.order = order
        self.seq = seq
        self.catch = catch
        self.enabled = True
        self.errors = 0


class _Target(object):
    def __init__(self, owner, name, raw):
        self.owner = owner
        self.name = name
        self.static = isinstance(raw, staticmethod)
        self.raw = raw                                        # what the owner held before us
        self.original = raw.__func__ if self.static else raw  # the game's function
        self.own = name in getattr(owner, '__dict__', {})     # defined on the owner itself
        self.entries = []
        self.top = None
        self.installed = False
        owner_name = getattr(owner, '__name__', None) or repr(owner)
        self.label = '{}.{}'.format(owner_name, name)


def _make_link(target, entry, inner):
    wrapper = entry.wrapper
    if entry.catch and not _is_generator(wrapper):
        def link(*args, **kwargs):
            if not entry.enabled or (entry.first_person_only and not _first_person()):
                return inner(*args, **kwargs)
            return _guarded(target, entry, inner, args, kwargs)
    else:
        def link(*args, **kwargs):
            if not entry.enabled or (entry.first_person_only and not _first_person()):
                return inner(*args, **kwargs)
            return wrapper(inner, *args, **kwargs)
    return link


def _is_generator(fn):
    code = getattr(fn, '__code__', None)
    return bool(code is not None and code.co_flags & 0x20)  # CO_GENERATOR


def _guarded(target, entry, inner, args, kwargs):
    state = [False, None, None]  # next called, its result, its exception

    def nxt(*a, **k):
        state[0] = True
        try:
            result = inner(*a, **k)
        except BaseException as e:
            state[2] = e
            raise
        state[1] = result
        return result

    try:
        return entry.wrapper(nxt, *args, **kwargs)
    except Exception as e:
        if e is state[2]:
            raise  # the game's own error, not the wrapper's
        entry.errors += 1
        if entry.errors <= 5:
            native._log('hooks: {} wrapper of {} failed: {!r}'.format(entry.tag, target.label, e))
        if state[0]:
            if state[2] is not None:
                raise state[2]
            return state[1]  # the wrapper failed after the game's function ran: keep its result
        return inner(*args, **kwargs)


def _apply(target):
    """Make the owner hold the chain of the enabled wrappers (or the game's own function)."""
    entries = sorted((e for e in target.entries if e.enabled), key=lambda e: (e.order, e.seq))
    owner, name = target.owner, target.name
    if not entries:
        if target.installed:
            _put(target, target.raw)
            target.installed = False
        target.top = None
        return
    # Disabled entries stay out of the chain, so each link is cheap.
    inner = target.original
    for entry in entries:
        inner = _make_link(target, entry, inner)
    top = inner
    try:
        top.__name__ = name
        top.__qualname__ = getattr(target.original, '__qualname__', name)
        top.__doc__ = getattr(target.original, '__doc__', None)
    except Exception:
        pass
    top.__wrapped__ = target.original
    top._sims4vr_hooks = True
    held = getattr(owner, '__dict__', {}).get(name)
    held = held.__func__ if isinstance(held, staticmethod) else held
    if held is not None and target.installed and held is not target.top and held is not target.original:
        native._log('hooks: {} was replaced by something else since we wrapped it; wrapping over it'.format(
            target.label))
    target.top = top
    _put(target, staticmethod(top) if target.static else top)
    target.installed = True


def _put(target, value):
    if value is target.raw and not target.own:
        try:
            delattr(target.owner, target.name)  # it was inherited: give the inheritance back
            return
        except Exception:
            pass
    setattr(target.owner, target.name, value)


def _is_disabled(tag, target):
    if not _disabled:
        return False
    label = target.label.lower()
    return tag.lower() in _disabled or label in _disabled or '{}:{}'.format(tag.lower(), label) in _disabled


def _read_switches():
    global _disabled
    value = native.setting('hooks', 'disable', '') or ''
    _disabled = set(p.strip().lower() for p in value.replace(';', ',').split(',') if p.strip())


def hook(tag, owner, name, wrapper, first_person_only=False, order=0, catch=True):
    """Wrap owner.name with wrapper (see the module doc). Returns True when it is wrapped.
    Registering the same tag on the same target again replaces its wrapper."""
    global _seq
    try:
        key = (id(owner), name)
        target = _targets.get(key)
        if target is None:
            raw = getattr(owner, '__dict__', {}).get(name)
            if raw is None:
                raw = getattr(owner, name, None)
            if raw is None:
                native._log('hooks: {}.{} not found ({}); {} not wrapped'.format(
                    getattr(owner, '__name__', owner), name, tag, tag))
                return False
            plain = raw.__func__ if isinstance(raw, staticmethod) else raw
            if not isinstance(plain, types.FunctionType):
                native._log('hooks: {}.{} is not a plain function ({!r}); {} not wrapped'.format(
                    getattr(owner, '__name__', owner), name, type(raw).__name__, tag))
                return False
            target = _targets[key] = _Target(owner, name, raw)
        _seq += 1
        entry = _Entry(tag, wrapper, first_person_only, order, _seq, catch)
        for i, old in enumerate(target.entries):
            if old.tag == tag:
                entry.seq = old.seq
                target.entries[i] = entry
                break
        else:
            target.entries.append(entry)
        if _disabled is None and _started:
            _read_switches()
        if _is_disabled(tag, target):
            entry.enabled = False
        _apply(target)
        if _started:
            native._log(_summary_line(target))
        return True
    except Exception as e:
        native._log('hooks: wrapping {}.{} for {} failed: {!r}'.format(
            getattr(owner, '__name__', owner), name, tag, e))
        return False


def original(owner, name):
    """The game's own function behind owner.name (None if nothing wraps it)."""
    target = _targets.get((id(owner), name))
    return target.original if target is not None else None


def disable(tag=None, label=None):
    """Skip wrappers of this tag and/or target label at runtime."""
    _switch(tag, label, False)


def enable(tag=None, label=None):
    _switch(tag, label, True)


def remove(tag, owner=None, name=None):
    """Drop a tag's wrappers (all targets, or one) for good."""
    for key, target in list(_targets.items()):
        if owner is not None and (target.owner is not owner or target.name != name):
            continue
        target.entries = [e for e in target.entries if e.tag != tag]
        _apply(target)


def _switch(tag, label, on):
    for target in _targets.values():
        if label is not None and target.label != label:
            continue
        changed = False
        for entry in target.entries:
            if (tag is None or entry.tag == tag) and entry.enabled != on:
                entry.enabled = on
                changed = True
        if changed:
            _apply(target)


def caller_frame(depth=1):
    """sys._getframe(depth) as seen from a wrapper, skipping this module's own
    frames: the code that called the wrapped game function."""
    frame = sys._getframe(depth + 1)
    while frame is not None and frame.f_globals.get('__name__') == __name__:
        frame = frame.f_back
    return frame


def _summary_line(target):
    parts = []
    for entry in sorted(target.entries, key=lambda e: (e.order, e.seq)):
        parts.append('{}{}{}'.format(entry.tag, '*' if entry.first_person_only else '',
                                     '' if entry.enabled else ' (off)'))
    return 'hooks: {}: {}{}'.format(target.label, ', '.join(parts), ' [inner to outer; * first person only]'
                                    if len(parts) > 1 or any(e.first_person_only for e in target.entries) else '')


def start():
    """After the DLL and its ini are loaded: apply the [hooks] switches, then one line per wrapped target."""
    global _started
    _started = True
    try:
        _read_switches()
        for target in _targets.values():
            for entry in target.entries:
                if _is_disabled(entry.tag, target):
                    entry.enabled = False
            _apply(target)
    except Exception as e:
        native._log('hooks: reading the [hooks] switches failed: {!r}'.format(e))
    live = [t for t in _targets.values() if t.entries]
    for target in live:
        native._log(_summary_line(target))
    native._log('hooks: {} game functions wrapped, {} wrappers ({} off)'.format(
        len(live), sum(len(t.entries) for t in live), sum(1 for t in live for e in t.entries if not e.enabled)))
