#!/usr/bin/env python3
"""Build Verdad's optional Spanish morphology SQLite pack."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Iterable, Iterator, Sequence


SCHEMA_VERSION = 2
GENERATOR_VERSION = "2"
DEFAULT_SOURCE_URL = "https://github.com/apertium/apertium-spa"
DEFAULT_LICENSE = "GPL-2"


@dataclass(frozen=True)
class MorphRecord:
    surface: str
    lemma: str
    pos: str
    features: str
    confidence: int = 100


POS_NAMES = {
    "adj": "adjective",
    "adv": "adverb",
    "det": "determiner",
    "n": "noun",
    "np": "proper noun",
    "num": "numeral",
    "pr": "preposition",
    "prn": "pronoun",
    "vaux": "auxiliary verb",
    "vblex": "verb",
    "vbmod": "modal verb",
    "vbser": "verb",
}

FEATURE_NAMES = {
    "pri": "present",
    "ifi": "preterite",
    "pii": "imperfect",
    "fti": "future",
    "cni": "conditional",
    "prs": "present subjunctive",
    "pis": "imperfect subjunctive",
    "fts": "future subjunctive",
    "imp": "imperative",
    "inf": "infinitive",
    "ger": "gerund",
    "pp": "past participle",
    "p1": "1st person",
    "p2": "2nd person",
    "p3": "3rd person",
    "sg": "singular",
    "pl": "plural",
    "m": "masculine",
    "f": "feminine",
    "mf": "masculine or feminine",
    "sp": "singular or plural",
    "ref": "reflexive",
    "enc": "enclitic",
    "pro": "proclitic",
}


def normalize_token(text: str) -> str:
    text = unicodedata.normalize("NFC", text)
    start = 0
    while start < len(text):
        category = unicodedata.category(text[start])
        if not (text[start].isspace() or category.startswith("P")):
            break
        start += 1
    end = len(text)
    while end > start:
        category = unicodedata.category(text[end - 1])
        if not (text[end - 1].isspace() or category.startswith("P")):
            break
        end -= 1
    return text[start:end].casefold()


def split_unescaped(text: str, delimiter: str) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    escaped = False
    for char in text:
        if escaped:
            current.append(char)
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == delimiter:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    if escaped:
        current.append("\\")
    parts.append("".join(current))
    return parts


def parse_analysis(surface: str, lexical: str, confidence: int) -> MorphRecord | None:
    if not lexical or lexical.startswith("*"):
        return None
    lemma_end = lexical.find("<")
    if lemma_end <= 0:
        return None
    lemma = lexical[:lemma_end]
    tags = re.findall(r"<([^<>]+)>", lexical)
    if not lemma or not tags:
        return None

    pos = next((POS_NAMES[tag] for tag in tags if tag in POS_NAMES), "")
    features: list[str] = []
    person = next((FEATURE_NAMES[tag] for tag in tags if tag in {"p1", "p2", "p3"}), "")
    number = next((FEATURE_NAMES[tag] for tag in tags if tag in {"sg", "pl"}), "")
    for tag in tags:
        if tag in {"p1", "p2", "p3"}:
            feature = person + (f" {number}" if number else "")
        elif person and tag in {"sg", "pl"}:
            continue
        else:
            feature = FEATURE_NAMES.get(tag)
        if feature and feature not in features:
            features.append(feature)
    return MorphRecord(surface, lemma, pos, ", ".join(features), confidence)


def parse_lt_proc_output(text: str) -> list[MorphRecord]:
    records: list[MorphRecord] = []
    for match in re.finditer(r"\^((?:\\.|[^$])*)\$", text, re.DOTALL):
        fields = split_unescaped(match.group(1), "/")
        if len(fields) < 2:
            continue
        surface = fields[0].replace("\n", "").strip()
        if not surface:
            continue
        for index, lexical in enumerate(fields[1:]):
            record = parse_analysis(surface, lexical, max(1, 100 - index * 5))
            if record:
                records.append(record)
    return records


def parse_expanded_input(text: str) -> list[MorphRecord]:
    if "^" in text and "$" in text:
        return parse_lt_proc_output(text)

    records: list[MorphRecord] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            surface, lexical = line.split("\t", 1)
        elif ":" in line:
            surface, lexical = line.split(":", 1)
        else:
            continue
        if "<" in surface and "<" not in lexical:
            surface, lexical = lexical, surface
        record = parse_analysis(surface.strip(), lexical.strip(), 100)
        if record:
            records.append(record)
    return records


class ApertiumDictionaryExpander:
    """Expand finite surface-to-lexical mappings from an Apertium monodix."""

    def __init__(self, path: Path):
        self.path = path
        self.root = ET.parse(path).getroot()
        self.pardefs = {
            pardef.attrib["n"]: pardef
            for pardef in self.root.findall("./pardefs/pardef")
        }
        self.active_paradigms: set[str] = set()

    def render_fragment(self, element: ET.Element, lexical: bool) -> str:
        text = element.text or ""
        for child in element:
            if child.tag == "b":
                text += " "
            elif child.tag == "j":
                text += "+"
            elif child.tag == "a":
                pass
            elif child.tag == "s":
                if lexical:
                    text += f"<{child.attrib.get('n', '')}>"
            else:
                text += self.render_fragment(child, lexical)
            text += child.tail or ""
        return text

    def expand_entry(self, entry: ET.Element) -> list[tuple[str, str]]:
        # RL entries are generation-only. Regex entries represent infinite
        # languages and cannot be materialized into a finite SQLite pack.
        if entry.attrib.get("r") == "RL" or entry.find(".//re") is not None:
            return []

        values = [("", "")]
        for child in entry:
            additions: Sequence[tuple[str, str]] | None = None
            if child.tag == "i":
                identity = self.render_fragment(child, lexical=False)
                additions = [(identity, identity)]
            elif child.tag == "p":
                left = child.find("l")
                right = child.find("r")
                if left is None or right is None:
                    continue
                additions = [
                    (
                        self.render_fragment(left, lexical=False),
                        self.render_fragment(right, lexical=True),
                    )
                ]
            elif child.tag == "par":
                additions = self.expand_paradigm(child.attrib["n"])
            elif child.tag == "re":
                return []

            if additions is not None:
                values = [
                    (left + add_left, right + add_right)
                    for left, right in values
                    for add_left, add_right in additions
                ]
        return values

    @lru_cache(maxsize=None)
    def expand_paradigm(self, name: str) -> tuple[tuple[str, str], ...]:
        if name in self.active_paradigms:
            raise RuntimeError(f"Cyclic Apertium paradigm: {name}")
        paradigm = self.pardefs.get(name)
        if paradigm is None:
            raise RuntimeError(f"Undefined Apertium paradigm: {name}")

        self.active_paradigms.add(name)
        mappings: list[tuple[str, str]] = []
        try:
            for entry in paradigm.findall("./e"):
                mappings.extend(self.expand_entry(entry))
        finally:
            self.active_paradigms.remove(name)
        return tuple(mappings)

    def records(self) -> Iterator[MorphRecord]:
        for section in self.root.findall("./section"):
            if section.attrib.get("id") != "main":
                continue
            for entry in section.findall("./e"):
                for surface, lexical in self.expand_entry(entry):
                    surface = surface.strip()
                    lexical = lexical.strip()
                    if not surface or any(char.isspace() for char in surface):
                        continue
                    record = parse_analysis(surface, lexical, 100)
                    if record:
                        yield record


def expand_apertium_dictionary(path: Path) -> Iterator[MorphRecord]:
    return ApertiumDictionaryExpander(path).records()


def read_wordlist(path: Path) -> list[str]:
    words: list[str] = []
    seen: set[str] = set()
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        word = raw_line.strip()
        if not word or word.startswith("#") or word in seen:
            continue
        seen.add(word)
        words.append(word)
    return words


def analyze_wordlist(words: Sequence[str], lt_proc: str, analyzer: Path) -> list[MorphRecord]:
    if not words:
        return []
    if not analyzer.is_file():
        raise RuntimeError(f"Apertium analyzer not found: {analyzer}")
    executable = shutil.which(lt_proc)
    if not executable:
        raise RuntimeError(f"lt-proc executable not found: {lt_proc}")

    completed = subprocess.run(
        [executable, "-w", str(analyzer)],
        input="\n".join(words) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        message = completed.stderr.strip() or "lt-proc failed"
        raise RuntimeError(message)
    return parse_lt_proc_output(completed.stdout)


def detect_apertium_version() -> str:
    dpkg_query = shutil.which("dpkg-query")
    if not dpkg_query:
        return "unknown"
    completed = subprocess.run(
        [dpkg_query, "-W", "-f=${Version}", "apertium-spa"],
        text=True,
        capture_output=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def default_output_path() -> Path:
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        return base / "Verdad" / "dictionaries" / "morphology" / "es-morph-apertium.sqlite3"
    if sys.platform == "darwin":
        return (
            Path.home()
            / "Library"
            / "Application Support"
            / "Verdad"
            / "dictionaries"
            / "morphology"
            / "es-morph-apertium.sqlite3"
        )
    return (
        Path.home()
        / ".config"
        / "verdad"
        / "dictionaries"
        / "morphology"
        / "es-morph-apertium.sqlite3"
    )


def discover_apertium_dictionary(explicit: Path | None) -> Path:
    if explicit:
        if explicit.is_file():
            return explicit
        raise RuntimeError(f"Apertium Spanish dictionary not found: {explicit}")

    candidates: list[Path] = []
    environment_path = os.environ.get("APERTIUM_SPA_DIX")
    if environment_path:
        candidates.append(Path(environment_path))
    candidates.extend(
        [
            Path("/usr/share/apertium/apertium-spa/apertium-spa.spa.dix"),
            Path("/usr/local/share/apertium/apertium-spa/apertium-spa.spa.dix"),
            Path("/opt/homebrew/share/apertium/apertium-spa/apertium-spa.spa.dix"),
        ]
    )

    dpkg_query = shutil.which("dpkg-query")
    if dpkg_query:
        completed = subprocess.run(
            [dpkg_query, "-L", "apertium-spa"],
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode == 0:
            candidates.extend(
                Path(line)
                for line in completed.stdout.splitlines()
                if line.endswith("apertium-spa.spa.dix")
            )

    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(
        "Could not locate apertium-spa.spa.dix; install apertium-spa or use --dictionary"
    )


def create_database(
    output: Path,
    records: Iterable[MorphRecord],
    source_version: str,
    source_url: str,
    license_name: str,
    source_file: str = "",
) -> int:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.unlink(missing_ok=True)

    generated_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    with sqlite3.connect(temporary) as database:
        database.executescript(
            """
            PRAGMA journal_mode = OFF;
            PRAGMA synchronous = OFF;
            PRAGMA temp_store = MEMORY;
            PRAGMA user_version = 2;
            CREATE TABLE metadata (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE morph_lemmas (
                id INTEGER PRIMARY KEY,
                source_lang TEXT NOT NULL DEFAULT 'es',
                lemma TEXT NOT NULL,
                lemma_norm TEXT NOT NULL,
                pos TEXT NOT NULL,
                provider TEXT NOT NULL DEFAULT 'apertium-spa',
                UNIQUE(source_lang, lemma_norm, pos, provider)
            );
            CREATE TABLE morph_features (
                id INTEGER PRIMARY KEY,
                features TEXT NOT NULL UNIQUE
            );
            CREATE TABLE morph_forms (
                source_lang TEXT NOT NULL DEFAULT 'es',
                surface_norm TEXT NOT NULL,
                lemma_id INTEGER NOT NULL,
                features_id INTEGER NOT NULL,
                confidence INTEGER NOT NULL DEFAULT 100,
                PRIMARY KEY(source_lang, surface_norm, lemma_id, features_id)
            ) WITHOUT ROWID;
            """
        )
        metadata = {
            "provider": "apertium-spa",
            "generator": "tools/import_apertium_spanish_morphology.py",
            "generator_version": GENERATOR_VERSION,
            "source_url": source_url,
            "source_version": source_version,
            "license": license_name,
            "generated_at": generated_at,
            "schema_version": str(SCHEMA_VERSION),
        }
        if source_file:
            metadata["source_file"] = source_file
        database.executemany(
            "INSERT INTO metadata(key, value) VALUES (?, ?)", metadata.items()
        )
        insert_form_sql = """
            INSERT INTO morph_forms(
                source_lang, surface_norm, lemma_id, features_id, confidence
            ) VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(source_lang, surface_norm, lemma_id, features_id)
            DO UPDATE SET confidence = MAX(confidence, excluded.confidence)
        """
        lemma_ids: dict[tuple[str, str, str, str], int] = {}
        feature_ids: dict[str, int] = {}
        batch = []
        for record in records:
            surface_norm = normalize_token(record.surface)
            lemma_norm = normalize_token(record.lemma)
            if not surface_norm or not lemma_norm:
                continue
            provider = "apertium-spa"
            lemma_key = ("es", lemma_norm, record.pos, provider)
            lemma_id = lemma_ids.get(lemma_key)
            if lemma_id is None:
                cursor = database.execute(
                    """
                    INSERT INTO morph_lemmas(
                        source_lang, lemma, lemma_norm, pos, provider
                    ) VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(source_lang, lemma_norm, pos, provider) DO NOTHING
                    """,
                    (
                        "es",
                        unicodedata.normalize("NFC", record.lemma),
                        lemma_norm,
                        record.pos,
                        provider,
                    ),
                )
                if cursor.rowcount == 1:
                    lemma_id = cursor.lastrowid
                else:
                    lemma_id = database.execute(
                        """
                        SELECT id FROM morph_lemmas
                        WHERE source_lang = ? AND lemma_norm = ?
                          AND pos = ? AND provider = ?
                        """,
                        lemma_key,
                    ).fetchone()[0]
                lemma_ids[lemma_key] = lemma_id

            feature_id = feature_ids.get(record.features)
            if feature_id is None:
                cursor = database.execute(
                    "INSERT INTO morph_features(features) VALUES (?) "
                    "ON CONFLICT(features) DO NOTHING",
                    (record.features,),
                )
                if cursor.rowcount == 1:
                    feature_id = cursor.lastrowid
                else:
                    feature_id = database.execute(
                        "SELECT id FROM morph_features WHERE features = ?",
                        (record.features,),
                    ).fetchone()[0]
                feature_ids[record.features] = feature_id

            batch.append(
                (
                    "es",
                    surface_norm,
                    lemma_id,
                    feature_id,
                    record.confidence,
                )
            )
            if len(batch) >= 10000:
                database.executemany(insert_form_sql, batch)
                batch.clear()
        if batch:
            database.executemany(insert_form_sql, batch)
        database.commit()
        count = database.execute("SELECT COUNT(*) FROM morph_forms").fetchone()[0]
        database.execute("VACUUM")

    temporary.replace(output)
    write_notice(output, source_version, source_url, license_name, generated_at)
    return count


def write_notice(
    output: Path,
    source_version: str,
    source_url: str,
    license_name: str,
    generated_at: str,
) -> None:
    notice = output.with_suffix(".NOTICE")
    notice.write_text(
        "\n".join(
            [
                "Spanish morphology data generated from apertium-spa.",
                f"Source: {source_url}",
                f"Source version: {source_version}",
                f"Generated at: {generated_at}",
                "Generated by: Verdad tools/import_apertium_spanish_morphology.py",
                f"Source data license: {license_name}",
                "The generated database is derived from the selected apertium-spa source.",
                "Preserve this notice when redistributing the database.",
                "",
            ]
        ),
        encoding="utf-8",
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=("expand-dix", "analyze-wordlist", "import-expanded"),
        default="expand-dix",
    )
    parser.add_argument("--dictionary", type=Path)
    parser.add_argument("--wordlist", type=Path)
    parser.add_argument("--expanded-file", type=Path)
    parser.add_argument("--output", type=Path, default=default_output_path())
    parser.add_argument("--lt-proc", default="lt-proc")
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=Path("/usr/share/apertium/apertium-spa/spa.automorf.bin"),
    )
    parser.add_argument("--source-version")
    parser.add_argument("--source-url", default=DEFAULT_SOURCE_URL)
    parser.add_argument("--license", default=DEFAULT_LICENSE)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        source_file = ""
        if args.mode == "expand-dix":
            dictionary = discover_apertium_dictionary(args.dictionary)
            print(f"Expanding complete Apertium Spanish dictionary: {dictionary}")
            records = expand_apertium_dictionary(dictionary)
            source_file = str(dictionary)
        elif args.mode == "analyze-wordlist":
            if not args.wordlist:
                raise RuntimeError("--wordlist is required for analyze-wordlist mode")
            if not args.wordlist.is_file():
                raise RuntimeError(f"Wordlist not found: {args.wordlist}")
            records = analyze_wordlist(
                read_wordlist(args.wordlist), args.lt_proc, args.analyzer
            )
        else:
            if not args.expanded_file:
                raise RuntimeError("--expanded-file is required for import-expanded mode")
            if not args.expanded_file.is_file():
                raise RuntimeError(f"Expanded input not found: {args.expanded_file}")
            records = parse_expanded_input(
                args.expanded_file.read_text(encoding="utf-8")
            )

        if not records:
            raise RuntimeError("No usable morphology records were parsed")
        source_version = args.source_version or detect_apertium_version()
        count = create_database(
            args.output,
            records,
            source_version,
            args.source_url,
            args.license,
            source_file,
        )
    except (OSError, sqlite3.Error, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"Wrote {count} morphology records to {args.output}")
    print(f"Wrote attribution to {args.output.with_suffix('.NOTICE')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
