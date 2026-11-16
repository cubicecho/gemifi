"""gemifi_build.py migrate: rewrite a device YAML from the keys of components that moved onto a base
component to the current form, keeping its comments and layout.

    new_text, notes = migrate(text)

What moves comes from the components (old_keys): each hub type that is a platform of a base now
(`ws2812:` -> `led_strip:` with `platform: ws2812`, `tm1628:` -> `led_driver:` ...), and each display
platform (`ssd1306:` -> `display:` with `platform: ssd1306`). A moved list becomes the base's list,
with `platform:` on each item; a second list of the same base is merged into the first; the old key
as a pin key (`{ ws2812: strip, number: 0 }`) or a text display's screen (`ssd1306: oled`) becomes the
base's key. It works on the text, line by line: what it can't rewrite (a flow list over several
lines, say) is left as it is and said in the notes, and the builder then reports it.
"""
import re

import gemifi_device as gd


def moves():
    """old key -> (the base's key, the platform: name), from the components."""
    out = {old: (t.domain, t.key) for t in gd.REG.HUB_DEFS.values() if t.domain for old in t.old_keys}
    out.update({old: ("display", t.platform) for t in gd.REG.DISPLAY_PLATFORMS.values() for old in t.old_keys})
    return out


TOP_KEY = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):(.*)$")
ITEM = re.compile(r"^(\s*)-(\s+|$)")


def _chunks(lines):
    """The file as top-level chunks: [key or None, [lines]]. Comments and blank lines at column 0 just
    before a key go with that key."""
    chunks = [[None, []]]
    for line in lines:
        m = TOP_KEY.match(line)
        if m:
            cur = chunks[-1][1]
            lead = []
            while cur and (not cur[-1].strip() or cur[-1].startswith("#")):
                lead.insert(0, cur.pop())
            chunks.append([m.group(1), lead + [line]])
        else:
            chunks[-1][1].append(line)
    return chunks


def _key_line(chunk):
    return next(i for i, l in enumerate(chunk[1]) if TOP_KEY.match(l))


def _flow(text, item_depth):
    """A flow value on one line: the (start, end) of each item mapping (`{ ... }` at item_depth: 1 in a
    list, 0 for a list item's own mapping), and where the value ends; None if it doesn't close here."""
    spans, depth, quote, start = [], 0, None, 0
    for i, c in enumerate(text):
        if quote:
            quote = None if c == quote else quote
        elif c in "\"'":
            quote = c
        elif c == "#" and depth == 0 and (i == 0 or text[i - 1].isspace()):
            break
        elif c in "[{":
            if c == "{" and depth == item_depth:
                start = i
            depth += 1
        elif c in "]}":
            depth -= 1
            if c == "}" and depth == item_depth:
                spans.append((start, i + 1))
            if depth == 0:
                return spans, i + 1
    return None


def _with_platform(item, plat):
    """`{ id: a, ... }` -> `{ id: a, platform: plat, ... }` (or platform: first, without an id:)."""
    m = re.match(r"\{(\s*id:\s*[A-Za-z0-9_]+)", item)
    if m:
        return item[:m.end()] + f", platform: {plat}" + item[m.end():]
    return "{ platform: " + plat + "," + item[1:]


def _add_platform_flow(text, item_depth, plat):
    """Each item mapping in the flow value text gets platform:, or None if it doesn't close here."""
    f = _flow(text, item_depth)
    if f is None:
        return None
    for a, b in reversed(f[0]):
        text = text[:a] + _with_platform(text[a:b], plat) + text[b:]
    return text


def _convert_list(chunk, base, plat, notes, lineno):
    """A moved list's chunk: its key becomes base, each item gets platform: plat."""
    lines = chunk[1]
    k = _key_line(chunk)
    rest = TOP_KEY.match(lines[k]).group(2)
    value = rest.split("#", 1)[0].strip()
    if value:
        flow = _add_platform_flow(rest, 1, plat)
        if flow is None:
            notes.append(f"line {lineno + k}: {chunk[0]}: is a flow list over several lines: add "
                         f"`platform: {plat}` to each item by hand")
            flow = rest
        lines[k] = f"{base}:{flow}"
        chunk[0] = base
        return
    lines[k] = f"{base}:{rest}"
    chunk[0] = base
    out, indent = lines[:k + 1], None
    for line in lines[k + 1:]:
        m = ITEM.match(line)
        if m and line.strip() and (indent is None or len(m.group(1)) == indent):
            indent = len(m.group(1))
            body = line[m.end():]
            if body.startswith("{"):
                flow = _add_platform_flow(body, 0, plat)
                if flow is None:
                    notes.append(f"{base}: an item is a flow mapping over several lines: add "
                                 f"`platform: {plat}` to it by hand")
                    out.append(line)
                else:
                    out.append(line[:m.end()] + flow)
                continue
            out.append(line)
            out.append(" " * (indent + 2) + f"platform: {plat}")
            continue
        out.append(line)
    chunk[1] = out


def _to_block(chunk):
    """A list given as a one-line flow list, `key: [{ ... }, { ... }]  # c`, as block items (`- { ... }`
    lines); False if it isn't one."""
    lines = chunk[1]
    k = _key_line(chunk)
    rest = TOP_KEY.match(lines[k]).group(2)
    lead = len(rest) - len(rest.lstrip())
    if not rest.lstrip().startswith("["):
        return False
    f = _flow(rest, 1)
    if f is None or rest[lead + 1:f[1] - 1].strip() and not f[0]:
        return False
    comment = rest[f[1]:].strip()
    chunk[1] = (lines[:k] + [f"{chunk[0]}:" + (f"  {comment}" if comment else "")] +
                [f"  - {rest[a:b]}" for a, b in f[0]] + lines[k + 1:])
    return True


def _items(chunk):
    """The lines after the key line, and the items' indent (None for a flow value or no items)."""
    lines = chunk[1]
    k = _key_line(chunk)
    if TOP_KEY.match(lines[k]).group(2).split("#", 1)[0].strip():
        return None, None
    body = lines[k + 1:]
    for line in body:
        m = ITEM.match(line)
        if m:
            return body, len(m.group(1))
    return body, None


def migrate(text):
    """-> (the migrated text, [notes]). Unchanged text when nothing moved."""
    table = moves()
    old = "|".join(sorted(table, key=len, reverse=True))
    flow_key = re.compile(rf"([{{,]\s*)({old}):(\s)")
    block_key = re.compile(rf"^(\s+(?:-\s+)?)({old}):(\s|$)")
    notes = []
    lines = text.split("\n")
    chunks = _chunks(lines)
    # nested uses first: pin keys, a text display's screen
    for ch in chunks:
        ch[1] = [block_key.sub(lambda m: f"{m.group(1)}{table[m.group(2)][0]}:{m.group(3)}",
                               flow_key.sub(lambda m: f"{m.group(1)}{table[m.group(2)][0]}:{m.group(3)}", ln))
                 if not ln.lstrip().startswith("#") else ln for ln in ch[1]]
    # the lists
    starts, n = [], 1
    for ch in chunks:
        starts.append(n)
        n += len(ch[1])
    for ch, n in zip(chunks, starts):
        if ch[0] in table:
            base, plat = table[ch[0]]
            notes.append(f"line {n + _key_line(ch)}: {ch[0]}: -> {base}: with platform: {plat}")
            _convert_list(ch, base, plat, notes, n)
    # one list per base
    bases = {b for b, _ in table.values()}
    first = {}
    out = []
    for ch in chunks:
        if ch[0] not in bases:
            out.append(ch)
            continue
        if ch[0] not in first:
            first[ch[0]] = ch
            out.append(ch)
            continue
        into = first[ch[0]]
        for c in (ch, into):
            if _items(c)[0] is None:
                _to_block(c)
        body, indent = _items(ch)
        into_body, into_indent = _items(into)
        if body is None or into_body is None:
            notes.append(f"two {ch[0]}: lists, one of them a flow list: merge them by hand")
            out.append(ch)
            continue
        target = into_indent if into_indent is not None else (indent or 0)
        shift = target - (indent or 0)
        moved = [(" " * shift + l if shift > 0 and l.strip() else l[-shift:] if shift < 0 and l.strip() else l)
                 for l in body]
        while moved and not moved[-1].strip():
            moved.pop()
        # its comments go with its items
        moved = [" " * target + l.strip() for l in ch[1][:_key_line(ch)] if l.strip()] + moved
        tail = []
        while into[1] and not into[1][-1].strip():
            tail.insert(0, into[1].pop())
        into[1] += moved + tail
        notes.append(f"merged a second {ch[0]}: list into the first (hubs are numbered in file order; their "
                     "ids, and so the pins, are unchanged)")
    new = "\n".join(l for ch in out for l in ch[1])
    return new, (notes if new != text else [])
