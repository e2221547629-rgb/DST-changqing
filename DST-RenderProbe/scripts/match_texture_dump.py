"""Match top-mip BC3 DDS payloads against locally extracted TEX fingerprints."""
import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path


def match_dump(directory, fingerprints):
    known = json.loads(Path(fingerprints).read_text(encoding="utf-8-sig"))
    rows = list(csv.DictReader((Path(directory) / "textures.csv").open(encoding="utf-8-sig", newline="")))
    result = []
    for row in rows:
        item = dict(row, matches=[])
        if int(row["read_hresult"]) != 0 or not row["file"]:
            item["status"] = "read_failed; not evidence of mismatch"
        else:
            name = row["file"]
            if Path(name).name != name or "/" in name or "\\" in name:
                raise ValueError("Invalid dump filename")
            data = (Path(directory) / name).read_bytes()
            if len(data) < 128 or data[:4] != b"DDS ":
                raise ValueError("Invalid DDS header")
            h = struct.unpack_from("<31I", data, 4)
            width, height = int(row["width"]), int(row["height"])
            expected = ((width + 3) // 4) * ((height + 3) // 4) * 16
            if (h[0], h[2], h[3], h[18], h[20]) != (124, height, width, 32, 0x35545844):
                raise ValueError("DDS metadata mismatch")
            if len(data) != 128 + expected or int(row["bytes"]) != expected:
                raise ValueError("DDS payload length mismatch")
            digest = hashlib.sha256(data[128:]).hexdigest()
            item["payload_sha256"] = digest
            item["matches"] = [dict(package=k["package"], atlas=k["atlas"]) for k in known
                if k["width"] == width and k["height"] == height
                and k["top_level_compressed_payload_sha256"] == digest]
            item["status"] = "exact_compressed_content_match" if item["matches"] else "no_exact_compressed_match"
        result.append(item)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_dir", type=Path)
    parser.add_argument("fingerprints", type=Path)
    args = parser.parse_args()
    print(json.dumps(match_dump(args.dump_dir, args.fingerprints), ensure_ascii=False, indent=2))
