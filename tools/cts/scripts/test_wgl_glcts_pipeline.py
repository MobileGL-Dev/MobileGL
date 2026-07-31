import argparse
import json
from pathlib import Path
import struct
import tempfile
import unittest

import wgl_glcts_pipeline as pipeline


def write_fake_pe(path: Path, machine: int = pipeline.PE_MACHINE_AMD64, payload: bytes = b"") -> None:
    data = bytearray(0x88)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    path.write_bytes(bytes(data) + payload)


class ArgumentTests(unittest.TestCase):
    def test_versions_accept_gl_and_dotted_spellings(self):
        self.assertEqual("30", pipeline.normalize_version("GL30"))
        self.assertEqual("46", pipeline.normalize_version("4.6"))
        with self.assertRaises(argparse.ArgumentTypeError):
            pipeline.normalize_version("4.7")

    def test_environment_assignment_validation(self):
        self.assertEqual(("MOBILEGL_TEST", "a=b"), pipeline.parse_assignment("MOBILEGL_TEST=a=b"))
        with self.assertRaises(argparse.ArgumentTypeError):
            pipeline.parse_assignment("9BAD=value")

    def test_windows_environment_keys_are_canonical_and_last_wins(self):
        self.assertEqual(
            {"FOO": "x", "PATH": "second"},
            pipeline.canonicalize_windows_environment(
                [("Path", "first"), ("FOO", "x"), ("pAtH", "second")]
            ),
        )


class CommandTests(unittest.TestCase):
    def test_visual_studio_configure_commands_are_x64_and_wgl_default(self):
        mobilegl = pipeline.mobilegl_configure_command(
            Path("C:/src/MobileGL"), Path("D:/work/mg"), "Visual Studio 17 2022", "x64", []
        )
        self.assertIn("-A", mobilegl)
        self.assertIn("x64", mobilegl)
        self.assertIn("-DMOBILEGL_BUILD_TEST=OFF", mobilegl)

        cts = pipeline.cts_configure_command(
            Path("D:/src/VK-GL-CTS"), Path("D:/work/cts"), "Visual Studio 17 2022", "x64", []
        )
        self.assertIn("-DDEQP_TARGET=default", cts)
        self.assertNotIn("-DDEQP_TARGET=mobilegl", cts)

    def test_runner_command_contains_identity_preserving_wgl_flags(self):
        command = pipeline.runner_command(
            Path("runner.py"),
            Path("runtime/glcts.exe"),
            Path("cts/modules"),
            Path("gl46-main.txt"),
            Path("results/gl46"),
            "DirectVulkan",
            300,
            0,
            10000,
            {"MOBILEGL_LOG_FILE_PATH": "result/mobilegl.log"},
            pipeline.DEFAULT_DEQP_ARGS,
            "run-fingerprint",
        )
        self.assertIn("--backend", command)
        self.assertIn("DirectVulkan", command)
        self.assertIn("--deqp-arg=--deqp-gl-context-type=wgl", command)
        self.assertIn("--deqp-arg=--deqp-surface-type=fbo", command)
        self.assertIn("--env", command)
        self.assertIn("MOBILEGL_LOG_FILE_PATH=result/mobilegl.log", command)
        self.assertIn("--run-identity", command)
        self.assertIn("run-fingerprint", command)

    def test_report_command_requires_the_pipeline_run_identity(self):
        command = pipeline.report_command(
            Path("report.py"),
            [("DirectVulkan", "gl46", Path("gl46.txt"), Path("results/gl46"))],
            Path("summary.md"),
            Path("summary.json"),
            False,
            "run-fingerprint",
        )
        self.assertIn("--expected-run-identity", command)
        self.assertIn("run-fingerprint", command)


class RuntimeTests(unittest.TestCase):
    def test_pe_machine_rejects_non_x64(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "x86.dll"
            write_fake_pe(path, machine=0x14C)
            self.assertEqual(0x14C, pipeline.pe_machine(path))
            with self.assertRaises(pipeline.PipelineError):
                pipeline.require_x64_pe(path, "test DLL")

    def test_runtime_is_hash_keyed_and_copies_only_declared_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            glcts = root / "source-glcts.exe"
            mobilegl = root / "source-MobileGL.dll"
            write_fake_pe(glcts, payload=b"glcts")
            write_fake_pe(mobilegl, payload=b"mobilegl")
            sources = {"glcts.exe": glcts, "opengl32.dll": mobilegl}
            fingerprint, hashes = pipeline.runtime_fingerprint(sources)

            runtime = pipeline.assemble_runtime(root / "work", sources, fingerprint, hashes)

            self.assertEqual(fingerprint[:16], runtime.name)
            self.assertEqual(hashes["glcts.exe"], pipeline.sha256_file(runtime / "glcts.exe"))
            self.assertEqual(hashes["opengl32.dll"], pipeline.sha256_file(runtime / "opengl32.dll"))
            manifest = json.loads((runtime / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(fingerprint, manifest["fingerprint"])
            self.assertFalse((runtime / "libEGL.dll").exists())

    def test_run_fingerprint_changes_with_execution_semantics(self):
        base = pipeline.run_fingerprint(
            "runtime", "data", {"runner": "tool"}, {"30": "caselist"}, ["--deqp-surface-type=fbo"], {"FLAG": "1"}
        )
        self.assertEqual(
            base,
            pipeline.run_fingerprint(
                "runtime", "data", {"runner": "tool"}, {"30": "caselist"}, ["--deqp-surface-type=fbo"], {"FLAG": "1"}
            ),
        )
        self.assertNotEqual(
            base,
            pipeline.run_fingerprint(
                "runtime", "data", {"runner": "tool"}, {"30": "caselist"}, ["--deqp-surface-type=window"], {"FLAG": "1"}
            ),
        )
        self.assertNotEqual(
            base,
            pipeline.run_fingerprint(
                "runtime", "data", {"runner": "tool"}, {"30": "different"}, ["--deqp-surface-type=fbo"], {"FLAG": "1"}
            ),
        )
        self.assertNotEqual(
            base,
            pipeline.run_fingerprint(
                "runtime", "different-data", {"runner": "tool"}, {"30": "caselist"}, ["--deqp-surface-type=fbo"], {"FLAG": "1"}
            ),
        )
        self.assertNotEqual(
            base,
            pipeline.run_fingerprint(
                "runtime", "data", {"runner": "different-tool"}, {"30": "caselist"}, ["--deqp-surface-type=fbo"], {"FLAG": "1"}
            ),
        )

        timeout_baseline = pipeline.run_fingerprint(
            "runtime",
            "data",
            {"runner": "tool"},
            {"30": "caselist"},
            ["--deqp-surface-type=fbo"],
            {"FLAG": "1"},
            {"idle_timeout_seconds": 300.0, "max_round_seconds": 0.0},
        )
        idle_changed = pipeline.run_fingerprint(
            "runtime",
            "data",
            {"runner": "tool"},
            {"30": "caselist"},
            ["--deqp-surface-type=fbo"],
            {"FLAG": "1"},
            {"idle_timeout_seconds": 1.0, "max_round_seconds": 0.0},
        )
        max_round_changed = pipeline.run_fingerprint(
            "runtime",
            "data",
            {"runner": "tool"},
            {"30": "caselist"},
            ["--deqp-surface-type=fbo"],
            {"FLAG": "1"},
            {"idle_timeout_seconds": 300.0, "max_round_seconds": 60.0},
        )
        self.assertNotEqual(timeout_baseline, idle_changed)
        self.assertNotEqual(timeout_baseline, max_round_changed)

    def test_tracked_environment_is_case_insensitive_and_narrow(self):
        ambient, effective = pipeline.tracked_run_environment(
            {
                "Path": "ambient-path",
                "mobilegl_debug": "0",
                "LibGL_Driver": "ambient-libgl",
                "vK_iCd_fIlEnAmEs": "ambient-icd",
                "ANGLE_DEFAULT_PLATFORM": "vulkan",
                "Egl_Test": "1",
                "D3D_Feature": "1",
                "DxVk_Config": "ambient-dxvk",
                "HOME": "ignored",
                "PATH_EXTRA": "ignored",
                "MOBILEGL": "ignored",
            },
            {"pAtH": "explicit-path", "vk_icd_filenames": "explicit-icd", "CUSTOM": "kept"},
        )
        self.assertEqual("ambient-path", ambient["PATH"])
        self.assertNotIn("HOME", ambient)
        self.assertNotIn("PATH_EXTRA", ambient)
        self.assertNotIn("MOBILEGL", ambient)
        self.assertEqual("explicit-path", effective["PATH"])
        self.assertEqual("explicit-icd", effective["VK_ICD_FILENAMES"])
        self.assertEqual("kept", effective["CUSTOM"])

    def test_reserved_environment_names_cannot_hide_behind_case(self):
        overrides = pipeline.canonicalize_windows_environment(
            [("mobilegl_backend_type", "DirectGLES")]
        )
        self.assertEqual(
            {"MOBILEGL_BACKEND_TYPE"},
            pipeline.CONTROLLED_ENVIRONMENT_NAMES & set(overrides),
        )

    def test_directgles_requires_complete_angle_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            glcts = root / "glcts.exe"
            mobilegl = root / "MobileGL.dll"
            write_fake_pe(glcts)
            write_fake_pe(mobilegl)
            with self.assertRaises(pipeline.PipelineError):
                pipeline.runtime_source_files(glcts, mobilegl, ["DirectGLES"], root / "angle")

            angle = root / "angle"
            angle.mkdir()
            for name in pipeline.ANGLE_REQUIRED_DLLS:
                write_fake_pe(angle / name, payload=name.encode("ascii"))
            files = pipeline.runtime_source_files(glcts, mobilegl, ["DirectGLES"], angle)
            self.assertEqual(
                {"glcts.exe", "opengl32.dll", *pipeline.ANGLE_REQUIRED_DLLS}, set(files)
            )


class CaselistTests(unittest.TestCase):
    def test_preflight_prefers_small_buffer_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            caselist = Path(temporary) / "gl30-main.txt"
            caselist.write_text(
                "KHR-GL30.api.coverage\nKHR-GL30.buffer_objects.gen_buffers\n",
                encoding="utf-8",
            )
            self.assertEqual("KHR-GL30.buffer_objects.gen_buffers", pipeline.choose_preflight_case(caselist))


class PreflightTests(unittest.TestCase):
    def write_identity(self, root: Path, renderer: str, version: str = "4.6"):
        qpa = root / "chunk0000.qpa"
        qpa.write_text(
            '#sessionInfo vendor "MobileGL-Dev"\n'
            f'#sessionInfo renderer "{renderer}"\n'
            '#sessionInfo commandLineParameters "--deqp-gl-context-type=wgl --deqp-surface-type=fbo"\n',
            encoding="utf-8",
        )
        log = root / "mobilegl.log"
        log.write_text(f"Target OpenGL Version: {version}\n", encoding="utf-8")
        return qpa, log

    def test_identity_accepts_both_mobilegl_renderers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            qpa, log = self.write_identity(root, "Magma (MobileGL Core)")
            identity = pipeline.parse_preflight_identity([qpa], log, "DirectVulkan", (4, 6))
            self.assertEqual("4.6", identity["target_gl_version"])

            qpa, log = self.write_identity(root, "Espryt (MobileGL Core)")
            identity = pipeline.parse_preflight_identity([qpa], log, "DirectGLES", (3, 3))
            self.assertIn("Espryt", identity["renderer"])

    def test_identity_rejects_system_driver_or_low_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            qpa, log = self.write_identity(root, "NVIDIA GeForce RTX", version="4.6")
            qpa.write_text(
                '#sessionInfo vendor "NVIDIA Corporation"\n'
                '#sessionInfo renderer "NVIDIA GeForce RTX"\n'
                '#sessionInfo commandLineParameters "--deqp-gl-context-type=wgl"\n',
                encoding="utf-8",
            )
            with self.assertRaises(pipeline.PipelineError):
                pipeline.parse_preflight_identity([qpa], log, "DirectVulkan", (4, 6))

            qpa, log = self.write_identity(root, "Magma (MobileGL Core)", version="4.5")
            with self.assertRaises(pipeline.PipelineError):
                pipeline.parse_preflight_identity([qpa], log, "DirectVulkan", (4, 6))


if __name__ == "__main__":
    unittest.main()
