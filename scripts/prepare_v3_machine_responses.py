"""Generate a private build header from AMIIBO_V3_RESPONSES_B64."""
import base64
import json
import os
from pathlib import Path


def crc16(data):
    crc = 0xffff
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc


def main():
    encoded = os.environ.get("AMIIBO_V3_RESPONSES_B64", "")
    if not encoded:
        raise ValueError("Configure the AMIIBO_V3_RESPONSES_B64 build secret")
    responses = json.loads(base64.b64decode(encoded, validate=True))
    if not responses:
        raise ValueError("Machine response bundle is empty")
    lines = ["#pragma once", "#include <array>", "#include <cstdint>",
             "namespace amiibo_library {",
             "struct V3MachineResponse { std::uint32_t head, tail; std::array<std::uint8_t, 64> bytes; };",
             f"inline constexpr std::array<V3MachineResponse, {len(responses)}> V3_MACHINE_RESPONSES{{{{"]
    for identity, encoded_response in sorted(responses.items()):
        if len(identity) != 16 or any(c not in "0123456789abcdef" for c in identity):
            raise ValueError("Invalid Amiibo identity in machine response bundle")
        response = base64.b64decode(encoded_response, validate=True)
        if len(response) != 64 or crc16(response[:62]) != int.from_bytes(response[62:], "big"):
            raise ValueError("Invalid machine response size or CRC")
        values = ",".join(f"0x{value:02x}" for value in response)
        lines.append(f"{{0x{identity[:8]}u, 0x{identity[8:]}u, {{{values}}}}},")
    lines.extend(["}};", "}"])
    output = Path(__file__).resolve().parents[1] / "server/generated/amiibo_v3_machine_responses.hpp"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n")
    print(f"Prepared {len(responses)} v3 machine responses")

    encoded_factories = os.environ.get("AMIIBO_V3_FACTORY_B64", "")
    if not encoded_factories:
        raise ValueError("Configure the AMIIBO_V3_FACTORY_B64 build secret")
    factories = json.loads(base64.b64decode(encoded_factories, validate=True))
    if not factories:
        raise ValueError("V3 factory image bundle is empty")
    lines = ["#pragma once", "#include <array>", "#include <cstdint>",
             "namespace amiibo_library {",
             "struct V3FactoryImage { std::uint32_t head, tail; std::array<std::uint8_t, 2048> bytes; };",
             f"inline constexpr std::array<V3FactoryImage, {len(factories)}> V3_FACTORY_IMAGES{{{{"]
    for identity, encoded_image in sorted(factories.items()):
        if len(identity) != 16 or any(c not in "0123456789abcdef" for c in identity):
            raise ValueError("Invalid Amiibo identity in v3 factory image bundle")
        image = base64.b64decode(encoded_image, validate=True)
        if (len(image) != 2048 or image[0] != 4 or image[7:9] != b"\x00\x44"
                or image[0x54:0x5c].hex() != identity
                or crc16(image[0x3c0:0x3fe]) != int.from_bytes(image[0x3fe:0x400], "big")):
            raise ValueError("Invalid v3 factory image identity, size, or machine CRC")
        values = ",".join(f"0x{value:02x}" for value in image)
        lines.append(f"{{0x{identity[:8]}u, 0x{identity[8:]}u, {{{values}}}}},")
    lines.extend(["}};", "}"])
    factory_output = output.with_name("amiibo_v3_factory_images.hpp")
    factory_output.write_text("\n".join(lines) + "\n")
    print(f"Prepared {len(factories)} v3 factory images")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Never include decoded secret data in CI diagnostics.
        raise SystemExit("Invalid or missing v3 machine-response build secret")
