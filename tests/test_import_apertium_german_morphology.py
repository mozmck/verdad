import importlib.util
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "import_apertium_german_morphology.py"
SPEC = importlib.util.spec_from_file_location("german_apertium_importer", SCRIPT)
IMPORTER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = IMPORTER
SPEC.loader.exec_module(IMPORTER)


class GermanImporterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = ROOT / "tests" / "fixtures" / "apertium-mini-deu.dix"
        cls.records = list(IMPORTER.expand_apertium_dictionary(fixture))
        cls.by_surface = {}
        for record in cls.records:
            cls.by_surface.setdefault(record.surface, []).append(record)

    def test_maps_nouns_cases_gender_and_number(self):
        frauen = self.by_surface["Frauen"]
        self.assertEqual({record.lemma for record in frauen}, {"Frau"})
        self.assertEqual({record.pos for record in frauen}, {"noun"})
        self.assertEqual(
            {record.features for record in frauen},
            {
                "feminine, plural, nominative",
                "feminine, plural, genitive",
                "feminine, plural, dative",
                "feminine, plural, accusative",
            },
        )
        self.assertEqual(self.by_surface["Häuser"][0].lemma, "Haus")
        self.assertEqual(
            self.by_surface["Kindern"][0].features,
            "neuter, plural, dative",
        )

    def test_maps_verbs_participles_and_adjective_declension(self):
        self.assertEqual(
            self.by_surface["gegangen"][0].features,
            "past participle",
        )
        self.assertEqual(
            self.by_surface["war"][0].features,
            "past indicative, 3rd person singular",
        )
        self.assertEqual(
            self.by_surface["größeren"][0].features,
            "synthetic, comparative, masculine, singular, accusative, "
            "mixed adjective declension",
        )

    def test_preserves_ambiguity_capitalization_and_separable_verbs(self):
        self.assertEqual(
            {(record.lemma, record.pos) for record in self.by_surface["band"]},
            {("Band", "noun"), ("binden", "verb")},
        )
        self.assertEqual(self.by_surface["frauen"][0].lemma, "Frau")
        separable = self.by_surface["aufgestanden"][0]
        self.assertEqual(separable.lemma, "aufstehen")
        self.assertIn("separable verb", separable.features)
        self.assertNotIn("steht auf", self.by_surface)

    def test_excludes_non_runtime_records(self):
        surfaces = set(self.by_surface)
        self.assertNotIn("Heuristiken", surfaces)
        self.assertNotIn("haus", surfaces)
        self.assertNotIn("erzeugt", surfaces)
        self.assertNotIn("Zahl", surfaces)

    def test_builds_german_schema_metadata_and_notice(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "de-morph-apertium.sqlite3"
            count = IMPORTER.create_database(
                output,
                self.records,
                "test-commit",
                IMPORTER.DEFAULT_SOURCE_URL,
                "GPL-3.0",
                "apertium-deu.deu.dix",
                "abc123",
                "test-commit",
            )
            self.assertEqual(count, 10)
            notice = output.with_suffix(".NOTICE").read_text(encoding="utf-8")
            self.assertIn("German morphology", notice)
            self.assertIn("Source commit: test-commit", notice)
            self.assertIn("Source dictionary SHA-256: abc123", notice)

            with sqlite3.connect(output) as database:
                self.assertEqual(database.execute("PRAGMA user_version").fetchone()[0], 2)
                metadata = dict(database.execute("SELECT key, value FROM metadata"))
                self.assertEqual(metadata["provider"], "apertium-deu")
                self.assertEqual(metadata["source_commit"], "test-commit")
                self.assertEqual(metadata["source_sha256"], "abc123")
                self.assertEqual(metadata["license"], "GPL-3.0")
                row = database.execute(
                    "SELECT lemmas.lemma, lemmas.pos, features.features "
                    "FROM morph_forms AS forms "
                    "JOIN morph_lemmas AS lemmas ON lemmas.id = forms.lemma_id "
                    "JOIN morph_features AS features ON features.id = forms.features_id "
                    "WHERE forms.source_lang = 'de' AND forms.surface_norm = 'häuser'"
                ).fetchone()
                self.assertEqual(row, ("Haus", "noun", "neuter, plural, nominative"))
                frauen_features = database.execute(
                    "SELECT features.features "
                    "FROM morph_forms AS forms "
                    "JOIN morph_lemmas AS lemmas ON lemmas.id = forms.lemma_id "
                    "JOIN morph_features AS features ON features.id = forms.features_id "
                    "WHERE forms.source_lang = 'de' AND forms.surface_norm = 'frauen' "
                    "AND lemmas.lemma = 'Frau'"
                ).fetchone()[0]
                self.assertIn("feminine, plural, nominative", frauen_features)
                self.assertIn("feminine, plural, dative", frauen_features)

    def test_default_output_matches_linux_verdad_config(self):
        if sys.platform.startswith("linux"):
            self.assertEqual(
                IMPORTER.default_output_path(),
                Path.home()
                / ".config"
                / "verdad"
                / "dictionaries"
                / "morphology"
                / "de-morph-apertium.sqlite3",
            )

    def test_normalizes_decomposed_german(self):
        self.assertEqual(IMPORTER.normalize_token("Gro\u0308ßeren"), "größeren")


if __name__ == "__main__":
    unittest.main()
