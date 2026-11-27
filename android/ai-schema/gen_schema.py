#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only
"""Builds the settings schema of the AI per-game config (see README.md).

    python -I android/ai-schema/gen_schema.py <repo root> --list            # every switch found, for tiering
    python -I android/ai-schema/gen_schema.py <repo root> <out.json>        # the schema (allow + risky only)

Sources:
  1. The app's per-game core options: the CoreOption enum in Settings.kt (values and English titles).
  2. Every `CachedValue x("debug.capy.<name>", default)` under core/src, with its default and the
     comment closest to it. These are the hidden switches the settings screens do not show.
tiers.json (next to this file) decides what the AI may touch and gives the descriptions it sees.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PREFIX = "debug.capy."
SOURCE_EXT = (".cpp", ".h", ".hpp", ".inc", ".cc")

DECL = re.compile(r'CachedValue\s+(\w+)\s*[({]\s*"debug\.capy\.([a-z0-9_]+)"\s*,', re.S)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def strings_xml(path):
    out = {}
    for m in re.finditer(r'<string name="([^"]+)"[^>]*>(.*?)</string>', read(path), re.S):
        text = m.group(2).replace("\\'", "'").replace('\\"', '"').replace("&amp;", "&")
        out[m.group(1)] = re.sub(r"\s+", " ", text).strip()
    return out


def balanced_arg(text, pos):
    """The default expression: text from pos up to the closing bracket of the declaration."""
    depth = 0
    for i in range(pos, len(text)):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return text[pos:i].strip(), i
            depth -= 1
    return text[pos:].strip(), len(text)


def parse_int(expr):
    e = expr.strip()
    e = re.sub(r"^(static_cast<\w+>|int64_t|uint64_t)\((.*)\)$", r"\2", e)
    e = re.sub(r"(?<=\d)[uUlL]+$", "", e)
    try:
        return int(e, 0)
    except ValueError:
        return None


def nearest_comment(lines, line_index, decl_end_line):
    """Comment on the declaration's own lines, else the comment block right above it."""
    for i in range(line_index, decl_end_line + 1):
        m = re.search(r"//\s?(.*)$", lines[i])
        if m and '"debug.capy.' not in m.group(1):
            return m.group(1).strip()
    block = []
    i = line_index - 1
    while i >= 0 and len(block) < 6:
        s = lines[i].strip()
        if s.startswith("//"):
            block.insert(0, s[2:].strip())
        elif s.startswith("*") or s.startswith("/*") or s.endswith("*/"):
            block.insert(0, s.strip("/* ").strip())
        else:
            break
        i -= 1
    return " ".join(x for x in block if x)


def scan_core(root):
    found = {}
    src = os.path.join(root, "core", "src")
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = [d for d in dirnames if d.lower() not in ("3rdparty", "thirdparty", "external")]
        for name in sorted(filenames):
            if not name.endswith(SOURCE_EXT):
                continue
            path = os.path.join(dirpath, name)
            text = read(path)
            if "CachedValue" not in text:
                continue
            lines = text.split("\n")
            for m in DECL.finditer(text):
                var, prop = m.group(1), m.group(2)
                expr, end = balanced_arg(text, m.end())
                line = text.count("\n", 0, m.start())
                end_line = text.count("\n", 0, end)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                uses = re.findall(r"\b%s\.Get\(\)\s*([!=<>]=?|\))?\s*(-?\w+)?" % re.escape(var), text)
                entry = found.setdefault(prop, {"key": prop, "sites": []})
                entry["sites"].append({
                    "file": rel, "line": line + 1, "default_expr": expr,
                    "default": parse_int(expr), "comment": nearest_comment(lines, line, end_line),
                    "uses": ["%s %s" % u for u in uses],
                })
    for entry in found.values():
        defaults = {s["default"] for s in entry["sites"]}
        entry["default"] = entry["sites"][0]["default"] if len(defaults) == 1 else None
        entry["conflicting_defaults"] = len(defaults) > 1
        entry["comment"] = next((s["comment"] for s in entry["sites"] if s["comment"]), "")
        # A switch only ever compared with 0 (or read as a truth value) is an on/off switch.
        ops = [u.strip() for s in entry["sites"] for u in s["uses"]]
        entry["looks_bool"] = bool(ops) and all(o in ("!= 0", "== 0", ") ", ")") for o in ops)
    return found


def scan_app_options(root, strings):
    path = os.path.join(root, "android", "app", "src", "main", "java", "org", "capyps5", "emulator", "Settings.kt")
    text = read(path)
    body = text[text.index("enum class CoreOption("):]
    body = body[body.index(") {") + 3: body.index("\n    ;")]
    options = []
    for m in re.finditer(r"\n    (\w+)\(\s*R\.string\.(\w+),\s*R\.string\.(\w+),(.*?)\n    \),", body, re.S):
        name, title_res, desc_res, rest = m.groups()
        prop = re.search(r'"debug\.capy\.([a-z0-9_]+)"', rest).group(1)
        choices = re.findall(r"(-?\w+?)L? to R\.string\.(\w+)", rest)
        default = re.findall(r"\n\s*(-?[\w]+?)L?,\s*$", rest.rstrip())
        options.append({
            "option": name, "key": prop, "title_res": title_res,
            "title": strings.get(title_res, name), "description": strings.get(desc_res, ""),
            "choices": [(c[0], strings.get(c[1], c[1])) for c in choices],
            "default_text": default[-1] if default else None,
        })
    return options


def resolve(value, consts):
    v = parse_int(value)
    if v is None:
        v = consts.get(value)
    return v


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    root, out = sys.argv[1], sys.argv[2]
    strings = strings_xml(os.path.join(root, "android", "app", "src", "main", "res", "values", "strings.xml"))
    tiers = json.loads(read(os.path.join(HERE, "tiers.json"))) if os.path.exists(os.path.join(HERE, "tiers.json")) else {}
    consts = {"AUTO_LANGUAGE": -1}
    app = scan_app_options(root, strings)
    core = scan_core(root)

    if out == "--list":
        tiered = {k for t in ("allow", "risky", "preference") for k in tiers.get(t, [])} | set(tiers.get("never_reasons", {}))
        for o in app:
            print("APP\t%s\t%s\t%s" % (o["key"], o["default_text"], o["title"]))
        for key in sorted(core):
            e = core[key]
            s = e["sites"][0]
            mark = "" if key in tiered else "UNTIERED "
            print("%s%s\tdef=%s%s\tbool=%s\t%s:%d\t%s" % (
                mark, key, e["default"], " CONFLICT" if e["conflicting_defaults"] else "",
                e["looks_bool"], s["file"], s["line"], e["comment"][:160]))
        return 0

    allow, risky = set(tiers.get("allow", [])), set(tiers.get("risky", []))
    known = {o["key"] for o in app} | set(core)
    missing = sorted((allow | risky) - known)
    if missing:
        print("tiers.json names switches that no longer exist: %s" % ", ".join(missing), file=sys.stderr)
        return 1
    descriptions = tiers.get("descriptions", {})
    types = tiers.get("types", {})
    settings = []
    app_keys = set()
    for o in app:
        app_keys.add(o["key"])
        tier = "allow" if o["key"] in allow else "risky" if o["key"] in risky else None
        if tier is None:
            continue
        enum = []
        for value, label in o["choices"]:
            v = resolve(value, consts)
            enum.append({"name": label, "value": v})
        default = resolve(o["default_text"], consts)
        settings.append({
            "key": o["key"], "tier": tier, "type": "enum", "default": str(default), "enum": enum,
            "ui_title": o["title"], "ui_title_res": o["title_res"], "source": "app",
            "description": descriptions.get(o["key"], o["description"]),
        })
    for key in sorted(core):
        if key in app_keys:
            continue
        tier = "allow" if key in allow else "risky" if key in risky else None
        if tier is None:
            continue
        e = core[key]
        t = types.get(key, {"type": "bool"} if e["looks_bool"] else None)
        if t is None:
            print("%s is not an on/off switch: give it a type with min/max in tiers.json" % key, file=sys.stderr)
            return 1
        if e["conflicting_defaults"] and "default" not in t:
            print("%s has different defaults in different places: set one in tiers.json" % key, file=sys.stderr)
            return 1
        default = t.get("default", e["default"])
        if default is None:
            print("%s has a default the generator cannot read: set one in tiers.json" % key, file=sys.stderr)
            return 1
        entry = {"key": key, "tier": tier, "type": t["type"]}
        if t["type"] == "bool":
            entry["default"] = "true" if default else "false"
        else:
            entry["default"] = str(default)
            entry["min"], entry["max"] = str(t["min"]), str(t["max"])
            if "enum" in t:
                entry["enum"] = t["enum"]
        entry["source"] = "hidden"
        entry["description"] = descriptions.get(key) or e["comment"]
        if not entry["description"]:
            print("%s has no description: add one to tiers.json" % key, file=sys.stderr)
            return 1
        settings.append(entry)
    schema = {"app": "capyps5", "schema_version": tiers.get("schema_version", 1), "settings": settings}
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(schema, f, indent=1, ensure_ascii=False)
        f.write("\n")
    counts = {t: sum(1 for s in settings if s["tier"] == t) for t in ("allow", "risky")}
    print("%d settings (%d allow, %d risky) from %d app options and %d core switches" % (
        len(settings), counts["allow"], counts["risky"], len(app), len(core)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
