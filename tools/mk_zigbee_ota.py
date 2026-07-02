#!/usr/bin/env python3
import argparse
import hashlib
import json
import struct
from pathlib import Path

OTA_MAGIC = 0x0BEEF11E
HEADER_VERSION = 0x0100
HEADER_LENGTH = 56
ZIGBEE_STACK_VERSION = 0x0002
HEADER_STRING_LENGTH = 32


def parse_u16(value: str) -> int:
    return int(value, 0) & 0xFFFF


def parse_u32(value: str) -> int:
    return int(value, 0) & 0xFFFFFFFF


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a Zigbee OTA image and Zigbee2MQTT index.")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--index", required=True, type=Path)
    parser.add_argument("--url-base", required=True)
    parser.add_argument("--manufacturer-code", default="0x131B", type=parse_u16)
    parser.add_argument("--image-type", default="0x0001", type=parse_u16)
    parser.add_argument("--file-version", required=True, type=parse_u32)
    parser.add_argument("--manufacturer-name", default="ZigbeeHive")
    parser.add_argument("--model-id", default="WaterMeter")
    parser.add_argument("--comment", default="")
    args = parser.parse_args()

    firmware = args.input.read_bytes()
    image_size = HEADER_LENGTH + len(firmware)
    header_string = b"ZigbeeHive WaterMeter"[:HEADER_STRING_LENGTH].ljust(HEADER_STRING_LENGTH, b"\x00")
    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_MAGIC,
        HEADER_VERSION,
        HEADER_LENGTH,
        0,
        args.manufacturer_code,
        args.image_type,
        args.file_version,
        ZIGBEE_STACK_VERSION,
        header_string,
        image_size,
    )
    if len(header) != HEADER_LENGTH:
        raise RuntimeError(f"Unexpected Zigbee OTA header size: {len(header)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    ota_image = header + firmware
    args.output.write_bytes(ota_image)

    file_url = f"{args.url_base.rstrip('/')}/{args.output.name}"
    index = [
        {
            "fileVersion": args.file_version,
            "fileSize": image_size,
            "url": file_url,
            "manufacturerCode": args.manufacturer_code,
            "imageType": args.image_type,
            "manufacturerName": args.manufacturer_name,
            "modelId": args.model_id,
            "sha512": hashlib.sha512(ota_image).hexdigest(),
            "comment": args.comment,
        }
    ]
    args.index.parent.mkdir(parents=True, exist_ok=True)
    args.index.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
