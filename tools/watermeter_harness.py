#!/usr/bin/env python3
"""Operational harness for the zigbee-watermeter project."""

from __future__ import annotations

import argparse
import csv
import difflib
import glob
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Iterable, Mapping, Sequence


ROOT_DIR = Path(__file__).resolve().parent.parent
EXPECTED_IDF_VERSION = "v5.4.1"
TARGET = "esp32c6"
FLASH_SIZE_BYTES = 4 * 1024 * 1024
MAX_OTA_FILE_VERSION = 0xFFFFFFFF
PARTITION_TABLE_PATH = ROOT_DIR / "partitions.csv"
PRODUCTION_BUILD_DIR = ROOT_DIR / "build"
DIAGNOSTIC_BUILD_DIR = ROOT_DIR / "build-diagnostic"
PRODUCTION_SDKCONFIG = ROOT_DIR / "sdkconfig"
DIAGNOSTIC_SDKCONFIG = ROOT_DIR / "sdkconfig.diagnostic"
CANONICAL_CONVERTER = ROOT_DIR / "external_converters" / "zigbeehive-watermeter.mjs"
DEFAULT_OTA_URL_BASE = "https://tabakovKonstantin.github.io/zigbee-watermeter/ota"

DEFAULT_Z2M_HOST = "homelab.lan"
DEFAULT_Z2M_DATA_DIR = PurePosixPath("/mnt/projects/smart_home/zigbee2mqtt-data")
DEFAULT_Z2M_COMPOSE_FILE = PurePosixPath("/mnt/projects/smart_home/compose.yml")
DEFAULT_Z2M_SERVICE = "zigbee2mqtt"
DEFAULT_Z2M_CONTAINER = "zigbee2mqtt"
REMOTE_CONVERTER_NAME = "zigbeehive-watermeter.mjs"

CONFIRM_ERASE_ZIGBEE = "ERASE ZIGBEE"
CONFIRM_ERASE_ALL = "ERASE ALL"
CONFIRM_Z2M_DEPLOY = "DEPLOY Z2M"
CONFIRM_Z2M_ROLLBACK = "ROLLBACK Z2M"


class HarnessError(RuntimeError):
    """Expected operational error with a concise user-facing message."""


@dataclass(frozen=True)
class Partition:
    name: str
    partition_type: str
    subtype: str
    offset: int
    size: int


@dataclass(frozen=True)
class BuildProfile:
    name: str
    build_dir: Path
    sdkconfig: Path
    defaults: tuple[Path, ...]

    def idf_arguments(self) -> list[str]:
        args = ["-B", str(self.build_dir)]
        if self.name == "diagnostic":
            args.extend(
                [
                    f"-DSDKCONFIG={self.sdkconfig}",
                    f"-DSDKCONFIG_DEFAULTS={';'.join(str(path) for path in self.defaults)}",
                ]
            )
        return args


@dataclass(frozen=True)
class IdfEnvironment:
    idf_path: Path
    activation_script: Path
    python_executable: Path

    @property
    def idf_script(self) -> Path:
        return self.idf_path / "tools" / "idf.py"

    @property
    def monitor_script(self) -> Path:
        return self.idf_path / "tools" / "idf_monitor.py"


@dataclass(frozen=True)
class Z2MConfig:
    host: str = DEFAULT_Z2M_HOST
    data_dir: PurePosixPath = DEFAULT_Z2M_DATA_DIR
    compose_file: PurePosixPath = DEFAULT_Z2M_COMPOSE_FILE
    service: str = DEFAULT_Z2M_SERVICE
    container: str = DEFAULT_Z2M_CONTAINER

    @property
    def converter_dir(self) -> PurePosixPath:
        return self.data_dir / "external_converters"

    @property
    def converter_path(self) -> PurePosixPath:
        return self.converter_dir / REMOTE_CONVERTER_NAME

    @property
    def container_converter_path(self) -> PurePosixPath:
        return PurePosixPath("/app/data/external_converters") / REMOTE_CONVERTER_NAME

    @property
    def backup_root(self) -> PurePosixPath:
        return self.data_dir / ".watermeter-backups"


@dataclass(frozen=True)
class Z2MAuditResult:
    matches: bool
    conflicts: tuple[PurePosixPath, ...]
    approval_token: str


def parse_int(value: str) -> int:
    return int(value.strip(), 0)


def parse_partition_table(path: Path) -> dict[str, Partition]:
    partitions: dict[str, Partition] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        rows = csv.reader(line for line in handle if not line.lstrip().startswith("#"))
        for row in rows:
            if not row or all(not field.strip() for field in row):
                continue
            if len(row) < 5:
                raise HarnessError(f"Invalid partition row in {path}: {row}")

            name, partition_type, subtype, offset_text, size_text = (field.strip() for field in row[:5])
            if not name or not offset_text or not size_text:
                raise HarnessError(f"Partition name, offset, and size must be explicit in {path}: {row}")
            if name in partitions:
                raise HarnessError(f"Duplicate partition {name!r} in {path}")

            partitions[name] = Partition(
                name=name,
                partition_type=partition_type,
                subtype=subtype,
                offset=parse_int(offset_text),
                size=parse_int(size_text),
            )

    validate_partition_layout(partitions)
    return partitions


def validate_partition_layout(partitions: Mapping[str, Partition]) -> None:
    previous: Partition | None = None
    for partition in sorted(partitions.values(), key=lambda item: item.offset):
        if partition.offset < 0 or partition.size <= 0:
            raise HarnessError(f"Partition {partition.name!r} has an invalid offset or size")
        if partition.offset + partition.size > FLASH_SIZE_BYTES:
            raise HarnessError(f"Partition {partition.name!r} exceeds the 4 MB flash boundary")
        if previous and previous.offset + previous.size > partition.offset:
            raise HarnessError(f"Partitions {previous.name!r} and {partition.name!r} overlap")
        previous = partition


def ota_slot_limit(partitions: Mapping[str, Partition]) -> int:
    try:
        slots = (partitions["ota_0"], partitions["ota_1"])
    except KeyError as error:
        raise HarnessError(f"Missing OTA partition: {error.args[0]}") from error
    if slots[0].size != slots[1].size:
        raise HarnessError("ota_0 and ota_1 must have equal sizes")
    return slots[0].size


def required_partition(partitions: Mapping[str, Partition], name: str) -> Partition:
    try:
        return partitions[name]
    except KeyError as error:
        raise HarnessError(f"Missing required partition: {name}") from error


def zigbee_storage_partition(partitions: Mapping[str, Partition]) -> Partition:
    partition = required_partition(partitions, "zb_storage")
    if partition.partition_type != "data" or partition.subtype != "fat":
        raise HarnessError("zb_storage must be a data/fat partition")
    return partition


def verify_binary_size(binary_path: Path, slot_size: int) -> int:
    if not binary_path.is_file():
        raise HarnessError(f"Firmware binary not found: {binary_path}")
    size = binary_path.stat().st_size
    if size >= slot_size:
        raise HarnessError(f"{binary_path.name} is {size} bytes; OTA slot limit is {slot_size} bytes")
    return size


def profile_config(name: str) -> BuildProfile:
    if name == "production":
        return BuildProfile(name, PRODUCTION_BUILD_DIR, PRODUCTION_SDKCONFIG, (ROOT_DIR / "sdkconfig.defaults",))
    if name == "diagnostic":
        return BuildProfile(
            name,
            DIAGNOSTIC_BUILD_DIR,
            DIAGNOSTIC_SDKCONFIG,
            (ROOT_DIR / "sdkconfig.defaults", ROOT_DIR / "sdkconfig.diagnostic.defaults"),
        )
    raise HarnessError(f"Unknown build profile: {name}")


def filter_serial_ports(paths: Iterable[str], system_name: str | None = None) -> list[str]:
    system_name = system_name or platform.system()
    prefixes = {
        "Darwin": ("/dev/cu.usbmodem", "/dev/cu.usbserial", "/dev/cu.wchusbserial", "/dev/cu.SLAB_USBtoUART"),
        "Linux": ("/dev/ttyACM", "/dev/ttyUSB"),
    }.get(system_name, ("/dev/ttyACM", "/dev/ttyUSB", "/dev/cu.usbmodem"))
    return sorted({path for path in paths if path.startswith(prefixes)})


def discover_serial_ports() -> list[str]:
    candidates: list[str] = []
    for pattern in ("/dev/cu.*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates.extend(glob.glob(pattern))
    return filter_serial_ports(candidates)


def select_serial_port(requested: str | None, available: Sequence[str] | None = None) -> str:
    if requested:
        if not Path(requested).exists():
            raise HarnessError(f"Serial port does not exist: {requested}")
        return requested

    ports = list(available) if available is not None else discover_serial_ports()
    if not ports:
        raise HarnessError("No ESP32-C6 serial port found; connect the board and run `ports`")
    if len(ports) > 1:
        raise HarnessError("Multiple serial ports found; pass --port explicitly:\n  " + "\n  ".join(ports))
    return ports[0]


def require_confirmation(actual: str | None, expected: str) -> None:
    if actual != expected:
        raise HarnessError(f"Refusing operation: pass --confirm {expected!r}")


def validate_ota_version(file_version: int, known_versions: Iterable[int]) -> int:
    if not 0 < file_version <= MAX_OTA_FILE_VERSION:
        raise HarnessError(f"OTA file version must be between 1 and {MAX_OTA_FILE_VERSION}")
    known = list(known_versions)
    if known and file_version <= max(known):
        raise HarnessError(f"OTA file version {file_version} must be greater than known version {max(known)}")
    return file_version


def known_ota_versions() -> list[int]:
    versions = [1]
    index_paths = (
        ROOT_DIR / "ota" / "index.json",
        ROOT_DIR / "public" / "ota" / "index.json",
        PRODUCTION_BUILD_DIR / "ota" / "index.json",
    )
    for index_path in index_paths:
        if not index_path.is_file():
            continue
        try:
            entries = json.loads(index_path.read_text(encoding="utf-8"))
            versions.extend(int(entry["fileVersion"]) for entry in entries)
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise HarnessError(f"Cannot parse OTA versions from {index_path}: {error}") from error
    return versions


def _candidate_idf_paths() -> list[Path]:
    candidates: list[Path] = []
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]).expanduser())
    candidates.extend(
        [
            Path.home() / ".espressif" / "v5.4.1" / "esp-idf",
            Path.home() / "esp" / "esp-idf",
        ]
    )
    return candidates


def _candidate_activation_scripts() -> list[Path]:
    candidates: list[Path] = []
    if os.environ.get("IDF_ACTIVATE_SCRIPT"):
        candidates.append(Path(os.environ["IDF_ACTIVATE_SCRIPT"]).expanduser())
    if os.environ.get("IDF_PATH"):
        candidates.append(Path(os.environ["IDF_PATH"]).expanduser() / "export.sh")
    candidates.extend(
        [
            Path.home() / ".espressif" / "tools" / "activate_idf_v5.4.1.sh",
            Path.home() / "esp" / "esp-idf" / "export.sh",
        ]
    )
    return candidates


def _candidate_idf_python_executables() -> list[Path]:
    candidates: list[Path] = []
    if os.environ.get("IDF_PYTHON_ENV_PATH"):
        candidates.append(Path(os.environ["IDF_PYTHON_ENV_PATH"]).expanduser() / "bin" / "python3")
    candidates.extend(
        [
            Path.home() / ".espressif" / "tools" / "python" / "v5.4.1" / "venv" / "bin" / "python3",
        ]
    )
    return candidates


def _python_from_activation_script(activation_script: Path) -> Path | None:
    shell_command = (
        f". {shlex.quote(str(activation_script))} >/dev/null && "
        "(command -v python3 || command -v python)"
    )
    result = subprocess.run(
        ["sh", "-c", shell_command],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return None
    candidate = Path(result.stdout.strip()).expanduser().absolute()
    return candidate if candidate.is_file() else None


def discover_idf_environment() -> IdfEnvironment:
    idf_path = next((path.resolve() for path in _candidate_idf_paths() if (path / "tools" / "idf.py").is_file()), None)
    activation = next((path.resolve() for path in _candidate_activation_scripts() if path.is_file()), None)
    python_executable = next((path.absolute() for path in _candidate_idf_python_executables() if path.is_file()), None)
    if idf_path is None:
        raise HarnessError("ESP-IDF was not found; set IDF_PATH or install ESP-IDF v5.4.1")
    if activation is None:
        raise HarnessError("ESP-IDF activation script was not found; set IDF_ACTIVATE_SCRIPT")
    if python_executable is None:
        python_executable = _python_from_activation_script(activation)
    if python_executable is None:
        raise HarnessError("ESP-IDF Python environment was not found; set IDF_PYTHON_ENV_PATH")
    return IdfEnvironment(idf_path=idf_path, activation_script=activation, python_executable=python_executable)


def run_command(
    command: Sequence[str],
    *,
    cwd: Path = ROOT_DIR,
    env: Mapping[str, str] | None = None,
    capture: bool = False,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(str(part) for part in command), flush=True)
    return subprocess.run(
        [str(part) for part in command],
        cwd=cwd,
        env=dict(env) if env is not None else None,
        check=True,
        text=True,
        input=input_text,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )


def run_idf(
    idf: IdfEnvironment,
    arguments: Sequence[str],
    *,
    extra_env: Mapping[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    shell_command = (
        f". {shlex.quote(str(idf.activation_script))} >/dev/null && "
        f"{shlex.quote(str(idf.python_executable))} {shlex.quote(str(idf.idf_script))} {shlex.join(arguments)}"
    )
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    return run_command(["sh", "-c", shell_command], env=env, capture=capture)


def validate_idf(idf: IdfEnvironment) -> str:
    result = run_idf(idf, ["--version"], capture=True)
    version = (result.stdout or "").strip()
    if EXPECTED_IDF_VERSION not in version:
        raise HarnessError(f"Expected ESP-IDF {EXPECTED_IDF_VERSION}, got {version!r}")
    return version


def clean_profile(profile: BuildProfile) -> None:
    if profile.build_dir.exists():
        run_command(["cmake", "-E", "remove_directory", str(profile.build_dir)])


def check_profile_binary(profile: BuildProfile) -> int:
    slot_size = ota_slot_limit(parse_partition_table(PARTITION_TABLE_PATH))
    binary_path = profile.build_dir / "watermeter.bin"
    size = verify_binary_size(binary_path, slot_size)
    print(f"{binary_path.name}: {size} bytes; OTA slot: {slot_size} bytes; free: {slot_size - size} bytes")
    return size


def build_profile(profile: BuildProfile, *, clean: bool = False, ota_file_version: int | None = None) -> int:
    idf = discover_idf_environment()
    version = validate_idf(idf)
    print(f"Using {version} from {idf.idf_path}")
    if clean:
        clean_profile(profile)

    arguments = [*profile.idf_arguments(), "set-target", TARGET, "build"]
    extra_env = {"OTA_FILE_VERSION": str(ota_file_version)} if ota_file_version is not None else None
    run_idf(idf, arguments, extra_env=extra_env)
    return check_profile_binary(profile)


def run_host_tests() -> None:
    python_test_dir = ROOT_DIR / "tests" / "python"
    run_command([sys.executable, "-m", "unittest", "discover", "-s", str(python_test_dir), "-p", "test_*.py"])
    host_build_dir = ROOT_DIR / "build-host-tests"
    run_command(["cmake", "-S", str(ROOT_DIR / "tests"), "-B", str(host_build_dir)])
    run_command(["cmake", "--build", str(host_build_dir)])
    run_command(["ctest", "--test-dir", str(host_build_dir), "--output-on-failure"])


def command_doctor(_: argparse.Namespace) -> None:
    idf = discover_idf_environment()
    version = validate_idf(idf)
    partitions = parse_partition_table(PARTITION_TABLE_PATH)
    slot_size = ota_slot_limit(partitions)
    print(f"Repository: {ROOT_DIR}")
    print(f"ESP-IDF: {version} ({idf.idf_path})")
    print(f"Activation: {idf.activation_script}")
    print(f"IDF Python: {idf.python_executable}")
    print(f"Target: {TARGET}")
    print(f"OTA slots: 0x{slot_size:X} bytes each")
    nvs_partition = required_partition(partitions, "nvs")
    zigbee_partition = zigbee_storage_partition(partitions)
    print(f"NVS: offset=0x{nvs_partition.offset:X} size=0x{nvs_partition.size:X}")
    print(f"Zigbee: offset=0x{zigbee_partition.offset:X} size=0x{zigbee_partition.size:X}")
    for executable in ("cmake", "git", "ssh", "scp"):
        location = shutil.which(executable)
        if not location:
            raise HarnessError(f"Required executable not found: {executable}")
        print(f"{executable}: {location}")


def command_ports(_: argparse.Namespace) -> None:
    ports = discover_serial_ports()
    if not ports:
        print("No ESP32-C6 serial ports found")
        return
    print("Detected serial ports:")
    for port in ports:
        print(f"  {port}")


def command_test(_: argparse.Namespace) -> None:
    run_host_tests()


def command_build(args: argparse.Namespace) -> None:
    build_profile(profile_config(args.profile), clean=args.clean)


def command_check_size(args: argparse.Namespace) -> None:
    check_profile_binary(profile_config(args.profile))


def command_flash(args: argparse.Namespace) -> None:
    profile = profile_config(args.profile)
    build_profile(profile, clean=args.clean)
    port = select_serial_port(args.port)
    idf = discover_idf_environment()
    validate_idf(idf)
    run_idf(idf, [*profile.idf_arguments(), "-p", port, "flash"])
    print("Flash complete. If the XIAO remains in ROM download mode, press RESET once without holding BOOT.")
    print("NVS and Zigbee storage were not erased.")


def command_monitor(args: argparse.Namespace) -> None:
    profile = profile_config(args.profile)
    port = select_serial_port(args.port)
    elf_path = profile.build_dir / "watermeter.elf"
    if not elf_path.is_file():
        raise HarnessError(f"ELF not found for {profile.name}; run the matching build first")
    idf = discover_idf_environment()
    validate_idf(idf)
    shell_command = (
        f". {shlex.quote(str(idf.activation_script))} >/dev/null && "
        f"{shlex.quote(str(idf.python_executable))} {shlex.quote(str(idf.monitor_script))} "
        f"-p {shlex.quote(port)} -b 115200 "
        f"--toolchain-prefix riscv32-esp-elf- --target {TARGET} --decode-panic backtrace "
        f"{shlex.quote(str(elf_path))}"
    )
    run_command(["sh", "-c", shell_command])


def command_erase_zigbee(args: argparse.Namespace) -> None:
    require_confirmation(args.confirm, CONFIRM_ERASE_ZIGBEE)
    port = select_serial_port(args.port)
    partition = zigbee_storage_partition(parse_partition_table(PARTITION_TABLE_PATH))
    idf = discover_idf_environment()
    validate_idf(idf)
    shell_command = (
        f". {shlex.quote(str(idf.activation_script))} >/dev/null && "
        f"{shlex.quote(str(idf.python_executable))} -m esptool --chip {TARGET} "
        f"-p {shlex.quote(port)} erase_region "
        f"0x{partition.offset:X} 0x{partition.size:X}"
    )
    run_command(["sh", "-c", shell_command])
    print("Zigbee pairing was erased. Water pulse NVS was preserved; permit join is required.")


def command_erase_all(args: argparse.Namespace) -> None:
    require_confirmation(args.confirm, CONFIRM_ERASE_ALL)
    port = select_serial_port(args.port)
    idf = discover_idf_environment()
    validate_idf(idf)
    run_idf(idf, ["-p", port, "erase-flash"])
    print("Entire flash erased: firmware, pulse count, OTA state, and Zigbee pairing are gone.")


def command_ota_build(args: argparse.Namespace) -> None:
    file_version = validate_ota_version(args.file_version, known_ota_versions())
    build_profile(profile_config("production"), clean=args.clean, ota_file_version=file_version)

    output_dir = PRODUCTION_BUILD_DIR / "ota"
    output_path = output_dir / f"watermeter-{file_version}.ota"
    index_path = output_dir / "index.json"
    git_sha = run_command(["git", "rev-parse", "HEAD"], capture=True).stdout.strip()
    run_command(
        [
            sys.executable,
            str(ROOT_DIR / "tools" / "mk_zigbee_ota.py"),
            "--input",
            str(PRODUCTION_BUILD_DIR / "watermeter.bin"),
            "--output",
            str(output_path),
            "--index",
            str(index_path),
            "--url-base",
            args.url_base,
            "--file-version",
            str(file_version),
            "--comment",
            git_sha,
        ]
    )
    print(f"OTA image: {output_path}")
    print(f"OTA index: {index_path}")


def _remote_shell(config: Z2MConfig, script: str, *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return run_command(["ssh", config.host, script], capture=capture)


def _remote_read(config: Z2MConfig, path: PurePosixPath, *, missing_ok: bool = False) -> str | None:
    quoted = shlex.quote(str(path))
    script = f"if test -f {quoted}; then cat {quoted}; else exit 44; fi"
    try:
        return _remote_shell(config, script, capture=True).stdout
    except subprocess.CalledProcessError as error:
        if missing_ok and error.returncode == 44:
            return None
        raise


def _remote_converter_files(config: Z2MConfig) -> list[PurePosixPath]:
    aggregate = config.data_dir / "external_converters.js"
    converter_dir = config.converter_dir
    script = (
        f"if test -f {shlex.quote(str(aggregate))}; then printf '%s\\n' {shlex.quote(str(aggregate))}; fi; "
        f"if test -d {shlex.quote(str(converter_dir))}; then "
        f"find {shlex.quote(str(converter_dir))} -maxdepth 1 -type f "
        "\\( -name '*.js' -o -name '*.mjs' \\) -print; "
        f"elif test -e {shlex.quote(str(converter_dir))}; then exit 46; fi"
    )
    output = _remote_shell(config, script, capture=True).stdout
    return [PurePosixPath(line) for line in output.splitlines() if line.strip()]


def _legacy_watermeter_conflicts(config: Z2MConfig) -> list[PurePosixPath]:
    conflicts: list[PurePosixPath] = []
    for path in _remote_converter_files(config):
        if path == config.converter_path:
            continue
        content = _remote_read(config, path, missing_ok=True)
        if content and ("ZigbeeHive-WaterMeter" in content or "manufacturerName: 'ZigbeeHive'" in content):
            conflicts.append(path)
    return conflicts


def _z2m_config_from_args(args: argparse.Namespace) -> Z2MConfig:
    return Z2MConfig(
        host=args.host,
        data_dir=PurePosixPath(args.data_dir),
        compose_file=PurePosixPath(args.compose_file),
        service=args.service,
        container=args.container,
    )


def z2m_approval_token(config: Z2MConfig, local_content: str, remote_content: str | None) -> str:
    state = {
        "host": config.host,
        "data_dir": str(config.data_dir),
        "path": str(config.converter_path),
        "compose_file": str(config.compose_file),
        "service": config.service,
        "container": config.container,
        "local_sha256": hashlib.sha256(local_content.encode()).hexdigest(),
        "remote_sha256": None if remote_content is None else hashlib.sha256(remote_content.encode()).hexdigest(),
    }
    payload = json.dumps(state, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode()).hexdigest()


def z2m_audit(config: Z2MConfig) -> Z2MAuditResult:
    local_content = CANONICAL_CONVERTER.read_text(encoding="utf-8")
    print(f"Zigbee2MQTT host: {config.host}")
    print(f"Data directory: {config.data_dir}")

    _remote_shell(config, f"test -d {shlex.quote(str(config.data_dir))}")
    compose_status = _remote_shell(
        config,
        f"docker compose -f {shlex.quote(str(config.compose_file))} ps {shlex.quote(config.service)}",
        capture=True,
    ).stdout
    print(compose_status.rstrip())

    configuration = _remote_read(config, config.data_dir / "configuration.yaml") or ""
    external_js_enabled = bool(re.search(r"^\s*enable_external_js:\s*true\s*$", configuration, re.MULTILINE))
    print(f"advanced.enable_external_js: {'enabled' if external_js_enabled else 'NOT ENABLED'}")

    syntax_command = (
        f"docker exec -i {shlex.quote(config.container)} "
        "node --input-type=module --check"
    )
    run_command(["ssh", config.host, syntax_command], input_text=local_content)
    print("Canonical converter syntax: valid in the Zigbee2MQTT container")

    remote_content = _remote_read(config, config.converter_path, missing_ok=True)
    matches = remote_content == local_content
    if matches:
        print(f"Canonical converter is current: {config.converter_path}")
    else:
        before = [] if remote_content is None else remote_content.splitlines()
        diff = difflib.unified_diff(
            before,
            local_content.splitlines(),
            fromfile=str(config.converter_path) if remote_content is not None else "/dev/null",
            tofile=str(config.converter_path),
            lineterm="",
        )
        print("\n".join(diff))

    conflicts = _legacy_watermeter_conflicts(config)
    if conflicts:
        print("Conflicting legacy WaterMeter definitions:")
        for path in conflicts:
            print(f"  {path}")
    else:
        print("No conflicting legacy WaterMeter definitions found")

    if not external_js_enabled:
        raise HarnessError("Zigbee2MQTT advanced.enable_external_js must be true before deployment")
    approval_token = z2m_approval_token(config, local_content, remote_content)
    if not conflicts and not matches:
        print(f"Deployment approval token: {approval_token}")
    return Z2MAuditResult(matches, tuple(conflicts), approval_token)


def command_z2m_audit(args: argparse.Namespace) -> None:
    z2m_audit(_z2m_config_from_args(args))


def z2m_logs_have_converter_error(logs: str) -> bool:
    patterns = (
        r"SyntaxError",
        r"Cannot find (?:module|package)",
        r"external converter.*(?:error|failed)",
        r"failed to load.*converter",
    )
    return any(re.search(pattern, logs, re.IGNORECASE) for pattern in patterns)


def z2m_logs_show_ready(logs: str) -> bool:
    return bool(re.search(r"Zigbee2MQTT started", logs, re.IGNORECASE))


def _z2m_restart_and_report(config: Z2MConfig) -> None:
    restart_started = datetime.now(timezone.utc).isoformat()
    _remote_shell(
        config,
        f"docker compose -f {shlex.quote(str(config.compose_file))} restart {shlex.quote(config.service)}",
    )
    state = ""
    logs = ""
    ready = False
    logs_command = (
        f"docker logs --since {shlex.quote(restart_started)} --tail 200 "
        f"{shlex.quote(config.container)} 2>&1"
    )
    for _ in range(12):
        time.sleep(5)
        state = _remote_shell(
            config,
            "docker inspect --format "
            + shlex.quote("{{.State.Running}} {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}")
            + f" {shlex.quote(config.container)}",
            capture=True,
        ).stdout.strip()
        logs = _remote_shell(config, logs_command, capture=True).stdout
        if state == "true healthy" or (state == "true none" and z2m_logs_show_ready(logs)):
            ready = True
            break
    status = _remote_shell(
        config,
        f"docker compose -f {shlex.quote(str(config.compose_file))} ps {shlex.quote(config.service)}",
        capture=True,
    ).stdout
    print(status.rstrip())
    print(logs.rstrip())
    if not ready:
        raise HarnessError(f"Zigbee2MQTT did not become ready after restart: {state or 'unknown state'}")
    if z2m_logs_have_converter_error(logs):
        raise HarnessError("Recent Zigbee2MQTT logs contain an external-converter load error")


def command_z2m_deploy(args: argparse.Namespace) -> None:
    require_confirmation(args.confirm, CONFIRM_Z2M_DEPLOY)
    config = _z2m_config_from_args(args)
    audit = z2m_audit(config)
    if audit.conflicts:
        raise HarnessError(
            "Refusing deployment while a legacy bundle still defines WaterMeter. "
            "Use the watermeter-z2m skill to split that bundle and review the diff first."
        )
    if audit.matches:
        print("Canonical converter is already current; no deployment or restart is required.")
        return
    if args.approved_audit != audit.approval_token:
        raise HarnessError(
            "Refusing deployment: run z2m-audit, review its diff, and pass the current "
            "--approved-audit token"
        )

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup_dir = config.backup_root / timestamp
    backup_path = backup_dir / REMOTE_CONVERTER_NAME
    absent_marker = backup_dir / f"{REMOTE_CONVERTER_NAME}.absent"
    target = shlex.quote(str(config.converter_path))
    backup = shlex.quote(str(backup_path))
    marker = shlex.quote(str(absent_marker))
    remote_backup = (
        f"mkdir -p {shlex.quote(str(backup_dir))} {shlex.quote(str(config.converter_dir))} && "
        f"if test -f {target}; then cp -p {target} {backup}; else : > {marker}; fi"
    )
    _remote_shell(config, remote_backup)

    temporary_name = f".{REMOTE_CONVERTER_NAME}.{timestamp}.tmp.mjs"
    temporary_path = config.converter_dir / temporary_name
    run_command(["scp", str(CANONICAL_CONVERTER), f"{config.host}:{temporary_path}"])
    container_temporary = PurePosixPath("/app/data/external_converters") / temporary_name
    _remote_shell(
        config,
        f"docker exec {shlex.quote(config.container)} node --check {shlex.quote(str(container_temporary))}",
    )
    _remote_shell(config, f"mv {shlex.quote(str(temporary_path))} {target}")
    try:
        _z2m_restart_and_report(config)
    except (HarnessError, subprocess.CalledProcessError):
        _print_z2m_rollback(backup_dir, timestamp)
        raise
    _print_z2m_rollback(backup_dir, timestamp)


def _print_z2m_rollback(backup_dir: PurePosixPath, timestamp: str) -> None:
    print(f"Backup retained at {backup_dir}")
    print(
        "Rollback requires a separate confirmation:\n"
        f"  {Path(__file__).name} z2m-rollback --backup {timestamp} --confirm {CONFIRM_Z2M_ROLLBACK!r}"
    )


def command_z2m_rollback(args: argparse.Namespace) -> None:
    require_confirmation(args.confirm, CONFIRM_Z2M_ROLLBACK)
    if not re.fullmatch(r"\d{8}T\d{6}Z", args.backup):
        raise HarnessError("Backup must be a UTC timestamp such as 20260731T153000Z")
    config = _z2m_config_from_args(args)
    backup_dir = config.backup_root / args.backup
    backup_path = backup_dir / REMOTE_CONVERTER_NAME
    absent_marker = backup_dir / f"{REMOTE_CONVERTER_NAME}.absent"
    target = config.converter_path
    script = (
        f"if test -f {shlex.quote(str(backup_path))}; then "
        f"cp -p {shlex.quote(str(backup_path))} {shlex.quote(str(target))}; "
        f"elif test -f {shlex.quote(str(absent_marker))}; then rm -f {shlex.quote(str(target))}; "
        "else exit 45; fi"
    )
    _remote_shell(config, script)
    _z2m_restart_and_report(config)
    print(f"Restored converter state from {backup_dir}")


def add_port_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", help="Serial port; auto-detected when exactly one board is connected")


def add_profile_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--profile", choices=("production", "diagnostic"), default="production")


def add_z2m_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", default=DEFAULT_Z2M_HOST)
    parser.add_argument("--data-dir", default=str(DEFAULT_Z2M_DATA_DIR))
    parser.add_argument("--compose-file", default=str(DEFAULT_Z2M_COMPOSE_FILE))
    parser.add_argument("--service", default=DEFAULT_Z2M_SERVICE)
    parser.add_argument("--container", default=DEFAULT_Z2M_CONTAINER)


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    doctor = subparsers.add_parser("doctor", help="Validate the local toolchain and partition layout")
    doctor.set_defaults(handler=command_doctor)

    ports = subparsers.add_parser("ports", help="List likely ESP32-C6 USB serial ports")
    ports.set_defaults(handler=command_ports)

    test = subparsers.add_parser("test", help="Run Python and C host tests")
    test.set_defaults(handler=command_test)

    build = subparsers.add_parser("build", help="Build a firmware profile and enforce the OTA slot size")
    add_profile_argument(build)
    build.add_argument("--clean", action="store_true", help="Remove only the selected build directory first")
    build.set_defaults(handler=command_build)

    check_size = subparsers.add_parser("check-size", help="Check an existing binary against the parsed OTA slot")
    add_profile_argument(check_size)
    check_size.set_defaults(handler=command_check_size)

    build_diagnostic = subparsers.add_parser("build-diagnostic", help="Build the diagnostic profile")
    build_diagnostic.add_argument("--clean", action="store_true")
    build_diagnostic.set_defaults(handler=command_build, profile="diagnostic")

    flash = subparsers.add_parser("flash", help="Build and flash without erasing NVS or Zigbee storage")
    add_profile_argument(flash)
    add_port_argument(flash)
    flash.add_argument("--clean", action="store_true")
    flash.set_defaults(handler=command_flash)

    monitor = subparsers.add_parser("monitor", help="Monitor logs using the matching profile ELF")
    add_profile_argument(monitor)
    add_port_argument(monitor)
    monitor.set_defaults(handler=command_monitor)

    erase_zigbee = subparsers.add_parser("erase-zigbee", help="Erase only the parsed zb_storage partition")
    add_port_argument(erase_zigbee)
    erase_zigbee.add_argument("--confirm", help=f"Must be exactly {CONFIRM_ERASE_ZIGBEE!r}")
    erase_zigbee.set_defaults(handler=command_erase_zigbee)

    erase_all = subparsers.add_parser("erase-all", help="Erase the entire flash, including the pulse count")
    add_port_argument(erase_all)
    erase_all.add_argument("--confirm", help=f"Must be exactly {CONFIRM_ERASE_ALL!r}")
    erase_all.set_defaults(handler=command_erase_all)

    ota_build = subparsers.add_parser("ota-build", help="Build a versioned Zigbee OTA image and index")
    ota_build.add_argument("--file-version", type=int, required=True)
    ota_build.add_argument("--url-base", default=DEFAULT_OTA_URL_BASE)
    ota_build.add_argument("--clean", action="store_true")
    ota_build.set_defaults(handler=command_ota_build)

    z2m_audit_parser = subparsers.add_parser("z2m-audit", help="Read-only Zigbee2MQTT audit and converter diff")
    add_z2m_arguments(z2m_audit_parser)
    z2m_audit_parser.set_defaults(handler=command_z2m_audit)

    z2m_deploy = subparsers.add_parser("z2m-deploy", help="Backup, validate, deploy, and restart Zigbee2MQTT")
    add_z2m_arguments(z2m_deploy)
    z2m_deploy.add_argument("--approved-audit", help="Token printed by a separately reviewed z2m-audit")
    z2m_deploy.add_argument("--confirm", help=f"Must be exactly {CONFIRM_Z2M_DEPLOY!r}")
    z2m_deploy.set_defaults(handler=command_z2m_deploy)

    z2m_rollback = subparsers.add_parser("z2m-rollback", help="Restore one WaterMeter converter backup")
    add_z2m_arguments(z2m_rollback)
    z2m_rollback.add_argument("--backup", required=True, help="UTC backup timestamp")
    z2m_rollback.add_argument("--confirm", help=f"Must be exactly {CONFIRM_Z2M_ROLLBACK!r}")
    z2m_rollback.set_defaults(handler=command_z2m_rollback)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = create_parser().parse_args(argv)
    try:
        args.handler(args)
    except HarnessError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, file=sys.stderr)
        print(f"error: command failed with exit code {error.returncode}", file=sys.stderr)
        return error.returncode or 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
