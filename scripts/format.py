import shutil
import subprocess
import sys
from pathlib import Path


def find_clang_format(allow_auto_install=True):
    """Find clang-format: system binary first, then pip package."""
    # 1. System-installed clang-format (brew, apt, choco, etc.)
    exe = shutil.which("clang-format")
    if exe:
        return exe

    # 2. Fallback: pip clang-format package
    try:
        import clang_format
        return clang_format.get_executable("clang-format")
    except ImportError:
        pass

    # 3. Auto-install pip package (works on Windows; skipped on managed envs). Skipped in
    # --check mode: CI should fail with clear instructions, not silently pull an unpinned
    # package from PyPI mid-run on a persistent self-hosted runner.
    if allow_auto_install:
        print("clang-format not found. Attempting pip install...")
        try:
            subprocess.check_call(
                [sys.executable, "-m", "pip", "install", "--user", "clang-format"],
                stdout=subprocess.DEVNULL,
            )
            import clang_format
            return clang_format.get_executable("clang-format")
        except (subprocess.CalledProcessError, ImportError):
            pass

    print("Error: clang-format not found.", file=sys.stderr)
    print("Install via: brew install clang-format (macOS)")
    print("             apt install clang-format  (Linux)")
    print("             pip install clang-format   (Windows)")
    sys.exit(1)


def get_source_files(root_dir, targets=None):
    files_to_format = []

    if not targets:
        # Default directories
        targets = [str(root_dir / d) for d in ["src", "include", "components"]]

    for target in targets:
        target_path = Path(target)
        if not target_path.exists():
            print(f"Warning: Target '{target}' does not exist.")
            continue

        if target_path.is_file():
            # If it's a file, add it directly if it has a valid extension
            if target_path.suffix in [".c", ".h", ".cpp", ".hpp"]:
                files_to_format.append(str(target_path.absolute()))
        else:
            # If it's a directory, scan for valid extensions
            for ext in ["*.c", "*.h", "*.cpp", "*.hpp"]:
                for file_path in target_path.rglob(ext):
                    # Skip managed components and build directories
                    path_str = str(file_path)
                    if any(ignored in path_str for ignored in
                           ["managed_components", ".pio", "build", "vendor", "components/microlink",
                            "components/wireguard_lwip"]):
                        continue
                    files_to_format.append(str(file_path.absolute()))

    # Remove duplicates just in case
    return list(set(files_to_format))


def main():
    project_root = Path(__file__).parent.parent

    verbose = "-v" in sys.argv
    check = "--check" in sys.argv
    # Get targets (everything else that is not script name, -v, or --check)
    targets = [arg for arg in sys.argv[1:] if arg not in ("-v", "--check")]

    files = get_source_files(project_root, targets)

    if not files:
        print("No source files found to format.")
        return

    if verbose:
        action = "Checking" if check else "Formatting"
        print(f"{action} {len(files)} files:")
        for f in files:
            print(f"  - {Path(f).relative_to(project_root)}")
    else:
        action = "Checking" if check else "Formatting"
        print(f"{action} {len(files)} files...")

    clang_exe = find_clang_format(allow_auto_install=not check)
    cmd = [clang_exe, "--dry-run", "-Werror"] + files if check else [clang_exe, "-i"] + files

    try:
        subprocess.check_call(cmd, cwd=project_root)
        print("Formatting complete!" if not check else "All files formatted correctly.")
    except subprocess.CalledProcessError as e:
        if check:
            print("Formatting check failed — run ./format.sh to fix.", file=sys.stderr)
        else:
            print(f"Error formatting files: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
