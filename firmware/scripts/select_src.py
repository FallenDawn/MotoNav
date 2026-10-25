from pathlib import Path

Import("env")


env_name = env.subst("$PIOENV")
board = {
    "esp32s3-175": "175",
    "esp32s3-185b": "185b",
    "esp32c3_128x160": "esp32c3_128x160",
}.get(env_name)
if board is None:
    raise RuntimeError("Unsupported MotoNav environment: " + env_name)
source_dir = Path(env.subst("$PROJECT_DIR")) / "boards" / board / "MotoNav"

if board == "185b":
    audio_path = source_dir / "1.wav"
    audio_header = source_dir / "BootAudioData.h"
    if not audio_path.is_file():
        raise RuntimeError("Missing 185B boot audio: " + str(audio_path))
    data = audio_path.read_bytes()
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const uint8_t motonav_boot_wav[] PROGMEM = {",
    ]
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append("  " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
    lines.extend([
        "};",
        "static constexpr size_t motonav_boot_wav_len = sizeof(motonav_boot_wav);",
        "",
    ])
    audio_header.write_text("\n".join(lines), encoding="ascii", newline="\n")

# Select the board source directory directly. This keeps the board-specific
# C++ entry point and headers together and avoids wrapper translation units.
env.Replace(PROJECT_SRC_DIR=str(source_dir))
env.Replace(PROJECTSRC_DIR=str(source_dir))
