#!/usr/bin/env python3
"""
Extracts the Qobuz application credentials (app_id, app_secret) from a stock
HiByOS player binary and writes them into a streaming-keys.ini the player
reads.

Why this exists
---------------
Qobuz does not issue application credentials for personal or open-source
integrations, so there is no key pair this project could apply for. The stock
TempoTec V1 firmware talks to Qobuz anyway: its player, usr/bin/hiby_player,
carries HiBy's own app_id/app_secret as ordinary C strings in its read-only
data (the Sonix Qobuz client was reconstructed from that binary -- see the
offset notes in src/system/streaming/qobuz.c, and the same finding in the
HiBy R1 firmware teardown: "TIDAL and Qobuz client credentials sit in the
binary in plain text").

This tool pulls those two strings out of a binary the user already possesses
-- their own device's stock firmware -- and turns them into the same
streaming-keys.ini the rest of the workflow uses. Nothing here is downloaded,
and nothing the tool writes belongs in git: streaming-keys.ini and
streaming-keys.bin are both ignored. Do not publish an image that carries the
result; the credentials are HiBy's, not this project's.

    python3 tools/extract_streamkeys.py path/to/hiby_player
    python3 tools/extract_streamkeys.py unpacked/rootfs        # finds usr/bin/hiby_player
    python3 tools/extract_streamkeys.py --check unpacked/rootfs
    python3 tools/extract_streamkeys.py --list-candidates hiby_player

With no --out the ini is written next to the sealing tool's default, at
sonix-player/streaming-keys.ini. An existing ini is merged: its other
sections (Tidal, podcasts, Last.fm) are kept and only [qobuz] is replaced.

The heuristic never guesses silently. When more than one string could be the
id or the secret, it stops and --list-candidates shows what was found so the
pair can be forced with --app-id/--app-secret.

Tidal cannot be extracted this way on the V1 binary: its secret is assembled
byte by byte at run time and never appears as a string.
"""

import argparse
import os
import re
import sys

PLAYER_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_INI = os.path.join(PLAYER_ROOT, "streaming-keys.ini")
STOCK_PLAYER_REL = os.path.join("usr", "bin", "hiby_player")

# A string this short is noise in a five-megabyte binary; every value we look
# for is longer.
MIN_STRING_LEN = 4

# Qobuz application ids are numeric: five digits in the oldest partner apps,
# nine in current ones. Standalone NUL-terminated strings, not substrings.
APP_ID_RE = re.compile(rb"^\d{5,12}$")
# Every Qobuz app secret seen in the wild is 32 lowercase hex characters.
SECRET_HEX_RE = re.compile(rb"^[0-9a-f]{32}$")
# Fallback for a partner secret that is alphanumeric rather than hex: the same
# length, a weaker score so a hex string always wins.
SECRET_WORD_RE = re.compile(rb"^[0-9A-Za-z]{32}$")

# Strings that prove the binary is the right one and locate its Qobuz code.
# The stock V1 binary speaks cleartext http://www.qobuz.com/api.json/0.2; the
# match is on the host part so either scheme works.
ANCHOR_RE = re.compile(rb"qobuz\.com/api\.json|device_manufacturer_id|"
                       rb"trackgetfileurl|request_sig|x-app-id|qobuz", re.IGNORECASE)
# A match on the bare word could occur in an unrelated binary; one of these
# has to be present for the file to count as a Qobuz client at all.
STRONG_ANCHOR_RE = re.compile(rb"qobuz\.com/api\.json|device_manufacturer_id|"
                              rb"trackgetfileurl|request_sig|x-app-id", re.IGNORECASE)


class ExtractError(Exception):
    """The credentials could not be identified with enough certainty."""


def _is_printable(byte):
    return 0x20 <= byte <= 0x7E


def iter_strings(data):
    """Yields (offset, text) for every run of printable ASCII bytes.

    The split character is anything non-printable, which covers the NULs and
    alignment padding between C strings in .rodata as well as MIPS code bytes
    around them. The scan is deliberately format-agnostic: it works on an ELF,
    a squashfs member pulled out on its own, or a raw blob.
    """
    start = None
    for i, byte in enumerate(data):
        if _is_printable(byte):
            if start is None:
                start = i
        elif start is not None:
            if i - start >= MIN_STRING_LEN:
                yield start, data[start:i]
            start = None
    if start is not None and len(data) - start >= MIN_STRING_LEN:
        yield start, data[start:]


def _nearest(offset, anchors):
    return min(abs(offset - anchor) for anchor in anchors)


def _scan(data):
    """All strings first, so every distance is measured against all anchors."""
    strings = list(iter_strings(data))
    anchors = [offset for offset, text in strings if ANCHOR_RE.search(text)]
    strong = [offset for offset, text in strings if STRONG_ANCHOR_RE.search(text)]
    return strings, anchors, strong


def _record(hits, text, score):
    # The same literal can appear more than once; its closest occurrence wins.
    if text not in hits or score < hits[text]:
        hits[text] = score


def extract(data):
    """Returns (app_id, app_secret) as str, or raises ExtractError."""
    strings, anchors, strong_anchors = _scan(data)
    if not anchors or not strong_anchors:
        raise ExtractError(
            "no Qobuz client strings found (looked for qobuz.com/api.json, "
            "device_manufacturer_id, trackgetFileUrl, request_sig, X-App-Id). "
            "Is this the stock hiby_player binary?")

    id_hits = {}      # value text -> nearest anchor distance
    secret_hits = {}
    for offset, text in strings:
        if APP_ID_RE.match(text):
            _record(id_hits, text, _nearest(offset, anchors))
        if SECRET_HEX_RE.match(text) or SECRET_WORD_RE.match(text):
            score = _nearest(offset, anchors)
            # A hex string is the known shape; an all-words 32-char string is
            # only a fallback, so push its score back.
            if not SECRET_HEX_RE.match(text):
                score += 1
            _record(secret_hits, text, score)

    if not id_hits:
        raise ExtractError("no numeric app_id string found")
    if not secret_hits:
        raise ExtractError("no 32-character app_secret string found")

    def choose(hits, label):
        best_score = min(hits.values())
        winners = sorted(value for value, score in hits.items() if score == best_score)
        if len(winners) > 1:
            shown = ", ".join(mask(w.decode("ascii")) for w in winners)
            raise ExtractError(
                f"more than one candidate for the {label} sits at the same "
                f"distance from the Qobuz strings: {shown}. Re-run with "
                f"--list-candidates and force the right one with --app-id/"
                f"--app-secret.")
        return winners[0].decode("ascii")

    app_id = choose(id_hits, "app_id")
    app_secret = choose(secret_hits, "app_secret")
    if app_id == app_secret:
        raise ExtractError("the app_id and app_secret resolved to the same string")
    return app_id, app_secret


def list_candidates(data):
    """Human-readable diagnostic: every plausible string, masked, with offset."""
    strings, anchors, _ = _scan(data)
    rows = []
    for offset, text in strings:
        if APP_ID_RE.match(text) or SECRET_HEX_RE.match(text) or SECRET_WORD_RE.match(text):
            kind = "id?    " if APP_ID_RE.match(text) else "secret?"
            distance = _nearest(offset, anchors) if anchors else -1
            rows.append((distance, offset, kind, mask(text.decode("ascii"))))
    rows.sort()
    lines = ["distance  offset    kind      value"]
    lines += [f"{dist:>8}  0x{offset:08x}  {kind}  {value}" for dist, offset, kind, value in rows]
    if not anchors:
        lines.append("(no Qobuz anchor strings were found in this file)")
    return "\n".join(lines)


def mask(value):
    """Keeps a credential out of a log that may be shared. First four, length."""
    if len(value) <= 4:
        return "*" * len(value)
    return f"{value[:4]}... ({len(value)} chars)"


def find_binary(path):
    """A file is used directly; a directory is searched for the stock player."""
    if os.path.isfile(path):
        return path
    if not os.path.isdir(path):
        raise ExtractError(f"{path} is neither a file nor a directory")
    direct = os.path.join(path, STOCK_PLAYER_REL)
    if os.path.isfile(direct):
        return direct
    # Fallback for an unusual unpack layout. An extracted rootfs is only a few
    # thousand files, so an exhaustive walk stays cheap; pruning directories
    # here could skip the very tree the binary lives in.
    for root, dirs, files in os.walk(path):
        if "hiby_player" in files:
            candidate = os.path.join(root, "hiby_player")
            if os.path.isfile(candidate):
                return candidate
    raise ExtractError(
        f"no usr/bin/hiby_player under {path}. Point this at an unpacked "
        "stock rootfs or at the binary itself.")


def _parse_ini(text):
    sections = {}
    order = []
    section = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if section not in sections:
                sections[section] = {}
                order.append(section)
            continue
        if section and "=" in line:
            key, value = line.split("=", 1)
            sections[section][key.strip()] = value.strip()
    return sections, order


def merge_ini(existing, app_id, app_secret):
    """Replaces [qobuz], preserving every other section of an existing ini."""
    sections, order = _parse_ini(existing or "")
    if "qobuz" not in sections:
        order.insert(0, "qobuz")
    sections["qobuz"] = {"app_id": app_id, "app_secret": app_secret}

    out = [
        "# Streaming service credentials, extracted from the stock firmware of",
        "# this device. Do not commit or share this file, and do not publish an",
        "# image that carries it: the Qobuz pair belongs to HiBy, not to Sonix.",
        "",
    ]
    for name in order:
        out.append(f"[{name}]")
        for key, value in sections[name].items():
            out.append(f"{key} = {value}")
        out.append("")
    return "\n".join(out)


def _seal(plain, key_path, bin_out):
    # Same directory as this file: the sealing format it implements is the one
    # streamkeys.c reads. Imported lazily so extraction never needs it.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import seal_streamkeys

    key = seal_streamkeys.read_key(key_path)
    if key is None:
        raise ExtractError(
            f"no sealing key at {key_path}. Build the player once to create "
            "streaming-keys.key, or leave --seal off for a plain ini that a "
            "keyless build reads anyway.")
    blob = seal_streamkeys.seal(key, plain.encode("utf-8"))
    if len(blob) > seal_streamkeys.MAX_FILE:
        raise ExtractError("the sealed file would be too large")
    os.makedirs(os.path.dirname(os.path.abspath(bin_out)), exist_ok=True)
    with open(bin_out, "wb") as handle:
        handle.write(blob)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Extract Qobuz credentials from a stock HiByOS player binary.")
    parser.add_argument("path", help="hiby_player binary, or an unpacked rootfs directory")
    parser.add_argument("-o", "--out", default=DEFAULT_INI,
                        help="streaming-keys.ini to write (default: %(default)s)")
    parser.add_argument("--seal", action="store_true",
                        help="also write a sealed streaming-keys.bin")
    parser.add_argument("--key", default=os.path.join(PLAYER_ROOT, "streaming-keys.key"),
                        help="sealing key from the player build (default: %(default)s)")
    parser.add_argument("--bin-out", help="destination of the sealed .bin (with --seal)")
    parser.add_argument("--check", action="store_true",
                        help="report whether a pair can be extracted, write nothing")
    parser.add_argument("--list-candidates", action="store_true",
                        help="print every plausible string and exit")
    parser.add_argument("--reveal", action="store_true",
                        help="print full values instead of masking them")
    parser.add_argument("--app-id", help="force the app_id (manual fallback)")
    parser.add_argument("--app-secret", help="force the app_secret (manual fallback)")
    args = parser.parse_args(argv)

    try:
        binary = find_binary(args.path)
        with open(binary, "rb") as handle:
            data = handle.read()

        if args.list_candidates:
            print(list_candidates(data))
            return 0

        if args.app_id and args.app_secret:
            app_id, app_secret = args.app_id, args.app_secret
        elif args.app_id or args.app_secret:
            raise ExtractError("--app-id and --app-secret must be given together")
        else:
            app_id, app_secret = extract(data)

        shown_id = app_id if args.reveal else mask(app_id)
        shown_secret = app_secret if args.reveal else mask(app_secret)
        print(f"source:      {binary}")
        print(f"qobuz app_id:     {shown_id}")
        print(f"qobuz app_secret: {shown_secret}")

        if args.check:
            print("check only: nothing written")
            return 0

        existing = ""
        if os.path.exists(args.out):
            with open(args.out, "r", encoding="utf-8", errors="replace") as handle:
                existing = handle.read()
        text = merge_ini(existing, app_id, app_secret)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
        try:
            os.chmod(args.out, 0o600)
        except OSError:
            pass
        print(f"written:     {args.out}")

        if args.seal:
            if not args.bin_out:
                raise ExtractError("--seal needs --bin-out")
            _seal(text, args.key, args.bin_out)
            print(f"sealed:      {args.bin_out}")
        return 0
    except ExtractError as err:
        print(f"extract_streamkeys: {err}", file=sys.stderr)
        return 1
    except OSError as err:
        print(f"extract_streamkeys: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
