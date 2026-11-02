# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real remuxed MP4/WebM parts, size limits and upload failure cleanup."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VideoSplitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("ffmpeg", "ffprobe", "pkg-config", os.environ.get("HOST_CXX", "clang++")):
            if not shutil.which(tool):
                raise unittest.SkipTest(f"{tool} is required for real-media splitting tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="pshare-split-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.driver = cls.work / "driver"
        flags = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil", "libswscale", "libswresample", "libcurl"],
            text=True,
        )
        sources = [ROOT / "src/pshare/core" / (name + ".cpp") for name in
                   ("config", "ini", "text", "media", "local_copy", "upload_smb", "upload_curl", "upload", "video_split", "video_convert")]
        subprocess.run([os.environ.get("HOST_CXX", "clang++"), "-std=c++20", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-pthread", f"-I{ROOT / 'src'}",
                        str(ROOT / "tests/video_split_driver.cpp"), *map(str, sources),
                        *shlex.split(flags), "-o", str(cls.driver)], check=True)
        cls.sources = {}
        for extension, encoder, audio in (("mp4", "libx264", "aac"), ("webm", "libvpx-vp9", "libopus")):
            path = cls.work / f"source.{extension}"
            subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                            "testsrc2=size=640x360:rate=30:duration=12", "-f", "lavfi", "-i",
                            "sine=frequency=440:duration=12", "-c:v", encoder,
                            "-b:v", "2000k", "-g", "30", "-threads", "2",
                            "-c:a", audio, str(path)], check=True)
            cls.sources[extension] = path

    def run_driver(self, path, mode):
        output = self.work / (mode + "-parts" + path.suffix)
        output.mkdir(exist_ok=True)
        before = hashlib.sha256(path.read_bytes()).digest()
        result = subprocess.run([str(self.driver), str(path), str(output), mode],
                                check=True, text=True, capture_output=True)
        self.assertEqual(before, hashlib.sha256(path.read_bytes()).digest())
        return [Path(line) for line in result.stdout.splitlines()]

    def packets(self, path):
        result = subprocess.check_output(["ffprobe", "-v", "error", "-show_packets",
                                          "-show_data_hash", "sha256", "-of", "json", str(path)])
        return json.loads(result)["packets"]

    def test_parts_are_decodable_lossless_and_bounded(self):
        for extension, path in self.sources.items():
            with self.subTest(extension=extension):
                parts = self.run_driver(path, "split")
                self.assertGreater(len(parts), 1)
                actual = {0: [], 1: []}
                for part in parts:
                    self.assertLessEqual(part.stat().st_size, 500000)
                    subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(part),
                                    "-f", "null", "-"], check=True, capture_output=True)
                    packets = self.packets(part)
                    video = next(p for p in packets if p["stream_index"] == 0)
                    self.assertIn("K", video["flags"])
                    for packet in packets:
                        actual[packet["stream_index"]].append(packet["data_hash"])
                original = self.packets(path)
                for stream in (0, 1):
                    self.assertEqual([p["data_hash"] for p in original if p["stream_index"] == stream],
                                     actual[stream])

    def test_upload_and_cleanup_on_success_failure_and_cancel(self):
        for path in self.sources.values():
            for mode in ("success", "failure", "cancel", "precancel"):
                with self.subTest(path=path.name, mode=mode):
                    self.run_driver(path, mode)

    def test_oversized_keyframe_interval(self):
        path = self.work / "long-gop.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(self.sources["mp4"]),
                        "-c:v", "libx264", "-b:v", "2000k", "-g", "1000", "-keyint_min", "1000",
                        "-sc_threshold", "0", "-threads", "2", "-c:a", "copy", str(path)], check=True)
        self.run_driver(path, "oversized")

    def test_cloud_limit_overrides_larger_configured_limit(self):
        path = self.work / "large.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-stream_loop", "22", "-i",
                        str(self.sources["mp4"]), "-c", "copy", str(path)], check=True)
        self.assertGreater(path.stat().st_size, 50000000)
        self.run_driver(path, "cloudlimit")

    def test_ps5_opus_header_is_preserved(self):
        path = self.work / "ps5-opus.webm"
        data = bytearray(self.sources["webm"].read_bytes())
        offset = data.index(b"OpusHead") + 8
        data[offset] = 16  # PS5 recordings use a version FFmpeg's decoder rejects.
        path.write_bytes(data)
        parts = self.run_driver(path, "split")
        self.assertGreater(len(parts), 1)
        for part in parts:
            data = part.read_bytes()
            self.assertEqual(data[data.index(b"OpusHead") + 8], 16)

    def test_conversion_to_h264_aac(self):
        source = self.sources["webm"]
        converted, = self.run_driver(source, "convert")
        probe = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(converted)]))
        video, audio = probe["streams"]
        self.assertEqual(video["codec_name"], "h264")
        self.assertEqual(audio["codec_name"], "aac")
        self.assertLessEqual(video["width"], 1920)
        self.assertLessEqual(video["height"], 1080)
        self.assertLessEqual(float(video["r_frame_rate"].split("/")[0]) /
                             float(video["r_frame_rate"].split("/")[1]), 30)
        self.assertAlmostEqual(float(probe["format"]["duration"]), 12, delta=0.2)
        self.assertAlmostEqual(float(video["duration"]), float(audio["duration"]), delta=0.15)
        subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(converted),
                        "-f", "null", "-"], check=True, capture_output=True)
        for mode in ("convert-upload", "convert-cancel", "convert-precancel", "convert-midcancel", "convert-http"):
            self.run_driver(source, mode)

    def test_global_conversion_applies_to_folder_exports(self):
        converted, = self.run_driver(self.sources["webm"], "convert-folder")
        streams = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-of", "json", str(converted)]))["streams"]
        self.assertEqual([s["codec_name"] for s in streams], ["h264", "aac"])

    def test_conversion_handles_ps5_opus_header_and_silent_video(self):
        ps5 = self.work / "ps5-convert.webm"
        data = bytearray(self.sources["webm"].read_bytes())
        data[data.index(b"OpusHead") + 8] = 16
        ps5.write_bytes(data)
        converted, = self.run_driver(ps5, "convert")
        subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(converted),
                        "-f", "null", "-"], check=True, capture_output=True)
        silent = self.work / "silent.webm"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(self.sources["webm"]),
                        "-an", "-c:v", "copy", str(silent)], check=True)
        converted, = self.run_driver(silent, "convert")
        streams = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-of", "json", str(converted)]))["streams"]
        self.assertEqual(len(streams), 1)
        self.assertEqual(streams[0]["codec_name"], "h264")

    def test_conversion_scales_and_caps_frame_rate(self):
        source = self.work / "1080p60.webm"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        "testsrc2=size=1920x1080:rate=60:duration=1", "-c:v", "libvpx-vp9",
                        "-deadline", "realtime", "-cpu-used", "8", "-threads", "2", str(source)], check=True)
        converted, = self.run_driver(source, "convert")
        streams = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-of", "json", str(converted)]))["streams"]
        self.assertEqual((streams[0]["width"], streams[0]["height"]), (1920, 1080))
        self.assertEqual(streams[0]["r_frame_rate"], "30/1")

    def test_conversion_downscales_1440p_to_1080p(self):
        source = self.work / "1440p.webm"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        "testsrc2=size=2560x1440:rate=30:duration=0.4", "-c:v", "libvpx-vp9",
                        "-deadline", "realtime", "-cpu-used", "8", "-threads", "2", str(source)], check=True)
        converted, = self.run_driver(source, "convert")
        streams = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-of", "json", str(converted)]))["streams"]
        self.assertEqual((streams[0]["width"], streams[0]["height"]), (1920, 1080))

    def test_hdr_conversion_is_rejected(self):
        source = self.work / "hdr.webm"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(self.sources["webm"]), "-t", "1",
                        "-vf", "setparams=color_trc=smpte2084", "-an", "-c:v", "libvpx-vp9",
                        "-deadline", "realtime", "-cpu-used", "8", "-threads", "2", str(source)], check=True)
        self.run_driver(source, "convert-invalid")

    def test_invalid_media(self):
        path = self.work / "invalid.webm"
        path.write_bytes(b"invalid video")
        self.run_driver(path, "invalid")
        self.run_driver(path, "convert-invalid")
