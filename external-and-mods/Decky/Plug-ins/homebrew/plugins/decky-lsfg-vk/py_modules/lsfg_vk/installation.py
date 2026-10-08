"""PB-OS: setup for the image's lsfg-vk 1.x layer.

Upstream unpacked the x86 lsfg-vk 2.0 build into ~/.local. On PB-OS the
aarch64 lsfg-vk 1.x layer is part of the image, so "installing" only writes
the plugin's own files: the profile store, the 1.x layer config and the
~/.lsfg wrapper. It also clears what the x86 installs left in $HOME and
keeps launch options from earlier PB-OS builds working.
"""

import traceback

from .base_service import BaseService
from .config_schema import ConfigurationManager, UnsupportedConfigurationVersion
from .constants import LEGACY_HOME_FILES, SCRIPT_NAME, V2_BACKUP_FILENAME
from .runtime_service import RuntimeService
from .steam_service import SteamService
from .types import InstallationCheckResponse, InstallationResponse, UninstallationResponse


class InstallationService(BaseService):
    # ~/lsfg as written by decky-lsfg-vk 0.12.2 (PB-OS alpha builds; both of
    # its script variants export LSFG_PROCESS) and the shim that replaces it
    OLD_SCRIPT_LINE = "export LSFG_PROCESS="
    SHIM_MARKER = "# PB-OS: ~/lsfg now runs ~/.lsfg"

    def __init__(
        self,
        logger=None,
        runtime_service: RuntimeService = None,
        steam_service: SteamService = None,
        wrapper_service=None,
    ):
        super().__init__(logger)
        self.steam_service = steam_service or SteamService(logger=self.log)
        self.runtime_service = runtime_service or RuntimeService(logger=self.log, steam_service=self.steam_service)
        self.wrapper_service = wrapper_service
        self.old_script_path = self.user_home / SCRIPT_NAME
        self.v2_backup_path = self.config_dir / V2_BACKUP_FILENAME

    def needs_setup(self) -> bool:
        if not self.runtime_service.layer_present():
            return False
        if not (self.config_file_path.is_file() and self.layer_config_path.is_file()):
            return True
        if not self._layer_config_is_v1():
            return True
        if self._old_script_needs_shim():
            return True
        return any((self.user_home / rel).exists() for rel in LEGACY_HOME_FILES)

    def install(self) -> InstallationResponse:
        try:
            if not self.runtime_service.layer_present():
                raise FileNotFoundError("The lsfg-vk layer is missing from this image")
            self.config_dir.mkdir(parents=True, exist_ok=True)
            self._remove_legacy_home_files()
            self._keep_v2_config_aside()
            data = self._profile_data()
            content = ConfigurationManager.generate_toml_content_multi_profile(data)
            self.runtime_service.validate_config_content(content)
            self._write_file(self.config_file_path, content, 0o644)
            self.runtime_service.render_layer_config(data)
            if self.wrapper_service is not None:
                result = self.wrapper_service.repair(force=True)
                if not result.get("success"):
                    raise RuntimeError(result.get("error") or "Could not write ~/.lsfg")
            self._shim_old_script()
            return self._success_response(InstallationResponse, "lsfg-vk 1.x is set up")
        except Exception as error:
            self.log.error(f"Error setting up lsfg-vk: {error}")
            return self._error_response(InstallationResponse, str(error))

    def _remove_legacy_home_files(self) -> None:
        for rel in LEGACY_HOME_FILES:
            try:
                if self._remove_if_exists(self.user_home / rel):
                    self.log.info(f"Removed leftover {rel}")
            except OSError as error:
                self.log.warning(f"Could not remove leftover {rel}: {error}")

    def _layer_config_is_v1(self) -> bool:
        try:
            content = self.layer_config_path.read_text(encoding="utf-8")
        except OSError:
            return False
        first = next((line.strip() for line in content.splitlines()
                      if line.strip() and not line.strip().startswith("#")), "")
        return first.replace(" ", "") == "version=1"

    def _keep_v2_config_aside(self) -> None:
        """conf.toml now belongs to lsfg-vk 1.x; keep a 2.0 one as a backup"""
        if not self.layer_config_path.is_file() or self._layer_config_is_v1():
            return
        if self.v2_backup_path.exists():
            self._remove_if_exists(self.layer_config_path)
        else:
            self.layer_config_path.replace(self.v2_backup_path)
            self.log.info(f"Kept the lsfg-vk 2.0 config as {self.v2_backup_path}")

    def _profile_data(self):
        if self.config_file_path.is_file():
            try:
                return ConfigurationManager.parse_toml_content_multi_profile(
                    self.config_file_path.read_text(encoding="utf-8"))
            except (OSError, ValueError, UnsupportedConfigurationVersion) as error:
                self.log.warning(f"Profile store unreadable ({error}); starting again")
        try:
            data = ConfigurationManager.parse_toml_content_multi_profile(
                self.v2_backup_path.read_text(encoding="utf-8"))
            self.log.info(f"Imported {len(data['profiles'])} profiles from {self.v2_backup_path}")
            return data
        except (OSError, ValueError):
            return {"profiles": {}, "global_config": {"dll": "", "no_fp16": True}}

    def _old_script_needs_shim(self) -> bool:
        try:
            content = self.old_script_path.read_text(encoding="utf-8")
        except OSError:
            return False
        return self.SHIM_MARKER not in content and any(
            line.startswith(self.OLD_SCRIPT_LINE) for line in content.splitlines())

    def _shim_old_script(self) -> None:
        """Games set up with `~/lsfg %command%` (decky-lsfg-vk 0.12.2) keep working"""
        if not self._old_script_needs_shim():
            return
        self._write_file(
            self.old_script_path,
            "#!/bin/sh\n"
            f"{self.SHIM_MARKER}\n"
            'exec "$HOME/.lsfg" "$@"\n',
            0o755,
        )

    def check_installation(self) -> InstallationCheckResponse:
        try:
            installed = (self.runtime_service.layer_present()
                         and self.config_file_path.is_file()
                         and self._layer_config_is_v1())
            lossless_scaling = self.runtime_service.check_lossless_scaling()
            return {
                "installed": installed,
                "lossless_scaling_installed": bool(lossless_scaling["installed"]),
                "lossless_scaling_status": str(lossless_scaling["status"]),
                "error": None,
            }
        except Exception as error:
            return {
                "installed": False,
                "lossless_scaling_installed": False,
                "lossless_scaling_status": str(error),
                "error": str(error),
            }

    def uninstall(self) -> UninstallationResponse:
        """Remove the plugin's own files; the layer belongs to the image"""
        try:
            removed = [
                str(path)
                for path in (self.config_file_path, self.layer_config_path)
                if self._remove_if_exists(path)
            ]
            if self.old_script_path.is_file() and self.SHIM_MARKER in self.old_script_path.read_text(encoding="utf-8"):
                self._remove_if_exists(self.old_script_path)
                removed.append(str(self.old_script_path))
            if not removed:
                return self._success_response(
                    UninstallationResponse,
                    "No lsfg-vk files found to remove",
                    removed_files=None,
                )
            return self._success_response(
                UninstallationResponse,
                f"Removed {len(removed)} lsfg-vk files (the layer stays in the image).",
                removed_files=removed,
            )
        except Exception as error:
            return self._error_response(
                UninstallationResponse,
                str(error),
                removed_files=None,
            )

    def cleanup_on_uninstall(self) -> bool:
        try:
            result = self.uninstall()
            if not result.get("success"):
                self.log.error(f"Error cleaning up lsfg-vk files during uninstall: {result.get('error')}")
                return False
            return True
        except Exception as error:
            self.log.error(f"Error cleaning up lsfg-vk files during uninstall: {error}")
            self.log.error(traceback.format_exc())
            return False
