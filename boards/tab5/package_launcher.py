# PlatformIO post-build hook: stage Launcher-ready artifacts in dist/.
#
# The bmorcelli M5Stack Launcher installs a third-party app by reading the app
# image from SD (or a URL), dynamically carving a new OTA app partition sized to
# the image, flashing it there, and setting it as the boot partition. So all it
# needs is a standard ESP-IDF app image (0xE9 magic) — no fixed offset, no custom
# header/signature, no icon. Crucially, the Launcher derives the on-screen APP
# NAME from the FILENAME, so the app-only image is named exactly "ESP32-DIV.bin".
Import("env")  # noqa: F821
import os
import shutil


def _package_launcher(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    dist = os.path.join(env.subst("$PROJECT_DIR"), "dist")
    os.makedirs(dist, exist_ok=True)

    app = os.path.join(build_dir, "firmware.bin")             # app partition image
    factory = os.path.join(build_dir, "firmware.factory.bin")  # merged bootloader+table+app

    # App-only image for Launcher SD install. Filename == on-screen name (<=20 chars).
    out_app = os.path.join(dist, "ESP32-DIV.bin")
    shutil.copyfile(app, out_app)
    print("[package] Launcher app image (SD install) -> %s (%d bytes)"
          % (out_app, os.path.getsize(out_app)))

    # Merged full image for a direct esptool 0x0 flash or an external OTA URL.
    if os.path.exists(factory):
        out_full = os.path.join(dist, "ESP32-DIV-tab5-full.bin")
        shutil.copyfile(factory, out_full)
        print("[package] merged full image (0x0 flash / OTA URL) -> %s (%d bytes)"
              % (out_full, os.path.getsize(out_full)))


# Registered from an extra_scripts (post:) entry, so this runs after the platform's
# own firmware.factory.bin post-action — factory.bin exists by the time we copy.
env.AddPostAction("$BUILD_DIR/firmware.bin", _package_launcher)  # noqa: F821
