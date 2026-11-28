import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "packaging/windows/voice"))
from gpt_training_runtime import unpack
from voice_download import DownloadError, cache_path, download


class Response(io.BytesIO):
    def __init__(self, data, status, **headers):
        super().__init__(data)
        self.status = status
        self.headers = headers


class VoiceDownloadTests(unittest.TestCase):
    def setUp(self):
        self.content = b"the pinned component"
        self.entry = {
            "path": "model.bin", "url": "https://example.test/model.bin",
            "bytes": len(self.content), "sha256": hashlib.sha256(self.content).hexdigest(),
        }
        self.cache = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.file = cache_path(self.cache, self.entry)
        self.part = self.file.with_suffix(".bin.part")
        self.resume = self.file.with_suffix(".bin.resume.json")
        self.opener = Mock()

    def download(self):
        return download(self.cache, self.entry, Mock(), self.opener)

    def test_matching_range_resumes_and_reuses_verified_file(self):
        self.opener.open.side_effect = [
            Response(self.content[:5], 200, ETag='"version"'),
            Response(self.content[5:], 206, ETag='"version"',
                     **{"Content-Range": f"bytes 5-{len(self.content) - 1}/{len(self.content)}"}),
        ]
        with self.assertRaises(DownloadError):
            self.download()
        self.assertEqual(self.part.read_bytes(), self.content[:5])
        self.assertEqual(self.download().read_bytes(), self.content)
        headers = self.opener.open.call_args.args[0].headers
        self.assertEqual(headers["Range"], "bytes=5-")
        self.assertEqual(headers["If-range"], '"version"')
        self.assertFalse(self.resume.exists())
        self.assertEqual(self.download(), self.file)
        self.assertEqual(self.opener.open.call_count, 2)

    def test_ignored_range_restarts_without_appending_a_second_copy(self):
        self.part.parent.mkdir(parents=True)
        self.part.write_bytes(self.content[:5])
        self.resume.write_text(json.dumps({**self.entry, "etag": '"previous"'}), encoding="utf-8")
        self.opener.open.side_effect = [Response(self.content, 200), Response(self.content, 200)]
        self.assertEqual(self.download().read_bytes(), self.content)
        self.assertNotIn("Range", self.opener.open.call_args.args[0].headers)

    def test_equal_length_content_with_wrong_digest_is_discarded(self):
        self.opener.open.return_value = Response(b"z" * len(self.content), 200)
        with self.assertRaisesRegex(DownloadError, "SHA-256"):
            self.download()
        self.assertFalse(self.part.exists())
        self.assertFalse(self.file.exists())


class TrainingArchiveTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.archive = self.root / "engine.zip"
        self.destination = self.root / "engine"

    def test_pinned_engine_archive_extracts_into_runtime_directory(self):
        with ZipFile(self.archive, "w") as archive:
            archive.writestr("GPT-SoVITS-revision/GPT_SoVITS/s1_train.py", b"123")
        remaining = unpack(self.archive, self.destination, 4, strip_root=True)
        self.assertEqual(remaining, 1)
        self.assertEqual((self.destination / "GPT_SoVITS/s1_train.py").read_bytes(), b"123")

    def test_archive_cannot_escape_runtime_directory(self):
        with ZipFile(self.archive, "w") as archive:
            archive.writestr("GPT-SoVITS-revision/../unexpected.txt", b"123")
        with self.assertRaises(DownloadError):
            unpack(self.archive, self.destination, 4, strip_root=True)
        self.assertFalse((self.root / "unexpected.txt").exists())


if __name__ == "__main__":
    unittest.main()
