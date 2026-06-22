#!/usr/bin/env python3
"""Shared helpers for building Verdad Apertium morphology packs."""

from __future__ import annotations

import datetime as dt
import hashlib
import os
import re
import sqlite3
import subprocess
import sys
import unicodedata
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Callable, Iterable, Iterator, Sequence


SCHEMA_VERSION = 2


@dataclass(frozen=True)
class MorphRecord:
    surface: str
    lemma: str
    pos: str
    features: str
    confidence: int = 100


@dataclass(frozen=True)
class PackConfig:
    source_language: str
    provider: str
    generator: str
    generator_version: str
    notice_title: str
    aggregate_variants: bool = False


AnalysisParser = Callable[[str, str, int, str], MorphRecord | None]


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
    return text[start:end].lower()


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


def parse_lt_proc_output(text: str, parser: AnalysisParser) -> list[MorphRecord]:
    records: list[MorphRecord] = []
    for match in re.finditer(r"\^((?:\\.|[^$])*)\$", text, re.DOTALL):
        fields = split_unescaped(match.group(1), "/")
        if len(fields) < 2:
            continue
        surface = fields[0].replace("\n", "").strip()
        if not surface:
            continue
        for index, lexical in enumerate(fields[1:]):
            record = parser(surface, lexical, max(1, 100 - index * 5), "")
            if record:
                records.append(record)
    return records


def parse_expanded_input(text: str, parser: AnalysisParser) -> list[MorphRecord]:
    if "^" in text and "$" in text:
        return parse_lt_proc_output(text, parser)

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
        record = parser(surface.strip(), lexical.strip(), 100, "")
        if record:
            records.append(record)
    return records


class ApertiumDictionaryExpander:
    """Expand finite surface-to-lexical mappings from an Apertium monodix."""

    def __init__(self, path: Path, parser: AnalysisParser):
        self.path = path
        self.parser = parser
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
                lemma_override = entry.attrib.get("lm", "").strip()
                for surface, lexical in self.expand_entry(entry):
                    surface = surface.strip()
                    lexical = lexical.strip()
                    if not surface or any(char.isspace() for char in surface):
                        continue
                    record = self.parser(
                        surface, lexical, 100, lemma_override
                    )
                    if record:
                        yield record


def expand_apertium_dictionary(
        path: Path, parser: AnalysisParser) -> Iterator[MorphRecord]:
    return ApertiumDictionaryExpander(path, parser).records()


def default_output_path(file_name: str) -> Path:
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        return base / "Verdad" / "dictionaries" / "morphology" / file_name
    if sys.platform == "darwin":
        return (
            Path.home()
            / "Library"
            / "Application Support"
            / "Verdad"
            / "dictionaries"
            / "morphology"
            / file_name
        )
    return Path.home() / ".config" / "verdad" / "dictionaries" / "morphology" / file_name


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_commit_for(path: Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(path.parent), "rev-parse", "HEAD"],
        text=True,
        capture_output=True,
        check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else ""


def create_database(
    output: Path,
    records: Iterable[MorphRecord],
    config: PackConfig,
    source_version: str,
    source_url: str,
    license_name: str,
    source_file: str = "",
    source_sha256: str = "",
    source_commit: str = "",
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
                source_lang TEXT NOT NULL,
                lemma TEXT NOT NULL,
                lemma_norm TEXT NOT NULL,
                pos TEXT NOT NULL,
                provider TEXT NOT NULL,
                UNIQUE(source_lang, lemma_norm, pos, provider)
            );
            CREATE TABLE morph_features (
                id INTEGER PRIMARY KEY,
                features TEXT NOT NULL UNIQUE
            );
            CREATE TABLE morph_forms (
                source_lang TEXT NOT NULL,
                surface_norm TEXT NOT NULL,
                lemma_id INTEGER NOT NULL,
                features_id INTEGER NOT NULL,
                confidence INTEGER NOT NULL DEFAULT 100,
                PRIMARY KEY(source_lang, surface_norm, lemma_id, features_id)
            ) WITHOUT ROWID;
            """
        )
        if config.aggregate_variants:
            database.executescript(
                """
                CREATE TABLE morph_form_variants (
                    source_lang TEXT NOT NULL,
                    surface_norm TEXT NOT NULL,
                    lemma_id INTEGER NOT NULL,
                    features TEXT NOT NULL,
                    confidence INTEGER NOT NULL DEFAULT 100,
                    PRIMARY KEY(
                        source_lang, surface_norm, lemma_id, features
                    )
                ) WITHOUT ROWID;
                """
            )
        metadata = {
            "provider": config.provider,
            "generator": config.generator,
            "generator_version": config.generator_version,
            "source_url": source_url,
            "source_version": source_version,
            "license": license_name,
            "generated_at": generated_at,
            "schema_version": str(SCHEMA_VERSION),
        }
        if config.aggregate_variants:
            metadata["variant_separator"] = " | "
        if source_file:
            metadata["source_file"] = source_file
        if source_sha256:
            metadata["source_sha256"] = source_sha256
        if source_commit:
            metadata["source_commit"] = source_commit
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
        insert_variant_sql = """
            INSERT INTO morph_form_variants(
                source_lang, surface_norm, lemma_id, features, confidence
            ) VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(source_lang, surface_norm, lemma_id, features)
            DO UPDATE SET confidence = MAX(confidence, excluded.confidence)
        """
        lemma_ids: dict[tuple[str, str, str, str], int] = {}
        feature_ids: dict[str, int] = {}

        def feature_id_for(features: str) -> int:
            feature_id = feature_ids.get(features)
            if feature_id is not None:
                return feature_id
            cursor = database.execute(
                "INSERT INTO morph_features(features) VALUES (?) "
                "ON CONFLICT(features) DO NOTHING",
                (features,),
            )
            if cursor.rowcount == 1:
                feature_id = cursor.lastrowid
            else:
                feature_id = database.execute(
                    "SELECT id FROM morph_features WHERE features = ?",
                    (features,),
                ).fetchone()[0]
            feature_ids[features] = feature_id
            return feature_id

        batch = []
        for record in records:
            surface_norm = normalize_token(record.surface)
            lemma_norm = normalize_token(record.lemma)
            if not surface_norm or not lemma_norm:
                continue
            lemma_key = (
                config.source_language,
                lemma_norm,
                record.pos,
                config.provider,
            )
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
                        config.source_language,
                        unicodedata.normalize("NFC", record.lemma),
                        lemma_norm,
                        record.pos,
                        config.provider,
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

            if config.aggregate_variants:
                batch.append(
                    (
                        config.source_language,
                        surface_norm,
                        lemma_id,
                        record.features,
                        record.confidence,
                    )
                )
            else:
                batch.append(
                    (
                        config.source_language,
                        surface_norm,
                        lemma_id,
                        feature_id_for(record.features),
                        record.confidence,
                    )
                )
            if len(batch) >= 10000:
                database.executemany(
                    insert_variant_sql if config.aggregate_variants
                    else insert_form_sql,
                    batch,
                )
                batch.clear()
        if batch:
            database.executemany(
                insert_variant_sql if config.aggregate_variants
                else insert_form_sql,
                batch,
            )

        if config.aggregate_variants:
            form_batch = []
            current_key: tuple[str, int] | None = None
            variants: list[str] = []
            confidence = 0

            def flush_variants() -> None:
                nonlocal variants, confidence
                if current_key is None:
                    return
                combined = " | ".join(variant for variant in variants if variant)
                form_batch.append(
                    (
                        config.source_language,
                        current_key[0],
                        current_key[1],
                        feature_id_for(combined),
                        confidence,
                    )
                )
                if len(form_batch) >= 10000:
                    database.executemany(insert_form_sql, form_batch)
                    form_batch.clear()
                variants = []
                confidence = 0

            rows = database.execute(
                "SELECT surface_norm, lemma_id, features, confidence "
                "FROM morph_form_variants "
                "ORDER BY surface_norm, lemma_id, features"
            )
            for surface_norm, lemma_id, features, row_confidence in rows:
                key = (surface_norm, lemma_id)
                if current_key != key:
                    flush_variants()
                    current_key = key
                variants.append(features)
                confidence = max(confidence, row_confidence)
            flush_variants()
            if form_batch:
                database.executemany(insert_form_sql, form_batch)
            database.execute("DROP TABLE morph_form_variants")

        database.commit()
        count = database.execute("SELECT COUNT(*) FROM morph_forms").fetchone()[0]
        database.execute("VACUUM")

    temporary.replace(output)
    write_notice(
        output,
        config,
        source_version,
        source_url,
        license_name,
        generated_at,
        source_sha256,
        source_commit,
    )
    return count


def write_notice(
    output: Path,
    config: PackConfig,
    source_version: str,
    source_url: str,
    license_name: str,
    generated_at: str,
    source_sha256: str = "",
    source_commit: str = "",
) -> None:
    lines = [
        config.notice_title,
        f"Source: {source_url}",
        f"Source version: {source_version}",
    ]
    if source_commit:
        lines.append(f"Source commit: {source_commit}")
    if source_sha256:
        lines.append(f"Source dictionary SHA-256: {source_sha256}")
    lines.extend(
        [
            f"Generated at: {generated_at}",
            f"Generated by: Verdad {config.generator}",
            f"Generator version: {config.generator_version}",
            f"Source data license: {license_name}",
            f"The generated database is derived from {config.provider} source data.",
            "Preserve this notice when redistributing the database.",
            "",
        ]
    )
    output.with_suffix(".NOTICE").write_text("\n".join(lines), encoding="utf-8")
