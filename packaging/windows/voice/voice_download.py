"""Stream pinned components into a cache on the installation volume."""

import hashlib
import http.client
import json
import os
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener, getproxies_environment


class DownloadError(ValueError):
    pass


class HTTPSRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, destination):
        if urlsplit(destination).scheme != "https":
            raise DownloadError("模型下载地址重定向到非 HTTPS 来源")
        return super().redirect_request(request, response, code, message, headers, destination)


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, ensure_ascii=False)
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


def download_opener():
    proxies = {}
    if sys.platform == "win32":
        from urllib.request import getproxies_registry

        proxies.update(getproxies_registry())
    environment = getproxies_environment()
    proxies.update(environment)
    return build_opener(ProxyHandler(proxies), HTTPSRedirect())


def cache_path(cache, entry):
    return cache / entry["sha256"] / Path(entry["path"]).name


def verified(path, entry):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != entry["bytes"]:
        return False
    with path.open("rb") as content:
        return hashlib.file_digest(content, "sha256").hexdigest() == entry["sha256"]


def resumable_offset(part, metadata, entry):
    try:
        with metadata.open(encoding="utf-8") as source:
            saved = json.load(source)
        size = part.stat().st_size
        if (saved.get("url") == entry["url"] and saved.get("bytes") == entry["bytes"]
                and saved.get("sha256") == entry["sha256"] and 0 < size < entry["bytes"]
                and isinstance(saved.get("etag"), str) and saved["etag"].startswith('"')
                and saved["etag"].endswith('"')):
            return size, saved["etag"]
    except (OSError, ValueError, AttributeError):
        pass
    return 0, ""


def receive(opener, entry, part, metadata, progress):
    url = entry["url"]
    if urlsplit(url).scheme != "https":
        raise DownloadError("模型下载来源必须使用 HTTPS")
    headers = {"Accept-Encoding": "identity", "User-Agent": "MeowLive2D/voice-models"}
    offset, etag = resumable_offset(part, metadata, entry)
    response = None
    if offset:
        try:
            response = opener.open(Request(url, headers={**headers, "Range": f"bytes={offset}-",
                                                         "If-Range": etag}), timeout=45)
        except HTTPError as error:
            if error.code != 416:
                raise
            error.close()
        if response is None or not (
            response.status == 206 and response.headers.get("ETag") == etag
            and response.headers.get("Content-Range") == f"bytes {offset}-{entry['bytes'] - 1}/{entry['bytes']}"
        ):
            if response is not None:
                response.close()
            response = None
            offset = 0

    if response is None:
        response = opener.open(Request(url, headers=headers), timeout=45)
    with response:
        length = response.headers.get("Content-Length")
        if ((offset == 0 and response.status != 200)
                or response.headers.get("Content-Encoding", "identity") != "identity"
                or (length is not None and length != str(entry["bytes"] - offset))):
            raise DownloadError("模型文件响应与清单长度或范围不符")
        with part.open("ab" if offset else "wb") as output:
            write_json(metadata, {**entry, "etag": response.headers.get("ETag", "")})
            size = offset
            progress("downloading", size)
            reported = size
            while chunk := response.read(512 * 1024):
                size += len(chunk)
                if size > entry["bytes"]:
                    raise DownloadError("模型文件超过清单长度")
                output.write(chunk)
                if size - reported >= 16 * 1024 * 1024:
                    progress("downloading", size)
                    reported = size
            output.flush()
            os.fsync(output.fileno())
        if size != entry["bytes"]:
            raise DownloadError("下载提前结束，可重试继续下载")


def download(cache, entry, progress, opener=None):
    target = cache_path(cache, entry)
    target.parent.mkdir(parents=True, exist_ok=True)
    progress("verifying", 0)
    if verified(target, entry):
        progress("verifying", entry["bytes"])
        return target
    target.unlink(missing_ok=True)
    part = target.with_name(target.name + ".part")
    metadata = target.with_name(target.name + ".resume.json")
    if not verified(part, entry):
        try:
            receive(opener or download_opener(), entry, part, metadata, progress)
        except (OSError, http.client.HTTPException) as error:
            raise DownloadError("无法下载模型文件，请检查网络后重试") from error
        progress("verifying", entry["bytes"])
        if not verified(part, entry):
            part.unlink(missing_ok=True)
            metadata.unlink(missing_ok=True)
            raise DownloadError("模型文件 SHA-256 与发布清单不符")
    part.replace(target)
    metadata.unlink(missing_ok=True)
    progress("verifying", entry["bytes"])
    return target
