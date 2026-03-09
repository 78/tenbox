"""
TenBox Publish Manager - GUI tool for managing image and installer releases.

Usage:
    python publish.py

Prerequisites:
    pip install oss2 python-dotenv
"""

import hashlib
import json
import os
import sys
import threading
import time
import tkinter as tk
import xml.etree.ElementTree as ET
from datetime import date, datetime
from email.utils import format_datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    import oss2
except ImportError:
    oss2 = None

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None

SCRIPTS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPTS_DIR.parent
IMAGES_JSON_PATH = PROJECT_DIR / "website" / "public" / "api" / "images.json"
VERSION_JSON_PATH = PROJECT_DIR / "website" / "public" / "api" / "version.json"
APPCAST_XML_PATH = PROJECT_DIR / "website" / "public" / "api" / "appcast.xml"

SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
DC_NS = "http://purl.org/dc/elements/1.1/"


def generate_appcast_xml(mac_info: dict) -> str:
    """Build an appcast.xml string from macOS release info.

    ``mac_info`` keys: latest_version, release_notes, release_date,
    download_url, sha256, size, min_os, ed_signature.
    """
    version = mac_info.get("latest_version", "")
    notes = mac_info.get("release_notes", "")
    release_date = mac_info.get("release_date", "")
    download_url = mac_info.get("download_url", "")
    ed_signature = mac_info.get("ed_signature", "")
    size = mac_info.get("size", 0)
    min_os = mac_info.get("min_os", "13.0").replace("macOS ", "")

    notes_html_lines = "".join(
        f"          <li>{line}</li>\n"
        for line in notes.replace("\r\n", "\n").split("\n")
        if line.strip()
    )
    description_html = (
        f"        <ul>\n{notes_html_lines}        </ul>"
    )

    try:
        dt = datetime.fromisoformat(release_date)
        pub_date = format_datetime(dt)
    except Exception:
        pub_date = release_date

    return f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="{SPARKLE_NS}" xmlns:dc="{DC_NS}">
  <channel>
    <title>TenBox</title>
    <link>https://tenbox.ai/api/appcast.xml</link>
    <description>TenBox macOS Updates</description>
    <language>zh-CN</language>
    <item>
      <title>Version {version}</title>
      <description><![CDATA[
{description_html}
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <sparkle:minimumSystemVersion>{min_os}</sparkle:minimumSystemVersion>
      <enclosure
        url="{download_url}"
        sparkle:version="{version}"
        sparkle:shortVersionString="{version}"
        sparkle:edSignature="{ed_signature}"
        length="{size}"
        type="application/octet-stream"
      />
    </item>
  </channel>
</rss>
"""


# ---------------------------------------------------------------------------
# OSS Client
# ---------------------------------------------------------------------------

class OSSClient:
    """Wrapper around Alibaba Cloud OSS for uploads with progress tracking."""

    def __init__(self):
        self.bucket = None
        self.public_url = ""
        self.images_dir = ""
        self.releases_dir = ""
        self._init_error = None
        self._try_init()

    def _try_init(self):
        if load_dotenv is None:
            self._init_error = "python-dotenv 未安装，请运行: pip install python-dotenv"
            return
        if oss2 is None:
            self._init_error = "oss2 未安装，请运行: pip install oss2"
            return

        env_path = SCRIPTS_DIR / ".env"
        if not env_path.exists():
            self._init_error = f".env 文件不存在: {env_path}\n请复制 .env.sample 为 .env 并填写配置"
            return

        load_dotenv(env_path)
        required = ["OSS_REGION", "OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET", "OSS_BUCKET_NAME"]
        missing = [v for v in required if not os.environ.get(v)]
        if missing:
            self._init_error = f"缺少环境变量: {', '.join(missing)}"
            return

        try:
            region = os.environ["OSS_REGION"]
            endpoint = f"https://{region}.aliyuncs.com"
            auth = oss2.Auth(os.environ["OSS_ACCESS_KEY_ID"], os.environ["OSS_ACCESS_KEY_SECRET"])
            self.bucket = oss2.Bucket(auth, endpoint, os.environ["OSS_BUCKET_NAME"])
            self.public_url = os.environ.get("OSS_PUBLIC_URL", "").rstrip("/")
            self.images_dir = os.environ.get("OSS_TENBOX_IMAGES_DIR", "tenbox/images").strip("/")
            self.releases_dir = os.environ.get("OSS_TENBOX_RELEASES_DIR", "tenbox/releases").strip("/")
        except Exception as e:
            self._init_error = f"OSS 初始化失败: {e}"

    @property
    def available(self) -> bool:
        return self.bucket is not None

    def get_public_url(self, oss_key: str) -> str:
        return f"{self.public_url}/{oss_key}"

    def upload(self, oss_key: str, local_path: str, progress_callback=None):
        """Upload a file to OSS with optional progress callback(bytes_sent, total_bytes)."""
        if not self.available:
            raise RuntimeError(self._init_error or "OSS 未初始化")
        oss2.resumable_upload(
            self.bucket, oss_key, local_path,
            multipart_threshold=10 * 1024 * 1024,
            part_size=2 * 1024 * 1024,
            num_threads=4,
            progress_callback=progress_callback,
        )


# ---------------------------------------------------------------------------
# SHA256 with progress
# ---------------------------------------------------------------------------

def sha256_file(path: str, progress_callback=None) -> str:
    """Calculate SHA256, calling progress_callback(bytes_read, total) periodically."""
    total = os.path.getsize(path)
    h = hashlib.sha256()
    read_bytes = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
            read_bytes += len(chunk)
            if progress_callback:
                progress_callback(read_bytes, total)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Upload Dialog
# ---------------------------------------------------------------------------

class UploadDialog(tk.Toplevel):
    """Modal dialog showing upload / hash progress with ETA."""

    def __init__(self, parent, title="上传中"):
        super().__init__(parent)
        self.title(title)
        self.resizable(False, False)
        self.transient(parent)
        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self._on_cancel)

        self._cancel_event = threading.Event()
        self._start_time = None

        frame = ttk.Frame(self, padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        self.label_file = ttk.Label(frame, text="准备中...")
        self.label_file.pack(anchor=tk.W)

        self.progress = ttk.Progressbar(frame, length=420, mode="determinate")
        self.progress.pack(pady=(8, 4), fill=tk.X)

        self.label_detail = ttk.Label(frame, text="")
        self.label_detail.pack(anchor=tk.W)

        self.label_eta = ttk.Label(frame, text="")
        self.label_eta.pack(anchor=tk.W)

        btn = ttk.Button(frame, text="取消", command=self._on_cancel)
        btn.pack(pady=(10, 0))

        self.geometry("480x190")
        self._center()

    def _center(self):
        self.update_idletasks()
        pw = self.master.winfo_width()
        ph = self.master.winfo_height()
        px = self.master.winfo_x()
        py = self.master.winfo_y()
        w = self.winfo_width()
        h = self.winfo_height()
        self.geometry(f"+{px + (pw - w) // 2}+{py + (ph - h) // 2}")

    def _on_cancel(self):
        self._cancel_event.set()
        self.destroy()

    @property
    def cancelled(self):
        return self._cancel_event.is_set()

    def set_phase(self, text: str):
        self.label_file.config(text=text)
        self._start_time = time.time()
        self.progress["value"] = 0
        self.label_detail.config(text="")
        self.label_eta.config(text="")

    def update_progress(self, sent: int, total: int):
        if total <= 0:
            return
        pct = sent / total * 100
        self.progress["value"] = pct

        sent_mb = sent / (1024 * 1024)
        total_mb = total / (1024 * 1024)
        self.label_detail.config(text=f"{sent_mb:.1f} MB / {total_mb:.1f} MB  ({pct:.0f}%)")

        if self._start_time and sent > 0:
            elapsed = time.time() - self._start_time
            speed = sent / elapsed if elapsed > 0 else 0
            remaining = (total - sent) / speed if speed > 0 else 0
            if remaining >= 60:
                eta_str = f"{int(remaining // 60)}分{int(remaining % 60)}秒"
            else:
                eta_str = f"{int(remaining)}秒"
            speed_mb = speed / (1024 * 1024)
            self.label_eta.config(text=f"速度: {speed_mb:.1f} MB/s  预计剩余: {eta_str}")


# ---------------------------------------------------------------------------
# Threaded upload helper
# ---------------------------------------------------------------------------

def do_upload_flow(parent: tk.Widget, oss_client: OSSClient, local_path: str,
                   oss_key: str) -> tuple[str, str, int] | None:
    """Run SHA256 + OSS upload in a background thread with a progress dialog.

    Returns (public_url, sha256, file_size_bytes) on success, or None on cancel / error.
    """
    if not oss_client.available:
        messagebox.showerror("OSS 错误", oss_client._init_error or "OSS 未初始化", parent=parent)
        return None

    file_size = os.path.getsize(local_path)
    dlg = UploadDialog(parent, title="上传文件")
    cancel_event = dlg._cancel_event
    result = {"url": None, "sha256": None, "error": None}

    def _safe_after(func):
        """Schedule *func* on the Tk main loop, silently skipping if the dialog is gone."""
        try:
            if not cancel_event.is_set():
                dlg.after(0, func)
        except tk.TclError:
            pass

    def _worker():
        try:
            _safe_after(lambda: dlg.set_phase(f"正在计算 SHA256: {Path(local_path).name}"))

            def sha_cb(sent, total):
                if cancel_event.is_set():
                    raise InterruptedError("用户取消")
                _safe_after(lambda s=sent, t=total: dlg.update_progress(s, t))

            sha = sha256_file(local_path, sha_cb)
            result["sha256"] = sha

            if cancel_event.is_set():
                return

            _safe_after(lambda: dlg.set_phase(f"正在上传: {Path(local_path).name}"))

            def oss_cb(sent, total):
                if cancel_event.is_set():
                    raise InterruptedError("用户取消")
                _safe_after(lambda s=sent, t=total: dlg.update_progress(s, t))

            oss_client.upload(oss_key, local_path, oss_cb)
            result["url"] = oss_client.get_public_url(oss_key)
        except InterruptedError:
            pass
        except Exception as e:
            if not cancel_event.is_set():
                result["error"] = str(e)
        finally:
            try:
                dlg.after(0, dlg.destroy)
            except tk.TclError:
                pass

    t = threading.Thread(target=_worker, daemon=True)
    t.start()
    parent.wait_window(dlg)

    if cancel_event.is_set():
        return None

    t.join(timeout=2)

    if result["error"]:
        messagebox.showerror("上传失败", result["error"], parent=parent)
        return None
    if result["url"] and result["sha256"]:
        return result["url"], result["sha256"], file_size
    return None


# ---------------------------------------------------------------------------
# Image Publisher Tab
# ---------------------------------------------------------------------------

class ImagePublisher(ttk.Frame):
    """Tab for managing images.json entries."""

    FILE_NAMES = ["vmlinuz", "initrd.gz", "rootfs.qcow2"]

    def __init__(self, parent, oss_client: OSSClient):
        super().__init__(parent, padding=8)
        self.oss = oss_client
        self.images_data: list[dict] = []
        self._build_ui()
        self.load_images()

    # ---- UI construction ----

    def _build_ui(self):
        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True)

        # Left panel - image list
        left = ttk.Frame(paned)
        paned.add(left, weight=1)

        ttk.Label(left, text="镜像列表", font=("", 10, "bold")).pack(anchor=tk.W, pady=(0, 4))

        cols = ("id", "version", "name", "platform")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", height=16)
        self.tree.heading("id", text="ID")
        self.tree.heading("version", text="版本")
        self.tree.heading("name", text="名称")
        self.tree.heading("platform", text="CPU 平台")
        self.tree.column("id", width=90, minwidth=60)
        self.tree.column("version", width=90, minwidth=60)
        self.tree.column("name", width=180, minwidth=100)
        self.tree.column("platform", width=80, minwidth=60)
        self.tree.pack(fill=tk.BOTH, expand=True)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        btn_row = ttk.Frame(left)
        btn_row.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(btn_row, text="新建镜像", command=self._new_image).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_row, text="复制选中", command=self._duplicate_image).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_row, text="删除选中", command=self._delete_image).pack(side=tk.LEFT)

        # Right panel - edit form
        right = ttk.Frame(paned)
        paned.add(right, weight=3)

        canvas = tk.Canvas(right, highlightthickness=0)
        scrollbar = ttk.Scrollbar(right, orient=tk.VERTICAL, command=canvas.yview)
        self.form_frame = ttk.Frame(canvas)
        self.form_frame.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=self.form_frame, anchor=tk.NW)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        def _on_mousewheel(event):
            canvas.yview_scroll(-1 * (event.delta // 120), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel, add="+")

        f = self.form_frame
        row = 0

        ttk.Label(f, text="基本信息", font=("", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(0, 4))
        row += 1

        self.var_id = tk.StringVar()
        self.var_version = tk.StringVar()
        self.var_name = tk.StringVar()
        self.var_min_ver = tk.StringVar(value="0.1.0")
        self.var_os = tk.StringVar(value="linux")
        self.var_arch = tk.StringVar(value="microvm")
        self.var_platform = tk.StringVar(value="x86_64")
        self.var_updated_at = tk.StringVar()

        fields = [
            ("ID:", self.var_id),
            ("版本:", self.var_version),
            ("显示名称:", self.var_name),
            ("最低应用版本:", self.var_min_ver),
        ]
        for label_text, var in fields:
            ttk.Label(f, text=label_text).grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
            ttk.Entry(f, textvariable=var, width=50).grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
            row += 1

        ttk.Label(f, text="描述:").grid(row=row, column=0, sticky=tk.NE, padx=(0, 4), pady=2)
        self.text_desc = tk.Text(f, width=50, height=3, wrap=tk.WORD)
        self.text_desc.grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="OS:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Combobox(f, textvariable=self.var_os, values=["linux", "windows", "macos"], state="readonly", width=15).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="架构:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Combobox(f, textvariable=self.var_arch, values=["microvm", "i440fx", "q35"], state="readonly", width=15).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="CPU 平台:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Combobox(f, textvariable=self.var_platform, values=["arm64", "x86_64"], state="readonly", width=15).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="更新时间:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        date_frame = ttk.Frame(f)
        date_frame.grid(row=row, column=1, columnspan=2, sticky=tk.W, pady=2)
        ttk.Entry(date_frame, textvariable=self.var_updated_at, width=20).pack(side=tk.LEFT)
        ttk.Button(date_frame, text="设为今天", command=lambda: self.var_updated_at.set(date.today().isoformat())).pack(side=tk.LEFT, padx=(6, 0))
        row += 1

        # File groups
        ttk.Separator(f, orient=tk.HORIZONTAL).grid(row=row, column=0, columnspan=3, sticky=tk.EW, pady=8)
        row += 1

        ttk.Label(f, text="文件配置", font=("", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(0, 4))
        row += 1

        self.file_vars: list[dict[str, tk.StringVar]] = []
        for fname in self.FILE_NAMES:
            ttk.Label(f, text=f"── {fname} ──", font=("", 9, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(8, 2))
            row += 1

            url_var = tk.StringVar()
            sha_var = tk.StringVar()
            size_var = tk.StringVar()
            self.file_vars.append({"url": url_var, "sha256": sha_var, "size": size_var})

            ttk.Label(f, text="URL:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
            ttk.Entry(f, textvariable=url_var, width=50).grid(row=row, column=1, sticky=tk.W + tk.E, pady=2)
            upload_btn = ttk.Button(f, text="选择文件上传", command=lambda fn=fname, idx=len(self.file_vars) - 1: self._upload_file(fn, idx))
            upload_btn.grid(row=row, column=2, padx=(4, 0), pady=2)
            row += 1

            ttk.Label(f, text="SHA256:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
            ttk.Entry(f, textvariable=sha_var, width=50).grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
            row += 1

            ttk.Label(f, text="大小 (bytes):").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
            ttk.Entry(f, textvariable=size_var, width=20).grid(row=row, column=1, sticky=tk.W, pady=2)
            row += 1

        f.columnconfigure(1, weight=1)

        # Bottom buttons
        ttk.Separator(f, orient=tk.HORIZONTAL).grid(row=row, column=0, columnspan=3, sticky=tk.EW, pady=8)
        row += 1

        btn_frame = ttk.Frame(f)
        btn_frame.grid(row=row, column=0, columnspan=3, sticky=tk.E)
        ttk.Button(btn_frame, text="保存当前到列表", command=self._save_current).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="保存到文件", command=self._save_to_file).pack(side=tk.LEFT)

    # ---- Data operations ----

    def load_images(self):
        self.images_data = []
        if IMAGES_JSON_PATH.exists():
            try:
                data = json.loads(IMAGES_JSON_PATH.read_text(encoding="utf-8"))
                self.images_data = data.get("images", [])
            except Exception as e:
                messagebox.showerror("加载失败", f"无法加载 images.json:\n{e}", parent=self)
        self._refresh_tree()

    def _refresh_tree(self):
        self.tree.delete(*self.tree.get_children())
        for i, img in enumerate(self.images_data):
            self.tree.insert("", tk.END, iid=str(i),
                             values=(img.get("id", ""), img.get("version", ""), img.get("name", ""), img.get("platform", "x86_64")))

    def _on_select(self, _event=None):
        sel = self.tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        img = self.images_data[idx]
        self.var_id.set(img.get("id", ""))
        self.var_version.set(img.get("version", ""))
        self.var_name.set(img.get("name", ""))
        self.text_desc.delete("1.0", tk.END)
        self.text_desc.insert("1.0", img.get("description", ""))
        self.var_min_ver.set(img.get("min_app_version", "0.1.0"))
        self.var_os.set(img.get("os", "linux"))
        self.var_arch.set(img.get("arch", "microvm"))
        self.var_platform.set(img.get("platform", "x86_64"))
        self.var_updated_at.set(img.get("updated_at", ""))

        files = img.get("files", [])
        file_map = {f["name"]: f for f in files}
        for i, fname in enumerate(self.FILE_NAMES):
            entry = file_map.get(fname, {})
            self.file_vars[i]["url"].set(entry.get("url", ""))
            self.file_vars[i]["sha256"].set(entry.get("sha256", ""))
            self.file_vars[i]["size"].set(str(entry.get("size", "")))

    def _collect_form(self) -> dict:
        files = []
        for i, fname in enumerate(self.FILE_NAMES):
            size_str = self.file_vars[i]["size"].get().strip()
            size_val = int(size_str) if size_str.isdigit() else 0
            entry = {
                "name": fname,
                "url": self.file_vars[i]["url"].get().strip(),
                "sha256": self.file_vars[i]["sha256"].get().strip(),
                "size": size_val,
            }
            files.append(entry)
        return {
            "id": self.var_id.get().strip(),
            "version": self.var_version.get().strip(),
            "name": self.var_name.get().strip(),
            "description": self.text_desc.get("1.0", tk.END).strip(),
            "min_app_version": self.var_min_ver.get().strip(),
            "os": self.var_os.get(),
            "arch": self.var_arch.get(),
            "platform": self.var_platform.get(),
            "updated_at": self.var_updated_at.get().strip(),
            "files": files,
        }

    def _new_image(self):
        self.tree.selection_remove(*self.tree.selection())
        self.var_id.set("")
        self.var_version.set("")
        self.var_name.set("")
        self.text_desc.delete("1.0", tk.END)
        self.var_min_ver.set("0.1.0")
        self.var_os.set("linux")
        self.var_arch.set("microvm")
        self.var_platform.set("x86_64")
        self.var_updated_at.set(date.today().isoformat())
        for fv in self.file_vars:
            fv["url"].set("")
            fv["sha256"].set("")
            fv["size"].set("")

    def _duplicate_image(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showinfo("提示", "请先选中一个镜像", parent=self)
            return
        self.tree.selection_remove(*sel)
        self.var_id.set(self.var_id.get() + "-copy")
        self.var_updated_at.set(date.today().isoformat())

    def _save_current(self):
        img = self._collect_form()
        if not img["id"] or not img["version"]:
            messagebox.showwarning("提示", "ID 和版本不能为空", parent=self)
            return

        sel = self.tree.selection()
        if sel:
            # 编辑已选中的镜像
            idx = int(sel[0])
            self.images_data[idx] = img
        else:
            # 新建或更新：检查 ID 是否已存在
            existing_idx = None
            for i, d in enumerate(self.images_data):
                if d["id"] == img["id"]:
                    existing_idx = i
                    break
            
            if existing_idx is not None:
                # ID 已存在，更新现有镜像
                self.images_data[existing_idx] = img
                # 自动选中更新后的镜像
                self._refresh_tree()
                self.tree.selection_set(str(existing_idx))
            else:
                # ID 不存在，添加新镜像
                self.images_data.append(img)

        self._refresh_tree()
        messagebox.showinfo("成功", "已保存到列表（尚未写入文件）", parent=self)

    def _delete_image(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showinfo("提示", "请先选中一个镜像", parent=self)
            return
        idx = int(sel[0])
        name = self.images_data[idx].get("name", "")
        if not messagebox.askyesno("确认删除", f"确定删除镜像 \"{name}\" 吗？", parent=self):
            return
        self.images_data.pop(idx)
        self._refresh_tree()
        self._new_image()

    def _save_to_file(self):
        data = {"images": self.images_data}
        try:
            IMAGES_JSON_PATH.write_text(
                json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            messagebox.showinfo("成功", f"已保存到:\n{IMAGES_JSON_PATH}", parent=self)
        except Exception as e:
            messagebox.showerror("保存失败", str(e), parent=self)

    def _upload_file(self, file_name: str, file_index: int):
        local_path = filedialog.askopenfilename(
            title=f"选择 {file_name}",
            parent=self,
        )
        if not local_path:
            return

        image_id = self.var_id.get().strip()
        if not image_id:
            messagebox.showwarning("提示", "请先填写镜像 ID", parent=self)
            return

        oss_key = f"{self.oss.images_dir}/{image_id}/{Path(local_path).name}"
        result = do_upload_flow(self, self.oss, local_path, oss_key)
        if result:
            url, sha, size = result
            self.file_vars[file_index]["url"].set(url)
            self.file_vars[file_index]["sha256"].set(sha)
            self.file_vars[file_index]["size"].set(str(size))


# ---------------------------------------------------------------------------
# Release Publisher Tab
# ---------------------------------------------------------------------------

class ReleasePublisher(ttk.Frame):
    """Tab for managing version.json."""

    def __init__(self, parent, oss_client: OSSClient):
        super().__init__(parent, padding=8)
        self.oss = oss_client
        self._build_ui()
        self.load_version()

    def _build_ui(self):
        canvas = tk.Canvas(self, highlightthickness=0)
        scrollbar = ttk.Scrollbar(self, orient=tk.VERTICAL, command=canvas.yview)
        self.form_frame = ttk.Frame(canvas)
        self.form_frame.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=self.form_frame, anchor=tk.NW)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        def _on_mousewheel(event):
            canvas.yview_scroll(-1 * (event.delta // 120), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel, add="+")

        f = self.form_frame
        row = 0

        ttk.Label(f, text="全局设置", font=("", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(0, 6))
        row += 1

        self.var_min_supported = tk.StringVar()

        ttk.Label(f, text="最低支持版本:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_min_supported, width=30).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        # Windows platform
        ttk.Separator(f, orient=tk.HORIZONTAL).grid(row=row, column=0, columnspan=3, sticky=tk.EW, pady=8)
        row += 1

        ttk.Label(f, text="Windows 平台", font=("", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(0, 4))
        row += 1

        self.var_win_version = tk.StringVar()
        self.var_win_date = tk.StringVar()
        self.var_win_url = tk.StringVar()
        self.var_win_sha = tk.StringVar()
        self.var_win_size = tk.StringVar()
        self.var_win_minos = tk.StringVar(value="Windows 10")

        ttk.Label(f, text="latest_version:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_win_version, width=30).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="release_date:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        win_date_frame = ttk.Frame(f)
        win_date_frame.grid(row=row, column=1, columnspan=2, sticky=tk.W, pady=2)
        ttk.Entry(win_date_frame, textvariable=self.var_win_date, width=20).pack(side=tk.LEFT)
        ttk.Button(win_date_frame, text="设为今天", command=lambda: self.var_win_date.set(date.today().isoformat())).pack(side=tk.LEFT, padx=(6, 0))
        row += 1

        ttk.Label(f, text="release_notes:").grid(row=row, column=0, sticky=tk.NE, padx=(0, 4), pady=2)
        self.text_win_notes = tk.Text(f, width=55, height=4, wrap=tk.WORD)
        self.text_win_notes.grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="download_url:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_win_url, width=55).grid(row=row, column=1, sticky=tk.W + tk.E, pady=2)
        ttk.Button(f, text="选择文件上传", command=lambda: self._upload_installer("windows")).grid(row=row, column=2, padx=(4, 0), pady=2)
        row += 1

        ttk.Label(f, text="SHA256:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_win_sha, width=55).grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="size (bytes):").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_win_size, width=20).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="min_os:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_win_minos, width=30).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        # macOS platform
        ttk.Separator(f, orient=tk.HORIZONTAL).grid(row=row, column=0, columnspan=3, sticky=tk.EW, pady=8)
        row += 1

        ttk.Label(f, text="macOS 平台", font=("", 10, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(0, 4))
        row += 1

        self.var_mac_version = tk.StringVar()
        self.var_mac_date = tk.StringVar()
        self.var_mac_minos = tk.StringVar(value="macOS 13.0")

        self.var_mac_dmg_url = tk.StringVar()
        self.var_mac_dmg_sha = tk.StringVar()
        self.var_mac_dmg_size = tk.StringVar()

        self.var_mac_zip_url = tk.StringVar()
        self.var_mac_zip_sha = tk.StringVar()
        self.var_mac_zip_size = tk.StringVar()

        ttk.Label(f, text="latest_version:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_version, width=30).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="release_date:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        mac_date_frame = ttk.Frame(f)
        mac_date_frame.grid(row=row, column=1, columnspan=2, sticky=tk.W, pady=2)
        ttk.Entry(mac_date_frame, textvariable=self.var_mac_date, width=20).pack(side=tk.LEFT)
        ttk.Button(mac_date_frame, text="设为今天", command=lambda: self.var_mac_date.set(date.today().isoformat())).pack(side=tk.LEFT, padx=(6, 0))
        row += 1

        ttk.Label(f, text="release_notes:").grid(row=row, column=0, sticky=tk.NE, padx=(0, 4), pady=2)
        self.text_mac_notes = tk.Text(f, width=55, height=4, wrap=tk.WORD)
        self.text_mac_notes.grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="min_os:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_minos, width=30).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        # DMG section (for website download / version.json)
        ttk.Label(f, text="── DMG (网站下载) ──", font=("", 9, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(8, 2))
        row += 1

        ttk.Label(f, text="DMG URL:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_dmg_url, width=55).grid(row=row, column=1, sticky=tk.W + tk.E, pady=2)
        ttk.Button(f, text="选择文件上传", command=lambda: self._upload_installer("macos_dmg")).grid(row=row, column=2, padx=(4, 0), pady=2)
        row += 1

        ttk.Label(f, text="DMG SHA256:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_dmg_sha, width=55).grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="DMG size (bytes):").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_dmg_size, width=20).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        # ZIP section (for Sparkle update / appcast.xml)
        ttk.Label(f, text="── ZIP (Sparkle 升级) ──", font=("", 9, "bold")).grid(row=row, column=0, columnspan=3, sticky=tk.W, pady=(8, 2))
        row += 1

        ttk.Label(f, text="ZIP URL:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_zip_url, width=55).grid(row=row, column=1, sticky=tk.W + tk.E, pady=2)
        ttk.Button(f, text="选择文件上传", command=lambda: self._upload_installer("macos_zip")).grid(row=row, column=2, padx=(4, 0), pady=2)
        row += 1

        ttk.Label(f, text="ZIP SHA256:").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_zip_sha, width=55).grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        ttk.Label(f, text="ZIP size (bytes):").grid(row=row, column=0, sticky=tk.E, padx=(0, 4), pady=2)
        ttk.Entry(f, textvariable=self.var_mac_zip_size, width=20).grid(row=row, column=1, sticky=tk.W, pady=2)
        row += 1

        ttk.Label(f, text="Sparkle edSignature:").grid(row=row, column=0, sticky=tk.NE, padx=(0, 4), pady=2)
        self.text_mac_ed_signature = tk.Text(f, width=55, height=2, wrap=tk.WORD)
        self.text_mac_ed_signature.grid(row=row, column=1, columnspan=2, sticky=tk.W + tk.E, pady=2)
        row += 1

        f.columnconfigure(1, weight=1)

        # Bottom save button
        ttk.Separator(f, orient=tk.HORIZONTAL).grid(row=row, column=0, columnspan=3, sticky=tk.EW, pady=8)
        row += 1

        btn_frame = ttk.Frame(f)
        btn_frame.grid(row=row, column=0, columnspan=3, sticky=tk.E)
        ttk.Button(btn_frame, text="重新加载", command=self.load_version).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btn_frame, text="保存到文件", command=self._save_to_file).pack(side=tk.LEFT)

    # ---- Data operations ----

    def load_version(self):
        if not VERSION_JSON_PATH.exists():
            return
        try:
            data = json.loads(VERSION_JSON_PATH.read_text(encoding="utf-8"))
        except Exception as e:
            messagebox.showerror("加载失败", f"无法加载 version.json:\n{e}", parent=self)
            return

        self.var_min_supported.set(data.get("min_supported_version", ""))

        platforms = data.get("platforms", {})

        win = platforms.get("windows", {})
        self.var_win_version.set(win.get("latest_version", ""))
        self.var_win_date.set(win.get("release_date", ""))
        self.text_win_notes.delete("1.0", tk.END)
        self.text_win_notes.insert("1.0", win.get("release_notes", ""))
        self.var_win_url.set(win.get("download_url", ""))
        self.var_win_sha.set(win.get("sha256", ""))
        self.var_win_size.set(str(win.get("size", "")))
        self.var_win_minos.set(win.get("min_os", "Windows 10"))

        mac = platforms.get("macos", {})
        self.var_mac_version.set(mac.get("latest_version", ""))
        self.var_mac_date.set(mac.get("release_date", ""))
        self.text_mac_notes.delete("1.0", tk.END)
        self.text_mac_notes.insert("1.0", mac.get("release_notes", ""))
        self.var_mac_minos.set(mac.get("min_os", "macOS 13.0"))

        self.var_mac_dmg_url.set(mac.get("download_url", ""))
        self.var_mac_dmg_sha.set(mac.get("sha256", ""))
        self.var_mac_dmg_size.set(str(mac.get("size", "")))

        self._load_zip_info_from_appcast()

    def _load_zip_info_from_appcast(self):
        """Load ZIP url, size, and edSignature from appcast.xml."""
        self.var_mac_zip_url.set("")
        self.var_mac_zip_sha.set("")
        self.var_mac_zip_size.set("")
        self.text_mac_ed_signature.delete("1.0", tk.END)
        if not APPCAST_XML_PATH.exists():
            return
        try:
            tree = ET.parse(APPCAST_XML_PATH)
            enc = tree.find(".//item/enclosure")
            if enc is not None:
                self.var_mac_zip_url.set(enc.get("url", ""))
                self.var_mac_zip_size.set(enc.get("length", ""))
                self.text_mac_ed_signature.insert(
                    "1.0", enc.get(f"{{{SPARKLE_NS}}}edSignature", ""))
        except Exception:
            pass

    @staticmethod
    def _parse_size(s: str) -> int:
        s = s.strip()
        return int(s) if s.isdigit() else 0

    def _collect_form(self) -> dict:
        win = {
            "latest_version": self.var_win_version.get().strip(),
            "release_notes": self.text_win_notes.get("1.0", tk.END).strip(),
            "release_date": self.var_win_date.get().strip(),
            "download_url": self.var_win_url.get().strip(),
            "sha256": self.var_win_sha.get().strip(),
            "size": self._parse_size(self.var_win_size.get()),
            "min_os": self.var_win_minos.get().strip(),
        }
        mac = {
            "latest_version": self.var_mac_version.get().strip(),
            "release_notes": self.text_mac_notes.get("1.0", tk.END).strip(),
            "release_date": self.var_mac_date.get().strip(),
            "download_url": self.var_mac_dmg_url.get().strip(),
            "sha256": self.var_mac_dmg_sha.get().strip(),
            "size": self._parse_size(self.var_mac_dmg_size.get()),
            "min_os": self.var_mac_minos.get().strip(),
        }
        # Top-level fallback fields mirror Windows for old clients (<=0.3.0)
        # that don't know about the "platforms" structure.
        return {
            "latest_version": win["latest_version"],
            "min_supported_version": self.var_min_supported.get().strip(),
            "release_notes": win["release_notes"],
            "release_date": win["release_date"],
            "download_url": win["download_url"],
            "sha256": win["sha256"],
            "platforms": {
                "windows": win,
                "macos": mac,
            },
        }

    def _save_to_file(self):
        data = self._collect_form()
        platforms = data.get("platforms", {})
        has_version = any(p.get("latest_version") for p in platforms.values())
        if not has_version:
            messagebox.showwarning("提示", "至少一个平台的版本号不能为空", parent=self)
            return
        try:
            VERSION_JSON_PATH.write_text(
                json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
        except Exception as e:
            messagebox.showerror("保存失败", str(e), parent=self)
            return

        saved_files = [str(VERSION_JSON_PATH)]

        mac = platforms.get("macos", {})
        if mac.get("latest_version"):
            appcast_info = {
                "latest_version": mac["latest_version"],
                "release_notes": mac.get("release_notes", ""),
                "release_date": mac.get("release_date", ""),
                "min_os": mac.get("min_os", "13.0"),
                "download_url": self.var_mac_zip_url.get().strip(),
                "size": self._parse_size(self.var_mac_zip_size.get()),
                "ed_signature": self.text_mac_ed_signature.get("1.0", tk.END).strip(),
            }
            try:
                APPCAST_XML_PATH.write_text(
                    generate_appcast_xml(appcast_info), encoding="utf-8",
                )
                saved_files.append(str(APPCAST_XML_PATH))
            except Exception as e:
                messagebox.showwarning("提示", f"version.json 已保存，但 appcast.xml 生成失败:\n{e}", parent=self)

        messagebox.showinfo("成功", f"已保存到:\n" + "\n".join(saved_files), parent=self)

    def _upload_installer(self, target: str):
        filetypes_map = {
            "windows": [("MSI files", "*.msi"), ("All files", "*.*")],
            "macos_dmg": [("DMG files", "*.dmg"), ("All files", "*.*")],
            "macos_zip": [("ZIP files", "*.zip"), ("All files", "*.*")],
        }
        filetypes = filetypes_map.get(target, [("All files", "*.*")])

        local_path = filedialog.askopenfilename(title="选择安装包文件", filetypes=filetypes, parent=self)
        if not local_path:
            return

        filename = Path(local_path).name
        oss_key = f"{self.oss.releases_dir}/{filename}"
        result = do_upload_flow(self, self.oss, local_path, oss_key)
        if not result:
            return

        url, sha, size = result
        if target == "windows":
            self.var_win_url.set(url)
            self.var_win_sha.set(sha)
            self.var_win_size.set(str(size))
        elif target == "macos_dmg":
            self.var_mac_dmg_url.set(url)
            self.var_mac_dmg_sha.set(sha)
            self.var_mac_dmg_size.set(str(size))
        elif target == "macos_zip":
            self.var_mac_zip_url.set(url)
            self.var_mac_zip_sha.set(sha)
            self.var_mac_zip_size.set(str(size))


# ---------------------------------------------------------------------------
# Main Application
# ---------------------------------------------------------------------------

def main():
    root = tk.Tk()
    root.title("TenBox 发布管理工具")
    root.geometry("1024x760")
    root.minsize(800, 550)

    oss_client = OSSClient()

    notebook = ttk.Notebook(root)
    notebook.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    image_tab = ImagePublisher(notebook, oss_client)
    release_tab = ReleasePublisher(notebook, oss_client)

    notebook.add(image_tab, text="  发布镜像  ")
    notebook.add(release_tab, text="  发布安装包  ")

    # Show OSS status in title
    if not oss_client.available:
        root.title("TenBox 发布管理工具  [OSS 未连接 - 仅支持手动输入 URL]")

    root.mainloop()


if __name__ == "__main__":
    main()
