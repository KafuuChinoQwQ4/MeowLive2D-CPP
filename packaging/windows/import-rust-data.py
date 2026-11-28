#!/usr/bin/env python3
"""Copy a Rust installation snapshot into a new native Windows data directory."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import tomllib


SETTINGS_SUFFIXES = ("-agent.json", "-llm.json", "-live.json")
TRAINING_MODEL_VERSIONS = {"v2", "v2Pro", "v2ProPlus", "v3", "v4"}
TRAINING_TERMINAL_STATES = {
    "succeeded": "completed", "failed": "failed", "cancelled": "cancelled",
    "interrupted": "failed",
}
TRAINING_INTERRUPTED_STATES = {"queued", "preparing", "training", "validating", "cancelling"}
TRAINING_ARTIFACTS = {"gpt": "artifacts/gpt.ckpt", "sovits": "artifacts/sovits.pth"}
SECTIONS = {
    "server": {"listen_address", "queue_capacity", "history_limit", "allowed_origins"},
    "auth": {
        "enabled", "admin_token_env", "device_token_env", "session_lifetime_seconds",
        "admin_token_file", "device_token_file",
    },
    "speech": {
        "base_url", "reference_audio", "prompt_text", "prompt_language", "text_language",
        "timeout_seconds", "max_audio_bytes", "sentence_batch_size", "max_concurrency",
    },
    "agent": {"persona", "system_prompt", "topic", "proactive_enabled", "cooldown_ms"},
    "llm": {
        "mode", "provider", "api_format", "base_url", "model", "api_key_env",
        "timeout_seconds", "max_tokens", "json_mode",
    },
    "live": {
        "enabled", "app_id", "access_key_id_env", "access_key_secret_env",
        "identity_code_env", "reconnect_initial_ms", "reconnect_max_ms",
    },
    "viewers": {"enabled", "scope_id", "database_url_env", "calendar_offset_minutes"},
    "memory": {
        "endpoint", "model", "api_key_env", "timeout_ms", "embedding_endpoint",
        "embedding_model", "embedding_api_key_env", "embedding_dimensions",
    },
    "graph": {"endpoint", "user", "password_env"},
}


def copy_regular(source, target):
    if not source.is_file() or source.is_symlink():
        raise ValueError("Source contains a missing or non-regular file")
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as original, target.open("xb") as result:
        shutil.copyfileobj(original, result)
    target.chmod(0o600)


def copy_directory(source, target):
    if not source.is_dir() or source.is_symlink():
        raise ValueError("Source asset directory must be a real directory")
    target.mkdir(parents=True)
    for item in source.iterdir():
        if item.is_dir() and not item.is_symlink():
            copy_directory(item, target / item.name)
        else:
            copy_regular(item, target / item.name)


def unique_json_object(entries):
    result = {}
    for key, value in entries:
        if key in result:
            raise ValueError("Duplicate field in Rust training data")
        result[key] = value
    return result


def training_json(path, limit):
    try:
        with path.open("rb") as source:
            content = source.read(limit + 1)
        if len(content) <= limit:
            result = json.loads(content.decode("utf-8"), object_pairs_hook=unique_json_object)
            if isinstance(result, dict):
                return result
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
        pass
    raise ValueError("Invalid Rust training JSON")


def write_training_json(path, data):
    with path.open("x", encoding="utf-8", newline="\n") as target:
        json.dump(data, target, ensure_ascii=False, separators=(",", ":"))
    path.chmod(0o600)


def verify_training_pair(directory, job_id, pair):
    if not isinstance(pair, dict) or not isinstance(pair.get("model_version"), str):
        raise ValueError("Invalid Rust training weight pair")
    version = pair["model_version"]
    if version not in TRAINING_MODEL_VERSIONS or any(
        pair.get(name) != relative for name, relative in TRAINING_ARTIFACTS.items()
    ):
        raise ValueError("Invalid Rust training weight pair")
    manifest = training_json(directory / "artifacts" / "manifest.json", 4096)
    hashes = manifest.get("sha256")
    if (
        manifest.get("job_id") != job_id or manifest.get("model_version") != version
        or not isinstance(hashes, dict)
    ):
        raise ValueError("Rust training weight manifest does not match the job")
    for name, relative in TRAINING_ARTIFACTS.items():
        path = directory / relative
        # Match the C++ runtime's accepted artifact size.
        if not path.is_file() or not 0 < path.stat().st_size <= 2 * 1024**3:
            raise ValueError("Rust training weight is missing, empty, or too large")
        with path.open("rb") as source:
            if hashlib.file_digest(source, "sha256").hexdigest() != hashes.get(name):
                raise ValueError("Rust training weight digest does not match the manifest")
    return version


def training_job_status(root, job):
    if not isinstance(job, dict):
        raise ValueError("Invalid Rust training job")
    job_id, name, voice_id, state, message, version = (
        job.get(key) for key in ("id", "name", "voice_id", "state", "message", "model_version")
    )
    job_id_pattern = r"[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}"
    if not isinstance(job_id, str) or not re.fullmatch(job_id_pattern, job_id):
        raise ValueError("Invalid Rust training job ID")
    if not isinstance(voice_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", voice_id):
        raise ValueError("Invalid Rust training voice ID")
    if not isinstance(name, str) or not 1 <= len(name.strip()) <= 80:
        raise ValueError("Invalid Rust training job name")
    if not isinstance(message, str) or len(message) > 200:
        raise ValueError("Invalid Rust training job message")
    if (
        not isinstance(state, str)
        or state not in TRAINING_TERMINAL_STATES and state not in TRAINING_INTERRUPTED_STATES
    ):
        raise ValueError("Invalid Rust training job state")
    if version is not None and (
        not isinstance(version, str) or version not in TRAINING_MODEL_VERSIONS
    ):
        raise ValueError("Invalid Rust training model version")

    bounds = {"progress": (0, 100), "clip_count": (2, 32),
              "created_at_ms": (0, 2**64 - 1), "updated_at_ms": (0, 2**64 - 1)}
    for key, (minimum, maximum) in bounds.items():
        value = job.get(key)
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError("Invalid Rust training job progress, clip count or timestamp")

    directory = root / "jobs" / job_id
    auditioned, saved = job.get("auditioned"), job.get("saved")
    if not directory.is_dir() or type(auditioned) is not bool or (
        saved is not None and (type(saved) is not bool or saved and not auditioned)
    ):
        raise ValueError("Invalid Rust training job directory or saved state")
    pair = job.get("artifacts")
    completed = state == "succeeded"
    if completed:
        version = verify_training_pair(directory, job_id, pair)
    elif pair is not None or auditioned:
        raise ValueError("Unfinished Rust training job has a published model")
    elif version is None and (directory / "job.json").is_file():
        prepared = training_json(directory / "job.json", 512 * 1024)
        persisted_version = prepared.get("model_version")
        if isinstance(persisted_version, str) and persisted_version in TRAINING_MODEL_VERSIONS:
            version = persisted_version

    status = {key: job[key] for key in ("id", "name", "voice_id", *bounds)}
    status["model_version"] = version or ""
    status["status"] = TRAINING_TERMINAL_STATES.get(state, "failed")
    status["message"] = "已中断的旧版训练任务" if state in TRAINING_INTERRUPTED_STATES else message
    status["version_id"] = job_id if completed else None
    return status


def convert_training(root):
    catalog_path = root / "catalog.json"
    if not catalog_path.exists():
        return
    catalog = training_json(catalog_path, 512 * 1024)
    jobs, active = catalog.get("jobs"), catalog.get("active_versions")
    if (
        type(catalog.get("schema")) is not int or catalog["schema"] != 1
        or not isinstance(jobs, list) or len(jobs) > 64
        or not isinstance(active, dict) or len(active) > 64
    ):
        raise ValueError("Invalid or unsupported Rust training catalog")

    imported = {}
    for job in jobs:
        status = training_job_status(root, job)
        job_id = status["id"]
        if job_id in imported:
            raise ValueError("Duplicate Rust training job")
        imported[job_id] = (job, status)

    for voice, job_id in active.items():
        if not isinstance(job_id, str) or job_id not in imported:
            raise ValueError("Invalid active Rust training model reference")
        job, status = imported[job_id]
        if (
            voice != status["voice_id"] or status["status"] != "completed"
            or not job["auditioned"] or job.get("saved") is False
        ):
            raise ValueError("Invalid active Rust training model reference")

    for job_id, (_, status) in imported.items():
        write_training_json(root / "jobs" / job_id / "status.json", status)
    write_training_json(root / "active-versions.json", active)


def toml_value(value):
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(item) for item in value) + "]"
    raise ValueError("Unexpected saved TOML field type")


def write_configuration(config, staging, options):
    sections = {}
    for section, keys in SECTIONS.items():
        original = config.get(section, {})
        sections[section] = {key: val for key, val in original.items() if key in keys}

    # A migrated copy first opens with a loopback listener and offline viewer storage.
    sections["server"]["listen_address"] = "127.0.0.1:19601"
    sections["viewers"]["enabled"] = False
    sections["memory"]["enabled"] = False
    sections["graph"]["enabled"] = False
    speech = sections["speech"]
    if speech.get("reference_audio"):
        if options.reference is None:
            raise ValueError("Default voice has a reference path. Pass its WAV with --reference")
        copy_regular(options.reference, staging / "resources" / "default-reference.wav")
        speech["reference_audio"] = "resources/default-reference.wav"

    auth = sections["auth"]
    for role in ("admin", "device"):
        field = f"{role}_token_file"
        original = auth.pop(field, None)
        if original:
            source = options.server.parent / original
            if not source.is_file():
                raise ValueError(f"Missing {role} credential file in source snapshot")
            target = Path("local") / f"{role}-token.txt"
            copy_regular(source, staging / target)
            auth[field] = target.as_posix()

    with (staging / "server.toml").open("x", encoding="utf-8", newline="\n") as target:
        for name, settings in sections.items():
            target.write(f"[{name}]\n")
            for key, value in settings.items():
                target.write(f"{key} = {toml_value(value)}\n")
            target.write("\n")
    (staging / "server.toml").chmod(0o600)


def run(options):
    if options.destination.exists() or options.destination.is_symlink():
        raise ValueError("Destination already exists")
    destination = options.destination.absolute()
    if not destination.parent.is_dir():
        raise ValueError("Create the destination parent directory first")
    with options.server.open("rb") as saved:
        config = tomllib.load(saved)

    staging = Path(tempfile.mkdtemp(prefix=".meowlive-import-", dir=destination.parent))
    try:
        for suffix in SETTINGS_SUFFIXES:
            source = options.server.parent / "local" / (options.server.stem + suffix)
            if source.exists() or source.is_symlink():
                copy_regular(source, staging / "local" / ("server" + suffix))
        if options.resources:
            copy_directory(options.resources, staging / "resources")
        if options.training:
            target = staging / "training"
            copy_directory(options.training, target)
            convert_training(target)
        if options.obs:
            copy_regular(options.obs, staging / "desktop.obs.local.json")
        if options.viewer_backup:
            copy_regular(options.viewer_backup, staging / "backup" / "viewers.dump")
        write_configuration(config, staging, options)
        if destination.exists() or destination.is_symlink():
            raise ValueError("Destination was created during import")
        os.rename(staging, destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path, help="Rust server TOML snapshot")
    parser.add_argument("destination", type=Path, help="new user data directory")
    parser.add_argument("--resources", type=Path)
    parser.add_argument("--training", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--obs", type=Path)
    parser.add_argument("--viewer-backup", type=Path)
    options = parser.parse_args()
    try:
        run(options)
    except (OSError, ValueError) as error:
        if isinstance(error, OSError):
            parser.exit(1, "Import failed while reading or writing the data snapshot.\n")
        if isinstance(error, tomllib.TOMLDecodeError):
            parser.exit(1, "Source TOML is invalid.\n")
        parser.exit(1, f"{error}\n")
    print("Imported user data into the new directory.")


if __name__ == "__main__":
    main()
