LOCAL_BIN = ".local/bin"
LOCAL_SHARE = ".local/share"
LOCAL_LIB = ".local/lib"
VULKAN_LAYER_DIR = ".local/share/vulkan/implicit_layer.d"
CONFIG_DIR = ".config/lsfg-vk"

SCRIPT_NAME = "lsfg"
WRAPPER_FILENAME = ".lsfg"
CONFIG_FILENAME = "conf.toml"
ARCHIVE_FILENAME = "lsfg-vk-2.0.0.tar.xz"
LIB_FILENAME = "liblsfg-vk-layer.so"
LIB_X86_FILENAME = "liblsfg-vk-layer.x86.so"
JSON_FILENAME = "VkLayer_LSFGVK_frame_generation.json"
JSON_X86_FILENAME = "VkLayer_LSFGVK_frame_generation.x86.json"
CLI_FILENAME = "lsfg-vk-cli"
UI_FILENAME = "lsfg-vk-ui"
UI_DESKTOP_FILENAME = "gay.pancake.lsfg-vk-ui.desktop"
UI_ICON_FILENAME = "gay.pancake.lsfg-vk-ui.png"
STEAM_LOSSLESS_SCALING_APP_ID = "993090"
STEAM_LOSSLESS_SCALING_BRANCH = "lsfg-vk"

LEGACY_LIB_FILENAME = "liblsfg-vk.so"
LEGACY_JSON_FILENAME = "VkLayer_LS_frame_generation.json"

BIN_DIR = "bin"

# PB-OS: lsfg-vk 1.x for aarch64 ships in the image (2.x draws garbage on
# Adreno/Turnip and its x86 layers cannot load into ARM64 games). The plugin
# keeps upstream's per-game profiles in PROFILES_FILENAME (lsfg-vk 2.0 format)
# and renders the 1.x layer config (conf.toml) from them; ~/.lsfg selects a
# game's [[game]] entry with LSFG_PROCESS=<PROCESS_PREFIX><appid>.
SYSTEM_LIB = "/usr/lib/liblsfg-vk-arm64.so"
SYSTEM_JSON = "/usr/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation_arm64.json"
PROFILES_FILENAME = "profiles.toml"
V2_BACKUP_FILENAME = "conf.toml.lsfg-vk-2.0"
LOSSLESS_DLL_FILENAME = "Lossless.dll"
PROCESS_PREFIX = "pbos-"

# Left in $HOME by the x86 lsfg-vk 2.0 / 1.x installs; removed on setup.
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
)
