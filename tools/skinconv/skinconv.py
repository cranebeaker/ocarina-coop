#!/usr/bin/env python3
"""Convert Link skin mods (OTR or O2R) into namespaced O2Rs for ZeldaOnline.


Usage:
    python skinconv.py skins/malon                  one skin -> malon_1.o2r, malon_2.o2r, ...
    python skinconv.py skins/malon -o malon.o2r     one skin -> a single merged malon.o2r
    python skinconv.py skins/malon linkle           override the skin name
    python skinconv.py skins --bulk                 every subfolder of skins/ is a skin
    python skinconv.py skins --bulk-merge           ditto, one merged <skin>.o2r each
    python skinconv.py skins/malon --dry-run        show the remapping, write nothing

Each run prints the line to paste into the skin config:
    malon|malon_1.o2r,malon_2.o2r
and when the skin is a single archive named after itself, the file half can be
dropped entirely -- "malon" is enough.

Needs mpq.py alongside it for OTR input.
"""

import argparse
import os
import posixpath
import re
import sys
import zipfile

# Entries that are archive bookkeeping rather than game resources.
MPQ_INTERNAL = {"(listfile)", "(attributes)", "(signature)", "(user data)"}

# Prefixes a mod may already be namespaced under.
KNOWN_NAMESPACES = ("alt/",)

# Top-level directories that indicate vanilla-path (unnamespaced) content.
VANILLA_ROOTS = ("objects/", "textures/", "scenes/", "overlays/", "misc/", "audio/")


class Entry:
    __slots__ = ("path", "data")

    def __init__(self, path, data):
        self.path = path
        self.data = data


def read_o2r(path):
    """Read a zip-based archive."""
    entries = []
    with zipfile.ZipFile(path, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            entries.append(Entry(info.filename.replace("\\", "/"), zf.read(info)))
    return entries


def read_otr(path):
    """Read an MPQ-based archive via its listfile."""
    try:
        from mpq import MPQArchive
    except ImportError:
        sys.exit("mpq.py must sit next to skinconv.py")

    archive = MPQArchive(path)

    try:
        names = archive.list_files()
    except Exception as exc:
        sys.exit(f"cannot read (listfile) from {path}: {exc}")

    entries = []
    missed = []

    for name in names:
        if not name or name in MPQ_INTERNAL:
            continue

        try:
            data = archive.read_file(name)
        except Exception as exc:
            missed.append((name, str(exc)))
            continue

        if not data:
            missed.append((name, "read as empty"))
            continue

        entries.append(Entry(name.replace("\\", "/"), data))

    if missed:
        print(f"  ERROR: {len(missed)} of {len(names)} entries could not be read and were DROPPED",
              file=sys.stderr)
        for name, why in missed[:10]:
            print(f"    {name}: {why}", file=sys.stderr)
        if len(missed) > 10:
            print(f"    ... and {len(missed) - 10} more", file=sys.stderr)

    return entries


def read_archive(path):
    # is_zipfile() insists on a clean central directory at the very end, so it
    # rejects archives that every zip tool opens happily. Just try it.
    try:
        return read_o2r(path)
    except zipfile.BadZipFile:
        pass
    return read_otr(path)


def detect_namespace(entries):
    """Return the existing namespace prefix, or None if the mod uses vanilla paths."""
    for entry in entries:
        low = entry.path.lower()
        for ns in KNOWN_NAMESPACES:
            if low.startswith(ns):
                return entry.path[: len(ns)]
    return None


def looks_like_resource(path):
    low = path.lower()
    return any(low.startswith(root) for root in VANILLA_ROOTS)


def remap(entries, skin_name):
    """Move every resource under skin_name/. Returns (new_entries, old_prefix)."""
    old_prefix = detect_namespace(entries)
    new_prefix = skin_name.rstrip("/") + "/"

    out = []
    for entry in entries:
        if entry.path in MPQ_INTERNAL:
            new_path = entry.path
        elif old_prefix and entry.path.lower().startswith(old_prefix.lower()):
            new_path = new_prefix + entry.path[len(old_prefix):]
        else:
            new_path = new_prefix + entry.path

        out.append(Entry(posixpath.normpath(new_path).replace("\\", "/"), entry.data))

    return out, old_prefix


# --- internal reference rewriting (from skinpath.py) ---------------------
#
# Matches ANY attribute value: Name="value" / name='value'. Deliberately not
# an enumeration of attribute names -- refs appear as Path=, DisplayList1=,
# texture refs inside DisplayList files and more. Everything is matched and the
# "does the skin provide this file?" check does the filtering, so Version="0"
# is ignored automatically because no such file exists.
PATH_ATTR = re.compile(r'(\b[A-Za-z_][A-Za-z0-9_]*\s*=\s*)(["\'])([^"\']*)\2')


def is_texty(blob):
    """Sniff for NUL bytes rather than trusting extensions -- resources here
    are extension-less."""
    if b"\x00" in blob[:8192]:
        return False
    return b"=" in blob and b"<" in blob


def rewrite_references(entries, provided, skin_name):
    """Prefix every internal reference the skin actually overrides.

    References the skin does not provide are left pointing at the vanilla path,
    so they fall through to the base game at runtime -- per-file fallback
    decided at pack time instead of at runtime.
    """
    prefix = skin_name.rstrip("/")
    stats = {"files": 0, "rewritten": 0, "left": 0, "binary": 0}
    misses = {}

    # SoH's resource lookup is case-insensitive, and mods are not internally
    # consistent about it -- one archive here provides "...RightForearmLimb..."
    # while its own reference asks for "...RightForeArmLimb...". Match without
    # case and emit the archive's real spelling.
    lookup = {p.lower(): p for p in provided}

    for entry in entries:
        if not is_texty(entry.data):
            stats["binary"] += 1
            continue

        try:
            text = entry.data.decode("utf-8")
        except UnicodeDecodeError:
            stats["binary"] += 1
            continue

        counts = {"rewritten": 0, "left": 0}

        def sub(m):
            head, quote, ref = m.group(1), m.group(2), m.group(3)

            # Already prefixed -> idempotent no-op.
            if ref.startswith(prefix + "/"):
                return m.group(0)

            actual = lookup.get(ref.lower())
            if actual is not None:
                counts["rewritten"] += 1
                return f"{head}{quote}{prefix}/{actual}{quote}"

            counts["left"] += 1
            if "/" in ref:
                misses[ref] = misses.get(ref, 0) + 1
            return m.group(0)

        new_text = PATH_ATTR.sub(sub, text)

        if counts["rewritten"]:
            entry.data = new_text.encode("utf-8")
            stats["files"] += 1
            stats["rewritten"] += counts["rewritten"]
        stats["left"] += counts["left"]

    return stats, misses


def unique_path(path):
    """Never clobber an existing file -- least surprising when the chosen output
    name collides with the input."""
    if not os.path.exists(path):
        return path

    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists(f"{stem}_{n}{ext}"):
        n += 1
    return f"{stem}_{n}{ext}"


def write_o2r(entries, path, level):
    mode = zipfile.ZIP_STORED if level == 0 else zipfile.ZIP_DEFLATED
    kwargs = {} if level == 0 else {"compresslevel": level}

    with zipfile.ZipFile(path, "w", mode, **kwargs) as zf:
        for entry in entries:
            zf.writestr(entry.path, entry.data)


def collect_archives(folder):
    """Archives sitting DIRECTLY in this folder, name-sorted."""
    return sorted(
        os.path.join(folder, name)
        for name in os.listdir(folder)
        if os.path.splitext(name)[1].lower() in (".otr", ".o2r")
        and os.path.isfile(os.path.join(folder, name))
    )


def collect_layers(folder, depth=0):
    """Walk a skin folder into override layers, shallowest first.

    A subfolder is a set of REPLACEMENT files: anything it supplies wins over
    the folder above it. That lets a partial mod (say an adult-only Christmas
    Malon) sit in a subfolder of a complete one and keep the base's child half.
    Nesting continues to any depth -- deeper always wins.
    """
    layers = []

    archives = collect_archives(folder)
    if archives:
        layers.append((folder, depth, archives))

    for name in sorted(os.listdir(folder)):
        child = os.path.join(folder, name)
        if os.path.isdir(child):
            layers.extend(collect_layers(child, depth + 1))

    return layers


def convert_skin(layers, skin_name, output, dry_run, compression):
    """layers: [(label, depth, [archive paths])] shallowest first."""
    # Everything is read in one pass, in order: shallowest folder first, and
    # within a folder alphabetically by filename. A later archive REPLACES any
    # path an earlier one supplied -- that is how an HD "...Tex" archive
    # overrides its base, and how a subfolder overrides the folder above it.
    by_path = {}
    order = []
    overrides = []
    sources = []          # [(archive path, [keys it still owns])] in read order
    owner = {}            # key -> index into sources
    archive_count = 0

    for label, depth, archives in layers:
        indent = "  " * depth

        for path in archives:
            archive_count += 1
            print(f"{indent}reading {path}")
            part = read_archive(path)

            source_index = len(sources)
            sources.append((path, []))

            replaced = 0
            for entry in part:
                key = entry.path.lower()
                if key in by_path:
                    overrides.append((entry.path, os.path.basename(path), depth))
                    replaced += 1
                else:
                    order.append(key)
                by_path[key] = entry
                owner[key] = source_index

            if replaced:
                print(f"{indent}  {len(part)} entries ({replaced} replacing earlier files)")
            else:
                print(f"{indent}  {len(part)} entries")

    for key in order:
        sources[owner[key]][1].append(key)

    entries = [by_path[key] for key in order]

    if archive_count > 1:
        print(f"\n{archive_count} archive(s) in {len(layers)} folder(s) -> {len(entries)} unique entries")

        if overrides:
            deep = sum(1 for _, _, depth in overrides if depth > 0)
            print(f"  {len(overrides)} file(s) replaced by a later archive"
                  f" ({deep} of them from a subfolder):")
            for path, source, _ in overrides[:12]:
                print(f"    {path}  <- {source}")
            if len(overrides) > 12:
                print(f"    ... and {len(overrides) - 12} more")

        if len(layers) > 1 and not any(depth > 0 for _, _, depth in overrides):
            print("  WARNING: nothing in a subfolder replaced anything above it --"
                  " check that the layers really overlap")

    if not entries:
        sys.exit("no entries found")

    remapped, old_prefix = remap(entries, skin_name)

    if old_prefix:
        print(f"  detected existing namespace '{old_prefix}' -> '{skin_name}/'")
    else:
        print(f"  vanilla paths detected -> moving under '{skin_name}/'")

    changed = sum(1 for a, b in zip(entries, remapped) if a.path != b.path)
    print(f"  {changed} of {len(entries)} entries remapped")

    stranded = [b.path for b in remapped
                if not b.path.lower().startswith(skin_name.rstrip("/").lower() + "/")
                and b.path not in MPQ_INTERNAL]

    if stranded:
        print(f"  WARNING: {len(stranded)} entr(ies) left at the archive root -- these will"
              f" override vanilla for every player:")
        for path in stranded[:12]:
            print(f"    {path}")
        if len(stranded) > 12:
            print(f"    ... and {len(stranded) - 12} more")

    if dry_run:
        for a, b in list(zip(entries, remapped))[:25]:
            if a.path != b.path:
                print(f"    {a.path}\n      -> {b.path}")
        if changed > 25:
            print(f"    ... and {changed - 25} more")
        return None

    # Every path the skin provides, archive-relative and un-prefixed -- this is
    # what an internal reference must match to be rewritten.
    provided = {e.path for e in entries}
    if old_prefix:
        provided = {p[len(old_prefix):] for p in provided if p.lower().startswith(old_prefix.lower())}

    stats, misses = rewrite_references(remapped, provided, skin_name)

    print(f"  {stats['rewritten']} refs prefixed across {stats['files']} files")
    print(f"  {stats['left']} refs left vanilla (skin doesn't override these)")
    print(f"  {stats['binary']} binary/no-ref files untouched")

    if misses:
        print("\n  refs that looked like paths but matched no file in the skin:")
        for ref in sorted(misses)[:12]:
            print(f"    {ref}")
        if len(misses) > 12:
            print(f"    ... and {len(misses) - 12} more distinct")

    print()

    remapped_by_key = {key: remapped[i] for i, key in enumerate(order)}
    written = []

    if output or archive_count == 1:
        # -o names ONE file, so it also means "merge everything into it".
        groups = [(output or (skin_name + ".o2r"), remapped)]
    else:
        # One output per SOURCE archive, carrying only the entries that archive
        # still owns after overriding. An archive whose every file was replaced
        # produces nothing.
        groups = []
        index = 0
        for path, keys in sources:
            if not keys:
                print(f"  {os.path.basename(path)}: every file was replaced, nothing to write")
                continue
            index += 1
            groups.append((f"{skin_name}_{index}.o2r", [remapped_by_key[k] for k in keys]))

    for out_name, group in groups:
        out_path = unique_path(out_name)
        if out_path != out_name:
            print(f"  {out_name} exists -> writing {out_path}")

        write_o2r(group, out_path, compression)
        written.append(os.path.basename(out_path))

        size = os.path.getsize(out_path)
        raw = sum(len(e.data) for e in group)
        label = "stored" if compression == 0 else f"level {compression}"
        print(f"wrote {out_path}  ({size / 1024:.0f} KiB from {raw / 1024:.0f} KiB, {label})")

    config = f"{skin_name}|{','.join(written)}"
    shorthand = (len(written) == 1 and written[0] == skin_name + ".o2r")

    print()
    print("add this to the skin config:")
    print(f"  {config}")
    if shorthand:
        print(f"  ...or just \"{skin_name}\" — the file name is optional when it matches the skin name")

    return skin_name if shorthand else config


def main():
    ap = argparse.ArgumentParser(description="Namespace a Link skin mod for ZeldaOnline.")
    ap.add_argument("folder", help="the skin's folder. Archives directly inside it are the base;\n"
                                   "archives in any SUBFOLDER replace files of the same name from\n"
                                   "the folder above. With --bulk, a folder OF skin folders.")
    ap.add_argument("skin_name", nargs="?", default=None,
                    help="skin name (default: the folder's name). Ignored with --bulk.")
    ap.add_argument("--bulk", action="store_true",
                    help="treat each subfolder of FOLDER as its own skin, named after the folder")
    ap.add_argument("--bulk-merge", action="store_true", dest="bulk_merge",
                    help="like --bulk, but each skin becomes ONE merged <skin>.o2r")
    ap.add_argument("-o", "--output",
                    help="write ONE merged .o2r with this name instead of one file per source\n"
                         "archive (default: <skin_name>_1.o2r, _2.o2r, ... or <skin_name>.o2r\n"
                         "when the skin is a single archive)")
    ap.add_argument("--dry-run", action="store_true", help="list the remapping without writing")
    ap.add_argument("-c", "--compression", type=int, choices=range(0, 10), default=6, metavar="0-9",
                    help="0 = store uncompressed (fastest load), 9 = smallest file (default: 6)")
    args = ap.parse_args()

    if not os.path.isdir(args.folder):
        sys.exit(f"not a folder: {args.folder}")

    if args.bulk_merge:
        args.bulk = True

    if args.bulk:
        if args.skin_name:
            sys.exit("--bulk names each skin after its folder; do not pass skin_name")
        if args.output:
            sys.exit("--bulk names its own outputs; -o does not apply")

        folders = sorted(
            name for name in os.listdir(args.folder)
            if os.path.isdir(os.path.join(args.folder, name))
        )
        if not folders:
            sys.exit(f"no subfolders in {args.folder}")

        lines = []
        skipped = []

        for name in folders:
            layers = collect_layers(os.path.join(args.folder, name))
            if not layers:
                skipped.append(name)
                continue

            print()
            print("=" * 60)
            print(f"{name}  ({sum(len(a) for _, _, a in layers)} archive(s)"
                  f" in {len(layers)} layer(s))")
            print("=" * 60)

            merged_name = f"{name}.o2r" if args.bulk_merge else None
            config = convert_skin(layers, name, merged_name, args.dry_run, args.compression)
            if config:
                lines.append(config)

        if skipped:
            print()
            print(f"skipped {len(skipped)} folder(s) with no archives:", ", ".join(skipped))

        if lines:
            print()
            print("=" * 60)
            print("skin config for all converted skins:")
            for line in lines:
                print(f"  {line}")
        return

    layers = collect_layers(args.folder)
    if not layers:
        sys.exit(f"no .otr/.o2r files found under {args.folder}")

    skin_name = args.skin_name or os.path.basename(os.path.normpath(args.folder))
    convert_skin(layers, skin_name, args.output, args.dry_run, args.compression)


if __name__ == "__main__":
    main()