"""Convert the connected/disconnected home GIFs to indexed 50x50 frames."""

from pathlib import Path
import sys

from PIL import Image, ImageSequence


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "boards" / "185b" / "MotoNav" / "assets"
DEFAULT_OUTPUT = ROOT / "boards" / "185b" / "MotoNav" / "HomeGifAnimation.cpp"
WIDTH = HEIGHT = 50
CONNECTED_FRAMES = 17


def read_frames(path: Path, expected: int):
    gif = Image.open(path)
    actual = getattr(gif, "n_frames", 1)
    if actual != expected:
        raise SystemExit(f"Expected {expected} frames in {path}, found {actual}")
    frames = []
    for frame in ImageSequence.Iterator(gif):
        image = frame.convert("RGBA").resize((WIDTH, HEIGHT), Image.Resampling.NEAREST)
        frames.append((image, max(20, int(frame.info.get("duration", 120)))))
    return frames


def rows(values, size=25):
    return [", ".join(str(v) for v in values[i:i + size]) for i in range(0, len(values), size)]


def main() -> None:
    connected_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ASSETS / "213.gif"
    disconnected_path = Path(sys.argv[2]) if len(sys.argv) > 2 else ASSETS / "222.gif"
    output = Path(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_OUTPUT
    connected = read_frames(connected_path, CONNECTED_FRAMES)
    disconnected = read_frames(disconnected_path, getattr(Image.open(disconnected_path), "n_frames", 1))
    all_frames = connected + disconnected

    sample = Image.new("RGB", (WIDTH * len(all_frames), HEIGHT))
    for index, (frame, _) in enumerate(all_frames):
        rgb = Image.new("RGB", frame.size, (0, 0, 0))
        rgb.paste(frame.convert("RGB"), mask=frame.getchannel("A"))
        sample.paste(rgb, (index * WIDTH, 0))
    palette_image = sample.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    palette = palette_image.getpalette()[:255 * 3]
    palette += [0] * (255 * 3 - len(palette))
    colors = [((palette[i] >> 3) << 11) | ((palette[i + 1] >> 2) << 5) | (palette[i + 2] >> 3)
              for i in range(0, 255 * 3, 3)]

    def indexed_frames(frames):
        result = []
        for frame, _ in frames:
            rgb = Image.new("RGB", frame.size, (0, 0, 0))
            rgb.paste(frame.convert("RGB"), mask=frame.getchannel("A"))
            indexed = rgb.quantize(palette=palette_image)
            values = list(indexed.get_flattened_data())
            alpha = frame.getchannel("A").get_flattened_data()
            result.append([index if opaque else 255 for index, opaque in zip(values, alpha)])
        return result

    def emit_pixels(name, frames):
        lines = [f"const uint8_t {name}[{len(frames)}][HOME_GIF_W * HOME_GIF_H] = {{"]
        for frame in frames:
            lines.append("  {")
            lines.extend("    " + line + "," for line in rows(frame))
            lines.append("  },")
        lines.append("};")
        return lines

    def emit_durations(name, frames):
        lines = [f"const uint16_t {name}[{len(frames)}] = {{"]
        lines.extend("  " + ", ".join(str(frames[i][1]) for i in range(start, min(start + 12, len(frames)))) + ","
                     for start in range(0, len(frames), 12))
        lines.append("};")
        return lines

    connected_data = indexed_frames(connected)
    disconnected_data = indexed_frames(disconnected)
    generated = ["// BEGIN GENERATED GIF FRAMES", "const uint16_t HOME_GIF_PALETTE[255] = {"]
    generated.extend("  " + ", ".join(f"0x{value:04X}" for value in colors[i:i + 12]) + ","
                     for i in range(0, len(colors), 12))
    generated.extend(["};", ""])
    generated.extend(emit_pixels("HOME_CONNECTED_PIXELS", connected_data))
    generated.append("")
    generated.extend(emit_durations("HOME_CONNECTED_FRAME_MS", connected))
    generated.append("")
    generated.extend(emit_pixels("HOME_DISCONNECTED_PIXELS", disconnected_data))
    generated.append("")
    generated.extend(emit_durations("HOME_DISCONNECTED_FRAME_MS", disconnected))
    generated.append("// END GENERATED GIF FRAMES")

    source = output.read_text(encoding="ascii")
    begin = source.index("// BEGIN GENERATED GIF FRAMES")
    end = source.index("// END GENERATED GIF FRAMES", begin) + len("// END GENERATED GIF FRAMES")
    output.write_text(source[:begin] + "\n".join(generated) + source[end:], encoding="ascii", newline="\n")
    print(f"Wrote {output}: connected {len(connected)} frames, disconnected {len(disconnected)} frames at {WIDTH}x{HEIGHT}")


if __name__ == "__main__":
    main()
