#!/usr/bin/env python3
"""Convert PNG toolbar icons to 48x48 BMP with magenta transparency."""

from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Installing Pillow...")
    import subprocess
    subprocess.check_call(["pip", "install", "Pillow"])
    from PIL import Image

ICON_SIZE = 48
MAGENTA = (255, 0, 255)
DARK_GRAY = (60, 60, 60)

BASE_DIR = Path(__file__).parent.parent / "src" / "manager" / "resources"
SRC_DIR = BASE_DIR / "material"
DST_DIR = BASE_DIR

ICONS = [
    "new_vm",
    "edit", 
    "delete",
    "start",
    "stop",
    "reboot",
    "shutdown",
    "shared_folders",
]

def convert_icon(name: str) -> bool:
    src_path = SRC_DIR / f"{name}.png"
    dst_path = DST_DIR / f"{name}.bmp"
    
    if not src_path.exists():
        print(f"  [SKIP] {src_path} not found")
        return False
    
    img = Image.open(src_path).convert("RGBA")
    img = img.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
    
    # Threshold alpha channel to eliminate anti-aliasing fringes
    r, g, b, a = img.split()
    # Alpha > 128 becomes fully opaque, otherwise fully transparent
    a = a.point(lambda x: 255 if x > 128 else 0)
    
    # Replace black with dark gray
    r = r.point(lambda x: DARK_GRAY[0] if x < 50 else x)
    g = g.point(lambda x: DARK_GRAY[1] if x < 50 else x)
    b = b.point(lambda x: DARK_GRAY[2] if x < 50 else x)
    
    img = Image.merge("RGBA", (r, g, b, a))
    
    # Create RGB image with magenta background
    result = Image.new("RGB", (ICON_SIZE, ICON_SIZE), MAGENTA)
    
    # Paste image using thresholded alpha as mask
    result.paste(img, mask=img.split()[3])
    
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    result.save(dst_path, "BMP")
    print(f"  [OK] {name}.bmp")
    return True

def main():
    print(f"Source: {SRC_DIR}")
    print(f"Output: {DST_DIR}")
    print()
    
    success = 0
    for name in ICONS:
        if convert_icon(name):
            success += 1
    
    print()
    print(f"Converted {success}/{len(ICONS)} icons")

if __name__ == "__main__":
    main()
