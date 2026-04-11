"""Update images.json with newly built image metadata.

Usage (called by CI after build jobs finish):
    python3 scripts/ci/update_images_json.py --target <target> --meta-dir <dir>
    python3 scripts/ci/update_images_json.py ... --enable-sha256  # optional: fill sha256 from meta

By default sha256 is written as "" so clients skip verification. That avoids a race where OSS
objects are updated before images.json is merged (users would otherwise download GBs then fail
SHA checks). Pass --enable-sha256 when you want checksums in the manifest.

Supported targets:
    rootfs-chromium, rootfs-copaw, rootfs-hermes, rootfs-openclaw  — updates rootfs.qcow2 for a specific image
    initramfs                                       — updates initrd.gz for ALL images of matching platform
    kernel                                          — updates vmlinuz for ALL images of matching platform

The meta-dir contains JSON files (one per arch) with:
    { "arch": "x86_64", "filename": "...", "sha256": "...", "size": 123456 }

Environment variables:
    OSS_PUBLIC_URL, OSS_TENBOX_IMAGES_DIR
"""

import argparse
import json
import os
from datetime import date
from pathlib import Path

IMAGES_JSON_PATH = Path(__file__).resolve().parent.parent.parent / "website" / "public" / "api" / "images.json"

DISPLAY_NAMES = {
    "chromium": "Chromium",
    "copaw": "CoPaw",
    "hermes": "Hermes",
    "openclaw": "OpenClaw",
}

PLATFORM_MAP = {
    "arm64": "arm64",
    "x86_64": "x86_64",
}


def get_image_id(target: str, arch: str) -> str:
    """Map target + arch to images.json id / OSS directory name.

    x86_64 omits arch suffix for backward compatibility with existing ids.
    """
    name = target.removeprefix("rootfs-")
    if arch == "arm64":
        name += "-arm64"
    return name


def get_oss_dir(target: str, arch: str) -> str:
    """Derive the OSS directory name."""
    if target in ("initramfs", "kernel"):
        name = target
        if arch == "arm64":
            name += "-arm64"
        return name
    return get_image_id(target, arch)


def extract_version(filename: str, target: str, arch: str) -> str:
    """Extract version from qcow2 filename.

    Filename format: rootfs-<name>[-<version>]-<arch>.qcow2

    Examples:
        rootfs-copaw-0.0.7-x86_64.qcow2 -> 0.0.7
        rootfs-copaw-0.0.7-arm64.qcow2 -> 0.0.7
        rootfs-openclaw-2026.3.11-x86_64.qcow2 -> 2026.3.11
        rootfs-chromium-x86_64.qcow2 -> ""
    """
    base = filename.removesuffix(f"-{arch}.qcow2")
    name = target.removeprefix("rootfs-")
    prefix = f"rootfs-{name}-"
    if base.startswith(prefix):
        return base[len(prefix):]
    return ""


def update_rootfs_entry(
    images: list[dict], target: str, meta: dict, *, use_sha256: bool
) -> str | None:
    arch = meta["arch"]
    image_id = get_image_id(target, arch)
    filename = meta["filename"]
    sha256 = meta["sha256"] if use_sha256 else ""
    size = meta["size"]
    version = extract_version(filename, target, arch)

    images_dir = os.environ.get("OSS_TENBOX_IMAGES_DIR", "tenbox/images").strip("/")
    public_url = os.environ.get("OSS_PUBLIC_URL", "").rstrip("/")
    download_url = f"{public_url}/{images_dir}/{image_id}/{filename}"

    image_entry = None
    for img in images:
        if img.get("id") == image_id:
            image_entry = img
            break

    if image_entry is None:
        print(f"  Image ID '{image_id}' not found in images.json, skipping")
        return None

    if version:
        image_entry["version"] = version
        name_base = target.removeprefix("rootfs-")
        display_name = DISPLAY_NAMES.get(name_base, name_base.title())
        if arch == "arm64":
            image_entry["name"] = f"{display_name} {version} (ARM64)"
        else:
            image_entry["name"] = f"{display_name} {version}"

    image_entry["updated_at"] = date.today().isoformat()

    for file_entry in image_entry.get("files", []):
        if file_entry["name"] == "rootfs.qcow2":
            file_entry["url"] = download_url
            file_entry["sha256"] = sha256
            file_entry["size"] = size
            break

    print(f"  Updated {image_id}: version={version or '(unchanged)'}, url={download_url}")
    return version or None


def update_kernel_entries(images: list[dict], meta: dict, *, use_sha256: bool) -> int:
    """Update vmlinuz for ALL images matching the platform."""
    arch = meta["arch"]
    filename = meta["filename"]
    sha256 = meta["sha256"] if use_sha256 else ""
    size = meta["size"]
    platform = PLATFORM_MAP.get(arch, arch)

    images_dir = os.environ.get("OSS_TENBOX_IMAGES_DIR", "tenbox/images").strip("/")
    public_url = os.environ.get("OSS_PUBLIC_URL", "").rstrip("/")
    oss_dir = get_oss_dir("kernel", arch)
    download_url = f"{public_url}/{images_dir}/{oss_dir}/{filename}"

    updated = 0
    for img in images:
        if img.get("platform") != platform:
            continue
        for file_entry in img.get("files", []):
            if file_entry["name"] == "vmlinuz":
                file_entry["url"] = download_url
                file_entry["sha256"] = sha256
                file_entry["size"] = size
                updated += 1
                break

    print(f"  {arch}: updated {updated} image(s) -> {download_url}")
    return updated


def update_initramfs_entries(images: list[dict], meta: dict, *, use_sha256: bool) -> int:
    arch = meta["arch"]
    filename = meta["filename"]
    sha256 = meta["sha256"] if use_sha256 else ""
    size = meta["size"]
    platform = PLATFORM_MAP.get(arch, arch)

    images_dir = os.environ.get("OSS_TENBOX_IMAGES_DIR", "tenbox/images").strip("/")
    public_url = os.environ.get("OSS_PUBLIC_URL", "").rstrip("/")
    oss_dir = get_oss_dir("initramfs", arch)
    download_url = f"{public_url}/{images_dir}/{oss_dir}/{filename}"

    updated = 0
    for img in images:
        if img.get("platform") != platform:
            continue
        for file_entry in img.get("files", []):
            if file_entry["name"] == "initrd.gz":
                file_entry["url"] = download_url
                file_entry["sha256"] = sha256
                file_entry["size"] = size
                updated += 1
                break

    print(f"  {arch}: updated {updated} image(s) -> {download_url}")
    return updated


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, help="Build target (e.g. rootfs-copaw, initramfs)")
    parser.add_argument("--meta-dir", required=True, help="Directory containing per-arch JSON metadata files")
    parser.add_argument(
        "--enable-sha256",
        action="store_true",
        help="Write sha256 from build metadata (default: write empty string, skip client verification)",
    )
    args = parser.parse_args()

    meta_dir = Path(args.meta_dir)
    meta_files = sorted(meta_dir.glob("*.json"))
    if not meta_files:
        print(f"No metadata files found in {meta_dir}")
        return

    data = json.loads(IMAGES_JSON_PATH.read_text(encoding="utf-8"))
    images = data.get("images", [])

    use_sha256 = args.enable_sha256
    print(f"Updating images.json for {args.target} (sha256={'from meta' if use_sha256 else 'empty'}):")
    versions = set()
    for meta_file in meta_files:
        meta = json.loads(meta_file.read_text())
        if args.target == "initramfs":
            update_initramfs_entries(images, meta, use_sha256=use_sha256)
        elif args.target == "kernel":
            update_kernel_entries(images, meta, use_sha256=use_sha256)
        else:
            ver = update_rootfs_entry(images, args.target, meta, use_sha256=use_sha256)
            if ver:
                versions.add(ver)

    IMAGES_JSON_PATH.write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    version_str = ", ".join(sorted(versions)) if versions else ""
    output_file = os.environ.get("GITHUB_OUTPUT")
    if output_file and version_str:
        with open(output_file, "a") as f:
            f.write(f"version={version_str}\n")
        print(f"Set output version={version_str}")

    print("Done.")


if __name__ == "__main__":
    main()
