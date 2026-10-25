#!/usr/bin/env python3
"""校验固定 Ubuntu 包并解到仓库 .deps，保留归档和各包许可证。"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="仅使用已缓存且哈希相符的包")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    lock_path = root / "cmake" / "arrow-dependencies.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    operating_system = platform.freedesktop_os_release()
    if operating_system.get("ID") != "ubuntu" or operating_system.get("VERSION_ID") != "24.04":
        raise RuntimeError("固定二进制包适用于 Ubuntu 24.04 / WSL2")
    architecture = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    if architecture != "amd64":
        raise RuntimeError("固定二进制包需要 amd64 架构")
    prefix = root / ".deps" / lock["installation"]
    archives = prefix / "packages"
    archives.mkdir(parents=True, exist_ok=True)
    if shutil.which("dpkg-deb") is None:
        raise RuntimeError("需要 Ubuntu 的 dpkg-deb 来提取固定开发包")

    for package in lock["packages"]:
        archive = archives / package["filename"]
        if not archive.exists():
            if args.offline:
                raise RuntimeError(f"离线缓存缺少 {archive.name}")
            temporary = archive.with_suffix(archive.suffix + ".part")
            print(f"下载 {archive.name}", flush=True)
            with urllib.request.urlopen(package["url"], timeout=60) as source:
                with temporary.open("wb") as output:
                    shutil.copyfileobj(source, output)
            if digest(temporary) != package["sha256"]:
                raise RuntimeError(f"SHA-256 不符：{temporary}")
            temporary.replace(archive)
        if digest(archive) != package["sha256"]:
            raise RuntimeError(f"SHA-256 不符：{archive}")

    for package in lock["packages"]:
        subprocess.run(
            ["dpkg-deb", "--extract", str(archives / package["filename"]), str(prefix)],
            check=True,
        )
    for package in lock["packages"]:
        if not (prefix / package["license_file"]).is_file():
            raise RuntimeError(f"许可证文件缺失：{package['license_file']}")
    runtime_environment = os.environ.copy()
    runtime_directory = prefix / "usr/lib/x86_64-linux-gnu"
    previous_path = runtime_environment.get("LD_LIBRARY_PATH", "")
    runtime_environment["LD_LIBRARY_PATH"] = str(runtime_directory)
    if previous_path:
        runtime_environment["LD_LIBRARY_PATH"] += ":" + previous_path
    resolution = subprocess.run(
        ["ldd", str(runtime_directory / "libparquet.so.2600.0.0")],
        env=runtime_environment, text=True, capture_output=True, check=True,
    )
    missing = [line.strip() for line in resolution.stdout.splitlines() if "not found" in line]
    if missing:
        raise RuntimeError("Ubuntu 系统运行库缺失：" + "; ".join(missing))
    shutil.copy2(lock_path, prefix / "installation-lock.json")
    print(f"已校验并提取 {len(lock['packages'])} 个包：{prefix / 'usr'}")
    print(f"各包许可证：{prefix / 'usr/share/doc'}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
