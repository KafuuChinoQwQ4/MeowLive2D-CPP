"""Prepare and probe the pinned GPT-SoVITS Windows training runtime."""

from contextlib import redirect_stdout
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_workspace import assets, copy_model, environment, workspace
from voice_download import DownloadError, verified


def unpack(archive_path, destination, budget, strip_root=False):
    with ZipFile(archive_path) as archive:
        entries = archive.infolist()
        unpacked = sum(entry.file_size for entry in entries)
        if unpacked > budget:
            raise DownloadError("训练运行环境解包大小超过清单上限")
        root = PurePosixPath(entries[0].filename).parts[:1] if entries else ()
        for entry in entries:
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or ".." in path.parts or ":" in entry.filename
                    or "\\" in entry.filename or stat.S_ISLNK(entry.external_attr >> 16)
                    or (strip_root and path.parts[:1] != root)):
                raise DownloadError("训练运行环境包含无效的压缩包路径")
            parts = path.parts[1:] if strip_root else path.parts
            if not parts or entry.is_dir():
                continue
            target = destination.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
    return budget - unpacked


def prepare_runtime(directory, archives, budget):
    engine = directory / "engine"
    budget = unpack(archives["gpt-sovits-engine.zip"], engine, budget, strip_root=True)
    budget = unpack(archives["G2PWModel.zip"], engine / "GPT_SoVITS/text", budget)
    for name, archive in archives.items():
        if name.startswith("nltk_data/"):
            budget = unpack(archive, directory / PurePosixPath(name).parent, budget)

    # The pinned Windows runtime uses the portable Jieba distribution.
    for filename in ("chinese.py", "chinese2.py", "tone_sandhi.py"):
        path = engine / "GPT_SoVITS/text" / filename
        code = path.read_text(encoding="utf-8")
        path.write_text(code.replace("jieba_fast", "jieba"), encoding="utf-8")

    executable = next((directory / "Lib/site-packages/imageio_ffmpeg/binaries").glob("ffmpeg-*.exe"), None)
    if executable is None:
        raise DownloadError("训练组件缺少 Windows FFmpeg")
    binary = directory / "bin" / "ffmpeg.exe"
    binary.parent.mkdir()
    copy_model(executable, binary)


def local_environment(runtime):
    result = {}
    if (runtime / "bin" / "ffmpeg.exe").is_file():
        result["PATH"] = str(runtime / "bin") + os.pathsep + os.environ.get("PATH", "")
    if (runtime / "nltk_data").is_dir():
        result["NLTK_DATA"] = str(runtime / "nltk_data")
    return result


def validate_managed_model(directory, job):
    manifest = Path(__file__).with_name("voice-models.json")
    catalog = json.loads(manifest.read_text(encoding="utf-8"))
    selected = next((model for model in catalog["models"]
                     if model["purpose"] == "training"
                     and model["id"] == job.get("base_model_id")
                     and model["revision"] == job.get("base_model_revision")
                     and model["model_version"] == job["model_version"]), None)
    if selected is None or not all(verified(directory / asset["path"], asset) for asset in selected["files"]):
        raise DownloadError("训练底模与选定的发布修订不符")


def probe(interpreter, version, model_path, runtime, work):
    args = [str(interpreter), "-I", "-B", "-X", "utf8", str(Path(__file__).resolve()),
            version, str(model_path), str(runtime), str(work)]
    try:
        result = subprocess.run(args, cwd=work, capture_output=True, check=True, timeout=300)
        record = json.loads(result.stdout)
        if record.get("error"):
            raise DownloadError(record["error"])
        return record["engine_version"]
    except subprocess.CalledProcessError as error:
        if error.stderr:
            sys.stderr.buffer.write(error.stderr[-8192:])
        raise DownloadError("训练底模或运行环境通过摘要校验，但预处理加载失败") from error
    except DownloadError:
        raise
    except (OSError, subprocess.TimeoutExpired, ValueError, KeyError) as error:
        raise DownloadError("无法完成训练底模的本机加载检测") from error


def check_runtime(version, model_path, runtime, directory):
    engine = runtime / "engine"
    model = assets(engine, model_path, version)
    work = workspace(engine, directory / "probe", model_path)
    os.chdir(work)
    os.environ.update(environment(work, model, version=version))
    os.environ.update(local_environment(runtime))
    sys.path.insert(0, str(work))
    sys.path.insert(0, str(work / "GPT_SoVITS"))
    with redirect_stdout(sys.stderr):
        import torch
        if not torch.cuda.is_available() or torch.ones(1, device="cuda").item() != 1:
            raise DownloadError("训练需要支持 CUDA 12.6 的 NVIDIA GPU 与驱动")
        import pytorch_lightning
        import torchaudio
        from nltk import data
        from text.cleaner import clean_text

        if not pytorch_lightning.__version__ or not torchaudio.__version__:
            raise DownloadError("Windows 训练运行环境缺少依赖")
        data.find("corpora/cmudict")
        data.find("taggers/averaged_perceptron_tagger_eng")
        subprocess.run([str(runtime / "bin/ffmpeg.exe"), "-version"],
                       stdout=subprocess.DEVNULL, check=True, timeout=15)
        phones, _, _ = clean_text("你好，欢迎来到直播间。", "zh", version=version)
        if not phones:
            raise RuntimeError("Chinese preprocessing produced no phonemes")
    return torch.__version__


if __name__ == "__main__":
    version, model, runtime, work = sys.argv[1:]
    try:
        release = check_runtime(version, Path(model), Path(runtime), Path(work))
        print(json.dumps({"engine_version": release}))
    except DownloadError as error:
        print(json.dumps({"error": str(error)}))
