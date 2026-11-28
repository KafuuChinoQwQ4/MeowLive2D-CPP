#!/usr/bin/env python3
"""Install one Windows voice model and publish its local receipt after a load test."""

import argparse
from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import sysconfig
import tempfile
from zipfile import BadZipFile, ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from voice_download import DownloadError, cache_path, download, verified, write_json
import gpt_training_runtime


def progress(state, received=0, total=0, filename="", message=""):
    print(json.dumps({"state": state, "received_bytes": received, "total_bytes": total,
                      "file": filename, "message": message}, ensure_ascii=False), flush=True)


@contextmanager
def installation_lock(path):
    import msvcrt

    with path.open("a+b") as handle:
        try:
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise DownloadError("这个数据目录已有模型安装任务") from error
        yield


def read_json(path):
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def revision_matches(directory, field, revision):
    try:
        receipt = read_json(directory / "receipt.json")
        return receipt.get("schema") == 1 and receipt.get(field) == revision
    except (OSError, ValueError, AttributeError):
        return False


def collect_assets(cache, files):
    total = sum(entry["bytes"] for entry in files)
    completed = 0
    for entry in files:
        def update(state, current):
            progress(state, completed + current, total, entry["path"])

        path = download(cache, entry, update)
        completed += entry["bytes"]
        yield entry, path


def wheel_install_size(wheels, limit):
    size = 0
    for wheel in wheels:
        with ZipFile(wheel) as archive:
            for entry in archive.infolist():
                name = PurePosixPath(entry.filename)
                if (name.is_absolute() or ".." in name.parts or ":" in entry.filename
                        or "\\" in entry.filename or stat.S_ISLNK(entry.external_attr >> 16)):
                    raise DownloadError("运行环境包含无效的 wheel 路径")
                size += entry.file_size
                if size > limit:
                    raise DownloadError("运行环境将超过清单安装占用上限")
    return size


def install_runtime(runtime, directory, staging, files):
    interpreter = directory / "Scripts/python.exe"
    if directory.exists():
        shutil.rmtree(directory)

    wheels = [files[entry["path"]] for entry in runtime["wheels"]]
    wheel_bytes = wheel_install_size(wheels, runtime["install_bytes"])
    sdists = [files[entry["path"]] for entry in runtime.get("sdists", ())]
    # Windows venv launchers retain their absolute path. The receipt publishes
    # the final location only after the model and this runtime load together.
    command = [str(interpreter), "-I", "-m", "pip", "--isolated", "install",
               "--disable-pip-version-check", "--no-index", "--no-deps", "--no-compile",
               "--no-cache-dir"]
    try:
        environment = {**os.environ, "TEMP": str(staging), "TMP": str(staging)}
        subprocess.run([sys.executable, "-I", "-B", "-m", "venv", str(directory)],
                       cwd=staging, env=environment, stdout=sys.stderr, check=True, timeout=120)
        requirements = staging / "wheels.txt"
        requirements.write_text("".join(f"{wheel.as_uri()}\n" for wheel in wheels), encoding="utf-8")
        subprocess.run([*command, "-r", str(requirements)], cwd=staging, env=environment,
                       stdout=sys.stderr, check=True, timeout=900)
        if sdists:
            subprocess.run([*command, "--no-build-isolation", *map(str, sdists)], cwd=staging,
                           env=environment, stdout=sys.stderr, check=True, timeout=300)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise DownloadError("安装 Windows Python 运行环境失败") from error
    archives = {entry["path"]: files[entry["path"]] for entry in runtime.get("assets", ())}
    if archives:
        gpt_training_runtime.prepare_runtime(directory, archives, runtime["install_bytes"] - wheel_bytes)
    return interpreter


def probe(interpreter, model, directory, cwd):
    recognizer = Path(__file__).with_name("voice_recognition.py")
    command = [str(interpreter), "-I", "-B", "-X", "utf8", str(recognizer),
               model["engine_id"], str(directory), "--languages", *model["languages"]]
    try:
        result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, check=True, timeout=180)
        release = json.loads(result.stdout)["engine_version"]
        if isinstance(release, str) and release:
            return release
    except (OSError, subprocess.SubprocessError, ValueError, KeyError):
        pass
    raise DownloadError("模型或运行环境通过摘要校验，但本机加载探测失败")


def install(catalog, identifier, revision, data):
    model = next((item for item in catalog["models"]
                  if item["id"] == identifier and item["revision"] == revision), None)
    if model is None or model["purpose"] not in {"transcription", "training"}:
        raise DownloadError("模型选择或修订无效")
    runtime = catalog["runtimes"][model["runtime_id"]]
    if (sys.platform != "win32" or sysconfig.get_platform() != "win-amd64"
            or f"{sys.version_info.major}.{sys.version_info.minor}" != runtime["python"]):
        raise DownloadError(f"请安装 Windows x64 CPython {runtime['python']} 后重新检测")
    if (not re.fullmatch(r"[a-zA-Z]:", data.drive)
            or ctypes.windll.kernel32.GetDriveTypeW(data.anchor) not in (2, 3)):
        raise DownloadError("模型须安装至 Windows 本机磁盘")

    voice = data / "voice"
    staging = voice / ".staging"
    cache = staging / "files"
    model_path = voice / "models" / identifier / revision
    runtime_path = voice / "runtimes" / model["runtime_id"] / runtime["revision"]
    staging.mkdir(parents=True, exist_ok=True)
    with installation_lock(staging / "install.lock"):
        for abandoned in staging.glob("model-*"):
            if abandoned.is_dir() and not abandoned.is_symlink():
                shutil.rmtree(abandoned)
        if model_path.exists():
            if (revision_matches(model_path, "revision", revision)
                    and revision_matches(runtime_path, "runtime_revision", runtime["revision"])
                    and (runtime_path / "Scripts/python.exe").is_file()
                    and all(verified(model_path / entry["path"], entry) for entry in model["files"])):
                return
            raise DownloadError("已有模型目录损坏，请先修复本机安装")

        has_runtime = (revision_matches(runtime_path, "runtime_revision", runtime["revision"])
                       and (runtime_path / "Scripts/python.exe").is_file())
        runtime_files = [] if has_runtime else [
            *runtime["wheels"], *runtime.get("sdists", ()), *runtime.get("assets", ())]
        files = [*runtime_files, *model["files"]]
        remaining = sum(entry["bytes"] for entry in files
                        if not verified(cache_path(cache, entry), entry))
        needed = remaining + sum(entry["bytes"] for entry in model["files"])
        if not has_runtime:
            needed += runtime["install_bytes"]
        if shutil.disk_usage(staging).free < needed:
            raise DownloadError("磁盘空间不足，无法同时保存下载和安装文件")

        try:
            with tempfile.TemporaryDirectory(prefix="model-", dir=staging) as temporary:
                work = Path(temporary)
                candidate = work / "model"
                candidate.mkdir()
                total = sum(entry["bytes"] for entry in files)
                verified_runtime = {}
                for index, (entry, path) in enumerate(collect_assets(cache, files)):
                    if index < len(runtime_files):
                        verified_runtime[entry["path"]] = path
                    else:
                        target = candidate / entry["path"]
                        target.parent.mkdir(parents=True, exist_ok=True)
                        try:
                            os.link(path, target)
                        except OSError:
                            shutil.copyfile(path, target)
                            if not verified(target, entry):
                                raise DownloadError("模型文件在安装时发生变化")
                progress("installing", total, total)
                python = (runtime_path / "Scripts/python.exe" if has_runtime else
                          install_runtime(runtime, runtime_path, work, verified_runtime))
                if model["purpose"] == "training":
                    engine_version = gpt_training_runtime.probe(
                        python, model["model_version"], candidate, runtime_path, work)
                else:
                    engine_version = probe(python, model, candidate, work)
                if not has_runtime:
                    write_json(runtime_path / "receipt.json", {"schema": 1, "runtime_revision": runtime["revision"],
                                                              "engine_version": engine_version})
                write_json(candidate / "receipt.json", {"schema": 1, "id": identifier, "revision": revision,
                                                       "runtime_revision": runtime["revision"],
                                                       "engine_version": engine_version})
                model_path.parent.mkdir(parents=True, exist_ok=True)
                candidate.rename(model_path)
        finally:
            if not has_runtime and not (runtime_path / "receipt.json").is_file() and runtime_path.exists():
                shutil.rmtree(runtime_path, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("id")
    parser.add_argument("revision")
    parser.add_argument("--data-dir", type=Path, default=Path(__file__).resolve().parents[1],
                        help="安装根目录，默认使用应用安装目录")
    args = parser.parse_args()
    try:
        catalog = read_json(Path(__file__).with_name("voice-models.json"))
        install(catalog, args.id, args.revision, args.data_dir.resolve())
        progress("ready")
    except DownloadError as error:
        progress("failed", message=str(error))
    except (OSError, RuntimeError, ValueError, BadZipFile) as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        progress("failed", message="无法完成本机模型安装，请检查磁盘和 Python 环境")


if __name__ == "__main__":
    main()
