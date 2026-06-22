# Offline German Morphology

Verdad can combine a user-supplied German-to-English WikDict database with an
optional Apertium-derived morphology pack:

```text
<dictionary folder>/wikdict/de-en.sqlite3
<dictionary folder>/morphology/de-morph-apertium.sqlite3
<dictionary folder>/morphology/de-morph-apertium.NOTICE
```

The morphology pack is consulted only when the surface form has no direct
WikDict gloss. Every derived lemma must then resolve through `de-en.sqlite3`
before Verdad displays it. Apertium is not required while Verdad is running.

## Generate the pack

From the Verdad source directory:

```bash
git clone --depth 1 \
  https://github.com/apertium/apertium-deu.git \
  tools/data/apertium-deu

python3 tools/import_apertium_german_morphology.py \
  --dictionary tools/data/apertium-deu/apertium-deu.deu.dix
```

On Linux this writes:

```text
~/.config/verdad/dictionaries/morphology/de-morph-apertium.sqlite3
~/.config/verdad/dictionaries/morphology/de-morph-apertium.NOTICE
```

Use `--output /path/to/de-morph-apertium.sqlite3` for another location. The
generator expands finite, single-token analyses for nouns, cases, gender,
number, verbs, participles, tense, mood, adjective declension, and joined forms
of separable verbs. It excludes regex entries, generation-only entries,
heuristic analyses, compound fragments, multiword forms, and invalid records.

The source inspected on June 9, 2026 produced 337,762 normalized surface forms
and 345,646 grouped lemma/POS records in a 15 MB database. Counts can change as
the upstream dictionary changes.

## Metadata and license

The generator records the upstream source URL, Git commit, dictionary SHA-256,
source license, generator version, schema version, and UTC generation time in
the database. The adjacent notice records the same provenance. The inspected
`apertium-deu` checkout is licensed under GPL-3.0; preserve the notice and meet
the upstream license requirements if the generated database is redistributed.

`tools/data/` is ignored by Git, and the default generated database is outside
the repository. German morphology databases are user-managed data. They are
not included in Verdad source archives, packaged releases, or settings exports.
