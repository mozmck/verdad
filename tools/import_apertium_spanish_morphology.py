#!/usr/bin/env python3
"""Build Verdad's optional Spanish morphology SQLite pack."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sqlite3
import subprocess
import sys
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


GENERATOR_VERSION = "3"
DEFAULT_SOURCE_URL = "https://github.com/apertium/apertium-spa"
DEFAULT_LICENSE = "GPL-2"
PACK_CONFIG = PackConfig(
    source_language="es",
    provider="apertium-spa",
    generator="tools/import_apertium_spanish_morphology.py",
    generator_version=GENERATOR_VERSION,
    notice_title="Spanish morphology data generated from apertium-spa.",
)

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
    lexical_lemma = lexical[:lemma_end]
    tags = re.findall(r"<([^<>]+)>", lexical)
    lemma = lemma_override or lexical_lemma
    if not lemma or not tags or any(char.isspace() for char in lemma):
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
    return parse_lt_proc(text, parse_analysis)


def parse_expanded_input(text: str) -> list[MorphRecord]:
    return parse_expanded(text, parse_analysis)


def expand_apertium_dictionary(path: Path):
    return expand_dictionary(path, parse_analysis)


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
        raise RuntimeError(completed.stderr.strip() or "lt-proc failed")
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
    return pack_output_path("es-morph-apertium.sqlite3")


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
        source_sha256 = ""
        source_commit = ""
        if args.mode == "expand-dix":
            dictionary = discover_apertium_dictionary(args.dictionary)
            print(f"Expanding complete Apertium Spanish dictionary: {dictionary}")
            records = expand_apertium_dictionary(dictionary)
            source_file = str(dictionary)
            source_sha256 = sha256_file(dictionary)
            source_commit = git_commit_for(dictionary)
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
            records = parse_expanded_input(args.expanded_file.read_text(encoding="utf-8"))

        if not records:
            raise RuntimeError("No usable morphology records were parsed")
        source_version = args.source_version or source_commit or detect_apertium_version()
        count = create_database(
            args.output,
            records,
            source_version,
            args.source_url,
            args.license,
            source_file,
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
