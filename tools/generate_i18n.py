"""Bake the editable per-locale i18n bundles (i18n/*.json) into a C++ source the
DLL compiles, and VALIDATE that every key exists in every bundle so adding a new
string can never silently drop a locale or leave the UI with an empty string.

Run by build.bat's gen_shared step. Output (gitignored, regenerated):
    src/generated_shared/goblin_i18n_strings.cpp

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
           ("Spanish", "spanish.json", "ES")]
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
    items = [x.strip().split("=")[0].strip() for x in body.split(",")]
    return [x for x in items if x]


def c_escape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            continue
        else:
            out.append(ch)
    return "".join(out)


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
def text_array(name, lang, ids, enum):
    rows = [f'    {{{enum}::{k}, "{c_escape(bundles[lang]["texts"][k])}"}},' for k in ids]
    return f"const TextKV {name}[] = {{\n" + "\n".join(rows) + "\n};"


def toast_array(name, lang, ids):
    rows = [f'    {{ToastId::{k}, L"{c_escape(bundles[lang]["toasts"][k])}"}},' for k in ids]
    return f"const ToastKV {name}[] = {{\n" + "\n".join(rows) + "\n};"


def name_array(name, lang, cat):
    items = bundles[lang][cat]
    rows = [f'    {{"{c_escape(k)}", "{c_escape(v)}"}},' for k, v in items.items()]
    body = ("\n".join(rows) + "\n") if rows else ""
    return f"const NameKV {name}[] = {{\n" + body + "};"


parts = []
parts.append('// GENERATED by tools/generate_i18n.py from i18n/*.json - DO NOT EDIT BY HAND.')
parts.append('#include "goblin_i18n_bundle.hpp"')
parts.append("")
parts.append("namespace goblin::i18n::detail")
parts.append("{")
parts.append("namespace")
parts.append("{")

# texts + toasts per locale (all locales). LABEL tables for ALL locales incl. English
# (English used to fall back to the raw snake_case key, which showed "show_spirits" etc. in
# the UI); COMMENT tables only for non-English (English tooltips fall back to the schema
# comment, which is the canonical English description).
for lang, _fn, suffix in LOCALES:
    parts.append(text_array(f"TEXT_{suffix}", lang, text_ids, "TextId"))
    parts.append(toast_array(f"TOAST_{suffix}", lang, toast_ids))
    parts.append(name_array(f"SECT_LABELS_{suffix}", lang, "section_labels"))
    parts.append(name_array(f"ENTRY_LABELS_{suffix}", lang, "entry_labels"))
    if lang != "English":
        parts.append(name_array(f"SECT_COMMENTS_{suffix}", lang, "section_comments"))
        parts.append(name_array(f"ENTRY_COMMENTS_{suffix}", lang, "entry_comments"))
    parts.append("")

parts.append("template <typename T, size_t N> constexpr size_t cnt(const T (&)[N]) { return N; }")
parts.append("")
# English: emit LABEL tables (so the UI shows human names, not keys); COMMENT tables stay
# null so entry_comment/section_comment fall back to the canonical schema comment.
parts.append("const LocaleBundle BUNDLE_EN{TEXT_EN, cnt(TEXT_EN), TOAST_EN, cnt(TOAST_EN),")
parts.append("    SECT_LABELS_EN, cnt(SECT_LABELS_EN), nullptr, 0, ENTRY_LABELS_EN, cnt(ENTRY_LABELS_EN), nullptr, 0};")
for lang, _fn, suffix in LOCALES:
    if lang == "English":
        continue
    parts.append(f"const LocaleBundle BUNDLE_{suffix}{{TEXT_{suffix}, cnt(TEXT_{suffix}), TOAST_{suffix}, cnt(TOAST_{suffix}),")
    parts.append(f"    SECT_LABELS_{suffix}, cnt(SECT_LABELS_{suffix}), SECT_COMMENTS_{suffix}, cnt(SECT_COMMENTS_{suffix}),")
    parts.append(f"    ENTRY_LABELS_{suffix}, cnt(ENTRY_LABELS_{suffix}), ENTRY_COMMENTS_{suffix}, cnt(ENTRY_COMMENTS_{suffix})}};")
parts.append("}  // namespace")
parts.append("")
parts.append("const LocaleBundle &bundle(Language language)")
parts.append("{")
parts.append("    switch (language)")
parts.append("    {")
for lang, _fn, suffix in LOCALES:
    if lang == "English":
        continue
    parts.append(f"    case Language::{lang}: return BUNDLE_{suffix};")
parts.append("    default: return BUNDLE_EN;")
parts.append("    }")
parts.append("}")
parts.append("}  // namespace goblin::i18n::detail")
parts.append("")

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text("\n".join(parts), encoding="utf-8")
print(f"[generate_i18n] wrote {OUT.relative_to(ROOT)} "
      f"(texts={len(text_ids)} toasts={len(toast_ids)} "
      f"sect={len(en['section_labels'])}/{len(en['section_comments'])} "
      f"entry={len(en['entry_labels'])}/{len(en['entry_comments'])}; bundles validated)")
