"""Upload a qcow2 file to Alibaba Cloud OSS and refresh CDN cache.

Usage (called by CI):
    python3 scripts/ci/upload_to_oss.py <qcow2_path> <target> <arch>

Environment variables (from GitHub Secrets):
    OSS_REGION, OSS_ACCESS_KEY_ID, OSS_ACCESS_KEY_SECRET,
    OSS_BUCKET_NAME, OSS_PUBLIC_URL, OSS_TENBOX_IMAGES_DIR
"""

import os
import sys
import time
from pathlib import Path

import oss2
from alibabacloud_cdn20180510.client import Client as CdnClient
from alibabacloud_cdn20180510 import models as cdn_models
from alibabacloud_tea_openapi.models import Config as OpenApiConfig

UPLOAD_TIMEOUT = 300
CONNECT_TIMEOUT = 120
PART_SIZE = 10 * 1024 * 1024
MAX_RETRIES = 5
RETRY_BASE_DELAY = 10


def get_oss_dir(target: str, arch: str) -> str:
    """Derive the OSS directory name from target and arch.

    x86_64 omits arch suffix for backward compatibility.

    Examples:
        rootfs-qwenpaw + x86_64  -> qwenpaw
        rootfs-qwenpaw + arm64   -> qwenpaw-arm64
        rootfs-chromium + x86_64 -> chromium
        rootfs-chromium + arm64  -> chromium-arm64
        initramfs + x86_64       -> initramfs
        initramfs + arm64        -> initramfs-arm64
        kernel + x86_64          -> kernel
        kernel + arm64           -> kernel-arm64
    """
    if target in ("initramfs", "kernel"):
        name = target
    else:
        name = target.removeprefix("rootfs-")
    if arch == "arm64":
        name += "-arm64"
    return name


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <qcow2_path> <target> <arch>")
        sys.exit(1)

    qcow2_path = sys.argv[1]
    target = sys.argv[2]
    arch = sys.argv[3]

    region = os.environ["OSS_REGION"]
    endpoint = f"https://{region}.aliyuncs.com"
    auth = oss2.Auth(os.environ["OSS_ACCESS_KEY_ID"], os.environ["OSS_ACCESS_KEY_SECRET"])
    bucket = oss2.Bucket(
        auth, endpoint, os.environ["OSS_BUCKET_NAME"],
        connect_timeout=CONNECT_TIMEOUT,
    )

    images_dir = os.environ.get("OSS_TENBOX_IMAGES_DIR", "tenbox/images").strip("/")
    oss_dir = get_oss_dir(target, arch)
    filename = Path(qcow2_path).name
    oss_key = f"{images_dir}/{oss_dir}/{filename}"

    file_size = os.path.getsize(qcow2_path)
    print(f"Uploading {qcow2_path} ({file_size / 1024 / 1024:.1f} MB)")
    print(f"  -> oss://{os.environ['OSS_BUCKET_NAME']}/{oss_key}")

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            oss2.resumable_upload(
                bucket, oss_key, qcow2_path,
                multipart_threshold=10 * 1024 * 1024,
                part_size=PART_SIZE,
                num_threads=4,
            )
            break
        except oss2.exceptions.RequestError as e:
            if attempt == MAX_RETRIES:
                raise
            delay = RETRY_BASE_DELAY * (2 ** (attempt - 1))
            print(f"  Upload attempt {attempt}/{MAX_RETRIES} failed: {e}")
            print(f"  Retrying in {delay}s ...")
            time.sleep(delay)

    public_url = os.environ.get("OSS_PUBLIC_URL", "").rstrip("/")
    download_url = f"{public_url}/{oss_key}"
    print(f"Upload complete: {download_url}")

    refresh_cdn_cache(download_url)

    output_file = os.environ.get("GITHUB_OUTPUT")
    if output_file:
        with open(output_file, "a") as f:
            f.write(f"download_url={download_url}\n")


def refresh_cdn_cache(url: str, timeout: int = 120):
    """Refresh CDN cache for the given URL and wait for completion."""
    print(f"Refreshing CDN cache: {url}")
    config = OpenApiConfig(
        access_key_id=os.environ["OSS_ACCESS_KEY_ID"],
        access_key_secret=os.environ["OSS_ACCESS_KEY_SECRET"],
        endpoint="cdn.aliyuncs.com",
    )
    client = CdnClient(config)

    request = cdn_models.RefreshObjectCachesRequest(
        object_path=url,
        object_type="File",
    )
    response = client.refresh_object_caches(request)
    task_id = response.body.refresh_task_id
    print(f"  Refresh task submitted: {task_id}")

    deadline = time.time() + timeout
    interval = 5
    while time.time() < deadline:
        time.sleep(interval)
        query = cdn_models.DescribeRefreshTasksRequest(task_id=task_id)
        result = client.describe_refresh_tasks(query)
        tasks = result.body.tasks.to_map().get("CDNTask", [])
        if not tasks:
            continue
        status = tasks[0].get("Status", "")
        progress = tasks[0].get("Process", "")
        print(f"  CDN refresh: {status} ({progress})")
        if status == "Complete":
            print("  CDN cache refreshed successfully.")
            return
        interval = min(interval * 2, 15)

    print("  Warning: CDN refresh did not complete within timeout, continuing anyway.")


if __name__ == "__main__":
    main()
