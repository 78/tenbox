#!/usr/bin/env python3
"""
TenBox Image Manager - Validate and manage images.json

Usage:
    python image_manager.py validate <path_to_images.json>
    python image_manager.py interactive <path_to_images.json>
"""

import json
import sys
import os
import hashlib
from pathlib import Path
from typing import Optional


def calculate_sha256(file_path: str) -> str:
    """Calculate SHA256 hash of a file."""
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256_hash.update(chunk)
    return sha256_hash.hexdigest()


def validate_image_entry(entry: dict, index: int) -> list[str]:
    """Validate a single image entry, return list of errors."""
    errors = []
    
    required_fields = ["id", "version", "name", "arch", "files"]
    for field in required_fields:
        if field not in entry:
            errors.append(f"Image {index}: missing required field '{field}'")
    
    if "id" in entry and not entry["id"]:
        errors.append(f"Image {index}: 'id' cannot be empty")
    
    if "version" in entry and not entry["version"]:
        errors.append(f"Image {index}: 'version' cannot be empty")
    
    if "arch" in entry:
        valid_archs = ["microvm", "i440fx", "q35"]
        if entry["arch"] not in valid_archs:
            errors.append(f"Image {index}: 'arch' must be one of {valid_archs}")
    
    if "os" in entry:
        valid_os = ["linux", "windows", "macos"]
        if entry["os"] not in valid_os:
            errors.append(f"Image {index}: 'os' must be one of {valid_os}")
    
    if "min_app_version" in entry:
        version = entry["min_app_version"]
        parts = version.split(".")
        if len(parts) != 3 or not all(p.isdigit() for p in parts):
            errors.append(f"Image {index}: 'min_app_version' must be in format X.Y.Z")
    
    if "files" in entry:
        if not isinstance(entry["files"], list):
            errors.append(f"Image {index}: 'files' must be an array")
        elif len(entry["files"]) == 0:
            errors.append(f"Image {index}: 'files' cannot be empty")
        else:
            for i, file_entry in enumerate(entry["files"]):
                if "name" not in file_entry:
                    errors.append(f"Image {index}, file {i}: missing 'name'")
                if "url" not in file_entry:
                    errors.append(f"Image {index}, file {i}: missing 'url'")
                elif not file_entry["url"].startswith(("http://", "https://")):
                    errors.append(f"Image {index}, file {i}: 'url' must start with http:// or https://")
                
                if "sha256" in file_entry and file_entry["sha256"]:
                    sha = file_entry["sha256"]
                    if len(sha) != 64 or not all(c in "0123456789abcdefABCDEF" for c in sha):
                        errors.append(f"Image {index}, file {i}: 'sha256' must be 64 hex characters")
    
    return errors


def validate_images_json(data: dict) -> list[str]:
    """Validate the entire images.json structure."""
    errors = []
    
    if "images" not in data:
        errors.append("Missing 'images' array at root level")
        return errors
    
    if not isinstance(data["images"], list):
        errors.append("'images' must be an array")
        return errors
    
    seen_ids = set()
    for i, entry in enumerate(data["images"]):
        entry_errors = validate_image_entry(entry, i)
        errors.extend(entry_errors)
        
        if "id" in entry and "version" in entry:
            cache_id = f"{entry['id']}-{entry['version']}"
            if cache_id in seen_ids:
                errors.append(f"Image {i}: duplicate id-version combination '{cache_id}'")
            seen_ids.add(cache_id)
    
    return errors


def validate_file(file_path: str) -> bool:
    """Validate an images.json file and print results."""
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        print(f"ERROR: Invalid JSON: {e}")
        return False
    except FileNotFoundError:
        print(f"ERROR: File not found: {file_path}")
        return False
    
    errors = validate_images_json(data)
    
    if errors:
        print(f"Validation FAILED with {len(errors)} error(s):")
        for error in errors:
            print(f"  - {error}")
        return False
    else:
        image_count = len(data.get("images", []))
        print(f"Validation PASSED: {image_count} image(s) found")
        return True


def list_images(data: dict):
    """List all images in the JSON."""
    images = data.get("images", [])
    if not images:
        print("No images found.")
        return
    
    print(f"\n{'#':<3} {'ID':<15} {'Version':<10} {'Name':<30} {'Arch':<10}")
    print("-" * 70)
    for i, img in enumerate(images):
        print(f"{i:<3} {img.get('id', 'N/A'):<15} {img.get('version', 'N/A'):<10} "
              f"{img.get('name', 'N/A'):<30} {img.get('arch', 'N/A'):<10}")


def show_image_details(data: dict, index: int):
    """Show detailed information about an image."""
    images = data.get("images", [])
    if index < 0 or index >= len(images):
        print(f"Invalid index. Valid range: 0-{len(images)-1}")
        return
    
    img = images[index]
    print(f"\n=== Image Details ===")
    print(f"ID:              {img.get('id', 'N/A')}")
    print(f"Version:         {img.get('version', 'N/A')}")
    print(f"Name:            {img.get('name', 'N/A')}")
    print(f"Description:     {img.get('description', 'N/A')}")
    print(f"OS:              {img.get('os', 'N/A')}")
    print(f"Architecture:    {img.get('arch', 'N/A')}")
    print(f"Min App Version: {img.get('min_app_version', 'N/A')}")
    print(f"\nFiles:")
    for f in img.get("files", []):
        sha = f.get("sha256", "")
        sha_display = sha[:16] + "..." if sha else "(none)"
        print(f"  - {f.get('name', 'N/A')}")
        print(f"    URL: {f.get('url', 'N/A')}")
        print(f"    SHA256: {sha_display}")


def add_image(data: dict) -> dict:
    """Interactively add a new image."""
    print("\n=== Add New Image ===")
    
    image = {}
    image["id"] = input("ID (e.g., 'copaw'): ").strip()
    image["version"] = input("Version (e.g., '0.0.4'): ").strip()
    image["name"] = input("Display Name (e.g., 'XFCE + CoPaw 0.0.4'): ").strip()
    image["description"] = input("Description (optional): ").strip()
    image["min_app_version"] = input("Min App Version (e.g., '0.1.0'): ").strip() or "0.0.0"
    image["os"] = input("OS (linux/windows/macos) [linux]: ").strip() or "linux"
    image["arch"] = input("Architecture (microvm/i440fx/q35) [microvm]: ").strip() or "microvm"
    
    print("\nAdd files (enter empty name to finish):")
    files = []
    while True:
        name = input("  File name (e.g., 'vmlinuz'): ").strip()
        if not name:
            break
        url = input(f"  URL for {name}: ").strip()
        sha256 = input(f"  SHA256 for {name} (optional): ").strip()
        files.append({"name": name, "url": url, "sha256": sha256})
    
    image["files"] = files
    
    if "images" not in data:
        data["images"] = []
    data["images"].append(image)
    
    print(f"\nImage '{image['name']}' added.")
    return data


def remove_image(data: dict, index: int) -> dict:
    """Remove an image by index."""
    images = data.get("images", [])
    if index < 0 or index >= len(images):
        print(f"Invalid index. Valid range: 0-{len(images)-1}")
        return data
    
    removed = images.pop(index)
    print(f"Removed image: {removed.get('name', 'N/A')}")
    return data


def save_json(data: dict, file_path: str):
    """Save data back to JSON file."""
    with open(file_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"Saved to {file_path}")


def interactive_mode(file_path: str):
    """Interactive management mode."""
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"File not found, creating new: {file_path}")
        data = {"images": []}
    except json.JSONDecodeError as e:
        print(f"ERROR: Invalid JSON: {e}")
        return
    
    print(f"Loaded: {file_path}")
    
    while True:
        print("\n=== Image Manager ===")
        print("1. List images")
        print("2. Show image details")
        print("3. Add image")
        print("4. Remove image")
        print("5. Validate")
        print("6. Save and exit")
        print("7. Exit without saving")
        
        choice = input("\nChoice: ").strip()
        
        if choice == "1":
            list_images(data)
        elif choice == "2":
            list_images(data)
            try:
                idx = int(input("Enter image index: "))
                show_image_details(data, idx)
            except ValueError:
                print("Invalid input")
        elif choice == "3":
            data = add_image(data)
        elif choice == "4":
            list_images(data)
            try:
                idx = int(input("Enter image index to remove: "))
                data = remove_image(data, idx)
            except ValueError:
                print("Invalid input")
        elif choice == "5":
            errors = validate_images_json(data)
            if errors:
                print(f"Validation FAILED with {len(errors)} error(s):")
                for error in errors:
                    print(f"  - {error}")
            else:
                print("Validation PASSED")
        elif choice == "6":
            save_json(data, file_path)
            break
        elif choice == "7":
            print("Exiting without saving.")
            break
        else:
            print("Invalid choice")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    
    command = sys.argv[1]
    
    if command == "validate":
        if len(sys.argv) < 3:
            print("Usage: python image_manager.py validate <path_to_images.json>")
            sys.exit(1)
        success = validate_file(sys.argv[2])
        sys.exit(0 if success else 1)
    
    elif command == "interactive":
        if len(sys.argv) < 3:
            print("Usage: python image_manager.py interactive <path_to_images.json>")
            sys.exit(1)
        interactive_mode(sys.argv[2])
    
    else:
        print(f"Unknown command: {command}")
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
