# Offline Spanish Morphology

Verdad can use two independent local SQLite resources for Spanish hover
translations:

```text
<dictionary folder>/wikdict/es-en.sqlite3
<dictionary folder>/morphology/es-morph-apertium.sqlite3
```

Flat WikDict files in the dictionary folder remain supported. The morphology
pack is optional. Without it, exact and normalized WikDict lookups continue to
work, but inflected forms such as `habló` and `mujeres` may not resolve.

## Generate the complete morphology pack

Install the `apertium-spa` package, then run this command from the Verdad source
directory:

```bash
python3 tools/import_apertium_spanish_morphology.py
```

This is the default `expand-dix` mode. It automatically locates
`apertium-spa.spa.dix`, expands every finite paradigm in its main dictionary,
and writes the result to Verdad's default dictionary directory. No wordlist is
required. On Linux the output is:

```text
~/.config/verdad/dictionaries/morphology/es-morph-apertium.sqlite3
~/.config/verdad/dictionaries/morphology/es-morph-apertium.NOTICE
```

The full verified Spanish source expands to approximately 1.8 million distinct
single-token analyses in a roughly 55 MB SQLite database. On the development
machine this takes about 17 seconds. Multiword phrases are excluded because
hover lookup is word-based. Regex entries for arbitrary numbers, URLs, email
addresses, and IP addresses are also excluded because those represent infinite
languages rather than enumerable morphology.

To use a nonstandard Apertium installation or output location:

```bash
python3 tools/import_apertium_spanish_morphology.py \
  --dictionary /path/to/apertium-spa.spa.dix \
  --output /path/to/es-morph-apertium.sqlite3
```

The older wordlist-analysis mode remains available for small tailored packs:

```bash
python3 tools/import_apertium_spanish_morphology.py \
  --mode analyze-wordlist \
  --wordlist tools/data/spanish_wordlist.txt \
  --analyzer /usr/share/apertium/apertium-spa/spa.automorf.bin \
  --output /tmp/es-morph-apertium.sqlite3
```

Precomputed analyzer output or `surface:lemma<tag>` mappings can be imported
without Apertium installed:

```bash
python3 tools/import_apertium_spanish_morphology.py \
  --mode import-expanded \
  --expanded-file build/apertium-spa-expanded.txt \
  --output dictionaries/morphology/es-morph-apertium.sqlite3 \
  --source-version VERSION \
  --license GPL-2
```

The importer writes source version, URL, license, generation time, schema
version, and generator version into the database. It also writes a neighboring
`es-morph-apertium.NOTICE`; preserve that file when redistributing the pack.
Morphology databases are user-managed and are not included in Verdad releases
or settings exports.

The locally verified `apertium-spa` package version is `1.1.0~r79716-2.1`, and
its language data is licensed under GPL-2 according to
`/usr/share/doc/apertium-spa/copyright`. Verify and pass the metadata for the
source version used on other systems.
