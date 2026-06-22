#!/usr/bin/env python3
"""Build Verdad's optional German morphology SQLite pack."""

from __future__ import annotations

import argparse
import os
import re
import sqlite3
import sys
import unicodedata
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from apertium_morphology import (  # noqa: E402
    MorphRecord,
    PackConfig,
    create_database as create_pack_database,
    default_output_path as pack_output_path,
    expand_apertium_dictionary as expand_dictionary,
    git_commit_for,
    normalize_token,
    parse_expanded_input as parse_expanded,
    parse_lt_proc_output as parse_lt_proc,
    sha256_file,
)


GENERATOR_VERSION = "1"
DEFAULT_SOURCE_URL = "https://github.com/apertium/apertium-deu"
DEFAULT_LICENSE = "GPL-3.0"
PACK_CONFIG = PackConfig(
    source_language="de",
    provider="apertium-deu",
    generator="tools/import_apertium_german_morphology.py",
    generator_version=GENERATOR_VERSION,
    notice_title="German morphology data generated from apertium-deu.",
    aggregate_variants=True,
)

POS_NAMES = {
    "adj": "adjective",
    "adv": "adverb",
    "cnjadv": "adverbial conjunction",
    "cnjcoo": "coordinating conjunction",
    "cnjsub": "subordinating conjunction",
    "det": "determiner",
    "ij": "interjection",
    "n": "noun",
    "np": "proper noun",
    "num": "numeral",
    "pr": "preposition",
    "pprep": "postposition",
    "preadv": "pre-adverb",
    "prn": "pronoun",
    "vaux": "auxiliary verb",
    "vbhaver": "verb",
    "vblex": "verb",
    "vbmod": "modal verb",
    "vbser": "verb",
}

FEATURE_NAMES = {
    "acc": "accusative",
    "an": "animate",
    "attr": "attributive",
    "comp": "comparative",
    "dat": "dative",
    "f": "feminine",
    "fm": "main-clause separated form",
    "frm": "formal",
    "fs": "subordinate-clause joined form",
    "gen": "genitive",
    "ger": "verbal noun",
    "imp": "imperative",
    "inf": "infinitive",
    "irr": "irregular",
    "ito": "infinitive with zu",
    "m": "masculine",
    "mf": "masculine or feminine",
    "mfn": "masculine, feminine, or neuter",
    "mix": "mixed adjective declension",
    "nn": "inanimate",
    "nom": "nominative",
    "nt": "neuter",
    "ord": "ordinal",
    "p1": "1st person",
    "p2": "2nd person",
    "p3": "3rd person",
    "pers": "personal",
    "pii": "past indicative",
    "pis": "past subjunctive",
    "pl": "plural",
    "pp": "past participle",
    "pprs": "present participle",
    "pred": "predicative",
    "pri": "present indicative",
    "prs": "present subjunctive",
    "pst": "positive",
    "ref": "reflexive",
    "sep": "separable verb",
    "sg": "singular",
    "sint": "synthetic",
    "sp": "singular or plural",
    "st": "strong adjective declension",
    "sup": "superlative",
    "sw": "weak adjective declension",
    "un": "unspecified gender",
}

EXCLUDED_TAGS = {
    "atp",
    "cmp",
    "cmp-split",
    "compound-only-L",
    "compound-R",
    "heur",
}


def valid_word(text: str) -> bool:
    return bool(text) and not any(
        char.isspace() or unicodedata.category(char).startswith("C")
        for char in text
    )


def parse_analysis(
    surface: str,
    lexical: str,
    confidence: int,
    lemma_override: str = "",
) -> MorphRecord | None:
    if not lexical or lexical.startswith("*"):
        return None
    lemma_end = lexical.find("<")
    if lemma_end <= 0:
        return None
    tags = re.findall(r"<([^<>]+)>", lexical)
    if not tags or EXCLUDED_TAGS.intersection(tags):
        return None

    lemma = lemma_override or lexical[:lemma_end]
    if not valid_word(surface) or not valid_word(lemma):
        return None
    pos = next((POS_NAMES[tag] for tag in tags if tag in POS_NAMES), "")
    if not pos:
        return None

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
    return parse_lt_proc(text, parse_analysis)


def parse_expanded_input(text: str) -> list[MorphRecord]:
    return parse_expanded(text, parse_analysis)


def expand_apertium_dictionary(path: Path):
    return expand_dictionary(path, parse_analysis)


def default_output_path() -> Path:
    return pack_output_path("de-morph-apertium.sqlite3")


def discover_apertium_dictionary(explicit: Path | None) -> Path:
    if explicit:
        if explicit.is_file():
            return explicit
        raise RuntimeError(f"Apertium German dictionary not found: {explicit}")

    candidates: list[Path] = []
    environment_path = os.environ.get("APERTIUM_DEU_DIX")
    if environment_path:
        candidates.append(Path(environment_path))
    candidates.extend(
        [
            Path("tools/data/apertium-deu/apertium-deu.deu.dix"),
            Path("/usr/share/apertium/apertium-deu/apertium-deu.deu.dix"),
            Path("/usr/local/share/apertium/apertium-deu/apertium-deu.deu.dix"),
            Path("/opt/homebrew/share/apertium/apertium-deu/apertium-deu.deu.dix"),
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(
        "Could not locate apertium-deu.deu.dix; clone apertium-deu or use --dictionary"
    )


def create_database(
    output: Path,
    records,
    source_version: str,
    source_url: str,
    license_name: str,
    source_file: str = "",
    source_sha256: str = "",
    source_commit: str = "",
) -> int:
    return create_pack_database(
        output,
        records,
        PACK_CONFIG,
        source_version,
        source_url,
        license_name,
        source_file,
        source_sha256,
        source_commit,
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dictionary", type=Path)
    parser.add_argument("--output", type=Path, default=default_output_path())
    parser.add_argument("--source-version")
    parser.add_argument("--source-url", default=DEFAULT_SOURCE_URL)
    parser.add_argument("--source-commit")
    parser.add_argument("--license", default=DEFAULT_LICENSE)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        dictionary = discover_apertium_dictionary(args.dictionary)
        source_commit = args.source_commit or git_commit_for(dictionary)
        source_version = args.source_version or source_commit or "unknown"
        source_sha256 = sha256_file(dictionary)
        print(f"Expanding complete Apertium German dictionary: {dictionary}")
        count = create_database(
            args.output,
            expand_apertium_dictionary(dictionary),
            source_version,
            args.source_url,
            args.license,
            str(dictionary),
            source_sha256,
            source_commit,
        )
    except (OSError, sqlite3.Error, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"Wrote {count} morphology records to {args.output}")
    print(f"Wrote attribution to {args.output.with_suffix('.NOTICE')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
