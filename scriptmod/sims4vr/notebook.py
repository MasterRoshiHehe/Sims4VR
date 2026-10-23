"""The game's notebook in your Sim's book (first person; user 2026-10-23,
thread "Notebook in VR"): the left-hip book shows what the game's Notebook
widget shows, drawn by the DLL (hud.cpp notebook pages, the game's own art).

Taking the book out (control 83) builds the notebook exactly as the game does
(NotebookTrackerSimInfo.generate_notebook_information, the command
notebook.generate_notebook the Notebook widget asks with), only the NotebookView
message it distributes is taken here instead of being sent to the game's UI,
and handed to the DLL as records (native.notebook_content):

  "nb"  \\x1f <Sim's name> \\x1f <portrait picture id or 0> \\x1f <notes text>
  "cat" \\x1f <name json> \\x1f <description json> \\x1f <icon> \\x1f <large icon>
  "sub" \\x1f <name json> \\x1f <icon>                       (of the last cat)
  "ent" \\x1f <text json> \\x1f <description json> \\x1f <icon> \\x1f <new 0/1>
                                                           (of the last sub)
  "itm" \\x1f <text json> \\x1f <icon> \\x1f <count> \\x1f <total>   (of the last ent)

Records split by 0x1e; icons are image instances or thumbnail picture ids
(hudinfo._picture), read by hudinfo's icon loop. Read only. Putting the book
away (control 84) sends b'' (gone).
"""
from sims4vr import native

_logs = 0
_MAX_ENTRIES = 80


def _log(message):
    global _logs
    _logs += 1
    if _logs <= 60:
        native._log('notebook: ' + message)


def _text(value):
    return str(value).replace('\x1e', ' ').replace('\x1f', ' ')


def _capture(tracker, category=None, subcategory=None):
    """The NotebookView the game would send for this Sim's tracker (None: failed)."""
    from distributor.system import Distributor
    distributor = Distributor.instance()
    taken = []
    original = distributor.add_op

    def take(obj, op, *args, **kwargs):
        buffer = getattr(op, 'protocol_buffer', None)
        if buffer is not None and hasattr(buffer, 'categories'):
            taken.append(buffer)
            return None
        return original(obj, op, *args, **kwargs)

    distributor.add_op = take
    try:
        tracker.generate_notebook_information(initial_selected_category=category,
                                              initial_selected_subcategory=subcategory)
    finally:
        try:
            del distributor.add_op  # back to the class's own method
        except AttributeError:
            distributor.add_op = original
    return taken[0] if taken else None


def _categories(tracker, msg):
    """The category messages with their subcategories filled in. The game fills in the subcategories of the
    category the widget opens on; if the plain message has none, ask once per category as the widget does."""
    if any(len(c.subcategories) for c in msg.categories):
        return list(msg.categories)
    found = []
    cache = getattr(tracker, '_notebook_entry_catsubcat_cache', None) or {}
    for category_id, subs in cache.items():
        first = next(iter(subs), None)
        asked = _capture(tracker, category_id, first)
        if asked is None:
            continue
        for c in asked.categories:
            if c.enum_name == category_id.name:
                found.append(c)
                break
    _log('the plain notebook had no subcategories; asked per category: {} of {}'.format(len(found), len(cache)))
    return found or list(msg.categories)


def send():
    try:
        import services
        from sims4vr import hudinfo, objmenu
        sim_info = services.active_sim_info()
        tracker = sim_info.notebook_tracker if sim_info is not None else None
        if tracker is None:
            _log('no notebook tracker')
            native.notebook_content(b'')
            return
        msg = _capture(tracker)
        if msg is None:
            _log('the game sent no notebook')
            native.notebook_content(b'')
            return
        loc = objmenu._loc
        records = ['nb\x1f{}\x1f{}\x1f{}'.format(
            _text('{} {}'.format(sim_info.first_name, sim_info.last_name).strip()),
            hudinfo._thumb('sims/f_{}_m_0'.format(sim_info.id)), msg.notes if msg.notes else '')]
        subs = entries = 0
        categories = _categories(tracker, msg)
        for category in categories:
            records.append('cat\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                loc(category.category_name), loc(category.category_description) if category.HasField('category_description') else '',
                hudinfo._picture(category.category_icon) if category.HasField('category_icon') else 0,
                hudinfo._picture(category.category_large_icon) if category.HasField('category_large_icon') else 0))
            for sub in category.subcategories:
                subs += 1
                records.append('sub\x1f{}\x1f{}'.format(
                    loc(sub.subcategory_name),
                    hudinfo._picture(sub.subcategory_icon) if sub.HasField('subcategory_icon') else 0))
                for entry in list(sub.entries)[:_MAX_ENTRIES]:
                    entries += 1
                    records.append('ent\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                        loc(entry.entry_message),
                        loc(entry.entry_message_description) if entry.HasField('entry_message_description') else '',
                        hudinfo._picture(entry.entry_icon) if entry.HasField('entry_icon') else 0,
                        1 if entry.new_entry else 0))
                    for item in list(entry.entry_list)[:24]:
                        records.append('itm\x1f{}\x1f{}\x1f{}\x1f{}'.format(
                            loc(item.item_message),
                            hudinfo._picture(item.item_icon) if item.HasField('item_icon') else 0,
                            item.item_count, item.item_total))
        native.notebook_content('\x1e'.join(records).encode('utf-8'))
        _log('sent {} categories, {} subcategories, {} entries for {}'.format(
            len(categories), subs, entries, sim_info))
    except Exception as e:
        _log('send failed: {!r}'.format(e))
        try:
            native.notebook_content(b'')
        except Exception:
            pass


def clear():
    try:
        native.notebook_content(b'')
    except Exception:
        pass
