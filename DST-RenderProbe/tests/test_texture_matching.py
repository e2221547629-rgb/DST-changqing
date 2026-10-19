"""Offline generated fixtures; no game assets. Run with Python and Pillow."""
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from PIL import Image

spec = importlib.util.spec_from_file_location("matcher", Path(__file__).parents[1] / "scripts/match_texture_dump.py")
matcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matcher)


class MatchingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        reference = self.root / "refs/test"
        reference.mkdir(parents=True)
        payload = b"".join(bytes([i*85, i*85]) + bytes(6) + struct.pack("<HHI", color, color, 0)
            for i, color in enumerate([0xf800, 0x07e0, 0x001f, 0xffff]))
        header = [0]*31
        header[0], header[1], header[2], header[3], header[4], header[6] = 124, 0x81007, 8, 8, 64, 1
        header[18], header[19], header[20], header[26] = 32, 4, 0x35545844, 0x1000
        (reference / "atlas-0.dds").write_bytes(b"DDS " + struct.pack("<31I", *header) + payload)
        known = [dict(package="test", atlas="atlas-0.tex", width=8, height=8,
            top_level_compressed_payload_sha256=hashlib.sha256(payload).hexdigest())]
        (self.root / "fingerprints.json").write_text(json.dumps(known))
        with Image.open(reference / "atlas-0.dds") as image:
            self.original = image.convert("RGBA")
        self.row = dict(texture_id=1, width=8, height=8, read_hresult=0, bytes=256, file="actual.tga")
        self.save(self.original)

    def save(self, image):
        header = bytearray(18)
        header[2], header[12], header[14], header[16], header[17] = 2, 8, 8, 32, 0x28
        (self.root / "actual.tga").write_bytes(header + image.tobytes("raw", "BGRA"))

    def match(self):
        with (self.root / "textures.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(self.row))
            writer.writeheader()
            writer.writerow(self.row)
        return matcher.match_dump(self.root, self.root / "fingerprints.json", self.root / "refs")[0]

    def test_exact_and_orientation(self):
        self.assertEqual(self.match()["matches"][0]["max_channel_error"], 0)
        self.save(self.original.transpose(Image.Transpose.FLIP_TOP_BOTTOM))
        self.assertEqual(self.match()["matches"][0]["orientation"], "vertical_flip")

    def test_transparent_rgb_tolerance_and_blank(self):
        image = self.original.copy()
        image.putpixel((0, 0), (253, 0, 0, 0))
        self.save(image)
        self.assertEqual(self.match()["matches"][0]["max_channel_error"], 2)
        image.putpixel((0, 0), (252, 0, 0, 0))
        self.save(image)
        self.assertFalse(self.match()["matches"])
        self.save(Image.new("RGBA", (8, 8), (0, 0, 0, 0)))
        self.assertFalse(self.match()["matches"])

    def test_truncation_and_filename(self):
        (self.root / "actual.tga").write_bytes(b"bad")
        with self.assertRaises(ValueError): self.match()
        self.row["file"] = "../actual.tga"
        with self.assertRaises(ValueError): self.match()

    def test_read_failure_is_not_mismatch(self):
        self.row.update(read_hresult=0x8876086c, bytes=0, file="")
        self.assertTrue(self.match()["status"].startswith("read_failed"))


if __name__ == "__main__":
    unittest.main()
