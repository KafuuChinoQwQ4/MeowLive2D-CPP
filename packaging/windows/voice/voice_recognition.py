"""Load a pinned local recognition model in its own Windows environment."""

import argparse
from contextlib import redirect_stdout
from importlib.metadata import version
import json
import os
from pathlib import Path
import sys


def load_recognizer(engine, directory):
    os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_DATASETS_OFFLINE="1",
                      MODELSCOPE_OFFLINE="1", TOKENIZERS_PARALLELISM="false")
    if not directory.is_dir():
        raise ValueError("找不到本机转写模型目录")
    if engine == "faster-whisper":
        from faster_whisper import WhisperModel

        model = WhisperModel(str(directory), device="cpu", compute_type="int8",
                             cpu_threads=2, num_workers=1, local_files_only=True)
        return model, version("faster-whisper")
    if engine == "funasr":
        from funasr import AutoModel

        model = AutoModel(model=str(directory), hub="hf", device="cpu", ncpu=2,
                          disable_update=True, disable_pbar=True, trust_remote_code=False)
        return model, version("funasr")
    raise ValueError("不支持的转写引擎")


def supports_language(engine, model, language):
    if engine == "faster-whisper":
        return (language in model.supported_languages
                and model.hf_tokenizer.token_to_id(f"<|{language}|>") is not None)
    return language == "zh"


def recognize(engine, model, audio, language):
    if engine == "faster-whisper":
        segments, info = model.transcribe(str(audio), language=language, beam_size=5,
                                          vad_filter=True, condition_on_previous_text=False)
        return {"text": "".join(segment.text for segment in segments).strip(),
                "language": info.language}

    import soundfile

    samples, sample_rate = soundfile.read(str(audio), dtype="float32", always_2d=True)
    result = model.generate(input=samples.mean(axis=1), fs=sample_rate, disable_pbar=True)
    return {"text": " ".join(result[0]["text"].split()) if result else "", "language": "zh"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", choices=("faster-whisper", "funasr"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("--languages", nargs="+", required=True)
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--model-id")
    parser.add_argument("--revision")
    options = parser.parse_args()
    # Only the result goes to the main service. Import and model diagnostics go to stderr.
    try:
        with redirect_stdout(sys.stderr):
            directory = options.directory.resolve()
            if options.model_id:
                sys.path.insert(0, str(Path(__file__).resolve().parent))
                from voice_download import verified

                with Path(__file__).with_name("voice-models.json").open(encoding="utf-8") as source:
                    models = json.load(source)["models"]
                entry = next((item for item in models if item["id"] == options.model_id
                              and item["revision"] == options.revision
                              and item["engine_id"] == options.engine), None)
                if entry is None or not all(verified(directory / file["path"], file)
                                            for file in entry["files"]):
                    print(json.dumps({"error": "model_integrity"}), file=sys.__stdout__, flush=True)
                    return
            model, release = load_recognizer(options.engine, directory)
            if not all(supports_language(options.engine, model, code) for code in options.languages):
                result = {"error": "language"}
            elif options.audio is not None:
                result = recognize(options.engine, model, options.audio, options.languages[0])
            else:
                result = {"engine_version": release}
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        result = {"error": "recognizer"}
    print(json.dumps(result, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
