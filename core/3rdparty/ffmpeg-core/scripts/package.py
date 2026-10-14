import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--triplet", required=True)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    vcpkg = Path(os.environ["VCPKG_ROOT"])
    package = vcpkg / "packages" / f"ffmpeg_{args.triplet}"
    build = vcpkg / "buildtrees" / "ffmpeg"
    stage = root / "_package"
    stage.mkdir()
    shutil.copytree(package / "lib", stage / "lib")
    shutil.copytree(package / "include", stage / "include")
    share = stage / "share" / "ffmpeg"
    share.mkdir(parents=True)
    shutil.copy2(package / "share" / "ffmpeg" / "copyright", share)
    configs = list((build / f"{args.triplet}-rel").rglob("config.h"))
    if not configs:
        raise RuntimeError("FFmpeg build configuration is missing")
    required = ("VP8_DECODER", "VP9_DECODER", "OPUS_DECODER", "VORBIS_DECODER", "MATROSKA_DEMUXER")
    for config in configs:
        text = config.read_text()
        if any(f"#define CONFIG_{option} 1" in text for option in ("GPL", "GPLV3", "NONFREE", "VERSION3")):
            raise RuntimeError("Unexpected FFmpeg license configuration")
        components = (config.parent / "config_components.h").read_text()
        for component in required:
            if f"#define CONFIG_{component} 1" not in components:
                raise RuntimeError(f"Missing {component}")
    log = build / f"build-{args.triplet}-rel-out.log"
    if "License: LGPL version 2.1 or later" not in log.read_text(errors="replace"):
        raise RuntimeError("Unexpected FFmpeg license")
    shutil.copy2(log, share / "build-log.txt")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    vcpkg_revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=vcpkg, text=True).strip()
    (share / "SOURCE.txt").write_text(
        "This software uses FFmpeg under the GNU LGPL version 2.1 or later.\n"
        "This software is based in part on the work of the Independent JPEG Group.\n"
        f"Build recipe: https://github.com/KytyPS5/ext-ffmpeg-core/tree/{revision}\n"
        f"Source: https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/{revision[:12]}/ffmpeg-source-{args.name}.tar.gz\n"
        f"vcpkg revision: {vcpkg_revision}\nTriplet: {args.triplet}\n",
        encoding="utf-8",
    )
    artifacts = root / "artifacts"
    artifacts.mkdir(exist_ok=True)
    with zipfile.ZipFile(artifacts / f"ffmpeg-{args.name}.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for file in sorted(stage.rglob("*")):
            if file.is_file():
                archive.write(file, file.relative_to(stage))
    sources = [path for path in (build / "src").iterdir() if (path / "configure").is_file()]
    if len(sources) != 1:
        raise RuntimeError(f"Expected one patched FFmpeg source tree, found {len(sources)}")
    with tarfile.open(artifacts / f"ffmpeg-source-{args.name}.tar.gz", "w:gz") as archive:
        archive.add(sources[0], arcname="ffmpeg")
        archive.add(share, arcname="build-info")
        archive.add(vcpkg / "ports" / "ffmpeg", arcname="vcpkg-port")
        for file in subprocess.check_output(["git", "ls-files", "-z"], cwd=root).split(b"\0"):
            if file:
                name = file.decode()
                archive.add(root / name, arcname=f"recipe/{name}")
        for config in configs:
            for name in ("config.h", "config_components.h", "ffbuild/config.mak", "ffbuild/config.log"):
                path = config.parent / name
                if path.is_file():
                    archive.add(path, arcname=f"configuration/{config.parent.name}/{name}")


if __name__ == "__main__":
    main()
