"""
Constants for the lsfg-vk plugin.
"""

from pathlib import Path

LOCAL_LIB = ".local/lib"
LOCAL_SHARE_BASE = ".local/share"
VULKAN_LAYER_DIR = ".local/share/vulkan/implicit_layer.d"
CONFIG_DIR = ".config/lsfg-vk"

SCRIPT_NAME = "lsfg"
CONFIG_FILENAME = "conf.toml"
LIB_FILENAME = "liblsfg-vk.so"
JSON_FILENAME = "VkLayer_LS_frame_generation.json"
ZIP_FILENAME = "lsfg-vk_noui.zip"

FLATPAK_23_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_23.08.flatpak"
FLATPAK_24_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_24.08.flatpak"
FLATPAK_25_08_FILENAME = "org.freedesktop.Platform.VulkanLayer.lsfg_vk_25.08.flatpak"

SO_EXT = ".so"
JSON_EXT = ".json"

BIN_DIR = "bin"

STEAM_COMMON_PATH = Path("steamapps/common/Lossless Scaling")
LOSSLESS_DLL_NAME = "Lossless.dll"

ENV_LSFG_DLL_PATH = "LSFG_DLL_PATH"
ENV_XDG_DATA_HOME = "XDG_DATA_HOME"
ENV_HOME = "HOME"


# PB-OS: the aarch64 lsfg-vk 1.x layer ships in the image (x86 layers cannot
# load into ARM64 games), so the plugin only manages the config and ~/lsfg.
SYSTEM_LIB = Path("/usr/lib/liblsfg-vk-arm64.so")
SYSTEM_JSON = Path("/usr/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation_arm64.json")

# Left in $HOME by decky-lsfg-vk 0.14 (lsfg-vk 2.0, x86) and by the x86 1.x
# install; removed on setup. ~/.lsfg (0.14's launch option) becomes a shim.
LEGACY_HOME_FILES = (
    ".local/lib/liblsfg-vk.so",
    ".local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json",
    ".local/lib/liblsfg-vk-layer.so",
    ".local/lib/liblsfg-vk-layer.x86.so",
    ".local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json",
    ".local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.x86.json",
    ".local/bin/lsfg-vk-cli",
    ".local/bin/lsfg-vk-ui",
    ".local/share/applications/gay.pancake.lsfg-vk-ui.desktop",
    ".local/share/icons/hicolor/256x256/apps/gay.pancake.lsfg-vk-ui.png",
    ".config/lsfg-vk/workarounds.json",
)
DOTLSFG_SHIM_NAME = ".lsfg"
