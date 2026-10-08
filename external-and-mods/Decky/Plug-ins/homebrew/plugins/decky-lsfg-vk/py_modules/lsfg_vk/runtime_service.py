"""PB-OS: checks profiles and renders the lsfg-vk 1.x layer config.

Upstream ran the x86 lsfg-vk 2.0 CLI here. The image's layer is lsfg-vk 1.x
for aarch64 and has no CLI, so the profiles are checked in Python and turned
into the 1.x config (conf.toml) that the layer reads.
"""

from pathlib import Path
from typing import Any, Optional

from .base_service import BaseService
from .config_schema import ConfigurationManager, ProfileData, render_layer_config
from .constants import LOSSLESS_DLL_FILENAME, PROCESS_PREFIX, SYSTEM_JSON, SYSTEM_LIB


class RuntimeService(BaseService):
    def __init__(self, logger=None, steam_service=None):
        super().__init__(logger)
        self.steam_service = steam_service

    def validate_config_content(self, content: str) -> None:
        # Raises on anything the profile store cannot hold.
        ConfigurationManager.parse_toml_content_multi_profile(content)

    def lossless_dll(self, configured_dll: str = "") -> Optional[Path]:
        """Lossless.dll for lsfg-vk 1.x; profiles may name 2.0's lsfg-vk.dll."""
        candidates = []
        if configured_dll:
            configured = Path(configured_dll)
            candidates.append(configured if configured.name == LOSSLESS_DLL_FILENAME
                              else configured.with_name(LOSSLESS_DLL_FILENAME))
        if self.steam_service is not None:
            directory = self.steam_service.lossless_scaling_path()
            if directory:
                candidates.append(directory / LOSSLESS_DLL_FILENAME)
        return next((path for path in candidates if path.is_file()), None)

    def render_layer_config(self, data: ProfileData) -> None:
        dll = self.lossless_dll(data.get("global_config", {}).get("dll", ""))
        content = render_layer_config(data, str(dll) if dll else "", PROCESS_PREFIX)
        self._write_file(self.layer_config_path, content, 0o644)

    @staticmethod
    def layer_present() -> bool:
        return Path(SYSTEM_LIB).is_file() and Path(SYSTEM_JSON).is_file()

    def is_healthy(self) -> bool:
        return self.layer_present() and self.layer_config_path.is_file()

    def check_lossless_scaling(self) -> dict[str, Any]:
        if not self.layer_present():
            return {"installed": False, "status": "The lsfg-vk layer is missing from this image"}
        if self.lossless_dll() is None:
            return {"installed": False, "status": "Lossless Scaling's Lossless.dll was not found"}
        return {"installed": True, "status": "Lossless Scaling detected"}
