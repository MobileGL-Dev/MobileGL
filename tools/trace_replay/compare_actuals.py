#!/usr/bin/env python3
"""Actual-vs-actual SSIM for archived retrace repeats, on the retrace gate's own SSIM.

WHY THIS EXISTS. `result.json` carries ONE number per run: this attempt's actual against its
golden. P7-7 E0 asks a different question - "are three repeats of the same case on the same
device the same picture?" - and no artefact in the tree answers it, because every repeat
overwrites the previous one's `.trace-work/android-retrace-result/<case>-<backend>/`. With the
repeats archived (`run_android_retrace_local.py --archive-dir`), this script computes the
missing pair: SSIM(repeat N, repeat 1) beside the recorded SSIM(repeat N, golden).

THE SSIM IS NOT A NEW ONE. It is a transcription of the retrace gate's own comparator,
`android-plugin/app/src/trace/cpp/trace_replay_core.cpp:630` (`ComputeChannelSsim`) and `:671`
(`ComputeRgbSsim`) - a single global window per channel, C1 = (0.01*255)^2, C2 = (0.03*255)^2,
the three colour channels averaged and alpha ignored - and the crop comes from the repeat's own
`result.json`, so the window is the one the gate scored. A different SSIM (a gaussian-windowed
one, say) would produce numbers that could not be read beside the recorded ones at all, which is
the whole reason this file does not import a library implementation.

Two modes:

    compare_actuals.py compare A.png B.png [--crop-x N --crop-y N --crop-width N --crop-height N]
    compare_actuals.py summary <archive-dir> [--json PATH]
    compare_actuals.py --self-test
"""
from __future__ import annotations

import argparse
import hashlib
import json
from operator import mul
from pathlib import Path
import re
import sys
import zlib

# (0.01 * 255)^2 and (0.03 * 255)^2, verbatim from trace_replay_core.cpp:662-663.
C1 = 6.5025
C2 = 58.5225

_SQUARES = [value * value for value in range(256)]


class ImageError(ValueError):
    """A PNG this reader will not decode, or a pair it will not compare."""


# ---------------------------------------------------------------------------------------------
# PNG decode. Deliberately stdlib-only (zlib): this script has to run in the device window on
# whatever interpreter is at hand, and an unavailable Pillow would turn "the repeats diverged"
# into "the tool did not run", which are not the same finding. The accepted subset is what
# libpng gives trace_replay_core after its own normalisation (ReadPngRgba, :278): 8- and 16-bit
# greyscale/RGB/greyscale+alpha/RGBA and 8-bit palette, non-interlaced. Anything else is
# REFUSED BY NAME rather than guessed at.
# ---------------------------------------------------------------------------------------------
_PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _chunks(data):
    if not data.startswith(_PNG_MAGIC):
        raise ImageError("not a PNG file")
    offset = len(_PNG_MAGIC)
    while offset + 8 <= len(data):
        length = int.from_bytes(data[offset:offset + 4], "big")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length:
            raise ImageError("truncated PNG chunk: " + kind.decode("ascii", "replace"))
        yield kind, body
        offset += 12 + length


def _unfilter(raw, width, height, bytes_per_pixel, row_bytes):
    out = bytearray(height * row_bytes)
    previous = bytearray(row_bytes)
    position = 0
    for y in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + row_bytes])
        position += row_bytes
        if len(line) != row_bytes:
            raise ImageError("truncated PNG scanline")
        if filter_type == 1:
            for index in range(bytes_per_pixel, row_bytes):
                line[index] = (line[index] + line[index - bytes_per_pixel]) & 0xFF
        elif filter_type == 2:
            for index in range(row_bytes):
                line[index] = (line[index] + previous[index]) & 0xFF
        elif filter_type == 3:
            for index in range(row_bytes):
                left = line[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                line[index] = (line[index] + ((left + previous[index]) >> 1)) & 0xFF
        elif filter_type == 4:
            for index in range(row_bytes):
                left = line[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                up = previous[index]
                upper_left = previous[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                estimate = left + up - upper_left
                distance_left = abs(estimate - left)
                distance_up = abs(estimate - up)
                distance_upper_left = abs(estimate - upper_left)
                if distance_left <= distance_up and distance_left <= distance_upper_left:
                    predictor = left
                elif distance_up <= distance_upper_left:
                    predictor = up
                else:
                    predictor = upper_left
                line[index] = (line[index] + predictor) & 0xFF
        elif filter_type != 0:
            raise ImageError("unknown PNG filter type %d" % filter_type)
        out[y * row_bytes:(y + 1) * row_bytes] = line
        previous = line
    return out


class Image:
    """An 8-bit RGBA raster, the shape trace_replay_core compares in."""

    __slots__ = ("width", "height", "pixels")

    def __init__(self, width, height, pixels):
        self.width = width
        self.height = height
        self.pixels = pixels

    def row(self, y):
        stride = self.width * 4
        return self.pixels[y * stride:(y + 1) * stride]


def read_png_rgba(path):
    data = Path(path).read_bytes()
    header = None
    palette = b""
    transparency = b""
    compressed = bytearray()
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            header = body
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            transparency = body
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if header is None or len(header) < 13:
        raise ImageError("PNG has no IHDR: " + str(path))
    width = int.from_bytes(header[0:4], "big")
    height = int.from_bytes(header[4:8], "big")
    depth = header[8]
    color_type = header[9]
    interlace = header[12]
    if width <= 0 or height <= 0:
        raise ImageError("PNG has a zero dimension: " + str(path))
    if interlace != 0:
        raise ImageError("interlaced PNG is not supported: " + str(path))
    if color_type not in _CHANNELS:
        raise ImageError("unsupported PNG colour type %d: %s" % (color_type, path))
    if depth not in (8, 16) or (color_type == 3 and depth != 8):
        raise ImageError("unsupported PNG bit depth %d (colour type %d): %s"
                         % (depth, color_type, path))
    channels = _CHANNELS[color_type]
    sample_bytes = depth // 8
    bytes_per_pixel = channels * sample_bytes
    row_bytes = width * bytes_per_pixel
    raw = _unfilter(zlib.decompress(bytes(compressed)), width, height, bytes_per_pixel, row_bytes)

    pixels = bytearray(width * height * 4)
    for y in range(height):
        source = raw[y * row_bytes:(y + 1) * row_bytes]
        # png_set_strip_16: keep the high byte, exactly as the gate's reader does.
        if sample_bytes == 2:
            source = source[0::2]
        base = y * width * 4
        if color_type == 6:
            pixels[base:base + width * 4] = source
            continue
        for x in range(width):
            out = base + x * 4
            if color_type == 0:
                value = source[x]
                pixels[out] = pixels[out + 1] = pixels[out + 2] = value
                pixels[out + 3] = 255
            elif color_type == 2:
                pixels[out:out + 3] = source[x * 3:x * 3 + 3]
                pixels[out + 3] = 255
            elif color_type == 4:
                value = source[x * 2]
                pixels[out] = pixels[out + 1] = pixels[out + 2] = value
                pixels[out + 3] = source[x * 2 + 1]
            else:  # palette
                index = source[x]
                if (index + 1) * 3 > len(palette):
                    raise ImageError("PNG palette index out of range: " + str(path))
                pixels[out:out + 3] = palette[index * 3:index * 3 + 3]
                pixels[out + 3] = transparency[index] if index < len(transparency) else 255
    return Image(width, height, bytes(pixels))


# ---------------------------------------------------------------------------------------------
# The gate's SSIM, transcribed.
# ---------------------------------------------------------------------------------------------
def channel_ssim(actual, golden, x0, y0, compare_width, compare_height, channel):
    """trace_replay_core.cpp:630 ComputeChannelSsim, one global window over the crop."""
    count = float(compare_width) * float(compare_height)
    sum_a = sum_g = sum_aa = sum_gg = sum_ag = 0
    stride = actual.width * 4
    golden_stride = golden.width * 4
    start_a = x0 * 4 + channel
    stop_a = (x0 + compare_width) * 4 + channel
    for row in range(compare_height):
        base_a = (y0 + row) * stride
        base_g = (y0 + row) * golden_stride
        a = actual.pixels[base_a + start_a:base_a + stop_a:4]
        g = golden.pixels[base_g + start_a:base_g + stop_a:4]
        sum_a += sum(a)
        sum_g += sum(g)
        sum_aa += sum(map(_SQUARES.__getitem__, a))
        sum_gg += sum(map(_SQUARES.__getitem__, g))
        sum_ag += sum(map(mul, a, g))

    mean_a = sum_a / count
    mean_g = sum_g / count
    variance_a = max(0.0, sum_aa / count - mean_a * mean_a)
    variance_g = max(0.0, sum_gg / count - mean_g * mean_g)
    covariance = sum_ag / count - mean_a * mean_g
    luminance = (2.0 * mean_a * mean_g + C1) / (mean_a * mean_a + mean_g * mean_g + C1)
    contrast_structure = (2.0 * covariance + C2) / (variance_a + variance_g + C2)
    return luminance * contrast_structure


def rgb_ssim(actual, golden, x0, y0, compare_width, compare_height):
    """trace_replay_core.cpp:671 ComputeRgbSsim - the three colour channels, alpha ignored."""
    total = 0.0
    for channel in range(3):
        total += channel_ssim(actual, golden, x0, y0, compare_width, compare_height, channel)
    return total / 3.0


def resolve_crop(actual, golden, crop_x=0, crop_y=0, crop_width=0, crop_height=0):
    """CompareAgainstOneGolden's bounds rules (trace_replay_core.cpp:683-717), verbatim.

    In particular: an uncropped pair of DIFFERENT sizes is a refusal, not a silent intersection.
    Two repeats that rendered at different sizes are a finding, and averaging over whichever
    rectangle happened to be common to both would bury it.
    """
    if crop_width <= 0 and crop_height <= 0 and (
            actual.width != golden.width or actual.height != golden.height):
        raise ImageError("image size %dx%d does not match %dx%d"
                         % (actual.width, actual.height, golden.width, golden.height))
    compare_width = crop_width if crop_width > 0 else actual.width
    compare_height = crop_height if crop_height > 0 else actual.height
    if (compare_width <= 0 or compare_height <= 0 or crop_x < 0 or crop_y < 0
            or crop_x + compare_width > actual.width or crop_y + compare_height > actual.height
            or crop_x + compare_width > golden.width or crop_y + compare_height > golden.height):
        raise ImageError("compare crop is outside image bounds")
    return crop_x, crop_y, compare_width, compare_height


def mismatch_pixels(actual, golden, x0, y0, compare_width, compare_height):
    """Exact RGB inequality count, the gate's `mismatchPixels` (trace_replay_core.cpp:719-736)."""
    total = 0
    stride = actual.width * 4
    golden_stride = golden.width * 4
    for row in range(compare_height):
        base_a = (y0 + row) * stride + x0 * 4
        base_g = (y0 + row) * golden_stride + x0 * 4
        a = actual.pixels[base_a:base_a + compare_width * 4]
        g = golden.pixels[base_g:base_g + compare_width * 4]
        if a == g:
            continue
        for index in range(compare_width):
            if a[index * 4:index * 4 + 3] != g[index * 4:index * 4 + 3]:
                total += 1
    return total


def compare_files(left, right, crop_x=0, crop_y=0, crop_width=0, crop_height=0):
    a = read_png_rgba(left)
    b = read_png_rgba(right)
    x0, y0, width, height = resolve_crop(a, b, crop_x, crop_y, crop_width, crop_height)
    return {"left": str(left), "right": str(right), "width": a.width, "height": a.height,
            "crop": {"x": x0, "y": y0, "width": width, "height": height},
            "ssim": rgb_ssim(a, b, x0, y0, width, height),
            "mismatch_pixels": mismatch_pixels(a, b, x0, y0, width, height)}


# ---------------------------------------------------------------------------------------------
# The archive summary.
# ---------------------------------------------------------------------------------------------
REPEAT_DIR = re.compile(r"^repeat-(\d+)$")


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _actual_png(directory):
    candidates = sorted(directory.glob("*-actual.png"))
    return candidates[0] if candidates else None


def summarize(archive_dir):
    """case x repeat x SSIM-vs-golden x SSIM-vs-first-repeat over an --archive-dir tree.

    SSIM-VS-GOLDEN IS READ, NOT RECOMPUTED. It is the number the device wrote into result.json,
    against the golden the device matched; recomputing it here would silently answer a different
    question whenever a run matched `alternate_golden`. SSIM-vs-first-repeat is the one this
    script owns, and it uses the SAME crop result.json recorded, so the two columns describe the
    same rectangle.
    """
    archive = Path(archive_dir)
    if not archive.is_dir():
        raise ImageError("not a directory: " + str(archive))
    rows = []
    for arm in sorted(p for p in archive.iterdir() if p.is_dir()):
        repeats = []
        for child in arm.iterdir():
            match = REPEAT_DIR.match(child.name) if child.is_dir() else None
            if match:
                repeats.append((int(match.group(1)), child))
        repeats.sort(key=lambda pair: pair[0])
        first_actual = None
        first_sha = None
        for index, directory in repeats:
            row = {"arm": arm.name, "repeat": index, "dir": str(directory)}
            result_path = directory / "result.json"
            result = None
            if result_path.is_file():
                try:
                    result = json.loads(result_path.read_text(encoding="utf-8"))
                except ValueError as error:
                    row["error"] = "unreadable result.json: " + str(error)
            else:
                row["error"] = "no result.json"
            if result is not None:
                row.update(backend=result.get("backend"),
                           passed=result.get("passed"), status_code=result.get("statusCode"),
                           ssim_vs_golden=result.get("ssim"),
                           ssim_threshold=result.get("ssimThreshold"),
                           matched_golden=Path(result.get("matchedGoldenPath") or "").name,
                           mismatch_pixels_vs_golden=result.get("mismatchPixels"))
            actual = _actual_png(directory)
            if actual is None:
                row.setdefault("error", "no *-actual.png")
                rows.append(row)
                continue
            row["actual"] = actual.name
            row["actual_sha256"] = digest(actual)
            if first_actual is None:
                first_actual, first_sha = actual, row["actual_sha256"]
            row["identical_to_first"] = row["actual_sha256"] == first_sha
            crop = (int((result or {}).get("cropX", 0) or 0), int((result or {}).get("cropY", 0) or 0),
                    int((result or {}).get("cropWidth", 0) or 0), int((result or {}).get("cropHeight", 0) or 0))
            try:
                comparison = compare_files(actual, first_actual, *crop)
                row["ssim_vs_first"] = comparison["ssim"]
                row["mismatch_pixels_vs_first"] = comparison["mismatch_pixels"]
                row["crop"] = comparison["crop"]
            except (ImageError, OSError) as error:
                row["error"] = "actual-vs-first comparison failed: " + str(error)
            rows.append(row)

    arms = {}
    for row in rows:
        arm = arms.setdefault(row["arm"], {"arm": row["arm"], "repeats": 0, "errors": 0,
                                           "all_bitwise_identical": True, "passed_all": True,
                                           "min_ssim_vs_golden": None, "min_ssim_vs_first": None})
        arm["repeats"] += 1
        if row.get("error"):
            arm["errors"] += 1
        if not row.get("identical_to_first", False):
            arm["all_bitwise_identical"] = False
        if row.get("passed") is not True:
            arm["passed_all"] = False
        for key, source in (("min_ssim_vs_golden", "ssim_vs_golden"), ("min_ssim_vs_first", "ssim_vs_first")):
            value = row.get(source)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                arm[key] = value if arm[key] is None else min(arm[key], value)
    return {"archive": str(archive), "rows": rows, "arms": [arms[name] for name in sorted(arms)]}


def _cell(value, width, decimals=None):
    if value is None:
        text = "-"
    elif isinstance(value, bool):
        text = "yes" if value else "NO"
    elif decimals is not None and isinstance(value, (int, float)):
        text = ("%." + str(decimals) + "f") % value
    else:
        text = str(value)
    return text.ljust(width)


def render_summary(summary, stream=sys.stdout):
    # The arm column is sized from the data. A truncated case name in a divergence table is a
    # name the reader has to guess at, and several of these differ only in their last word.
    arm_width = max([len("arm")] + [len(row["arm"]) for row in summary["rows"]])
    columns = [("arm", arm_width), ("rep", 3), ("pass", 5), ("ssim_vs_golden", 16),
               ("ssim_vs_first", 15), ("bit-identical", 13), ("px!=first", 9)]
    stream.write("  ".join(name.ljust(width) for name, width in columns) + "\n")
    stream.write("  ".join("-" * width for _, width in columns) + "\n")
    for row in summary["rows"]:
        stream.write("  ".join([
            _cell(row["arm"], arm_width), _cell(row["repeat"], 3), _cell(row.get("passed"), 5),
            _cell(row.get("ssim_vs_golden"), 16, 9), _cell(row.get("ssim_vs_first"), 15, 9),
            _cell(row.get("identical_to_first"), 13),
            _cell(row.get("mismatch_pixels_vs_first"), 9)]).rstrip() + "\n")
        if row.get("error"):
            stream.write("    ! " + row["error"] + "\n")
    stream.write("\n")
    for arm in summary["arms"]:
        stream.write("%s: %d repeat(s), %d error(s), all-bitwise-identical=%s, passed-all=%s, "
                     "min ssim vs golden=%s, min ssim vs first=%s\n"
                     % (arm["arm"], arm["repeats"], arm["errors"],
                        "yes" if arm["all_bitwise_identical"] else "NO",
                        "yes" if arm["passed_all"] else "NO",
                        "-" if arm["min_ssim_vs_golden"] is None else "%.9f" % arm["min_ssim_vs_golden"],
                        "-" if arm["min_ssim_vs_first"] is None else "%.9f" % arm["min_ssim_vs_first"]))


# ---------------------------------------------------------------------------------------------
# Self-test: synthetic pairs whose SSIM is a closed form, so the transcription above cannot
# drift without something going red here.
# ---------------------------------------------------------------------------------------------
def _png_bytes(width, height, rgba):
    def chunk(kind, body):
        return (len(body).to_bytes(4, "big") + kind + body
                + zlib.crc32(kind + body).to_bytes(4, "big"))
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * width * 4:(y + 1) * width * 4]
    return (_PNG_MAGIC
            + chunk(b"IHDR", width.to_bytes(4, "big") + height.to_bytes(4, "big")
                    + bytes([8, 6, 0, 0, 0]))
            + chunk(b"IDAT", zlib.compress(bytes(raw)))
            + chunk(b"IEND", b""))


def _solid(width, height, value):
    return bytes([value, value, value, 255] * (width * height))


def self_test():
    import tempfile

    failures = []

    def expect(name, actual, expected, tolerance=1e-12):
        if abs(actual - expected) > tolerance:
            failures.append("%s: got %.17g, expected %.17g" % (name, actual, expected))

    with tempfile.TemporaryDirectory(prefix="compare-actuals-selftest-") as directory:
        root = Path(directory)
        width, height = 8, 4
        black = root / "black.png"
        white = root / "white.png"
        black.write_bytes(_png_bytes(width, height, _solid(width, height, 0)))
        white.write_bytes(_png_bytes(width, height, _solid(width, height, 255)))
        # Identical constant images: luminance and contrast are both exactly 1.
        expect("identical", compare_files(black, black)["ssim"], 1.0, 0.0)
        # Constant 0 vs constant 255: contrast is 1 (no variance anywhere), luminance is
        # C1 / (255^2 + C1).
        expect("black vs white", compare_files(black, white)["ssim"], 9.999000099990002e-05)
        halves = bytearray()
        inverse = bytearray()
        for y in range(height):
            for x in range(width):
                left = 0 if x < width // 2 else 255
                halves += bytes([left, left, left, 255])
                right = 255 - left
                inverse += bytes([right, right, right, 255])
        a = root / "halves.png"
        b = root / "inverse.png"
        a.write_bytes(_png_bytes(width, height, bytes(halves)))
        b.write_bytes(_png_bytes(width, height, bytes(inverse)))
        # Equal means and variances, covariance = -variance: luminance 1, contrast negative.
        expect("inverted halves", compare_files(a, b)["ssim"], -0.9964064683569576)
        if compare_files(a, b)["mismatch_pixels"] != width * height:
            failures.append("inverted halves: every pixel should differ")
        # The crop is honoured: the left half of `halves` is black and the left half of
        # `inverse` is white, so cropping to it reproduces the black/white number.
        cropped = compare_files(a, b, 0, 0, width // 2, height)["ssim"]
        expect("cropped half", cropped, 9.999000099990002e-05)
        # A size mismatch with no crop is a refusal, not an intersection.
        small = root / "small.png"
        small.write_bytes(_png_bytes(4, 4, _solid(4, 4, 0)))
        try:
            compare_files(black, small)
        except ImageError:
            pass
        else:
            failures.append("a size mismatch with no crop was silently compared")

    if failures:
        for line in failures:
            print("compare_actuals self-test: " + line, file=sys.stderr)
        return 1
    print("compare_actuals self-test: all checks passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="mode")
    compare = sub.add_parser("compare", help="SSIM between two actual PNGs")
    compare.add_argument("left", type=Path)
    compare.add_argument("right", type=Path)
    compare.add_argument("--crop-x", type=int, default=0)
    compare.add_argument("--crop-y", type=int, default=0)
    compare.add_argument("--crop-width", type=int, default=0)
    compare.add_argument("--crop-height", type=int, default=0)
    summary = sub.add_parser("summary", help="case x repeat table over an --archive-dir tree")
    summary.add_argument("archive", type=Path)
    summary.add_argument("--json", type=Path, help="also write the table as JSON")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        if args.mode == "compare":
            print(json.dumps(compare_files(args.left, args.right, args.crop_x, args.crop_y,
                                           args.crop_width, args.crop_height),
                             indent=2, allow_nan=False))
            return 0
        if args.mode == "summary":
            report = summarize(args.archive)
            render_summary(report)
            if args.json:
                args.json.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n",
                                     encoding="utf-8")
            return 0 if all(not row.get("error") for row in report["rows"]) else 1
    except (ImageError, OSError) as error:
        print("compare_actuals: " + str(error), file=sys.stderr)
        return 2
    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
