"""Copy committed sdkconfig to build dir before cmake runs.

ESP-IDF only applies sdkconfig.defaults when generating a fresh sdkconfig.
On CI (clean workspace), the build dir is empty, so cmake regenerates it —
which loses the custom keys required for this project (ChaCha20-Poly1305,
BLE 4.2, etc.). Copying the full committed sdkconfig first tells cmake
to use it as-is and skip regeneration entirely.
"""
Import("env")
import os
import shutil

src = os.path.join(env.subst("$PROJECT_DIR"), "sdkconfig.lilygo-t-display-s3")
build_dir = env.subst("$BUILD_DIR")
dst = os.path.join(build_dir, "sdkconfig")

if not os.path.exists(src):
    print(f"[pre_copy_sdkconfig] WARNING: {src} not found — skipping")
elif os.path.exists(dst):
    print(f"[pre_copy_sdkconfig] sdkconfig already present at {dst}")
else:
    os.makedirs(build_dir, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[pre_copy_sdkconfig] Copied sdkconfig.lilygo-t-display-s3 → {dst}")
