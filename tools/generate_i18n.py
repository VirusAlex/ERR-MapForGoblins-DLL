"""Bake the editable per-locale i18n bundles (i18n/*.json) into a C++ source the
DLL compiles, and VALIDATE that every key exists in every bundle so adding a new
string can never silently drop a locale or leave the UI with an empty string.

Run by build.bat's gen_shared step. Output (gitignored, regenerated):
    src/generated_shared/goblin_i18n_strings.cpp
The bundles are packed and deflated there (tools/textblob.py has the layout), not
written out as string literals; src/goblin_i18n_blob.cpp expands them on first use.

Validation (fails the build, non-zero exit):
  * en.json `texts`/`toasts` keys must exactly match the TextId/ToastId enums.
  * every locale must carry the exact same key set as en.json, in every category.
English names (section/entry labels+comments) are implicit (fall back to the key
/ the schema comment), so only SC/TC name tables are emitted; en.json still lists
them as the canonical key set + translator reference.
"""
import json
import re
import sys
from pathlib import Path

import textblob

ROOT = Path(__file__).resolve().parent.parent
I18N_DIR = ROOT / "i18n"
HPP = ROOT / "src" / "goblin_i18n.hpp"
OUT = ROOT / "src" / "generated_shared" / "goblin_i18n_strings.cpp"

# (Language enum name, bundle filename, C-identifier suffix). English is the
# fallback base (no name tables emitted); every other locale gets full tables.
# To add a language: add its enum value in goblin_i18n.hpp, a bundle JSON here,
# and one row below - the emit + bundle() switch are generated from this list.
LOCALES = [("English", "en.json", "EN"),
           ("SimplifiedChinese", "schinese.json", "SC"),
           ("TraditionalChinese", "tchinese.json", "TC"),
           ("Korean", "korean.json", "KO"),
           ("Russian", "russian.json", "RU"),
           ("German", "german.json", "DE"),
           ("French", "french.json", "FR"),
           ("Spanish", "spanish.json", "ES"),
           ("Vietnamese", "vietnamese.json", "VI")]
CATEGORIES = ["texts", "toasts", "section_labels", "section_comments",
              "entry_labels", "entry_comments"]


def die(msg):
    print(f"[generate_i18n] ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_enum(src, name):
    m = re.search(r"enum class\s+" + name + r"\s*\{([^}]*)\}", src)
    if not m:
        die(f"could not find enum class {name} in {HPP}")
    body = re.sub(r"//.*", "", m.group(1))
    # The packed table stores each id as its position here, so an enumerator with an explicit
    # value would read back as the wrong one. (The static_asserts emitted below check the same.)
    if "=" in body:
        die(f"enum class {name} assigns an explicit value; the packed table stores positions")
    items = [x.strip() for x in body.split(",")]
    return [x for x in items if x]


def compiled_text(s):
    # The text as the DLL has always had it: the C++ literals this used to emit dropped every \r,
    # so the packed table drops it too.
    return s.replace("\r", "")


# ── load bundles ──
bundles = {}
for lang, fn, _suffix in LOCALES:
    p = I18N_DIR / fn
    if not p.exists():
        die(f"missing bundle: {p}")
    data = json.loads(p.read_text(encoding="utf-8"))
    for cat in CATEGORIES:
        data.setdefault(cat, {})
    bundles[lang] = data

# ── validation ──
hpp_src = HPP.read_text(encoding="utf-8")
text_ids = parse_enum(hpp_src, "TextId")
text_ids_set = set(text_ids)
toast_ids = parse_enum(hpp_src, "ToastId")
toast_ids_set = set(toast_ids)

en = bundles["English"]
errors = []

# 1) en.json texts/toasts must match the enums exactly.
for cat, ids_set in (("texts", text_ids_set), ("toasts", toast_ids_set)):
    have = set(en[cat])
    want = ids_set
    for k in want - have:
        errors.append(f"en.json[{cat}] is MISSING enum key '{k}' (UI would show an empty string)")
    for k in have - want:
        errors.append(f"en.json[{cat}] has key '{k}' with no matching {('TextId' if cat=='texts' else 'ToastId')} enum value")

# 2) every locale must have the exact same keys as en.json, in every category.
for lang, _fn, _suffix in LOCALES:
    if lang == "English":
        continue
    for cat in CATEGORIES:
        have = set(bundles[lang][cat])
        base = set(en[cat])
        for k in base - have:
            errors.append(f"{lang}[{cat}] is MISSING key '{k}' that en.json has (localization would be lost)")
        for k in have - base:
            errors.append(f"{lang}[{cat}] has extra key '{k}' not in en.json (stale/typo)")

if errors:
    print(f"[generate_i18n] {len(errors)} bundle key error(s):", file=sys.stderr)
    for e in errors:
        print("  - " + e, file=sys.stderr)
    sys.exit(1)


# ── emit ──
# One locale per LOCALES row, English first: it is the bundle every Language without one of its
# own falls back to (the `default:` of the switch this file used to emit). Texts + toasts for
# every locale, in enum order. LABEL tables for ALL locales incl. English (English used to fall
# back to the raw snake_case key, which showed "show_spirits" etc. in the UI); COMMENT tables
# only for non-English (English tooltips fall back to the schema comment, which is the canonical
# English description), so English packs them as absent and its bundle holds nullptr there.
languages = parse_enum(hpp_src, "Language")
if LOCALES[0][0] != "English":
    die("LOCALES must start with English, the fallback bundle")


def named_rows(lang, cat):
    return [(compiled_text(k), compiled_text(v)) for k, v in bundles[lang][cat].items()]


locales = []
for lang, _fn, _suffix in LOCALES:
    if lang not in languages:
        die(f"LOCALES names Language::{lang}, which goblin_i18n.hpp does not declare")
    b = bundles[lang]
    locales.append((languages.index(lang), {
        "texts": [(i, compiled_text(b["texts"][k])) for i, k in enumerate(text_ids)],
        "toasts": [(i, compiled_text(b["toasts"][k])) for i, k in enumerate(toast_ids)],
        "sect_labels": named_rows(lang, "section_labels"),
        "sect_comments": None if lang == "English" else named_rows(lang, "section_comments"),
        "entry_labels": named_rows(lang, "entry_labels"),
        "entry_comments": None if lang == "English" else named_rows(lang, "entry_comments"),
    }))

# The blob carries each id as its position in the enum. These pin every position to its NAME, so
# a header edit without a regenerated blob fails the compile instead of shifting the strings.
asserts = ["// Every packed id, by name: the blob stores positions (see tools/textblob.py)."]
for enum, names in (("Language", [l for l, _f, _s in LOCALES]), ("TextId", text_ids),
                    ("ToastId", toast_ids)):
    ordinal = languages if enum == "Language" else names
    asserts += [f"static_assert(static_cast<int>({enum}::{n}) == {ordinal.index(n)});" for n in names]
asserts.append("")

OUT.parent.mkdir(parents=True, exist_ok=True)
raw_len, packed_len = textblob.write_cpp(
    textblob.pack_i18n(locales), OUT, "I18N_BLOB", namespace="goblin::i18n::detail",
    generator="tools/generate_i18n.py from i18n/*.json",
    what="The overlay and ini text bundles, packed and deflated; src/goblin_i18n_blob.cpp expands them.",
    includes=["goblin_i18n_bundle.hpp"], extra=asserts)
print(f"[generate_i18n] wrote {OUT.relative_to(ROOT)} "
      f"(texts={len(text_ids)} toasts={len(toast_ids)} "
      f"sect={len(en['section_labels'])}/{len(en['section_comments'])} "
      f"entry={len(en['entry_labels'])}/{len(en['entry_comments'])}; bundles validated; "
      f"{raw_len / 1024:.0f} KB packed -> {packed_len / 1024:.0f} KB deflated)")
