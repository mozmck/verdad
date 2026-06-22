import importlib.util
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "import_apertium_spanish_morphology.py"
SPEC = importlib.util.spec_from_file_location("apertium_importer", SCRIPT)
IMPORTER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = IMPORTER
SPEC.loader.exec_module(IMPORTER)


class ImporterTests(unittest.TestCase):
    def test_expands_complete_finite_dictionary_paradigms(self):
        fixture = ROOT / "tests" / "fixtures" / "apertium-mini-spa.dix"
        records = list(IMPORTER.expand_apertium_dictionary(fixture))
        self.assertEqual(
            [(record.surface, record.lemma) for record in records],
            [("habló", "hablar"), ("hablaron", "hablar")],
        )
        self.assertEqual(records[0].features, "preterite, 3rd person singular")

    def test_parses_verified_lt_proc_output(self):
        fixture = (ROOT / "tests" / "fixtures" / "apertium-spa-analyzer.txt")
        records = IMPORTER.parse_lt_proc_output(fixture.read_text(encoding="utf-8"))
        by_surface = {}
        for record in records:
            by_surface.setdefault(record.surface, []).append(record)

        self.assertEqual(by_surface["habló"][0].lemma, "hablar")
        self.assertEqual(by_surface["habló"][0].pos, "verb")
        self.assertEqual(
            by_surface["habló"][0].features,
            "preterite, 3rd person singular",
        )
        self.assertEqual(by_surface["hablaron"][0].lemma, "hablar")
        self.assertEqual(by_surface["dijo"][0].lemma, "decir")
        self.assertEqual(by_surface["mujeres"][0].lemma, "mujer")
        self.assertEqual(len(by_surface["vino"]), 2)

    def test_builds_schema_metadata_and_notice(self):
        fixture = (ROOT / "tests" / "fixtures" / "apertium-spa-analyzer.txt")
        records = IMPORTER.parse_lt_proc_output(fixture.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "es-morph-apertium.sqlite3"
            count = IMPORTER.create_database(
                output,
                records,
                "test-version",
                IMPORTER.DEFAULT_SOURCE_URL,
                "GPL-2",
            )
            self.assertGreaterEqual(count, 6)
            self.assertTrue(output.with_suffix(".NOTICE").is_file())

            with sqlite3.connect(output) as database:
                version = database.execute("PRAGMA user_version").fetchone()[0]
                self.assertEqual(version, 2)
                row = database.execute(
                    "SELECT lemmas.lemma, lemmas.pos, features.features "
                    "FROM morph_forms AS forms "
                    "JOIN morph_lemmas AS lemmas ON lemmas.id = forms.lemma_id "
                    "JOIN morph_features AS features "
                    "ON features.id = forms.features_id "
                    "WHERE forms.surface_norm = ? "
                    "ORDER BY forms.confidence DESC",
                    ("habló",),
                ).fetchone()
                self.assertEqual(
                    row,
                    ("hablar", "verb", "preterite, 3rd person singular"),
                )
                license_name = database.execute(
                    "SELECT value FROM metadata WHERE key = 'license'"
                ).fetchone()[0]
                self.assertEqual(license_name, "GPL-2")

    def test_default_output_matches_linux_verdad_config(self):
        if sys.platform.startswith("linux"):
            self.assertEqual(
                IMPORTER.default_output_path(),
                Path.home()
                / ".config"
                / "verdad"
                / "dictionaries"
                / "morphology"
                / "es-morph-apertium.sqlite3",
            )

    def test_normalizes_decomposed_spanish(self):
        self.assertEqual(IMPORTER.normalize_token("¿Sen\u0303or?"), "señor")
        self.assertEqual(IMPORTER.normalize_token("hablo\u0301"), "habló")


if __name__ == "__main__":
    unittest.main()
