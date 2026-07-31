import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from unittest import mock


ROOT_DIR = Path(__file__).resolve().parents[2]
HARNESS_PATH = ROOT_DIR / "tools" / "watermeter_harness.py"
SPEC = importlib.util.spec_from_file_location("watermeter_harness", HARNESS_PATH)
HARNESS = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = HARNESS
SPEC.loader.exec_module(HARNESS)


class PartitionTests(unittest.TestCase):
    def test_parses_explicit_partition_offsets_and_sizes(self):
        content = """\
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,0x6000,
ota_0,app,ota_0,0x20000,0x1E0000,
ota_1,app,ota_1,0x200000,0x1E0000,
zb_storage,data,fat,0x3E0000,0x4000,
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partitions.csv"
            path.write_text(content, encoding="utf-8")
            partitions = HARNESS.parse_partition_table(path)

        self.assertEqual(partitions["zb_storage"].offset, 0x3E0000)
        self.assertEqual(HARNESS.ota_slot_limit(partitions), 0x1E0000)

    def test_rejects_implicit_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partitions.csv"
            path.write_text("ota_0,app,ota_0,,0x1E0000,\n", encoding="utf-8")
            with self.assertRaises(HARNESS.HarnessError):
                HARNESS.parse_partition_table(path)

    def test_rejects_unequal_ota_slots(self):
        partitions = {
            "ota_0": HARNESS.Partition("ota_0", "app", "ota_0", 0x20000, 100),
            "ota_1": HARNESS.Partition("ota_1", "app", "ota_1", 0x30000, 99),
        }
        with self.assertRaises(HARNESS.HarnessError):
            HARNESS.ota_slot_limit(partitions)

    def test_required_partition_has_clear_error(self):
        with self.assertRaisesRegex(HARNESS.HarnessError, "zb_storage"):
            HARNESS.required_partition({}, "zb_storage")

    def test_rejects_overlapping_partitions(self):
        partitions = {
            "first": HARNESS.Partition("first", "data", "nvs", 0x9000, 0x2000),
            "second": HARNESS.Partition("second", "data", "fat", 0xA000, 0x1000),
        }
        with self.assertRaisesRegex(HARNESS.HarnessError, "overlap"):
            HARNESS.validate_partition_layout(partitions)

    def test_rejects_partition_past_flash_boundary(self):
        partitions = {
            "bad": HARNESS.Partition("bad", "data", "fat", HARNESS.FLASH_SIZE_BYTES - 1, 2),
        }
        with self.assertRaisesRegex(HARNESS.HarnessError, "4 MB"):
            HARNESS.validate_partition_layout(partitions)

    def test_zigbee_erase_partition_must_be_data_fat(self):
        partitions = {
            "zb_storage": HARNESS.Partition("zb_storage", "app", "factory", 0x20000, 0x4000),
        }
        with self.assertRaisesRegex(HARNESS.HarnessError, "data/fat"):
            HARNESS.zigbee_storage_partition(partitions)


class BinarySizeTests(unittest.TestCase):
    def test_accepts_binary_strictly_smaller_than_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "watermeter.bin"
            path.write_bytes(b"x" * 15)
            self.assertEqual(HARNESS.verify_binary_size(path, 16), 15)

    def test_rejects_binary_equal_to_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "watermeter.bin"
            path.write_bytes(b"x" * 16)
            with self.assertRaises(HARNESS.HarnessError):
                HARNESS.verify_binary_size(path, 16)


class ProfileTests(unittest.TestCase):
    def test_diagnostic_profile_is_isolated(self):
        profile = HARNESS.profile_config("diagnostic")
        self.assertEqual(profile.build_dir.name, "build-diagnostic")
        self.assertEqual(profile.sdkconfig.name, "sdkconfig.diagnostic")
        self.assertEqual(len(profile.defaults), 2)

    def test_activation_script_is_derived_from_idf_path(self):
        with mock.patch.dict(HARNESS.os.environ, {"IDF_PATH": "/opt/esp/idf"}, clear=True):
            self.assertIn(Path("/opt/esp/idf/export.sh"), HARNESS._candidate_activation_scripts())

    def test_production_profile_uses_default_build(self):
        profile = HARNESS.profile_config("production")
        self.assertEqual(profile.build_dir.name, "build")
        self.assertEqual(profile.sdkconfig.name, "sdkconfig")

    def test_idf_python_can_be_discovered_from_activation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            idf_path = root / "esp-idf"
            activation = idf_path / "export.sh"
            python_path = root / "idf-python"
            (idf_path / "tools").mkdir(parents=True)
            (idf_path / "tools" / "idf.py").touch()
            activation.touch()
            python_path.touch()

            with mock.patch.object(HARNESS, "_candidate_idf_paths", return_value=[idf_path]), mock.patch.object(
                HARNESS, "_candidate_activation_scripts", return_value=[activation]
            ), mock.patch.object(HARNESS, "_candidate_idf_python_executables", return_value=[]), mock.patch.object(
                HARNESS, "_python_from_activation_script", return_value=python_path
            ):
                environment = HARNESS.discover_idf_environment()

        self.assertEqual(environment.python_executable, python_path)


class PortTests(unittest.TestCase):
    def test_filters_and_sorts_macos_ports(self):
        ports = HARNESS.filter_serial_ports(
            ["/dev/tty.Bluetooth-Incoming-Port", "/dev/cu.usbmodem3101", "/dev/cu.usbserial9"],
            "Darwin",
        )
        self.assertEqual(ports, ["/dev/cu.usbmodem3101", "/dev/cu.usbserial9"])

    def test_requires_explicit_selection_for_multiple_ports(self):
        with self.assertRaises(HARNESS.HarnessError):
            HARNESS.select_serial_port(None, ["/dev/cu.usbmodem1", "/dev/cu.usbmodem2"])

    def test_selects_only_detected_port(self):
        self.assertEqual(HARNESS.select_serial_port(None, ["/dev/cu.usbmodem1"]), "/dev/cu.usbmodem1")


class SafetyTests(unittest.TestCase):
    def test_confirmation_must_match_exactly(self):
        with self.assertRaises(HARNESS.HarnessError):
            HARNESS.require_confirmation("yes", HARNESS.CONFIRM_ERASE_ALL)
        HARNESS.require_confirmation(HARNESS.CONFIRM_ERASE_ALL, HARNESS.CONFIRM_ERASE_ALL)

    def test_ota_version_must_increase(self):
        with self.assertRaises(HARNESS.HarnessError):
            HARNESS.validate_ota_version(7, [1, 7])
        self.assertEqual(HARNESS.validate_ota_version(8, [1, 7]), 8)

    def test_ota_version_rejects_values_outside_u32(self):
        with self.assertRaisesRegex(HARNESS.HarnessError, "between"):
            HARNESS.validate_ota_version(HARNESS.MAX_OTA_FILE_VERSION + 1, [1])

    def test_known_ota_versions_include_local_build_index(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            build_dir = root / "build"
            index_path = build_dir / "ota" / "index.json"
            index_path.parent.mkdir(parents=True)
            index_path.write_text('[{"fileVersion": 12}]', encoding="utf-8")

            with mock.patch.object(HARNESS, "ROOT_DIR", root), mock.patch.object(
                HARNESS, "PRODUCTION_BUILD_DIR", build_dir
            ):
                self.assertEqual(HARNESS.known_ota_versions(), [1, 12])

    def test_z2m_paths_are_derived_from_data_directory(self):
        config = HARNESS.Z2MConfig(data_dir=PurePosixPath("/srv/z2m"))
        self.assertEqual(config.converter_path, PurePosixPath("/srv/z2m/external_converters/zigbeehive-watermeter.mjs"))
        self.assertEqual(config.backup_root, PurePosixPath("/srv/z2m/.watermeter-backups"))

    def test_detects_specific_converter_errors(self):
        self.assertTrue(HARNESS.z2m_logs_have_converter_error("Failed to load external converter x.mjs"))
        self.assertFalse(HARNESS.z2m_logs_have_converter_error("Device interview completed successfully"))

    def test_z2m_readiness_requires_startup_message(self):
        self.assertTrue(HARNESS.z2m_logs_show_ready("Zigbee2MQTT started!"))
        self.assertFalse(HARNESS.z2m_logs_show_ready("Starting Zigbee2MQTT"))

    def test_z2m_approval_token_binds_remote_state(self):
        config = HARNESS.Z2MConfig()
        absent = HARNESS.z2m_approval_token(config, "local", None)
        present = HARNESS.z2m_approval_token(config, "local", "remote")
        changed = HARNESS.z2m_approval_token(config, "changed", "remote")
        other_service = HARNESS.Z2MConfig(service="other")
        other_container = HARNESS.Z2MConfig(container="other")
        other_compose = HARNESS.Z2MConfig(compose_file=PurePosixPath("/srv/other.yml"))
        self.assertEqual(absent, HARNESS.z2m_approval_token(config, "local", None))
        self.assertNotEqual(absent, present)
        self.assertNotEqual(present, changed)
        self.assertNotEqual(present, HARNESS.z2m_approval_token(other_service, "local", "remote"))
        self.assertNotEqual(present, HARNESS.z2m_approval_token(other_container, "local", "remote"))
        self.assertNotEqual(present, HARNESS.z2m_approval_token(other_compose, "local", "remote"))

    def test_erase_all_without_confirmation_performs_no_operation(self):
        args = HARNESS.argparse.Namespace(confirm=None, port=None)
        with mock.patch.object(HARNESS, "select_serial_port") as select_port:
            with self.assertRaises(HARNESS.HarnessError):
                HARNESS.command_erase_all(args)
        select_port.assert_not_called()

    def test_deploy_with_stale_audit_token_performs_no_write(self):
        args = HARNESS.argparse.Namespace(
            confirm=HARNESS.CONFIRM_Z2M_DEPLOY,
            approved_audit="stale",
            host="homelab.lan",
            data_dir=str(HARNESS.DEFAULT_Z2M_DATA_DIR),
            compose_file=str(HARNESS.DEFAULT_Z2M_COMPOSE_FILE),
            service=HARNESS.DEFAULT_Z2M_SERVICE,
            container=HARNESS.DEFAULT_Z2M_CONTAINER,
        )
        audit = HARNESS.Z2MAuditResult(False, (), "current")
        with mock.patch.object(HARNESS, "z2m_audit", return_value=audit), mock.patch.object(
            HARNESS, "_remote_shell"
        ) as remote_shell, mock.patch.object(HARNESS, "run_command") as run_command:
            with self.assertRaisesRegex(HARNESS.HarnessError, "approved-audit"):
                HARNESS.command_z2m_deploy(args)
        remote_shell.assert_not_called()
        run_command.assert_not_called()

    def test_deploy_with_legacy_conflict_performs_no_write(self):
        args = HARNESS.argparse.Namespace(
            confirm=HARNESS.CONFIRM_Z2M_DEPLOY,
            approved_audit="current",
            host="homelab.lan",
            data_dir=str(HARNESS.DEFAULT_Z2M_DATA_DIR),
            compose_file=str(HARNESS.DEFAULT_Z2M_COMPOSE_FILE),
            service=HARNESS.DEFAULT_Z2M_SERVICE,
            container=HARNESS.DEFAULT_Z2M_CONTAINER,
        )
        conflict = PurePosixPath("/app/data/external_converters.js")
        audit = HARNESS.Z2MAuditResult(False, (conflict,), "current")
        with mock.patch.object(HARNESS, "z2m_audit", return_value=audit), mock.patch.object(
            HARNESS, "_remote_shell"
        ) as remote_shell, mock.patch.object(HARNESS, "run_command") as run_command:
            with self.assertRaisesRegex(HARNESS.HarnessError, "legacy bundle"):
                HARNESS.command_z2m_deploy(args)
        remote_shell.assert_not_called()
        run_command.assert_not_called()

    def test_rollback_refuses_legacy_split_backup(self):
        args = HARNESS.argparse.Namespace(
            confirm=HARNESS.CONFIRM_Z2M_ROLLBACK,
            backup="20260731T130909Z",
            host="homelab.lan",
            data_dir=str(HARNESS.DEFAULT_Z2M_DATA_DIR),
            compose_file=str(HARNESS.DEFAULT_Z2M_COMPOSE_FILE),
            service=HARNESS.DEFAULT_Z2M_SERVICE,
            container=HARNESS.DEFAULT_Z2M_CONTAINER,
        )
        with mock.patch.object(HARNESS, "_remote_read", return_value="legacy split"), mock.patch.object(
            HARNESS, "_remote_shell"
        ) as remote_shell:
            with self.assertRaisesRegex(HARNESS.HarnessError, "legacy-split backup"):
                HARNESS.command_z2m_rollback(args)
        remote_shell.assert_not_called()


if __name__ == "__main__":
    unittest.main()
