"""Match top-mip BC3 DDS payloads against locally extracted TEX fingerprints."""
import argparse
import csv
import hashlib
import json
import struct
from pathlib import Path


def compare_pixels(actual, reference):
    from PIL import Image, ImageChops
    best = None
    for orientation, source in [("native", reference), ("vertical_flip", reference.transpose(Image.Transpose.FLIP_TOP_BOTTOM))]:
        histogram = ImageChops.difference(actual, source).histogram()
        maximum = max(i % 256 for i, count in enumerate(histogram) if count)
        differing = sum(count for i, count in enumerate(histogram) if i % 256)
        result = dict(orientation=orientation, max_channel_error=maximum, differing_channels=differing)
        if best is None or (maximum, differing) < (best["max_channel_error"], best["differing_channels"]):
            best = result
    return best


def match_dump(directory, fingerprints, reference_root=None):
    known = json.loads(Path(fingerprints).read_text(encoding="utf-8-sig"))
    with (Path(directory) / "textures.csv").open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    result = []
    references = {}
    for row in rows:
        item = dict(row, matches=[])
        if int(row["read_hresult"]) != 0 or not row["file"]:
            item["status"] = "read_failed; not evidence of mismatch"
        else:
            name = row["file"]
            if Path(name).name != name or "/" in name or "\\" in name:
                raise ValueError("Invalid dump filename")
            data = (Path(directory) / name).read_bytes()
            if Path(name).suffix.lower() == ".tga":
                from PIL import Image
                width, height = int(row["width"]), int(row["height"])
                if len(data) != 18 + width * height * 4 or len(data) < 18:
                    raise ValueError("TGA payload length mismatch")
                if (data[0], data[1], data[2], data[16], data[17]) != (0, 0, 2, 32, 0x28):
                    raise ValueError("Unexpected TGA format/orientation")
                if struct.unpack_from("<HH", data, 12) != (width, height) or int(row["bytes"]) != width * height * 4:
                    raise ValueError("TGA metadata mismatch")
                with Image.open(Path(directory) / name) as image:
                    actual = image.convert("RGBA")
                item["decoded_rgba_sha256"] = hashlib.sha256(actual.tobytes()).hexdigest()
                item["comparisons"] = []
                if reference_root is not None:
                    for k in known:
                        if k["width"] != width or k["height"] != height:
                            continue
                        package, atlas = k["package"], Path(k["atlas"]).with_suffix(".dds").name
                        if Path(package).name != package or "/" in package or "\\" in package:
                            raise ValueError("Invalid reference package")
                        path = Path(reference_root) / package / atlas
                        key = str(path)
                        if key not in references:
                            with Image.open(path) as source:
                                if source.size != (width, height):
                                    raise ValueError("Reference dimensions mismatch")
                                references[key] = source.convert("RGBA")
                        comparison = dict(package=package, atlas=k["atlas"], **compare_pixels(actual, references[key]))
                        item["comparisons"].append(comparison)
                        if comparison["max_channel_error"] <= 2:
                            item["matches"].append(comparison)
                item["status"] = ("decoded_pixel_match_with_tolerance_2" if item["matches"] else
                    "no_decoded_pixel_match" if reference_root is not None else "read_ok_pixels_need_reference")
                item["limits"] = "Full RGBA pixels compared, including transparent RGB; both vertical orientations checked. " \
                    "Tolerance allows decoder rounding; this is not an exact compressed-payload match."
                result.append(item)
                continue
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
    parser.add_argument("--reference-root", type=Path, help="Local extracted package directories with original DDS atlases; Pillow required for TGA")
    args = parser.parse_args()
    print(json.dumps(match_dump(args.dump_dir, args.fingerprints, args.reference_root), ensure_ascii=False, indent=2))

