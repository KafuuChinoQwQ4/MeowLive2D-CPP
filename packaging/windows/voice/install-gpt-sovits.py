#!/usr/bin/env python3
"""Deploy a verified, locally supplied GPT-SoVITS v2 bundle on Windows."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import venv
import zipfile


BUNDLE_VERSION = {
    "schema": 1,
    "model_version": "v2",
    "torch_version": "2.6.0+cu126",
    "torchaudio_version": "2.6.0+cu126",
    "lightning_version": "2.6.1",
}
ENGINE_FILES = (
    "api_v2.py",
    "GPT_SoVITS/s1_train.py",
    "GPT_SoVITS/s2_train.py",
    "GPT_SoVITS/pretrained_models/chinese-hubert-base/pytorch_model.bin",
    "GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/pytorch_model.bin",
    "GPT_SoVITS/pretrained_models/gsv-v2final-pretrained/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt",
    "GPT_SoVITS/pretrained_models/gsv-v2final-pretrained/s2G2333k.pth",
    "GPT_SoVITS/pretrained_models/gsv-v2final-pretrained/s2D2333k.pth",
)


def bundle_path(entry):
    name = entry.orig_filename.rstrip("/")
    parts = name.split("/")
    component = parts[0] in {"GPT-SoVITS", "runtime"}
    if name == "bundle.json" or (component and (len(parts) > 1 or entry.is_dir())):
        if all(
            part not in {"", ".", ".."} and not part.endswith((" ", "."))
            and not PureWindowsPath(part).is_reserved()
            and not any(ord(char) < 32 or char in '<>:"\\|?*' for char in part)
            for part in parts
        ):
            mode = stat.S_IFMT(entry.external_attr >> 16)
            if mode in {0, stat.S_IFDIR, stat.S_IFREG}:
                return Path(*parts)
    raise ValueError("GPT-SoVITS 组件包包含无效的文件路径或类型")


def unpack(archive, expected_hash, staging):
    with archive.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != expected_hash:
            raise ValueError("GPT-SoVITS 组件包 SHA-256 校验失败")
        source.seek(0)
        with zipfile.ZipFile(source) as bundle:
            metadata = bundle.getinfo("bundle.json")
            if metadata.file_size > 4096 or json.loads(bundle.read(metadata)) != BUNDLE_VERSION:
                raise ValueError("GPT-SoVITS 组件包版本不兼容")
            seen = set()
            for entry in bundle.infolist():
                relative = bundle_path(entry)
                folded = relative.as_posix().casefold()
                if folded in seen:
                    raise ValueError("GPT-SoVITS 组件包包含重复路径")
                seen.add(folded)
                target = staging / relative
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with bundle.open(entry) as stored, target.open("xb") as output:
                        shutil.copyfileobj(stored, output, 1024 * 1024)


def validate_runtime(runtime, working_directory):
    interpreter = runtime / "Scripts/python.exe"
    probe = """
import fastapi
import nltk
import pytorch_lightning
import torch
import torchaudio
import uvicorn
assert torch.__version__ == '2.6.0+cu126'
assert torchaudio.__version__ == '2.6.0+cu126'
assert pytorch_lightning.__version__ == '2.6.1'
nltk.data.find('corpora/cmudict')
print('PyTorch 2.6.0+cu126, CUDA available:', torch.cuda.is_available(), flush=True)
"""
    environment = dict(os.environ)
    environment.pop("NLTK_DATA", None)
    subprocess.run([str(interpreter), "-I", "-B", "-c", probe], cwd=working_directory,
                   env=environment, timeout=180, check=True)


def install(archive, expected_hash, directory):
    if sys.platform != "win32" or struct.calcsize("P") != 8 or sys.version_info[:2] != (3, 11):
        raise ValueError("GPT-SoVITS 组件离线安装需要 Windows x64 CPython 3.11")
    voice = directory / "voice"
    voice.mkdir(parents=True, exist_ok=True)
    runtime = voice / "gpt-sovits-python"
    engine = voice / "GPT-SoVITS"
    lock_path = voice / "gpt-sovits-install.lock"
    with ExitStack() as cleanup:
        try:
            lock = lock_path.open("x")
        except FileExistsError as error:
            raise ValueError("已有 GPT-SoVITS 安装任务或上次任务异常退出") from error
        cleanup.callback(lock_path.unlink, missing_ok=True)
        cleanup.enter_context(lock)
        if any(path.exists() or path.is_symlink() for path in (runtime, engine)):
            raise ValueError("目标已有 GPT-SoVITS 组件，请指定其他数据目录")
        with tempfile.TemporaryDirectory(prefix=".gpt-sovits-", dir=voice) as temporary:
            staging = Path(temporary)
            unpack(archive, expected_hash, staging)
            staged_engine = staging / "GPT-SoVITS"
            for relative in ENGINE_FILES:
                path = staged_engine / relative
                if not path.is_file() or not path.stat().st_size:
                    raise ValueError(f"GPT-SoVITS 组件包缺少文件 {relative}")
            staged_runtime = staging / "runtime"
            required = ("Lib/site-packages", "nltk_data", "Scripts/ffmpeg.exe")
            for relative in required:
                if not (staged_runtime / relative).exists():
                    raise ValueError(f"GPT-SoVITS 组件包缺少运行时文件 {relative}")

            # The venv launchers contain the installation path.
            runtime.mkdir()
            try:
                venv.EnvBuilder(with_pip=False).create(runtime)
                site = runtime / "Lib/site-packages"
                site.rmdir()
                for relative in required:
                    (staged_runtime / relative).rename(runtime / relative)
                if (staged_runtime / "vendor").is_dir():
                    (staged_runtime / "vendor").rename(runtime / "vendor")
                validate_runtime(runtime, staging)
                staged_engine.rename(engine)
            except BaseException:
                shutil.rmtree(runtime)
                raise
    print("Windows GPT-SoVITS v2 组件已安装。", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--sha256", required=True, help="组件包供应方提供的 SHA-256")
    parser.add_argument("--data-dir", type=Path)
    options = parser.parse_args()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", options.sha256):
        parser.error("--sha256 需要 64 位十六进制摘要")
    data_dir = (options.data_dir or os.environ.get("MEOWLIVE_DATA_DIR")
                or Path(__file__).resolve().parents[1])
    install(options.bundle.absolute(), options.sha256.lower(), Path(data_dir).absolute())


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(str(error), file=sys.stderr, flush=True)
        sys.exit(1)
