#!/usr/bin/env python3
"""Install a verified offline Whisper bundle for MeowLive2D on Windows."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from urllib.request import urlopen
import venv


def digest(file):
    with file.open("rb") as content:
        return hashlib.file_digest(content, "sha256").hexdigest()


def checked_file(source, entry, destination=None):
    file = source / entry["filename"]
    if not file.is_file() or file.is_symlink() or digest(file) != entry["sha256"]:
        raise ValueError(f"缺少组件或 SHA-256 校验失败: {entry['filename']}")
    if destination:
        target = destination / entry["filename"]
        shutil.copyfile(file, target)
        if digest(target) != entry["sha256"]:
            raise ValueError(f"组件在复制期间发生变化: {entry['filename']}")


def prepare_bundle(manifest, bundle):
    sources = {
        "wheels": "https://files.pythonhosted.org",
        "model": f"https://huggingface.co/{manifest['model_repository']}/resolve/{manifest['model_revision']}",
    }
    from urllib.parse import quote
    for category, server in sources.items():
        destination = bundle / category
        destination.mkdir(parents=True, exist_ok=True)
        for entry in manifest[category]:
            file = destination / entry["filename"]
            if file.is_file() and digest(file) == entry["sha256"]:
                continue
            if category == "wheels":
                package = f"https://pypi.org/pypi/{entry['name']}/{entry['version']}/json"
                with urlopen(package, timeout=30) as response:
                    info = json.load(response)
                url = next(item["url"] for item in info["urls"] if item["filename"] == file.name)
                if not url.startswith(server + "/"):
                    raise ValueError(f"软件包下载地址无效: {file.name}")
            else:
                url = f"{server}/{quote(file.name)}"
            partial = file.with_name(file.name + ".part")
            try:
                with urlopen(url, timeout=60) as response, partial.open("wb") as output:
                    shutil.copyfileobj(response, output, 1024 * 1024)
                if digest(partial) != entry["sha256"]:
                    raise ValueError(f"下载文件摘要不符: {file.name}")
                partial.replace(file)
            finally:
                partial.unlink(missing_ok=True)
            print(f"已验证 {file.name}", flush=True)


def install_bundle(manifest, bundle, directory):
    if sys.platform != "win32" or struct.calcsize("P") != 8 or sys.version_info[:2] != (3, 11):
        raise ValueError("Whisper 安装需要 Windows x64 CPython 3.11")

    voice = directory / "voice"
    voice.mkdir(parents=True, exist_ok=True)
    runtime = voice / "whisper-python"
    model = voice / "models/faster-whisper-large-v3-turbo"
    lock_path = voice / "whisper-install.lock"
    with ExitStack() as cleanup:
        try:
            lock = lock_path.open("x")
        except FileExistsError as error:
            raise ValueError("已有 Whisper 安装任务或上次任务异常退出") from error
        cleanup.callback(lock_path.unlink, missing_ok=True)
        cleanup.enter_context(lock)
        if runtime.exists() or model.exists():
            raise ValueError("目标已有 Whisper 组件，请在其他数据目录安装或继续使用现有环境")

        with tempfile.TemporaryDirectory(prefix=".whisper-", dir=voice) as temporary:
            staging = Path(temporary)
            wheels = staging / "wheels"
            wheels.mkdir()
            new_model = staging / "model"
            new_model.mkdir()
            for component in manifest["wheels"]:
                checked_file(bundle / "wheels", component, wheels)
            for component in manifest["model"]:
                checked_file(bundle / "model", component, new_model)
            requirements = staging / "requirements.txt"
            requirements.write_text("".join(
                f"{wheel['name']}=={wheel['version']} --hash=sha256:{wheel['sha256']}\n"
                for wheel in manifest["wheels"]), encoding="utf-8")

            # Windows venv launchers embed their path. Build them at the final
            # location and expose the model once runtime validation succeeds.
            def rollback_runtime():
                if not model.exists():
                    shutil.rmtree(runtime, ignore_errors=True)

            cleanup.callback(rollback_runtime)
            venv.EnvBuilder(with_pip=True).create(runtime)
            interpreter = runtime / "Scripts/python.exe"
            subprocess.run([str(interpreter), "-I", "-m", "pip", "--isolated", "install",
                            "--disable-pip-version-check", "--no-index", "--only-binary=:all:",
                            "--require-hashes", "--no-deps", "--find-links", str(wheels),
                            "-r", str(requirements)], cwd=staging, check=True)
            subprocess.run([str(interpreter), "-I", "-B", "-c",
                            "import ctranslate2, faster_whisper; "
                            "assert faster_whisper.__version__ == '1.2.1'; "
                            "assert 'int8' in ctranslate2.get_supported_compute_types('cpu')"],
                           cwd=staging, check=True)
            model.parent.mkdir(exist_ok=True)
            new_model.rename(model)
    print("Windows Whisper 已安装，可在参考录音页转写。", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    download = commands.add_parser("download", help="从 PyPI 和 Hugging Face 准备离线目录")
    download.add_argument("bundle", type=Path)
    install = commands.add_parser("install", help="从已准备的目录安装 Windows Whisper 组件")
    install.add_argument("bundle", type=Path)
    install.add_argument("--data-dir", type=Path)
    options = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name("whisper-components.json").read_text(encoding="utf-8"))
    if options.command == "download":
        prepare_bundle(manifest, options.bundle.absolute())
        return
    data_dir = (options.data_dir or os.environ.get("MEOWLIVE_DATA_DIR")
                or Path(__file__).resolve().parents[1])
    install_bundle(manifest, options.bundle.absolute(), Path(data_dir).absolute())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(1)
