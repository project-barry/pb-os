// Decky Loader will pass this api in, it's versioned to allow for backwards compatibility.
// @ts-ignore

// Prevents it from being duplicated in output.
const manifest = {"name":"Decky LSFG-VK","author":"Kurt Himebauch (xXJSONDeruloXx)","flags":[],"api_version":1,"publish":{"tags":["installer","vulkan","lsfg","framegen","lossless","scaling"],"description":"Enable lossless scaling frame generation on the Steam Deck using lsfg-vk compatibility layer.","image":"https://raw.githubusercontent.com/xXJSONDeruloXx/decky-lsfg-vk/refs/heads/main/assets/Decky_LSFG-VK_Master_1.png"}};
const API_VERSION = 2;
const internalAPIConnection = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
// Initialize
if (!internalAPIConnection) {
    throw new Error('[@decky/api]: Failed to connect to the loader as as the loader API was not initialized. This is likely a bug in Decky Loader.');
}
// Version 1 throws on version mismatch so we have to account for that here.
let api;
try {
    api = internalAPIConnection.connect(API_VERSION, manifest.name);
}
catch {
    api = internalAPIConnection.connect(1, manifest.name);
    console.warn(`[@decky/api] Requested API version ${API_VERSION} but the running loader only supports version 1. Some features may not work.`);
}
if (api._version != API_VERSION) {
    console.warn(`[@decky/api] Requested API version ${API_VERSION} but the running loader only supports version ${api._version}. Some features may not work.`);
}
const callable = api.callable;
const toaster = api.toaster;
/**
 * Returns state indicating the visibility of quick access menu.
 *
 * @returns `true` if quick access menu is visible and `false` otherwise.
 *
 * @example
 * import { FC, useEffect } from "react";
 * import { useQuickAccessVisible } from "@decky/api";
 *
 * export const PluginPanelView: FC<{}> = ({ }) => {
 *   const isVisible = useQuickAccessVisible();
 *
 *   useEffect(() => {
 *     if (!isVisible) {
 *       return;
 *     }
 *
 *     const interval = setInterval(() => console.log("Hello world!"), 1000);
 *     return () => {
 *       clearInterval(interval);
 *     }
 *   }, [isVisible])
 *
 *   return (
 *     <div>
 *       {isVisible ? "VISIBLE" : "INVISIBLE"}
 *     </div>
 *   );
 * };
 */
const useQuickAccessVisible = api.useQuickAccessVisible;
const definePlugin = (fn) => {
    return (...args) => {
        // TODO: Maybe wrap this
        return fn(...args);
    };
};

var DefaultContext = {
  color: undefined,
  size: undefined,
  className: undefined,
  style: undefined,
  attr: undefined
};
var IconContext = SP_REACT.createContext && /*#__PURE__*/SP_REACT.createContext(DefaultContext);

var _excluded = ["attr", "size", "title"];
function _objectWithoutProperties(source, excluded) { if (source == null) return {}; var target = _objectWithoutPropertiesLoose(source, excluded); var key, i; if (Object.getOwnPropertySymbols) { var sourceSymbolKeys = Object.getOwnPropertySymbols(source); for (i = 0; i < sourceSymbolKeys.length; i++) { key = sourceSymbolKeys[i]; if (excluded.indexOf(key) >= 0) continue; if (!Object.prototype.propertyIsEnumerable.call(source, key)) continue; target[key] = source[key]; } } return target; }
function _objectWithoutPropertiesLoose(source, excluded) { if (source == null) return {}; var target = {}; for (var key in source) { if (Object.prototype.hasOwnProperty.call(source, key)) { if (excluded.indexOf(key) >= 0) continue; target[key] = source[key]; } } return target; }
function _extends() { _extends = Object.assign ? Object.assign.bind() : function (target) { for (var i = 1; i < arguments.length; i++) { var source = arguments[i]; for (var key in source) { if (Object.prototype.hasOwnProperty.call(source, key)) { target[key] = source[key]; } } } return target; }; return _extends.apply(this, arguments); }
function ownKeys(e, r) { var t = Object.keys(e); if (Object.getOwnPropertySymbols) { var o = Object.getOwnPropertySymbols(e); r && (o = o.filter(function (r) { return Object.getOwnPropertyDescriptor(e, r).enumerable; })), t.push.apply(t, o); } return t; }
function _objectSpread(e) { for (var r = 1; r < arguments.length; r++) { var t = null != arguments[r] ? arguments[r] : {}; r % 2 ? ownKeys(Object(t), !0).forEach(function (r) { _defineProperty(e, r, t[r]); }) : Object.getOwnPropertyDescriptors ? Object.defineProperties(e, Object.getOwnPropertyDescriptors(t)) : ownKeys(Object(t)).forEach(function (r) { Object.defineProperty(e, r, Object.getOwnPropertyDescriptor(t, r)); }); } return e; }
function _defineProperty(obj, key, value) { key = _toPropertyKey(key); if (key in obj) { Object.defineProperty(obj, key, { value: value, enumerable: true, configurable: true, writable: true }); } else { obj[key] = value; } return obj; }
function _toPropertyKey(t) { var i = _toPrimitive(t, "string"); return "symbol" == typeof i ? i : i + ""; }
function _toPrimitive(t, r) { if ("object" != typeof t || !t) return t; var e = t[Symbol.toPrimitive]; if (void 0 !== e) { var i = e.call(t, r || "default"); if ("object" != typeof i) return i; throw new TypeError("@@toPrimitive must return a primitive value."); } return ("string" === r ? String : Number)(t); }
function Tree2Element(tree) {
  return tree && tree.map((node, i) => /*#__PURE__*/SP_REACT.createElement(node.tag, _objectSpread({
    key: i
  }, node.attr), Tree2Element(node.child)));
}
function GenIcon(data) {
  return props => /*#__PURE__*/SP_REACT.createElement(IconBase, _extends({
    attr: _objectSpread({}, data.attr)
  }, props), Tree2Element(data.child));
}
function IconBase(props) {
  var elem = conf => {
    var {
        attr,
        size,
        title
      } = props,
      svgProps = _objectWithoutProperties(props, _excluded);
    var computedSize = size || conf.size || "1em";
    var className;
    if (conf.className) className = conf.className;
    if (props.className) className = (className ? className + " " : "") + props.className;
    return /*#__PURE__*/SP_REACT.createElement("svg", _extends({
      stroke: "currentColor",
      fill: "currentColor",
      strokeWidth: "0"
    }, conf.attr, attr, svgProps, {
      className: className,
      style: _objectSpread(_objectSpread({
        color: props.color || conf.color
      }, conf.style), props.style),
      height: computedSize,
      width: computedSize,
      xmlns: "http://www.w3.org/2000/svg"
    }), title && /*#__PURE__*/SP_REACT.createElement("title", null, title), props.children);
  };
  return IconContext !== undefined ? /*#__PURE__*/SP_REACT.createElement(IconContext.Consumer, null, conf => elem(conf)) : elem(DefaultContext);
}

// THIS FILE IS AUTO GENERATED
function GiPlasticDuck (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 512 512"},"child":[{"tag":"path","attr":{"d":"M322.8 50.96c-28.1.66-52.4 13.13-65.8 38.48-13.4 25.36-16.1 64.96 3.6 120.46v.2c3.2 9.4 2.4 19.2-2.6 26.4-5 7.3-12.9 11.6-21.9 14.5-18 5.8-42.3 6.4-69.3 4.5-48.7-3.5-105.4-15.7-142.38-27.9-2.34 56.3 13.28 113.7 45.28 157.2 34.2 46.5 86.2 77.5 156 76.2 45.3-.8 98.8-7.4 140.2-25.5 41.4-18 70-45.8 71.3-92.4v-.1c.6-19.8-18.4-47.1-36.3-74.7-8.9-13.8-17.3-27.8-21.9-42.4-4.6-14.5-5-30.3 3.2-44.5l.2-.3.2-.3c22.2-32.6 18.7-64.5 3.9-89.24-14.7-24.79-41.5-41.12-63.7-40.6zm30.5 42.05a18 18 0 0 1 18 17.99 18 18 0 0 1-18 18 18 18 0 0 1-18-18 18 18 0 0 1 18-17.99zM416 130.2c.4 14.3-2.4 29.3-9.2 44.2 19.5-1.2 38.8-3.4 53.6-8.4 9.6-3.1 17.1-7.4 21.8-12.3 2.7-2.9 4.5-6 5.6-9.7-24.7.3-51-6.3-71.8-13.8zm-72.6 142.5c6.5 13.6 6.1 28.2.7 40.9-5.4 12.7-15.3 23.8-27.7 33.9-24.7 20-59.6 35.5-93.6 44.8-34 9.3-66.4 12.8-88.7 4.8-11.2-4-20.6-12.6-22.2-24.5-1.6-12 3.6-24.8 14.4-39.8l14.6 10.6c-9.4 13-11.8 22.2-11.2 26.8.7 4.7 3.1 7.3 10.4 10 14.7 5.2 45.9 3.5 78-5.3 32-8.7 65.3-23.8 87-41.4 10.8-8.8 18.7-18.2 22.4-27 3.8-8.8 4.1-16.8-.3-26z"},"child":[]}]})(props);
}

// THIS FILE IS AUTO GENERATED
function FaSteam (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 496 512"},"child":[{"tag":"path","attr":{"d":"M496 256c0 137-111.2 248-248.4 248-113.8 0-209.6-76.3-239-180.4l95.2 39.3c6.4 32.1 34.9 56.4 68.9 56.4 39.2 0 71.9-32.4 70.2-73.5l84.5-60.2c52.1 1.3 95.8-40.9 95.8-93.5 0-51.6-42-93.5-93.7-93.5s-93.7 42-93.7 93.5v1.2L176.6 279c-15.5-.9-30.7 3.4-43.5 12.1L0 236.1C10.2 108.4 117.1 8 247.6 8 384.8 8 496 119 496 256zM155.7 384.3l-30.5-12.6a52.79 52.79 0 0 0 27.2 25.8c26.9 11.2 57.8-1.6 69-28.4 5.4-13 5.5-27.3.1-40.3-5.4-13-15.5-23.2-28.5-28.6-12.9-5.4-26.7-5.2-38.9-.6l31.5 13c19.8 8.2 29.2 30.9 20.9 50.7-8.3 19.9-31 29.2-50.8 21zm173.8-129.9c-34.4 0-62.4-28-62.4-62.3s28-62.3 62.4-62.3 62.4 28 62.4 62.3-27.9 62.3-62.4 62.3zm.1-15.6c25.9 0 46.9-21 46.9-46.8 0-25.9-21-46.8-46.9-46.8s-46.9 21-46.9 46.8c.1 25.8 21.1 46.8 46.9 46.8z"},"child":[]}]})(props);
}function FaArrowLeft (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 448 512"},"child":[{"tag":"path","attr":{"d":"M257.5 445.1l-22.2 22.2c-9.4 9.4-24.6 9.4-33.9 0L7 273c-9.4-9.4-9.4-24.6 0-33.9L201.4 44.7c9.4-9.4 24.6-9.4 33.9 0l22.2 22.2c9.5 9.5 9.3 25-.4 34.3L136.6 216H424c13.3 0 24 10.7 24 24v32c0 13.3-10.7 24-24 24H136.6l120.5 114.8c9.8 9.3 10 24.8.4 34.3z"},"child":[]}]})(props);
}function FaCube (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 512 512"},"child":[{"tag":"path","attr":{"d":"M239.1 6.3l-208 78c-18.7 7-31.1 25-31.1 45v225.1c0 18.2 10.3 34.8 26.5 42.9l208 104c13.5 6.8 29.4 6.8 42.9 0l208-104c16.3-8.1 26.5-24.8 26.5-42.9V129.3c0-20-12.4-37.9-31.1-44.9l-208-78C262 2.2 250 2.2 239.1 6.3zM256 68.4l192 72v1.1l-192 78-192-78v-1.1l192-72zm32 356V275.5l160-65v133.9l-160 80z"},"child":[]}]})(props);
}function FaExternalLinkAlt (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 512 512"},"child":[{"tag":"path","attr":{"d":"M432,320H400a16,16,0,0,0-16,16V448H64V128H208a16,16,0,0,0,16-16V80a16,16,0,0,0-16-16H48A48,48,0,0,0,0,112V464a48,48,0,0,0,48,48H400a48,48,0,0,0,48-48V336A16,16,0,0,0,432,320ZM488,0h-128c-21.37,0-32.05,25.91-17,41l35.73,35.73L135,320.37a24,24,0,0,0,0,34L157.67,377a24,24,0,0,0,34,0L435.28,133.32,471,169c15,15,41,4.5,41-17V24A24,24,0,0,0,488,0Z"},"child":[]}]})(props);
}function FaFileAlt (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 384 512"},"child":[{"tag":"path","attr":{"d":"M224 136V0H24C10.7 0 0 10.7 0 24v464c0 13.3 10.7 24 24 24h336c13.3 0 24-10.7 24-24V160H248c-13.2 0-24-10.8-24-24zm64 236c0 6.6-5.4 12-12 12H108c-6.6 0-12-5.4-12-12v-8c0-6.6 5.4-12 12-12h168c6.6 0 12 5.4 12 12v8zm0-64c0 6.6-5.4 12-12 12H108c-6.6 0-12-5.4-12-12v-8c0-6.6 5.4-12 12-12h168c6.6 0 12 5.4 12 12v8zm0-72v8c0 6.6-5.4 12-12 12H108c-6.6 0-12-5.4-12-12v-8c0-6.6 5.4-12 12-12h168c6.6 0 12 5.4 12 12zm96-114.1v6.1H256V0h6.1c6.4 0 12.5 2.5 17 7l97.9 98c4.5 4.5 7 10.6 7 16.9z"},"child":[]}]})(props);
}function FaGamepad (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 640 512"},"child":[{"tag":"path","attr":{"d":"M480.07 96H160a160 160 0 1 0 114.24 272h91.52A160 160 0 1 0 480.07 96zM248 268a12 12 0 0 1-12 12h-52v52a12 12 0 0 1-12 12h-24a12 12 0 0 1-12-12v-52H84a12 12 0 0 1-12-12v-24a12 12 0 0 1 12-12h52v-52a12 12 0 0 1 12-12h24a12 12 0 0 1 12 12v52h52a12 12 0 0 1 12 12zm216 76a40 40 0 1 1 40-40 40 40 0 0 1-40 40zm64-96a40 40 0 1 1 40-40 40 40 0 0 1-40 40z"},"child":[]}]})(props);
}function FaTools (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 512 512"},"child":[{"tag":"path","attr":{"d":"M501.1 395.7L384 278.6c-23.1-23.1-57.6-27.6-85.4-13.9L192 158.1V96L64 0 0 64l96 128h62.1l106.6 106.6c-13.6 27.8-9.2 62.3 13.9 85.4l117.1 117.1c14.6 14.6 38.2 14.6 52.7 0l52.7-52.7c14.5-14.6 14.5-38.2 0-52.7zM331.7 225c28.3 0 54.9 11 74.9 31l19.4 19.4c15.8-6.9 30.8-16.5 43.8-29.5 37.1-37.1 49.7-89.3 37.9-136.7-2.2-9-13.5-12.1-20.1-5.5l-74.4 74.4-67.9-11.3L334 98.9l74.4-74.4c6.6-6.6 3.4-17.9-5.7-20.2-47.4-11.7-99.6.9-136.6 37.9-28.5 28.5-41.9 66.1-41.2 103.6l82.1 82.1c8.1-1.9 16.5-2.9 24.7-2.9zm-103.9 82l-56.7-56.7L18.7 402.8c-25 25-25 65.5 0 90.5s65.5 25 90.5 0l123.6-123.6c-7.6-19.9-9.9-41.6-5-62.7zM64 472c-13.2 0-24-10.8-24-24 0-13.3 10.7-24 24-24s24 10.7 24 24c0 13.2-10.7 24-24 24z"},"child":[]}]})(props);
}

const installLsfgVk = callable("install_lsfg_vk");
const uninstallLsfgVk = callable("uninstall_lsfg_vk");
const checkLsfgVkInstalled = callable("check_lsfg_vk_installed");
const getLosslessScalingBranchStatus = callable("get_lossless_scaling_branch_status");
callable("get_config_file_content");
const getFlatpakApps = callable("get_flatpak_apps");
const enableFlatpakApp = callable("enable_flatpak_app");
const updateFlatpakConfig = callable("update_flatpak_config");
const setFlatpakWorkaroundState = callable("set_flatpak_workaround_state");
const removeFlatpakApp = callable("remove_flatpak_app");
const getRunningFlatpakApps = callable("get_running_flatpak_apps");
const getGameConfigs = callable("get_game_configs");
const getInstalledGames = callable("get_installed_games");
const updateGameConfig = callable("update_game_config");
const resetGameConfig = callable("reset_game_config");
const resetGameConfigs = callable("reset_game_configs");
callable("reset_all_game_configs");
const updateGlobalConfig = callable("update_global_config");
const getWorkaroundState = callable("get_workaround_state");
const setWorkaroundState = callable("set_workaround_state");
const removeWorkaroundState = callable("remove_workaround_state");
const getWorkaroundApps = callable("get_workaround_apps");
const getDebugFileContents = callable("get_debug_file_contents");

function numericValue(value) {
    return typeof value === "number" && Number.isFinite(value) ? value : -1;
}
function numericPid(value) {
    return value && /^\d+$/.test(value) ? Number(value) : -1;
}
function compareRunningProcesses(a, b) {
    if (a.active !== b.active)
        return a.active ? -1 : 1;
    const startDifference = numericValue(b.start_time) - numericValue(a.start_time);
    if (startDifference !== 0)
        return startDifference;
    return numericPid(b.pid) - numericPid(a.pid);
}
function selectMostRecentRunningFlatpak(apps, runningApps) {
    const newestProcessByApp = new Map();
    for (const running of runningApps) {
        const current = newestProcessByApp.get(running.app_id);
        if (!current || compareRunningProcesses(running, current) < 0) {
            newestProcessByApp.set(running.app_id, running);
        }
    }
    const candidates = Array.from(newestProcessByApp.values())
        .map((running) => ({
        running,
        app: apps.find((app) => app.app_id === running.app_id) || null,
    }))
        .filter((candidate) => candidate.app !== null);
    const activeCandidates = candidates.filter(({ running }) => running.active);
    const eligibleCandidates = activeCandidates.length > 0
        ? activeCandidates
        : candidates.length === 1
            ? candidates
            : [];
    eligibleCandidates.sort((a, b) => {
        const processDifference = compareRunningProcesses(a.running, b.running);
        if (processDifference !== 0)
            return processDifference;
        return a.running.app_id.localeCompare(b.running.app_id);
    });
    return eligibleCandidates[0]?.app || null;
}
function resolveNowPlayingTarget(runningGame, runningFlatpak) {
    if (runningGame?.source === "steam") {
        return runningGame.configured ? { kind: "steam", game: runningGame } : null;
    }
    if (runningFlatpak) {
        return {
            kind: "flatpak",
            app: runningFlatpak,
            launcher: runningGame?.source === "nonSteam" ? runningGame : null,
        };
    }
    if (runningGame?.source === "nonSteam" && runningGame.configured) {
        return { kind: "nonSteam", game: runningGame };
    }
    return null;
}

const showToast = (title, body) => {
    toaster.toast({ title, body });
};
const showSuccessToast = showToast;
const showErrorToast = showToast;
const ToastMessages = {
    INSTALL_SUCCESS: {
        title: "Installation Complete",
        body: "lsfg-vk has been installed successfully",
    },
    INSTALL_ERROR: {
        title: "Installation Failed",
        body: "Unknown error occurred",
    },
    UNINSTALL_SUCCESS: {
        title: "Uninstallation Complete",
        body: "lsfg-vk has been uninstalled successfully",
    },
    UNINSTALL_ERROR: {
        title: "Uninstallation Failed",
        body: "Unknown error occurred",
    },
    CONFIG_UPDATE_ERROR: {
        title: "Update Failed",
        body: "Failed to update configuration",
    },
};
const showInstallSuccessToast = () => showSuccessToast(ToastMessages.INSTALL_SUCCESS.title, ToastMessages.INSTALL_SUCCESS.body);
const showInstallErrorToast = (error) => showErrorToast(ToastMessages.INSTALL_ERROR.title, error || ToastMessages.INSTALL_ERROR.body);
const showUninstallSuccessToast = () => showSuccessToast(ToastMessages.UNINSTALL_SUCCESS.title, ToastMessages.UNINSTALL_SUCCESS.body);
const showUninstallErrorToast = (error) => showErrorToast(ToastMessages.UNINSTALL_ERROR.title, error || ToastMessages.UNINSTALL_ERROR.body);

function useFlatpakConfiguration(enabled) {
    const [apps, setApps] = SP_REACT.useState([]);
    const [runningApps, setRunningApps] = SP_REACT.useState([]);
    const [loading, setLoading] = SP_REACT.useState(false);
    const [busyAppId, setBusyAppId] = SP_REACT.useState("");
    const reload = SP_REACT.useCallback(async () => {
        if (!enabled) {
            setApps([]);
            return;
        }
        setLoading(true);
        try {
            const result = await getFlatpakApps();
            if (!result.success)
                throw new Error(result.error || "Could not list Flatpak applications");
            setApps(result.apps || []);
        }
        catch (error) {
            showErrorToast("Flatpak unavailable", error instanceof Error ? error.message : String(error));
        }
        finally {
            setLoading(false);
        }
    }, [enabled]);
    const pollRunning = SP_REACT.useCallback(async () => {
        if (!enabled) {
            setRunningApps([]);
            return;
        }
        try {
            const result = await getRunningFlatpakApps();
            if (result.success)
                setRunningApps(result.apps || []);
        }
        catch { }
    }, [enabled]);
    SP_REACT.useEffect(() => {
        void reload();
    }, [reload]);
    SP_REACT.useEffect(() => {
        void pollRunning();
        if (!enabled)
            return;
        const interval = window.setInterval(() => void pollRunning(), 2000);
        return () => window.clearInterval(interval);
    }, [enabled, pollRunning]);
    const operate = SP_REACT.useCallback(async (appId, operation, refresh = true) => {
        if (busyAppId)
            return { success: false };
        setBusyAppId(appId);
        try {
            const result = await operation();
            if (!result.success)
                throw new Error(result.error || "Flatpak operation failed");
            if (refresh) {
                await reload();
                await pollRunning();
            }
            return result;
        }
        catch (error) {
            showErrorToast("Flatpak operation failed", error instanceof Error ? error.message : String(error));
            return { success: false, error: error instanceof Error ? error.message : String(error) };
        }
        finally {
            setBusyAppId("");
        }
    }, [busyAppId, pollRunning, reload]);
    const enableApp = SP_REACT.useCallback(async (appId) => (await operate(appId, () => enableFlatpakApp(appId))).success, [operate]);
    const removeApp = SP_REACT.useCallback(async (appId) => (await operate(appId, () => removeFlatpakApp(appId))).success, [operate]);
    const enableAll = SP_REACT.useCallback(async () => {
        if (busyAppId)
            return;
        const available = apps.filter((app) => (!app.enabled && !(app.prepared && !app.owned) && !app.error));
        for (const app of available) {
            const result = await operate(app.app_id, () => enableFlatpakApp(app.app_id), false);
            if (!result.success)
                break;
        }
        await reload();
        await pollRunning();
    }, [apps, busyAppId, operate, pollRunning, reload]);
    const removeAll = SP_REACT.useCallback(async () => {
        if (busyAppId)
            return;
        for (const app of apps.filter((item) => item.enabled)) {
            const result = await operate(app.app_id, () => removeFlatpakApp(app.app_id), false);
            if (!result.success)
                break;
        }
        await reload();
        await pollRunning();
    }, [apps, busyAppId, operate, pollRunning, reload]);
    const updateConfig = SP_REACT.useCallback(async (appId, config) => {
        const result = await operate(appId, () => updateFlatpakConfig(appId, config), false);
        if (result.success) {
            setApps((current) => current.map((app) => (app.app_id === appId ? { ...app, config: result.config || config } : app)));
        }
        return result.success;
    }, [operate]);
    const updateWorkarounds = SP_REACT.useCallback(async (appId, state) => {
        const result = await operate(appId, () => setFlatpakWorkaroundState(appId, state), false);
        if (result.success) {
            setApps((current) => current.map((app) => (app.app_id === appId ? { ...app, workarounds: result.state || state } : app)));
        }
        return result.success;
    }, [operate]);
    const runningApp = SP_REACT.useMemo(() => selectMostRecentRunningFlatpak(apps, runningApps), [apps, runningApps]);
    return {
        apps,
        runningApps,
        runningApp,
        loading,
        busyAppId,
        reload,
        enableApp,
        enableAll,
        removeApp,
        removeAll,
        updateConfig,
        updateWorkarounds,
    };
}

var ConfigFieldType;
(function (ConfigFieldType) {
    ConfigFieldType["BOOLEAN"] = "boolean";
    ConfigFieldType["INTEGER"] = "integer";
    ConfigFieldType["FLOAT"] = "float";
    ConfigFieldType["STRING"] = "string";
    ConfigFieldType["ARRAY"] = "array";
})(ConfigFieldType || (ConfigFieldType = {}));
const MULTIPLIER = "multiplier";
const FLOW_SCALE = "flow_scale";
const PERFORMANCE_MODE = "performance_mode";
function getDefaults() { return { dll: "", no_fp16: false, active_in: [], pacing_mode: "vsync", multiplier: 2, flow_scale: 0.8, performance_mode: false, override_present_mode: true, preserve_swapchain_image_count: false }; }

const DEFAULT_WRAPPER_PATH = "~/.lsfg";
const COMMAND_TOKEN = "%command%";
const LEGACY_WRAPPER_TOKENS = new Set([
    "~/lsfg",
    "~/.local/bin/lsfg",
    "~/.local/bin/lsfg-vk-experimental",
    "~/.local/bin/mako-run",
    "mako-run",
    "~/.local/bin/mako-launch",
    "mako-launch",
]);
const LEGACY_ABSOLUTE_WRAPPER = /^\/(?:home|Users)\/[^/]+\/(?:lsfg|\.local\/bin\/(?:lsfg|lsfg-vk-experimental|mako-run|mako-launch))$/;
const MANAGED_ENV_KEYS = new Set([
    "ENABLE_GAMESCOPE_WSI", "DISABLE_GAMESCOPE_WSI", "DXVK_HDR", "SteamDeck",
    "DISABLE_VKBASALT", "ENABLE_VKBASALT", "MESA_LOADER_DRIVER_OVERRIDE",
    "__GLX_VENDOR_LIBRARY_NAME", "GALLIUM_DRIVER", "DXVK_FRAME_RATE",
]);
const DXVK_FRAME_RATE_SEGMENT = /^(?:dxvk\.maxFrameRate|dxgi\.maxFrameRate|d3d9\.maxFrameRate)\s*=/i;
function asError$2(error) {
    return error instanceof Error ? error : new Error(String(error));
}
function apps() {
    return globalThis.SteamClient?.Apps;
}
function validateAppId(appId) {
    if (!Number.isSafeInteger(appId) || appId <= 0)
        throw new Error("Invalid Steam App ID");
}
function timer() {
    const host = typeof window !== "undefined" ? window : globalThis;
    return {
        set: (handler, ms) => host.setTimeout(handler, ms),
        clear: (id) => host.clearTimeout(id),
    };
}
function snapshot(appId, nonSteam, details) {
    return {
        appId,
        nonSteam,
        options: nonSteam ? details.strShortcutLaunchOptions || "" : details.strLaunchOptions || "",
        details,
    };
}
function registerDetails(appId, onDetails) {
    validateAppId(appId);
    const register = apps()?.RegisterForAppDetails;
    if (!register)
        throw new Error("Steam app-details API is unavailable");
    let active = true;
    let registration;
    const unsubscribe = () => {
        active = false;
        try {
            registration?.unregister();
        }
        catch { }
    };
    registration = register.call(apps(), appId, (details) => {
        if (active && onDetails(details || {}) === false)
            unsubscribe();
    });
    if (!active)
        unsubscribe();
    return unsubscribe;
}
async function readSteamLaunchOptions(appId, nonSteam) {
    return new Promise((resolve, reject) => {
        let done = false;
        let unsubscribe = () => { };
        const clock = timer();
        const timeout = clock.set(() => finish(new Error("Timed out reading Steam app details")), 5000);
        const finish = (error, details) => {
            if (done)
                return;
            done = true;
            clock.clear(timeout);
            unsubscribe();
            if (error)
                reject(asError$2(error));
            else
                resolve(snapshot(appId, nonSteam, details || {}));
        };
        try {
            unsubscribe = registerDetails(appId, (details) => {
                finish(undefined, details);
                return false;
            });
        }
        catch (error) {
            finish(error);
        }
    });
}
function subscribeSteamLaunchOptions(appId, nonSteam, onSnapshot, onError) {
    return registerDetails(appId, (details) => {
        try {
            onSnapshot(snapshot(appId, nonSteam, details));
        }
        catch (error) {
            onError(asError$2(error));
        }
    });
}
function decodeToken(raw) {
    let value = "";
    let quote = null;
    for (let i = 0; i < raw.length; i++) {
        const c = raw[i];
        if (c === "\\" && quote !== "'" && i + 1 < raw.length)
            value += raw[++i];
        else if (quote) {
            if (c === quote)
                quote = null;
            else
                value += c;
        }
        else if (c === "'" || c === '"')
            quote = c;
        else
            value += c;
    }
    return value;
}
function tokenize(options) {
    const tokens = [];
    let start = -1;
    let quote = null;
    let escaped = false;
    const push = (end) => {
        if (start < 0)
            return;
        const raw = options.slice(start, end);
        tokens.push({ raw, value: decodeToken(raw) });
        start = -1;
    };
    for (let i = 0; i < options.length; i++) {
        const c = options[i];
        if (start < 0) {
            if (/\s/.test(c))
                continue;
            start = i;
        }
        if (escaped)
            escaped = false;
        else if (c === "\\" && quote !== "'")
            escaped = true;
        else if (quote) {
            if (c === quote)
                quote = null;
        }
        else if (c === "'" || c === '"')
            quote = c;
        else if (/\s/.test(c))
            push(i);
    }
    push(options.length);
    return tokens;
}
const serialize = (tokens) => tokens.map(({ raw }) => raw).join(" ");
function isCommandToken(token) {
    return token.value.toLowerCase() === COMMAND_TOKEN;
}
function isMalformedCommandToken(token) {
    const value = token.value.toLowerCase();
    return value === "%command" || value === "command%";
}
function normalizeCommandTokens(tokens) {
    for (const token of tokens) {
        if (isCommandToken(token) || isMalformedCommandToken(token)) {
            token.raw = COMMAND_TOKEN;
            token.value = COMMAND_TOKEN;
        }
    }
}
const commandIndex = (tokens) => tokens.findIndex(isCommandToken);
const isAssignment = (token) => /^[A-Za-z_][A-Za-z0-9_]*=/.test(token.value);
const isLegacyToken = (value) => LEGACY_WRAPPER_TOKENS.has(value) || LEGACY_ABSOLUTE_WRAPPER.test(value);
const isWrapperToken = (value, wrapperPath) => decodeToken(value) === wrapperPath || isLegacyWrapperToken(value);
const normalizeLaunchOptions = (options) => serialize(tokenize(options));
const isLegacyWrapperToken = (value) => isLegacyToken(decodeToken(value));
function removeMatchingWrappers(tokens, predicate) {
    const command = commandIndex(tokens);
    const prefixEnd = command >= 0 ? command : tokens.length;
    const kept = tokens.filter((token, i) => i >= prefixEnd || !predicate(token.value));
    if (kept.length === tokens.length)
        return false;
    tokens.splice(0, tokens.length, ...kept);
    return true;
}
function installLaunchOption(options, wrapperPath = DEFAULT_WRAPPER_PATH) {
    const tokens = tokenize(options);
    normalizeCommandTokens(tokens);
    removeMatchingWrappers(tokens, isLegacyToken);
    let command = commandIndex(tokens);
    if (command >= 0) {
        if (tokens[command - 1]?.value === wrapperPath)
            return { options: serialize(tokens), commandTokenAdded: false };
        removeMatchingWrappers(tokens, (value) => decodeToken(value) === wrapperPath);
        command = commandIndex(tokens);
        tokens.splice(command, 0, { raw: wrapperPath, value: wrapperPath });
        return { options: serialize(tokens), commandTokenAdded: false };
    }
    const existingWrapper = tokens.findIndex((token) => decodeToken(token.value) === wrapperPath);
    if (existingWrapper >= 0) {
        tokens.splice(existingWrapper + 1, 0, { raw: COMMAND_TOKEN, value: COMMAND_TOKEN });
        return { options: serialize(tokens), commandTokenAdded: true };
    }
    let insertion = 0;
    while (insertion < tokens.length && isAssignment(tokens[insertion]))
        insertion++;
    tokens.splice(insertion, 0, { raw: wrapperPath, value: wrapperPath }, { raw: COMMAND_TOKEN, value: COMMAND_TOKEN });
    return { options: serialize(tokens), commandTokenAdded: true };
}
function removeWrapperLaunchOption(options, wrapperPath = DEFAULT_WRAPPER_PATH, commandTokenAdded = false) {
    const tokens = tokenize(options);
    normalizeCommandTokens(tokens);
    if (removeMatchingWrappers(tokens, (value) => isWrapperToken(value, wrapperPath)) && commandTokenAdded) {
        const command = commandIndex(tokens);
        if (command >= 0)
            tokens.splice(command, 1);
    }
    return serialize(tokens);
}
function encodeAssignmentValue(value) {
    return /^[A-Za-z0-9_./:+,%=-]+$/.test(value)
        ? value
        : `"${value.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}
function cleanupPluginAssignments(options) {
    const tokens = tokenize(options);
    const command = commandIndex(tokens);
    const prefixEnd = command >= 0 ? command : tokens.length;
    return serialize(tokens.flatMap((token, i) => {
        if (i >= prefixEnd || !isAssignment(token))
            return [token];
        const split = token.value.indexOf("=");
        const key = token.value.slice(0, split);
        if (key === "DXVK_CONFIG") {
            const value = token.value.slice(split + 1).split(";").map((part) => part.trim())
                .filter((part) => part && !DXVK_FRAME_RATE_SEGMENT.test(part)).join("; ");
            return value ? [{ raw: `DXVK_CONFIG=${encodeAssignmentValue(value)}`, value: `DXVK_CONFIG=${value}` }] : [];
        }
        return MANAGED_ENV_KEYS.has(key) ? [] : [token];
    }));
}
function cleanupLegacyLaunchOptions(options) {
    const tokens = tokenize(options);
    removeMatchingWrappers(tokens, isLegacyToken);
    return serialize(tokens);
}
function hasWrapperLaunchIntegration(options, wrapperPath = DEFAULT_WRAPPER_PATH) {
    const tokens = tokenize(options);
    const command = commandIndex(tokens);
    return command > 0 && tokens[command - 1].value === wrapperPath;
}
function isWrapperIntegrationInstalled(steam, _nonSteam, wrapperPath = DEFAULT_WRAPPER_PATH) {
    return hasWrapperLaunchIntegration(steam.options, wrapperPath);
}
const queues = new Map();
function queued(appId, nonSteam, operation) {
    const key = `${nonSteam ? "shortcut" : "app"}:${appId}`;
    const previous = queues.get(key) || Promise.resolve();
    const current = previous.catch(() => undefined).then(operation);
    const cleanup = current.then(() => { if (queues.get(key) === cleanup)
        queues.delete(key); }, () => { if (queues.get(key) === cleanup)
        queues.delete(key); });
    queues.set(key, cleanup);
    return current;
}
async function waitFor(appId, nonSteam, matches, message) {
    const deadline = Date.now() + 5000;
    let lastError = null;
    while (Date.now() <= deadline) {
        try {
            const value = await readSteamLaunchOptions(appId, nonSteam);
            if (matches(value))
                return value;
        }
        catch (error) {
            lastError = asError$2(error);
        }
        if (Date.now() < deadline)
            await new Promise((resolve) => timer().set(resolve, 100));
    }
    throw lastError ? new Error(`${message}: ${lastError.message}`) : new Error(`${message} before the readback timeout`);
}
async function writeVerified(appId, nonSteam, previous, next, write, message) {
    try {
        await write(next);
        return await waitFor(appId, nonSteam, (value) => normalizeLaunchOptions(value.options) === normalizeLaunchOptions(next), message);
    }
    catch (error) {
        const failure = asError$2(error);
        try {
            await write(previous);
            await waitFor(appId, nonSteam, (value) => normalizeLaunchOptions(value.options) === normalizeLaunchOptions(previous), "Steam did not restore the previous launch options");
        }
        catch (rollback) {
            throw new Error(`${failure.message}; rollback also failed: ${asError$2(rollback).message}`);
        }
        throw failure;
    }
}
function writeOptions(appId, nonSteam, value) {
    const setter = nonSteam ? apps()?.SetShortcutLaunchOptions : apps()?.SetAppLaunchOptions;
    if (!setter)
        return Promise.reject(new Error(`Steam ${nonSteam ? "shortcut " : ""}launch options API is unavailable`));
    return Promise.resolve(setter.call(apps(), appId, value));
}
function installWrapperIntegration(appId, nonSteam, wrapperPath, commandTokenAdded = false) {
    return queued(appId, nonSteam, async () => {
        const current = await readSteamLaunchOptions(appId, nonSteam);
        const cleaned = cleanupPluginAssignments(cleanupLegacyLaunchOptions(current.options));
        const alreadyInstalled = hasWrapperLaunchIntegration(current.options, wrapperPath);
        const rewrite = installLaunchOption(cleaned, wrapperPath);
        if (rewrite.options === current.options)
            return { snapshot: current, commandTokenAdded, changed: false };
        const value = await writeVerified(appId, nonSteam, current.options, rewrite.options, (options) => writeOptions(appId, nonSteam, options), "Steam did not accept the launch options");
        return {
            snapshot: value,
            commandTokenAdded: alreadyInstalled ? commandTokenAdded : commandTokenAdded || rewrite.commandTokenAdded,
            changed: true,
        };
    });
}
function removeWrapperIntegration(appId, nonSteam, wrapperPath, commandTokenAdded = false) {
    return queued(appId, nonSteam, async () => {
        const current = await readSteamLaunchOptions(appId, nonSteam);
        const next = cleanupPluginAssignments(removeWrapperLaunchOption(current.options, wrapperPath, commandTokenAdded));
        return next === current.options ? current : writeVerified(appId, nonSteam, current.options, next, (options) => writeOptions(appId, nonSteam, options), "Steam did not clean the launch options");
    });
}
const getDefaultWrapperPath = () => DEFAULT_WRAPPER_PATH;

function sourceFromNonSteam(nonSteam) {
    return nonSteam ? "nonSteam" : "steam";
}
function getTargetSource(appid, installedGames, workaroundApps) {
    const workaround = workaroundApps.find((item) => item.appid === appid);
    if (workaround)
        return sourceFromNonSteam(workaround.non_steam);
    const installed = installedGames.find((game) => game.appid === appid);
    return installed ? sourceFromNonSteam(installed.nonSteam) : "unknown";
}
function mergeGameTargets(configs, installedGames, workaroundApps, runningGame = null) {
    const configuredIds = new Set(configs.map((game) => game.appid));
    const targets = installedGames.map((game) => {
        const source = getTargetSource(game.appid, installedGames, workaroundApps);
        return {
            ...game,
            nonSteam: source === "nonSteam",
            source,
            configured: configuredIds.has(game.appid),
        };
    });
    for (const game of configs) {
        if (targets.some((target) => target.appid === game.appid))
            continue;
        const source = getTargetSource(game.appid, installedGames, workaroundApps);
        targets.push({
            appid: game.appid,
            name: game.profile || `App ${game.appid}`,
            nonSteam: source === "nonSteam",
            source,
            configured: true,
        });
    }
    if (runningGame
        && !targets.some((target) => target.appid === runningGame.appid)
        && (runningGame.configured || runningGame.source !== "unknown")) {
        targets.unshift(runningGame);
    }
    return targets;
}
function targetsForSource(targets, source) {
    return targets.filter((target) => target.source === source || (target.source === "unknown" && target.configured));
}
function sourceLabel(source) {
    if (source === "nonSteam")
        return "Non-Steam";
    if (source === "steam")
        return "Steam";
    return "Unknown source";
}

async function getSteamShortcuts() {
    const apps = globalThis.SteamClient?.Apps;
    if (typeof apps?.GetAllShortcuts !== "function")
        return [];
    try {
        const shortcuts = await apps.GetAllShortcuts();
        if (!Array.isArray(shortcuts))
            return [];
        return shortcuts.flatMap((shortcut) => {
            const appid = Number(shortcut?.appid);
            const name = shortcut?.data?.strAppName;
            if (!Number.isInteger(appid) || appid === 0 || typeof name !== "string" || !name)
                return [];
            return [{
                    appid: String(appid >>> 0),
                    name,
                    nonSteam: true,
                }];
        });
    }
    catch {
        return [];
    }
}
function mergeInstalledGames(backendGames, shortcutGames) {
    const games = new Map(backendGames.map((game) => [game.appid, game]));
    for (const game of shortcutGames) {
        const existing = games.get(game.appid);
        games.set(game.appid, existing ? { ...existing, name: game.name, nonSteam: true } : game);
    }
    return Array.from(games.values());
}
const DEFAULT_WORKAROUND_STATE$1 = {
    dxvkFrameRate: 0,
    disableGamescopeWsi: true,
    disableHdr: true,
    disableSteamdeckMode: false,
    disableVkbasalt: false,
    enableZink: false,
};
function asError$1(error) {
    return error instanceof Error ? error : new Error(String(error));
}
function useGameConfiguration() {
    const [games, setGames] = SP_REACT.useState([]);
    const [globalConfig, setGlobalConfig] = SP_REACT.useState({ dll: "", no_fp16: false });
    const [installedGames, setInstalledGames] = SP_REACT.useState([]);
    const [workaroundApps, setWorkaroundApps] = SP_REACT.useState([]);
    const [configsLoaded, setConfigsLoaded] = SP_REACT.useState(false);
    const [selectedAppId, setSelectedAppId] = SP_REACT.useState("");
    const [runningGame, setRunningGame] = SP_REACT.useState(null);
    const [bulkOperationBusy, setBulkOperationBusy] = SP_REACT.useState(false);
    const bulkOperationLock = SP_REACT.useRef(false);
    const previousRunningAppId = SP_REACT.useRef(null);
    const previousQuickAccessVisible = SP_REACT.useRef(null);
    const quickAccessVisible = useQuickAccessVisible();
    const load = SP_REACT.useCallback(async () => {
        const [result, installed, shortcuts, workaroundResult] = await Promise.all([
            getGameConfigs(),
            getInstalledGames(),
            getSteamShortcuts(),
            getWorkaroundApps(),
        ]);
        if (result.success) {
            setGlobalConfig(result.global_config || { dll: "", no_fp16: false });
            setGames(result.games || []);
        }
        setInstalledGames(mergeInstalledGames(installed.success ? installed.games || [] : [], shortcuts));
        setWorkaroundApps(workaroundResult.success ? workaroundResult.apps || [] : []);
        setConfigsLoaded(true);
    }, []);
    SP_REACT.useEffect(() => {
        const initialLoad = previousQuickAccessVisible.current === null;
        const becameVisible = quickAccessVisible && previousQuickAccessVisible.current === false;
        previousQuickAccessVisible.current = quickAccessVisible;
        if (initialLoad || becameVisible)
            void load();
    }, [load, quickAccessVisible]);
    SP_REACT.useEffect(() => {
        const poll = () => {
            if (!configsLoaded)
                return;
            const app = DFL.Router.MainRunningApp;
            if (!app?.appid)
                return setRunningGame(null);
            const appid = String(app.appid);
            const installed = installedGames.find((game) => game.appid === appid);
            const name = app.display_name || installed?.name;
            if (!name)
                return setRunningGame(null);
            const source = getTargetSource(appid, installedGames, workaroundApps);
            const next = {
                ...(installed || { appid, name, nonSteam: source === "nonSteam" }),
                name,
                nonSteam: source === "nonSteam",
                source,
                configured: games.some((game) => game.appid === appid),
            };
            setRunningGame((current) => (current?.appid === next.appid
                && current.name === next.name
                && current.nonSteam === next.nonSteam
                && current.source === next.source
                && current.configured === next.configured
                ? current
                : next));
        };
        poll();
        const interval = window.setInterval(poll, 2000);
        return () => window.clearInterval(interval);
    }, [configsLoaded, games, installedGames, workaroundApps]);
    SP_REACT.useEffect(() => {
        const appid = runningGame?.appid || null;
        if (appid !== previousRunningAppId.current) {
            previousRunningAppId.current = appid;
            setSelectedAppId(appid || "");
        }
    }, [runningGame?.appid]);
    const targets = SP_REACT.useMemo(() => {
        return mergeGameTargets(games, installedGames, workaroundApps, runningGame);
    }, [games, installedGames, runningGame, workaroundApps]);
    const template = SP_REACT.useMemo(() => ({ ...getDefaults(), ...globalConfig }), [globalConfig]);
    const config = games.find((game) => game.appid === selectedAppId)?.config || template;
    const runningConfig = runningGame
        ? games.find((game) => game.appid === runningGame.appid)?.config || template
        : template;
    const ensureTargetWorkarounds = SP_REACT.useCallback(async (target) => {
        if (target.source === "unknown") {
            showErrorToast("Could not initialize workarounds", "The target source is unknown; re-discover the game before enabling it");
            return false;
        }
        if (!installedGames.some((game) => game.appid === target.appid))
            return true;
        const appId = Number(target.appid);
        let integration = null;
        let newState = false;
        let stateWriteAttempted = false;
        let wrapperPath = getDefaultWrapperPath();
        try {
            const existing = await getWorkaroundState(target.appid);
            if (!existing.success)
                throw new Error(existing.error || "Could not read workaround state");
            wrapperPath = existing.wrapper_path || getDefaultWrapperPath();
            const state = existing.state || { ...DEFAULT_WORKAROUND_STATE$1 };
            const commandTokenAdded = existing.command_token_added === true;
            newState = !existing.state;
            integration = await installWrapperIntegration(appId, target.nonSteam, wrapperPath, commandTokenAdded);
            stateWriteAttempted = true;
            const saved = await setWorkaroundState(target.appid, state, integration.commandTokenAdded, target.nonSteam);
            if (!saved.success)
                throw new Error(saved.error || "Could not save workaround state");
            return true;
        }
        catch (error) {
            let rollbackSucceeded = true;
            if (integration?.changed) {
                try {
                    await removeWrapperIntegration(appId, target.nonSteam, wrapperPath, integration.commandTokenAdded);
                }
                catch (rollbackError) {
                    showErrorToast("Workaround rollback failed", asError$1(rollbackError).message);
                    rollbackSucceeded = false;
                }
            }
            if (rollbackSucceeded && newState && stateWriteAttempted) {
                const restored = await removeWorkaroundState(target.appid);
                if (!restored.success) {
                    showErrorToast("Workaround rollback failed", restored.error || "Could not roll back workaround state");
                    rollbackSucceeded = false;
                }
            }
            showErrorToast("Could not initialize workarounds", asError$1(error).message);
            return false;
        }
    }, [installedGames]);
    const removeTargetWorkarounds = SP_REACT.useCallback(async (target) => {
        const installed = installedGames.some((game) => game.appid === target.appid);
        if (target.source === "unknown" && installed)
            return true;
        const appId = Number(target.appid);
        try {
            const existing = await getWorkaroundState(target.appid);
            if (!existing.success)
                throw new Error(existing.error || "Could not read workaround state");
            const wrapperPath = existing.wrapper_path || getDefaultWrapperPath();
            if (installed) {
                await removeWrapperIntegration(appId, target.nonSteam, wrapperPath, existing.command_token_added === true);
            }
            const removed = await removeWorkaroundState(target.appid);
            if (!removed.success)
                throw new Error(removed.error || "Could not remove workaround state");
            return true;
        }
        catch (error) {
            showErrorToast("Could not clean up game workarounds", asError$1(error).message);
            return false;
        }
    }, [installedGames]);
    const acquireBulkOperation = SP_REACT.useCallback(() => {
        if (bulkOperationLock.current)
            return false;
        bulkOperationLock.current = true;
        setBulkOperationBusy(true);
        return true;
    }, []);
    const releaseBulkOperation = SP_REACT.useCallback(() => {
        bulkOperationLock.current = false;
        setBulkOperationBusy(false);
    }, []);
    const cleanupAllWorkarounds = SP_REACT.useCallback(async () => {
        try {
            const result = await getWorkaroundApps();
            if (!result.success)
                throw new Error(result.error || "Could not read workaround state");
            const cleaned = new Set();
            const wrapperPath = result.wrapper_path || getDefaultWrapperPath();
            for (const entry of result.apps || []) {
                await removeWrapperIntegration(Number(entry.appid), entry.non_steam, wrapperPath, entry.command_token_added);
                const removed = await removeWorkaroundState(entry.appid);
                if (!removed.success)
                    throw new Error(removed.error || "Could not remove workaround state");
                cleaned.add(entry.appid);
            }
            // Also clean configured targets whose sidecar entry was lost. This
            // removes an old wrapper and only the plugin-managed launch pieces.
            for (const target of targets.filter((item) => item.configured && installedGames.some((game) => game.appid === item.appid))) {
                if (!cleaned.has(target.appid) && !(await removeTargetWorkarounds(target)))
                    return false;
            }
            return true;
        }
        catch (error) {
            showErrorToast("Could not clean up game launch options", asError$1(error).message);
            return false;
        }
    }, [installedGames, removeTargetWorkarounds, targets]);
    const saveFor = SP_REACT.useCallback(async (appid, next, cleanupLaunchOptions = false) => {
        const target = targets.find((item) => item.appid === appid);
        if (!target?.name)
            return false;
        if (cleanupLaunchOptions && !(await ensureTargetWorkarounds(target)))
            return false;
        const result = await updateGameConfig(appid, target.name, next);
        if (result.success)
            await load();
        return result.success;
    }, [ensureTargetWorkarounds, load, targets]);
    const save = SP_REACT.useCallback(async (next, cleanupLaunchOptions = false) => {
        if (!selectedAppId)
            return false;
        return saveFor(selectedAppId, next, cleanupLaunchOptions);
    }, [saveFor, selectedAppId]);
    const updateGlobal = SP_REACT.useCallback(async (next) => {
        const result = await updateGlobalConfig(next);
        if (!result.success)
            return false;
        setGlobalConfig(result.global_config || next);
        return true;
    }, []);
    const enable = SP_REACT.useCallback(async (appid) => {
        const target = targets.find((item) => item.appid === appid);
        if (!target?.name)
            return false;
        if (!(await ensureTargetWorkarounds(target)))
            return false;
        const result = await updateGameConfig(appid, target.name, template);
        if (result.success)
            await load();
        else
            await removeTargetWorkarounds(target);
        return result.success;
    }, [ensureTargetWorkarounds, load, removeTargetWorkarounds, targets, template]);
    const enableAll = SP_REACT.useCallback(async (source) => {
        if (!acquireBulkOperation())
            return;
        try {
            const available = targets.filter((target) => target.source === source && !target.configured && target.name);
            if (available.length === 0)
                return;
            for (const target of available) {
                if (!(await ensureTargetWorkarounds(target))) {
                    await load();
                    return;
                }
                const result = await updateGameConfig(target.appid, target.name, template);
                if (!result.success) {
                    await removeTargetWorkarounds(target);
                    showErrorToast("Could not enable all games", result.error || `Could not create a profile for ${target.name}`);
                    await load();
                    return;
                }
            }
            await load();
        }
        finally {
            releaseBulkOperation();
        }
    }, [acquireBulkOperation, ensureTargetWorkarounds, load, removeTargetWorkarounds, releaseBulkOperation, targets, template]);
    const repair = SP_REACT.useCallback(async (appid) => {
        const target = targets.find((item) => item.appid === appid);
        if (!target)
            return false;
        const success = await ensureTargetWorkarounds(target);
        if (success)
            await load();
        return success;
    }, [ensureTargetWorkarounds, load, targets]);
    const resetSelected = SP_REACT.useCallback(async () => {
        if (selectedAppId) {
            const selectedTarget = targets.find((target) => target.appid === selectedAppId);
            if (selectedTarget && !(await removeTargetWorkarounds(selectedTarget)))
                return;
            const result = await resetGameConfig(selectedAppId);
            if (result.success) {
                setRunningGame((current) => current?.appid === selectedAppId ? { ...current, configured: false } : current);
                setSelectedAppId("");
                await load();
            }
        }
    }, [load, removeTargetWorkarounds, selectedAppId, targets]);
    const resetAll = SP_REACT.useCallback(async (source) => {
        if (!acquireBulkOperation())
            return;
        try {
            const selectedTargets = targets.filter((item) => item.configured && item.source === source);
            if (selectedTargets.length === 0)
                return;
            for (const target of selectedTargets) {
                if (!(await removeTargetWorkarounds(target))) {
                    await load();
                    return;
                }
            }
            const result = await resetGameConfigs(selectedTargets.map((target) => target.appid));
            if (!result.success) {
                showErrorToast("Could not remove all profiles", result.error || "Could not remove the selected profiles");
                await load();
                return;
            }
            setRunningGame((current) => current?.source === source ? { ...current, configured: false } : current);
            setSelectedAppId("");
            await load();
        }
        finally {
            releaseBulkOperation();
        }
    }, [acquireBulkOperation, load, removeTargetWorkarounds, releaseBulkOperation, targets]);
    return { config, runningConfig, globalConfig, targets, runningGame, selectedAppId, setSelectedAppId, save, saveFor, updateGlobal, enable, enableAll, repair, resetSelected, resetAll, bulkOperationBusy, cleanupAllWorkarounds, reload: load };
}

function useInstallation(reloadConfig, beforeUninstall) {
    const [isInstalled, setIsInstalled] = SP_REACT.useState(false);
    const [installationStatus, setInstallationStatus] = SP_REACT.useState("");
    const [losslessScalingInstalled, setLosslessScalingInstalled] = SP_REACT.useState(false);
    const [losslessScalingStatus, setLosslessScalingStatus] = SP_REACT.useState("");
    const [steamBranchStatus, setSteamBranchStatus] = SP_REACT.useState(null);
    const [isInstalling, setIsInstalling] = SP_REACT.useState(false);
    const [isUninstalling, setIsUninstalling] = SP_REACT.useState(false);
    const checkInstallation = SP_REACT.useCallback(async () => {
        try {
            setSteamBranchStatus(await getLosslessScalingBranchStatus());
        }
        catch (error) {
            console.error("Error checking Lossless Scaling Steam branch:", error);
            setSteamBranchStatus(null);
        }
        try {
            const status = await checkLsfgVkInstalled();
            setIsInstalled(status.installed);
            setLosslessScalingInstalled(status.lossless_scaling_installed);
            setLosslessScalingStatus(status.lossless_scaling_status || "Lossless Scaling Not Installed");
            setInstallationStatus(status.installed ? "lsfg-vk Installed" : "lsfg-vk Not Installed");
            return status.installed;
        }
        catch {
            setSteamBranchStatus(null);
            setLosslessScalingInstalled(false);
            setLosslessScalingStatus("Lossless Scaling Not Installed");
            setInstallationStatus("lsfg-vk Not Installed");
            return false;
        }
    }, []);
    SP_REACT.useEffect(() => {
        void checkInstallation();
    }, [checkInstallation]);
    const setupComplete = isInstalled &&
        losslessScalingInstalled &&
        steamBranchStatus?.success === true &&
        steamBranchStatus.installed &&
        !steamBranchStatus.needs_switch;
    SP_REACT.useEffect(() => {
        if (setupComplete || isInstalling || isUninstalling)
            return;
        const interval = window.setInterval(() => {
            void checkInstallation();
        }, 2000);
        return () => window.clearInterval(interval);
    }, [checkInstallation, isInstalling, isUninstalling, setupComplete]);
    const install = async () => {
        setIsInstalling(true);
        setInstallationStatus("Installing lsfg-vk...");
        try {
            const result = await installLsfgVk();
            if (!result.success) {
                setInstallationStatus(`Installation failed: ${result.error}`);
                showInstallErrorToast(result.error ?? undefined);
                return;
            }
            setInstallationStatus("lsfg-vk installed");
            showInstallSuccessToast();
            await reloadConfig?.();
            await checkInstallation();
        }
        catch (error) {
            setInstallationStatus(`Installation failed: ${error}`);
            showInstallErrorToast(String(error));
        }
        finally {
            setIsInstalling(false);
        }
    };
    const uninstall = async () => {
        setIsUninstalling(true);
        setInstallationStatus("Uninstalling lsfg-vk...");
        try {
            if (beforeUninstall && !(await beforeUninstall())) {
                setInstallationStatus("Uninstallation cancelled: could not clean up launch options");
                return;
            }
            const result = await uninstallLsfgVk();
            if (!result.success) {
                setInstallationStatus(`Uninstallation failed: ${result.error}`);
                showUninstallErrorToast(result.error ?? undefined);
                return;
            }
            setIsInstalled(false);
            setInstallationStatus("lsfg-vk uninstalled successfully!");
            await checkInstallation();
            showUninstallSuccessToast();
        }
        catch (error) {
            setInstallationStatus(`Uninstallation failed: ${error}`);
            showUninstallErrorToast(String(error));
        }
        finally {
            setIsUninstalling(false);
        }
    };
    return {
        isInstalled,
        setupComplete,
        installationStatus,
        losslessScalingInstalled,
        losslessScalingStatus,
        steamBranchStatus,
        isInstalling,
        isUninstalling,
        install,
        uninstall,
        checkInstallation,
    };
}

const tabStyles = `
  .lsfg-vk-tabs > div > div:first-child {
    background: #0D141C;
    box-shadow: none;
    backdrop-filter: none;
  }

  .lsfg-vk-tabs [role="tabpanel"] {
    padding-left: 8px !important;
    padding-right: 8px !important;
  }

  .lsfg-vk-tabs .lsfg-vk-tab-content {
    padding-bottom: 96px; // workaround for in-game bottom bar padding behaving differently than in launcher, remove later?
  }

  .lsfg-vk-tabs [role="tablist"] {
    display: flex;
    flex-wrap: nowrap;
    justify-content: center;
  }

  .lsfg-vk-tabs [role="tab"] {
    flex: 0 1 auto;
    min-width: 0;
    box-sizing: border-box;
    padding-left: 6px !important;
    padding-right: 6px !important;
    display: flex !important;
    align-items: center;
    justify-content: center;
  }

  .lsfg-vk-tabs [role="tab"] svg {
    display: block;
    margin: 0;
  }

  .lsfg-vk-tabs--content-focused [role="tablist"][aria-orientation="horizontal"],
  .lsfg-vk-tabs--content-focused [role="tablist"][aria-orientation="horizontal"] > div,
  .lsfg-vk-tabs--content-focused [role="tablist"][aria-orientation="horizontal"] > div > div,
  .lsfg-vk-tabs--content-focused [role="tablist"][aria-orientation="horizontal"] [role="tab"] {
    animation: none !important;
    transition: none !important;
  }

  .lsfg-vk-tabs--content-focused [role="tablist"][aria-orientation="horizontal"] > div > div {
    scroll-behavior: auto !important;
    scroll-snap-type: none !important;
  }
`;

// THIS FILE IS AUTO GENERATED
function RiArrowDownSFill (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 24 24","fill":"currentColor"},"child":[{"tag":"path","attr":{"d":"M12 16L6 10H18L12 16Z"},"child":[]}]})(props);
}function RiArrowUpSFill (props) {
  return GenIcon({"tag":"svg","attr":{"viewBox":"0 0 24 24","fill":"currentColor"},"child":[{"tag":"path","attr":{"d":"M12 8L18 14H6L12 8Z"},"child":[]}]})(props);
}

var ja = {
	CONTENT_FPS_MULTIPLIER: "FPS倍率",
	CONTENT_NERD_STUFF: "詳細情報",
	CONTENT_FLATPAK_SETUP: "Flatpak設定",
	MULTIPLIER_OFF: "オフ",
	CONFIG_SECTION_TITLE: "設定",
	CONFIG_WORKAROUNDS_TITLE: "互換性設定",
	CONFIG_FLOW_SCALE: "フロースケール",
	CONFIG_FLOW_SCALE_DESC: "内部モーション推定解像度を下げて、パフォーマンスをわずかに向上させます",
	CONFIG_BASE_FPS_CAP: "基本FPS上限",
	CONFIG_BASE_FPS_CAP_OFF: "オフ",
	CONFIG_BASE_FPS_CAP_DESC: "フレーム生成前のDXVKゲームの基本上限。0で無効。ゲームの再起動が必要です。",
	CONFIG_PRESENT_MODE: "プレゼンテーションモード",
	CONFIG_PRESENT_MODE_FIFO: "FIFO - VSync",
	CONFIG_PRESENT_MODE_MAILBOX: "Mailbox",
	CONFIG_PRESENT_MODE_DESC: "FIFO - VSync（デフォルト）とMailboxプレゼンテーションモードを切り替えて、パフォーマンスまたは互換性を向上させます",
	CONFIG_PERFORMANCE_MODE: "パフォーマンスモード",
	CONFIG_PERFORMANCE_MODE_DESC: "FGに軽量なモデルを使用します（ほとんどのゲームに推奨）",
	CONFIG_HDR_MODE: "HDRモード",
	CONFIG_HDR_MODE_DESC: "HDRモードを有効化します（HDRをサポートするゲームのみ）",
	CONFIG_ENABLE_WSI: "WSIを有効化",
	CONFIG_ENABLE_WSI_DESC: "Gamescope WSIレイヤーを再有効化します。ゲームの再起動が必要。",
	CONFIG_DISABLE_GAMESCOPE_WSI: "Gamescope WSIを無効化",
	CONFIG_DISABLE_GAMESCOPE_WSI_DESC: "ENABLE_GAMESCOPE_WSI=0を追加します。ゲームの再起動が必要です。",
	CONFIG_DISABLE_HDR: "HDRを無効化",
	CONFIG_DISABLE_HDR_DESC: "DXVKがゲームにHDRを公開しないようにします。ゲームの再起動が必要です。",
	CONFIG_DISABLE_STEAMDECK_MODE: "Steam Deckモードを無効化",
	CONFIG_DISABLE_STEAMDECK_MODE_DESC: "ゲーム固有のSteam Deck互換スイッチを無効化します。ゲームの再起動が必要です。",
	CONFIG_DISABLE_VKBASALT: "vkBasaltを無効化",
	CONFIG_DISABLE_VKBASALT_DESC: "LSFGと競合する可能性のあるvkBasaltレイヤーを無効化します（Reshade、一部のDeckyプラグイン）",
	CONFIG_ENABLE_ZINK: "OpenGLゲームでZinkを強制",
	CONFIG_ENABLE_ZINK_DESC: "MesaのZink OpenGL-to-Vulkanドライバーを使用します。一部のゲームでクラッシュやフリーズが発生する可能性があります。ゲームの再起動が必要です。",
	INSTALL_INSTALLING: "インストール中...",
	INSTALL_UNINSTALLING: "アンインストール中...",
	INSTALL_UNINSTALL_BTN: "LSFG-VKをアンインストール",
	INSTALL_INSTALL_BTN: "LSFG-VKをインストール",
	FLATPAK_MODAL_TITLE: "Flatpak拡張",
	FLATPAK_RUNTIME_INSTALLER: "ランタイム拡張インストーラー",
	FLATPAK_RUNTIME_23: "ランタイム 23.08",
	FLATPAK_RUNTIME_24: "ランタイム 24.08",
	FLATPAK_RUNTIME_25: "ランタイム 25.08",
	FLATPAK_INSTALLED: "インストール済み",
	FLATPAK_NOT_INSTALLED: "未インストール",
	FLATPAK_UNINSTALL_TITLE: "ランタイム拡張をアンインストール",
	FLATPAK_UNINSTALL_CONFIRM_PREFIX: "本当に",
	FLATPAK_UNINSTALL_CONFIRM_SUFFIX: "ランタイム拡張をアンインストールしますか？",
	FLATPAK_UNINSTALL_BTN: "アンインストール",
	FLATPAK_INSTALL_BTN: "インストール",
	FLATPAK_APPS_TITLE: "Flatpakアプリケーション",
	FLATPAK_NO_APPS: "Flatpakアプリなし",
	FLATPAK_NO_APPS_DESC: "現在インストールされているFlatpakアプリケーションはありません",
	FLATPAK_STATUS_CONFIGURED: "設定済み",
	FLATPAK_STATUS_PARTIAL: "部分設定",
	FLATPAK_STATUS_NO_OVERRIDES: "オーバーライドなし",
	FLATPAK_ERROR: "エラー",
	FLATPAK_ERROR_STATUS: "拡張ステータスの確認に失敗しました",
	FLATPAK_ERROR_APPS: "Flatpakアプリケーションの読み込みに失敗しました",
	FLATPAK_CLOSE: "閉じる",
	NERD_LOADING: "情報を読み込み中...",
	NERD_DLL_PATH: "DLLパス",
	NERD_NOT_AVAILABLE: "利用不可",
	NERD_DLL_HASH: "DLL SHA256ハッシュ",
	NERD_DETECTION_SOURCE: "検出ソース",
	NERD_PATH_PREFIX: "パス:",
	NERD_NO_CONTENT: "コンテンツなし",
	NERD_CONFIG_FILE: "設定ファイル",
	NERD_CONFIG_NOT_FOUND_PREFIX: "設定が見つかりません:",
	NERD_CLOSE: "閉じる",
	PROFILE_CLOSE_GAME: "プロファイルを変更するにはゲームを終了してください。",
	PROFILE_SECTION_TITLE: "プロファイル:",
	PROFILE_DEFAULT: "デフォルト",
	PROFILE_NEW: "新しいプロファイル",
	PROFILE_NAME_LABEL: "名前",
	PROFILE_CREATE_TITLE: "新しいプロファイルを作成",
	PROFILE_CREATE_DESC: "新しいプロファイルの名前を入力してください。現在のプロファイルの設定がコピーされます。",
	PROFILE_CREATE_BTN: "作成",
	PROFILE_CANCEL_BTN: "キャンセル",
	PROFILE_RENAME_TITLE: "プロファイルの名前を変更",
	PROFILE_RENAME_DESC_PREFIX: "プロファイルの新しい名前を入力してください:",
	PROFILE_RENAME_BTN: "名前変更",
	PROFILE_CANNOT_DELETE_TITLE: "デフォルトプロファイルは削除できません",
	PROFILE_CANNOT_DELETE_MSG: "デフォルトプロファイルは削除できません",
	PROFILE_DELETE_TITLE: "プロファイルを削除",
	PROFILE_DELETE_DESC_PREFIX: "本当にこのプロファイルを削除しますか？",
	PROFILE_DELETE_DESC_SUFFIX: "この操作は取り消せません。",
	PROFILE_DELETE_BTN: "削除",
	PROFILE_CANNOT_RENAME_TITLE: "デフォルトプロファイルの名前は変更できません",
	PROFILE_CANNOT_RENAME_MSG: "デフォルトプロファイルの名前は変更できません"
};
var ko = {
	CONTENT_FPS_MULTIPLIER: "FPS 배율",
	CONTENT_NERD_STUFF: "상세 정보",
	CONTENT_FLATPAK_SETUP: "Flatpak 설정",
	MULTIPLIER_OFF: "끄기",
	CONFIG_SECTION_TITLE: "설정",
	CONFIG_WORKAROUNDS_TITLE: "호환성 설정",
	CONFIG_FLOW_SCALE: "흐름 배율",
	CONFIG_FLOW_SCALE_DESC: "내부 모션 추정 해상도를 낮춰 성능을 약간 향상시킵니다",
	CONFIG_BASE_FPS_CAP: "기본 FPS 상한",
	CONFIG_BASE_FPS_CAP_OFF: "끄기",
	CONFIG_BASE_FPS_CAP_DESC: "프레임 생성 전 DXVK 게임의 기본 제한입니다. 0은 비활성화합니다. 게임 재시작 필요.",
	CONFIG_PRESENT_MODE: "프레젠테이션 모드",
	CONFIG_PRESENT_MODE_FIFO: "FIFO - VSync",
	CONFIG_PRESENT_MODE_MAILBOX: "Mailbox",
	CONFIG_PRESENT_MODE_DESC: "FIFO - VSync(기본)와 Mailbox 프레젠테이션 모드를 전환하여 성능 또는 호환성을 개선합니다",
	CONFIG_PERFORMANCE_MODE: "성능 모드",
	CONFIG_PERFORMANCE_MODE_DESC: "FG에 더 가벼운 모델을 사용합니다 (대부분의 게임에 권장)",
	CONFIG_HDR_MODE: "HDR 모드",
	CONFIG_HDR_MODE_DESC: "HDR 모드를 활성화합니다 (HDR을 지원하는 게임에만 해당)",
	CONFIG_ENABLE_WSI: "WSI 활성화",
	CONFIG_ENABLE_WSI_DESC: "Gamescope WSI 레이어를 다시 활성화합니다. 게임 재시작 필요.",
	CONFIG_DISABLE_GAMESCOPE_WSI: "Gamescope WSI 비활성화",
	CONFIG_DISABLE_GAMESCOPE_WSI_DESC: "ENABLE_GAMESCOPE_WSI=0을 추가합니다. 게임 재시작 필요.",
	CONFIG_DISABLE_HDR: "HDR 비활성화",
	CONFIG_DISABLE_HDR_DESC: "DXVK가 게임에 HDR을 노출하지 않도록 합니다. 게임 재시작 필요.",
	CONFIG_DISABLE_STEAMDECK_MODE: "Steam Deck 모드 비활성화",
	CONFIG_DISABLE_STEAMDECK_MODE_DESC: "게임별 Steam Deck 호환 스위치를 비활성화합니다. 게임 재시작 필요.",
	CONFIG_DISABLE_VKBASALT: "vkBasalt 비활성화",
	CONFIG_DISABLE_VKBASALT_DESC: "LSFG와 충돌할 수 있는 vkBasalt 레이어를 비활성화합니다 (Reshade, 일부 Decky 플러그인)",
	CONFIG_ENABLE_ZINK: "OpenGL 게임에 Zink 강제",
	CONFIG_ENABLE_ZINK_DESC: "Mesa의 Zink OpenGL-to-Vulkan 드라이버를 사용합니다. 일부 게임에서 충돌 또는 멈춤이 발생할 수 있으며 게임 재시작이 필요합니다.",
	INSTALL_INSTALLING: "설치 중...",
	INSTALL_UNINSTALLING: "제거 중...",
	INSTALL_UNINSTALL_BTN: "LSFG-VK 제거",
	INSTALL_INSTALL_BTN: "LSFG-VK 설치",
	FLATPAK_MODAL_TITLE: "Flatpak 확장",
	FLATPAK_RUNTIME_INSTALLER: "런타임 확장 설치",
	FLATPAK_RUNTIME_23: "런타임 23.08",
	FLATPAK_RUNTIME_24: "런타임 24.08",
	FLATPAK_RUNTIME_25: "런타임 25.08",
	FLATPAK_INSTALLED: "설치됨",
	FLATPAK_NOT_INSTALLED: "설치 안 됨",
	FLATPAK_UNINSTALL_TITLE: "런타임 확장 제거",
	FLATPAK_UNINSTALL_CONFIRM_PREFIX: "정말로",
	FLATPAK_UNINSTALL_CONFIRM_SUFFIX: "런타임 확장을 제거하시겠습니까?",
	FLATPAK_UNINSTALL_BTN: "제거",
	FLATPAK_INSTALL_BTN: "설치",
	FLATPAK_APPS_TITLE: "Flatpak 애플리케이션",
	FLATPAK_NO_APPS: "Flatpak 앱 없음",
	FLATPAK_NO_APPS_DESC: "현재 설치된 Flatpak 애플리케이션이 없습니다",
	FLATPAK_STATUS_CONFIGURED: "설정됨",
	FLATPAK_STATUS_PARTIAL: "부분 설정",
	FLATPAK_STATUS_NO_OVERRIDES: "오버라이드 없음",
	FLATPAK_ERROR: "오류",
	FLATPAK_ERROR_STATUS: "확장 상태 확인 실패",
	FLATPAK_ERROR_APPS: "Flatpak 애플리케이션 로드 실패",
	FLATPAK_CLOSE: "닫기",
	NERD_LOADING: "정보 불러오는 중...",
	NERD_DLL_PATH: "DLL 경로",
	NERD_NOT_AVAILABLE: "사용 불가",
	NERD_DLL_HASH: "DLL SHA256 해시",
	NERD_DETECTION_SOURCE: "감지 소스",
	NERD_PATH_PREFIX: "경로:",
	NERD_NO_CONTENT: "내용 없음",
	NERD_CONFIG_FILE: "설정 파일",
	NERD_CONFIG_NOT_FOUND_PREFIX: "설정 없음:",
	NERD_CLOSE: "닫기",
	PROFILE_CLOSE_GAME: "프로필 변경을 위해 게임을 종료하세요.",
	PROFILE_SECTION_TITLE: "프로필:",
	PROFILE_DEFAULT: "기본",
	PROFILE_NEW: "새 프로필",
	PROFILE_NAME_LABEL: "이름",
	PROFILE_CREATE_TITLE: "새 프로필 만들기",
	PROFILE_CREATE_DESC: "새 프로필 이름을 입력하세요. 현재 프로필의 설정이 복사됩니다.",
	PROFILE_CREATE_BTN: "만들기",
	PROFILE_CANCEL_BTN: "취소",
	PROFILE_RENAME_TITLE: "프로필 이름 변경",
	PROFILE_RENAME_DESC_PREFIX: "프로필의 새 이름을 입력하세요:",
	PROFILE_RENAME_BTN: "이름 변경",
	PROFILE_CANNOT_DELETE_TITLE: "기본 프로필 삭제 불가",
	PROFILE_CANNOT_DELETE_MSG: "기본 프로필은 삭제할 수 없습니다",
	PROFILE_DELETE_TITLE: "프로필 삭제",
	PROFILE_DELETE_DESC_PREFIX: "정말로 프로필을 삭제하시겠습니까?",
	PROFILE_DELETE_DESC_SUFFIX: "이 작업은 취소할 수 없습니다.",
	PROFILE_DELETE_BTN: "삭제",
	PROFILE_CANNOT_RENAME_TITLE: "기본 프로필 이름 변경 불가",
	PROFILE_CANNOT_RENAME_MSG: "기본 프로필의 이름은 변경할 수 없습니다"
};
var language_metadata = {
	ko: {
		name: "한국어"
	},
	en: {
		name: "English"
	},
	ja: {
		name: "日本語"
	},
	zh: {
		name: "中文"
	}
};
var steam_language_map = {
	korean: "ko",
	koreana: "ko",
	english: "en",
	japanese: "ja",
	schinese: "zh",
	tchinese: "zh",
	spanish: "es",
	french: "fr",
	german: "de",
	italian: "it",
	portuguese: "pt",
	russian: "ru"
};
var template = {
	CONTENT_FPS_MULTIPLIER: "FPS Multiplier",
	CONTENT_NERD_STUFF: "Nerd Stuff",
	CONTENT_FLATPAK_SETUP: "Flatpak Setup",
	MULTIPLIER_OFF: "OFF",
	CONFIG_SECTION_TITLE: "Config",
	CONFIG_WORKAROUNDS_TITLE: "Workarounds",
	CONFIG_FLOW_SCALE: "Flow Scale",
	CONFIG_FLOW_SCALE_DESC: "Lowers internal motion estimation resolution, improving performance slightly",
	CONFIG_BASE_FPS_CAP: "Base FPS Cap",
	CONFIG_BASE_FPS_CAP_OFF: "Off",
	CONFIG_BASE_FPS_CAP_DESC: "Base cap for DXVK-backed games before frame generation; 0 disables. Requires game restart to apply.",
	CONFIG_PRESENT_MODE: "Present Mode",
	CONFIG_PRESENT_MODE_FIFO: "FIFO - VSync",
	CONFIG_PRESENT_MODE_MAILBOX: "Mailbox",
	CONFIG_PRESENT_MODE_DESC: "Toggle between FIFO - VSync (default) and Mailbox presentation modes for better performance or compatibility",
	CONFIG_PERFORMANCE_MODE: "Performance Mode",
	CONFIG_PERFORMANCE_MODE_DESC: "Uses a lighter model for FG (Recommended for most games)",
	CONFIG_HDR_MODE: "HDR Mode",
	CONFIG_HDR_MODE_DESC: "Enables HDR mode (only for games that support HDR)",
	CONFIG_ENABLE_WSI: "Enable WSI",
	CONFIG_ENABLE_WSI_DESC: "Re-Enable Gamescope WSI Layer. Requires game restart to apply.",
	CONFIG_DISABLE_GAMESCOPE_WSI: "Disable Gamescope WSI",
	CONFIG_DISABLE_GAMESCOPE_WSI_DESC: "Adds ENABLE_GAMESCOPE_WSI=0. Requires game restart to apply.",
	CONFIG_DISABLE_HDR: "Disable HDR",
	CONFIG_DISABLE_HDR_DESC: "Prevents DXVK from exposing HDR to the game. Requires game restart to apply.",
	CONFIG_DISABLE_STEAMDECK_MODE: "Disable Steam Deck Mode",
	CONFIG_DISABLE_STEAMDECK_MODE_DESC: "Disables a game-specific Steam Deck compatibility switch. Requires game restart to apply.",
	CONFIG_DISABLE_VKBASALT: "Disable vkBasalt",
	CONFIG_DISABLE_VKBASALT_DESC: "Disables vkBasalt layer which can conflict with LSFG (Reshade, some Decky plugins)",
	CONFIG_ENABLE_ZINK: "Force Zink for OpenGL Games",
	CONFIG_ENABLE_ZINK_DESC: "Uses Mesa's Zink OpenGL-to-Vulkan driver. May cause crashes or freezes with some games. Requires game restart to apply.",
	INSTALL_INSTALLING: "Installing...",
	INSTALL_UNINSTALLING: "Uninstalling...",
	INSTALL_UNINSTALL_BTN: "Uninstall LSFG-VK",
	INSTALL_INSTALL_BTN: "Install LSFG-VK",
	FLATPAK_MODAL_TITLE: "Flatpak Extensions",
	FLATPAK_RUNTIME_INSTALLER: "Runtime Extension Installer",
	FLATPAK_RUNTIME_23: "Runtime 23.08",
	FLATPAK_RUNTIME_24: "Runtime 24.08",
	FLATPAK_RUNTIME_25: "Runtime 25.08",
	FLATPAK_INSTALLED: "Installed",
	FLATPAK_NOT_INSTALLED: "Not installed",
	FLATPAK_UNINSTALL_TITLE: "Uninstall Runtime Extension",
	FLATPAK_UNINSTALL_CONFIRM_PREFIX: "Are you sure you want to uninstall the",
	FLATPAK_UNINSTALL_CONFIRM_SUFFIX: "runtime extension?",
	FLATPAK_UNINSTALL_BTN: "Uninstall",
	FLATPAK_INSTALL_BTN: "Install",
	FLATPAK_APPS_TITLE: "Flatpak Applications",
	FLATPAK_NO_APPS: "No Flatpak Apps Found",
	FLATPAK_NO_APPS_DESC: "No Flatpak applications are currently installed",
	FLATPAK_STATUS_CONFIGURED: "Configured",
	FLATPAK_STATUS_PARTIAL: "Partial",
	FLATPAK_STATUS_NO_OVERRIDES: "No overrides",
	FLATPAK_ERROR: "Error",
	FLATPAK_ERROR_STATUS: "Failed to check extension status",
	FLATPAK_ERROR_APPS: "Failed to load Flatpak applications",
	FLATPAK_CLOSE: "Close",
	NERD_LOADING: "Loading information...",
	NERD_DLL_PATH: "DLL Path",
	NERD_NOT_AVAILABLE: "Not available",
	NERD_DLL_HASH: "DLL SHA256 Hash",
	NERD_DETECTION_SOURCE: "Detection Source",
	NERD_PATH_PREFIX: "Path:",
	NERD_NO_CONTENT: "No content",
	NERD_CONFIG_FILE: "Configuration File",
	NERD_CONFIG_NOT_FOUND_PREFIX: "Config not found:",
	NERD_CLOSE: "Close",
	PROFILE_CLOSE_GAME: "Close game to change profile.",
	PROFILE_SECTION_TITLE: "Profile:",
	PROFILE_DEFAULT: "Default",
	PROFILE_NEW: "New Profile",
	PROFILE_NAME_LABEL: "Name",
	PROFILE_CREATE_TITLE: "Create New Profile",
	PROFILE_CREATE_DESC: "Enter a name for the new profile. The current profile's settings will be copied.",
	PROFILE_CREATE_BTN: "Create",
	PROFILE_CANCEL_BTN: "Cancel",
	PROFILE_RENAME_TITLE: "Rename Profile",
	PROFILE_RENAME_DESC_PREFIX: "Enter a new name for the profile",
	PROFILE_RENAME_BTN: "Rename",
	PROFILE_CANNOT_DELETE_TITLE: "Cannot delete default profile",
	PROFILE_CANNOT_DELETE_MSG: "The default profile cannot be deleted",
	PROFILE_DELETE_TITLE: "Delete Profile",
	PROFILE_DELETE_DESC_PREFIX: "Are you sure you want to delete the profile",
	PROFILE_DELETE_DESC_SUFFIX: "? This action cannot be undone.",
	PROFILE_DELETE_BTN: "Delete",
	PROFILE_CANNOT_RENAME_TITLE: "Cannot rename default profile",
	PROFILE_CANNOT_RENAME_MSG: "The default profile cannot be renamed"
};
var languages = {
	ja: ja,
	ko: ko,
	language_metadata: language_metadata,
	steam_language_map: steam_language_map,
	template: template
};

var languages$1 = /*#__PURE__*/Object.freeze({
    __proto__: null,
    default: languages,
    ja: ja,
    ko: ko,
    language_metadata: language_metadata,
    steam_language_map: steam_language_map,
    template: template
});

// languages.json build via CI build
// to generate for localhost/dev, run `build_i18n_json.sh` script
const languageData = languages$1;
const steamLanguageMap = languageData.steam_language_map;
const normalizeLanguage = (language) => {
    const normalized = language.trim().toLowerCase();
    return steamLanguageMap[normalized] ?? normalized;
};
function getLangs() {
    const langs = languageData.language_metadata;
    Object.keys(languageData).forEach((lang) => {
        if (lang === "language_metadata" || lang == "steam_language_map") {
            return;
        }
        const strs = languageData[lang];
        if (lang && strs && langs[lang]?.name) {
            langs[lang].strings = strs;
        }
    });
    return langs;
}
const LANGS = getLangs();
let cachedLang;
const getCurrentLanguage = () => {
    if (cachedLang)
        return cachedLang;
    const lang = normalizeLanguage(window.LocalizationManager.m_rgLocalesToUse[0]);
    cachedLang = lang;
    return lang;
};
/**
 * Translate a key to the current language
 *
 * @param key - Translation key
 * @param originalString - Original text (fallback)
 * @returns Translated string or original text if translation not found
 *
 * @example
 * t('CONTENT_FPS_MULTIPLIER', 'FPS Multiplier')
 */
const t = (key, originalString) => {
    const lang = getCurrentLanguage();
    // English always returns the original text
    if (lang === "en")
        return originalString;
    // Return translation if exists, otherwise return original text
    return LANGS[lang]?.strings?.[key] ?? originalString;
};

function usePersistentCollapsed$2(key) {
    const [collapsed, setCollapsed] = SP_REACT.useState(() => {
        try {
            return localStorage.getItem(key) !== "false";
        }
        catch {
            return true;
        }
    });
    SP_REACT.useEffect(() => {
        try {
            localStorage.setItem(key, String(collapsed));
        }
        catch {
            // Persisting the view preference is optional.
        }
    }, [collapsed, key]);
    return [collapsed, () => setCollapsed((value) => !value)];
}
function DebugFileSection({ file }) {
    const [collapsed, toggleCollapsed] = usePersistentCollapsed$2(`lsfg-debug-file-${file.id}-collapsed-v1`);
    const status = file.exists ? "Present" : "Not present";
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.Field, { label: file.label, description: `${file.path} · ${status}`, bottomSeparator: "none" })),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement("div", { className: "LSFG_DebugFileCollapseButton_Container", style: { marginTop: "-2px", marginBottom: "4px" } },
                window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", bottomSeparator: collapsed ? "standard" : "none", onClick: toggleCollapsed }, collapsed ? window.SP_REACT.createElement(RiArrowDownSFill, null) : window.SP_REACT.createElement(RiArrowUpSFill, null)))),
        !collapsed && (window.SP_REACT.createElement(DFL.PanelSectionRow, null, file.exists && file.content !== null && file.content !== undefined ? (window.SP_REACT.createElement("pre", { style: { whiteSpace: "pre-wrap", overflowWrap: "anywhere" } }, file.content)) : (window.SP_REACT.createElement(DFL.Field, { label: "File unavailable", description: file.error || "The file has not been created yet." }))))));
}
function ConfigFileTab() {
    const [result, setResult] = SP_REACT.useState(null);
    SP_REACT.useEffect(() => {
        getDebugFileContents().then(setResult).catch((error) => {
            setResult({ success: false, error: String(error) });
        });
    }, []);
    if (!result) {
        return (window.SP_REACT.createElement(DFL.PanelSection, { title: t("NERD_CONFIG_FILE", "Config / Debug") },
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Spinner, null))));
    }
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement("style", null, `
          .LSFG_DebugFileCollapseButton_Container > div > div > div > button,
          .LSFG_DebugFileCollapseButton_Container > div > div > div > div > button {
            height: 24px !important;
            min-height: 24px !important;
            padding: 0 !important;
            display: flex !important;
            align-items: center !important;
            justify-content: center !important;
          }

          .LSFG_DebugFileCollapseButton_Container svg {
            display: block;
            margin: 0;
          }
        `),
        window.SP_REACT.createElement(DFL.PanelSection, { title: t("NERD_CONFIG_FILE", "Config / Debug") },
            result.error && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Field, { label: "Error", description: result.error }))),
            result.success && result.files?.map((file) => (window.SP_REACT.createElement(DebugFileSection, { key: file.id, file: file }))))));
}

function ConfigurationSection({ config, onConfigChange }) {
    return window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.SliderField, { label: `Flow Scale (${Math.round(config.flow_scale * 100)}%)`, value: config.flow_scale, min: 0.25, max: 1, step: 0.01, onChange: (value) => onConfigChange(FLOW_SCALE, value) })),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.ToggleField, { label: "Performance Mode", checked: config.performance_mode, onChange: (value) => onConfigChange(PERFORMANCE_MODE, value) })));
}

function FpsMultiplierControl({ config, onConfigChange, autoFocus = false, onAutoFocus, }) {
    const focusableRef = SP_REACT.useRef(null);
    SP_REACT.useEffect(() => {
        if (!autoFocus)
            return;
        const frame = requestAnimationFrame(() => {
            const target = focusableRef.current?.querySelector('[role="button"], [role="slider"]');
            target?.focus();
            onAutoFocus?.();
        });
        return () => cancelAnimationFrame(frame);
    }, [autoFocus, onAutoFocus]);
    const multiplierLabel = config.multiplier === 1
        ? t("MULTIPLIER_OFF", "Off")
        : `${config.multiplier}x`;
    return (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
        window.SP_REACT.createElement(DFL.Focusable, { ref: focusableRef, noFocusRing: true },
            window.SP_REACT.createElement(DFL.SliderField, { label: `FPS multiplier · ${multiplierLabel}`, value: config.multiplier, min: 1, max: 6, step: 1, notchCount: 6, notchLabels: [
                    { notchIndex: 0, label: "OFF", value: 1 },
                    { notchIndex: 1, label: "2X", value: 2 },
                    { notchIndex: 2, label: "3X", value: 3 },
                    { notchIndex: 3, label: "4X", value: 4 },
                    { notchIndex: 4, label: "5X", value: 5 },
                    { notchIndex: 5, label: "6X", value: 6 },
                ], notchTicksVisible: true, showValue: false, onChange: (value) => void onConfigChange(MULTIPLIER, value) }))));
}

const SLIDER_DEBOUNCE_MS = 250;
const DEFAULT_WORKAROUND_STATE = {
    dxvkFrameRate: 0,
    disableGamescopeWsi: true,
    disableHdr: true,
    disableSteamdeckMode: false,
    disableVkbasalt: false,
    enableZink: false,
};
function asError(error) {
    return error instanceof Error ? error : new Error(String(error));
}
function makeSnapshot(steam, result, nonSteam) {
    if (!result.state)
        throw new Error("Workaround state is not initialized for this profile");
    const wrapperPath = result.wrapper_path || getDefaultWrapperPath();
    return {
        steam,
        state: result.state,
        wrapperPath,
        wrapperOwned: result.wrapper_owned === true,
        integrationInstalled: isWrapperIntegrationInstalled(steam, nonSteam, wrapperPath),
        commandTokenAdded: result.command_token_added === true,
    };
}
async function adoptWorkaroundState(appId, nonSteam, wrapperPath) {
    let integration = null;
    try {
        integration = await installWrapperIntegration(Number(appId), nonSteam, wrapperPath, false);
        const finalized = await setWorkaroundState(appId, DEFAULT_WORKAROUND_STATE, integration.commandTokenAdded, nonSteam);
        if (!finalized.success)
            throw new Error(finalized.error || "Could not finalize workaround state");
        return makeSnapshot(integration.snapshot, finalized, nonSteam);
    }
    catch (error) {
        let rollbackSucceeded = true;
        if (integration?.changed) {
            try {
                await removeWrapperIntegration(Number(appId), nonSteam, wrapperPath, integration.commandTokenAdded);
            }
            catch {
                rollbackSucceeded = false;
            }
        }
        if (rollbackSucceeded) {
            const removed = await removeWorkaroundState(appId);
            if (!removed.success)
                throw new Error(removed.error || "Could not roll back workaround state");
        }
        throw error;
    }
}
function usePerAppWorkarounds(appId, nonSteam) {
    const [status, setStatus] = SP_REACT.useState("loading");
    const [snapshot, setSnapshot] = SP_REACT.useState(null);
    const [error, setError] = SP_REACT.useState(null);
    const pendingSliderUpdate = SP_REACT.useRef(null);
    const numericAppId = Number(appId);
    const loadSnapshot = SP_REACT.useCallback(async () => {
        const [result, steam] = await Promise.all([
            getWorkaroundState(appId),
            readSteamLaunchOptions(numericAppId, nonSteam),
        ]);
        if (!result.success)
            throw new Error(result.error || "Could not read workaround state");
        if (!result.state) {
            return adoptWorkaroundState(appId, nonSteam, result.wrapper_path || getDefaultWrapperPath());
        }
        return makeSnapshot(steam, result, nonSteam);
    }, [appId, nonSteam, numericAppId]);
    const applySnapshot = SP_REACT.useCallback((next) => {
        setSnapshot(next);
        setStatus("ready");
        setError(null);
    }, []);
    const refresh = SP_REACT.useCallback(async () => {
        setStatus("loading");
        setError(null);
        try {
            applySnapshot(await loadSnapshot());
        }
        catch (refreshError) {
            const nextError = asError(refreshError);
            setStatus("error");
            setError(nextError.message);
        }
    }, [applySnapshot, loadSnapshot]);
    SP_REACT.useEffect(() => {
        let active = true;
        setStatus("loading");
        setSnapshot(null);
        setError(null);
        let unsubscribe = () => { };
        try {
            unsubscribe = subscribeSteamLaunchOptions(numericAppId, nonSteam, (steam) => {
                if (!active)
                    return;
                setSnapshot((current) => current ? {
                    ...current,
                    steam,
                    integrationInstalled: isWrapperIntegrationInstalled(steam, nonSteam, current.wrapperPath),
                } : current);
            }, (subscriptionError) => {
                if (!active)
                    return;
                setStatus("error");
                setError(subscriptionError.message);
            });
        }
        catch (subscriptionError) {
            if (active) {
                setStatus("error");
                setError(asError(subscriptionError).message);
            }
        }
        void loadSnapshot()
            .then((next) => { if (active)
            applySnapshot(next); })
            .catch((readError) => {
            if (active) {
                setStatus("error");
                setError(asError(readError).message);
            }
        });
        return () => {
            active = false;
            unsubscribe();
        };
    }, [applySnapshot, loadSnapshot, nonSteam, numericAppId]);
    const persistUpdate = SP_REACT.useCallback(async (field, value) => {
        const current = snapshot;
        if (!current)
            return false;
        setError(null);
        const nextState = { ...current.state, [field]: value };
        try {
            const result = await setWorkaroundState(appId, nextState, current.commandTokenAdded, nonSteam);
            if (!result.success || !result.state)
                throw new Error(result.error || "Could not save workaround state");
            applySnapshot({
                ...current,
                state: result.state,
                wrapperPath: result.wrapper_path || current.wrapperPath,
                wrapperOwned: result.wrapper_owned === true,
                commandTokenAdded: result.command_token_added === true,
            });
            return true;
        }
        catch (updateError) {
            const nextError = asError(updateError);
            setStatus("error");
            setError(nextError.message);
            showErrorToast("Workaround update failed", nextError.message);
            return false;
        }
    }, [appId, applySnapshot, snapshot]);
    const flushSliderUpdate = SP_REACT.useCallback(async () => {
        const pending = pendingSliderUpdate.current;
        if (!pending)
            return true;
        pendingSliderUpdate.current = null;
        clearTimeout(pending.timer);
        const success = await persistUpdate("dxvkFrameRate", pending.value);
        pending.waiters.forEach((resolve) => resolve(success));
        return success;
    }, [persistUpdate]);
    const update = SP_REACT.useCallback(async (field, value) => {
        if (field === "dxvkFrameRate") {
            setError(null);
            return new Promise((resolve) => {
                const pending = pendingSliderUpdate.current || { timer: 0, value: 0, waiters: [] };
                window.clearTimeout(pending.timer);
                pending.value = Number(value);
                pending.waiters.push(resolve);
                pending.timer = window.setTimeout(() => { void flushSliderUpdate(); }, SLIDER_DEBOUNCE_MS);
                pendingSliderUpdate.current = pending;
            });
        }
        const sliderSuccess = await flushSliderUpdate();
        if (!sliderSuccess)
            return false;
        return persistUpdate(field, value);
    }, [flushSliderUpdate, persistUpdate]);
    SP_REACT.useEffect(() => () => {
        const pending = pendingSliderUpdate.current;
        if (!pending)
            return;
        window.clearTimeout(pending.timer);
        pendingSliderUpdate.current = null;
        pending.waiters.forEach((resolve) => resolve(false));
    }, [numericAppId, nonSteam]);
    return SP_REACT.useMemo(() => ({ status, snapshot, refresh, update, error }), [error, refresh, snapshot, status, update]);
}

const WORKAROUNDS_COLLAPSED_KEY = "lsfg-workarounds-collapsed-v2";
const TOGGLE_ROWS = [
    {
        field: "disableSteamdeckMode",
        labelKey: "CONFIG_DISABLE_STEAMDECK_MODE",
        label: "Disable Steam Deck Mode",
        descriptionKey: "CONFIG_DISABLE_STEAMDECK_MODE_DESC",
        description: "Disables a game-specific Steam Deck compatibility switch. Requires game restart to apply.",
    },
    {
        field: "disableGamescopeWsi",
        labelKey: "CONFIG_DISABLE_GAMESCOPE_WSI",
        label: "Disable Gamescope WSI",
        descriptionKey: "CONFIG_DISABLE_GAMESCOPE_WSI_DESC",
        description: "Adds ENABLE_GAMESCOPE_WSI=0. Requires game restart to apply.",
    },
    {
        field: "disableHdr",
        labelKey: "CONFIG_DISABLE_HDR",
        label: "Disable HDR",
        descriptionKey: "CONFIG_DISABLE_HDR_DESC",
        description: "Prevents DXVK from exposing HDR to the game. Requires game restart to apply.",
    },
    {
        field: "disableVkbasalt",
        labelKey: "CONFIG_DISABLE_VKBASALT",
        label: "Disable vkBasalt",
        descriptionKey: "CONFIG_DISABLE_VKBASALT_DESC",
        description: "Disables vkBasalt layer which can conflict with LSFG (Reshade, some Decky plugins)",
    },
    {
        field: "enableZink",
        labelKey: "CONFIG_ENABLE_ZINK",
        label: "Force Zink for OpenGL Games",
        descriptionKey: "CONFIG_ENABLE_ZINK_DESC",
        description: "Uses Mesa's Zink OpenGL-to-Vulkan driver. May cause crashes or freezes with some games. Requires game restart to apply.",
    },
];
function usePersistentCollapsed$1() {
    const [collapsed, setCollapsed] = SP_REACT.useState(() => {
        try {
            const saved = localStorage.getItem(WORKAROUNDS_COLLAPSED_KEY);
            return saved !== null ? JSON.parse(saved) === true : true;
        }
        catch {
            return true;
        }
    });
    SP_REACT.useEffect(() => {
        try {
            localStorage.setItem(WORKAROUNDS_COLLAPSED_KEY, JSON.stringify(collapsed));
        }
        catch { }
    }, [collapsed]);
    return [collapsed, () => setCollapsed((value) => !value)];
}
function WorkaroundsSection({ appId, nonSteam, onRepair }) {
    const [collapsed, toggleCollapsed] = usePersistentCollapsed$1();
    const { status, snapshot, refresh, update, error } = usePerAppWorkarounds(appId, nonSteam);
    const [repairing, setRepairing] = SP_REACT.useState(false);
    const state = snapshot?.state;
    const controlsDisabled = status !== "ready" || state === undefined || snapshot?.wrapperOwned !== true || snapshot.integrationInstalled !== true;
    const [fpsValue, setFpsValue] = SP_REACT.useState(null);
    const effectiveFpsValue = fpsValue ?? state?.dxvkFrameRate ?? 0;
    const fpsLabel = effectiveFpsValue > 0
        ? `${effectiveFpsValue} FPS`
        : t("CONFIG_BASE_FPS_CAP_OFF", "Off");
    const handleRepair = async () => {
        if (!onRepair || repairing)
            return;
        setRepairing(true);
        try {
            if (await onRepair())
                await refresh();
        }
        finally {
            setRepairing(false);
        }
    };
    SP_REACT.useEffect(() => {
        setFpsValue(state?.dxvkFrameRate ?? null);
    }, [state?.dxvkFrameRate, status]);
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement("style", null, `
        .LSFG_WorkaroundsCollapseButton_Container > div > div > div > button,
        .LSFG_WorkaroundsCollapseButton_Container > div > div > div > div > button {
          height: 24px !important;
          padding: 0 !important;
          display: flex !important;
          align-items: center !important;
          justify-content: center !important;
        }
        `),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement("div", { style: {
                    fontSize: "14px",
                    fontWeight: "bold",
                    marginTop: "8px",
                    marginBottom: "6px",
                    borderBottom: "1px solid rgba(255, 255, 255, 0.2)",
                    paddingBottom: "3px",
                    color: "white",
                } }, t("CONFIG_WORKAROUNDS_TITLE", "Workarounds"))),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement("div", { className: "LSFG_WorkaroundsCollapseButton_Container", style: { marginTop: "-2px", marginBottom: "4px" } },
                window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", bottomSeparator: collapsed ? "standard" : "none", onClick: toggleCollapsed }, collapsed ? window.SP_REACT.createElement(RiArrowDownSFill, null) : window.SP_REACT.createElement(RiArrowUpSFill, null)))),
        !collapsed && (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
            status === "loading" && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Field, { label: "Reading launch options..." }))),
            status === "error" && (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
                window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.Field, { label: "Launch options unavailable", description: error || "Steam did not provide readable launch options." })),
                window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: () => void refresh() }, "Retry")))),
            status === "ready" && snapshot && (!snapshot.wrapperOwned || !snapshot.integrationInstalled) && (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
                window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.Field, { label: "Wrapper needs to be reinstalled" })),
                onRepair && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", disabled: repairing, onClick: () => void handleRepair() }, repairing ? "Reinstalling..." : "Reinstall wrapper"))))),
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.SliderField, { label: `${t("CONFIG_BASE_FPS_CAP", "Base FPS Cap")} (${fpsLabel})`, description: t("CONFIG_BASE_FPS_CAP_DESC", "Base cap for DXVK-backed games before frame generation; 0 disables. Requires game restart to apply."), value: effectiveFpsValue, min: 0, max: 60, step: 1, onChange: (value) => {
                        setFpsValue(value);
                        void update("dxvkFrameRate", value);
                    }, disabled: controlsDisabled })),
            TOGGLE_ROWS.map((row) => (window.SP_REACT.createElement(DFL.PanelSectionRow, { key: row.field },
                window.SP_REACT.createElement(DFL.ToggleField, { label: t(row.labelKey, row.label), description: t(row.descriptionKey, row.description), checked: Boolean(state?.[row.field]), onChange: (value) => void update(row.field, value), disabled: controlsDisabled }))))))));
}

function GameConfigurationControls({ config, onConfigChange, autoFocusFpsMultiplier, onFpsMultiplierFocused, showWorkarounds = false, workaroundTarget, onRepairWorkaround, }) {
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement(FpsMultiplierControl, { config: config, onConfigChange: onConfigChange, autoFocus: autoFocusFpsMultiplier, onAutoFocus: onFpsMultiplierFocused }),
        window.SP_REACT.createElement(ConfigurationSection, { config: config, onConfigChange: onConfigChange }),
        showWorkarounds && workaroundTarget && (window.SP_REACT.createElement(WorkaroundsSection, { appId: workaroundTarget.appid, nonSteam: workaroundTarget.nonSteam, onRepair: onRepairWorkaround }))));
}

const collapsibleItemGroupStyles = `
  .LSFG_GameGroupCollapseButton_Container {
    margin-top: -2px;
    margin-bottom: 4px;
  }

  .LSFG_GameGroupCollapseButton_Container > div > div > div > button,
  .LSFG_GameGroupCollapseButton_Container > div > div > div > div > button {
    height: 24px !important;
    min-height: 24px !important;
    padding: 0 !important;
    display: flex !important;
    align-items: center !important;
    justify-content: center !important;
  }

  .LSFG_GameGroupCollapseButton_Container svg {
    display: block;
    margin: 0;
  }
`;
function usePersistentCollapsed(key) {
    const [collapsed, setCollapsed] = SP_REACT.useState(() => {
        try {
            return localStorage.getItem(key) !== "false";
        }
        catch {
            return true;
        }
    });
    SP_REACT.useEffect(() => {
        try {
            localStorage.setItem(key, String(collapsed));
        }
        catch { }
    }, [collapsed, key]);
    return [collapsed, () => setCollapsed((value) => !value)];
}
function CollapsibleItemGroup({ title, items, collapsed, onToggle, onSelect, toggleRef, }) {
    if (items.length === 0)
        return null;
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.Field, { label: `${title} (${items.length})`, bottomSeparator: "none" })),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement("div", { ref: toggleRef, className: "LSFG_GameGroupCollapseButton_Container" },
                window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", bottomSeparator: collapsed ? "standard" : "none", onClick: onToggle }, collapsed ? window.SP_REACT.createElement(RiArrowDownSFill, null) : window.SP_REACT.createElement(RiArrowUpSFill, null)))),
        !collapsed && items.map((item) => (window.SP_REACT.createElement(DFL.PanelSectionRow, { key: item.id },
            window.SP_REACT.createElement(DFL.Field, { label: item.label, description: item.description, disabled: item.disabled, onActivate: item.disabled ? undefined : () => onSelect(item.id), highlightOnFocus: true }))))));
}

const ENABLED_COLLAPSED_KEY = "lsfg-enabled-games-collapsed-v4";
const AVAILABLE_COLLAPSED_KEY = "lsfg-available-games-collapsed-v3";
function targetDescription$1(game) {
    if (game.isFlatpakShortcut)
        return "Non-Steam | Use Flatpak Tab";
    return game.source === "unknown"
        ? "Unknown source · excluded from bulk actions"
        : sourceLabel(game.source);
}
function GameConfigurationSelector({ targets, runningGame, source, bulkOperationBusy, onSelect, onEnableAll, onResetAll, focusConfiguredToggle = false, onConfiguredToggleFocused, }) {
    const sortGames = (games) => [...games].sort((a, b) => {
        if (a.appid === runningGame?.appid)
            return -1;
        if (b.appid === runningGame?.appid)
            return 1;
        return a.name.localeCompare(b.name);
    });
    const enabledGames = sortGames(targets.filter((game) => game.configured));
    const availableGames = sortGames(targets.filter((game) => !game.configured));
    const enableableGames = availableGames.filter((game) => game.source === source && !game.isFlatpakShortcut);
    const removableGames = enabledGames.filter((game) => game.source === source && !game.isFlatpakShortcut);
    const sourceName = source === "nonSteam" ? "non-Steam shortcuts" : "Steam games";
    const emptyDescription = source === "nonSteam"
        ? "Steam has not reported any eligible non-Steam shortcuts"
        : "Steam has not reported any eligible installed games";
    const toItem = (game) => ({
        id: game.appid,
        label: game.name,
        description: targetDescription$1(game),
        disabled: game.isFlatpakShortcut,
    });
    const [enabledCollapsed, toggleEnabled] = usePersistentCollapsed(`${ENABLED_COLLAPSED_KEY}-${source}`);
    const [availableCollapsed, toggleAvailable] = usePersistentCollapsed(`${AVAILABLE_COLLAPSED_KEY}-${source}`);
    const enabledToggleRef = SP_REACT.useRef(null);
    SP_REACT.useEffect(() => {
        if (!focusConfiguredToggle)
            return;
        const frame = requestAnimationFrame(() => {
            enabledToggleRef.current?.querySelector('[role="button"], button')?.focus();
            onConfiguredToggleFocused?.();
        });
        return () => cancelAnimationFrame(frame);
    }, [enabledGames.length, focusConfiguredToggle, onConfiguredToggleFocused]);
    const confirmResetAll = () => {
        DFL.showModal(window.SP_REACT.createElement(DFL.ConfirmModal, { strTitle: `Remove all ${sourceName} profiles?`, strOKButtonText: "Remove all", strCancelButtonText: "Cancel", onOK: () => void onResetAll(source), onCancel: () => { } }));
    };
    const confirmEnableAll = () => {
        DFL.showModal(window.SP_REACT.createElement(DFL.ConfirmModal, { strTitle: `Enable all available ${sourceName}?`, strDescription: `Create individual LSFG-VK profiles for every available ${sourceName}. Unknown-source profiles are excluded. Flatpak profiles are managed separately in the Flatpak tab.`, strOKButtonText: "Enable all", strCancelButtonText: "Cancel", onOK: () => void onEnableAll(source), onCancel: () => { } }));
    };
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement("style", null, collapsibleItemGroupStyles),
        targets.length === 0 && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.Field, { label: `No ${sourceName} found`, description: emptyDescription }))),
        window.SP_REACT.createElement(CollapsibleItemGroup, { title: "Enabled", items: enabledGames.map(toItem), collapsed: enabledCollapsed, onToggle: toggleEnabled, onSelect: onSelect, toggleRef: enabledToggleRef }),
        window.SP_REACT.createElement(CollapsibleItemGroup, { title: "Available", items: availableGames.map(toItem), collapsed: availableCollapsed, onToggle: toggleAvailable, onSelect: onSelect }),
        enableableGames.length > 0 && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: confirmEnableAll, disabled: bulkOperationBusy }, `Enable all ${sourceName}`))),
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: confirmResetAll, disabled: bulkOperationBusy || removableGames.length === 0 }, `Remove all ${sourceLabel(source)} profiles`))));
}

function ProfileDetails({ description }) {
    const [expanded, setExpanded] = SP_REACT.useState(false);
    const detailsRef = SP_REACT.useRef(null);
    SP_REACT.useEffect(() => {
        if (!expanded)
            return;
        const frame = requestAnimationFrame(() => detailsRef.current?.scrollIntoView({ block: "nearest" }));
        return () => cancelAnimationFrame(frame);
    }, [expanded]);
    return (window.SP_REACT.createElement(DFL.Focusable, { ref: detailsRef, noFocusRing: true },
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", bottomSeparator: expanded ? "none" : "standard", onClick: () => setExpanded((value) => !value) },
                expanded ? window.SP_REACT.createElement(RiArrowUpSFill, null) : window.SP_REACT.createElement(RiArrowDownSFill, null),
                " Game Details")),
        expanded && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.Field, { label: "Game Details", description: description })))));
}

function ConfigurationTab({ title, source, config, targets, runningGame, onSelect, onConfigChange, onEnable, onEnableAll, bulkOperationBusy, onRepair, onReset, onResetAll, }) {
    const [detailAppId, setDetailAppId] = SP_REACT.useState(null);
    const [focusFpsMultiplier, setFocusFpsMultiplier] = SP_REACT.useState(false);
    const [focusDetailAction, setFocusDetailAction] = SP_REACT.useState(null);
    const [focusConfiguredToggle, setFocusConfiguredToggle] = SP_REACT.useState(false);
    const enableRef = SP_REACT.useRef(null);
    const closeDetails = SP_REACT.useCallback(() => {
        setFocusFpsMultiplier(false);
        setFocusDetailAction(null);
        setDetailAppId(null);
    }, []);
    const clearFpsFocusRequest = SP_REACT.useCallback(() => setFocusFpsMultiplier(false), []);
    const clearConfiguredToggleFocusRequest = SP_REACT.useCallback(() => setFocusConfiguredToggle(false), []);
    SP_REACT.useEffect(() => {
        if (!focusDetailAction)
            return;
        if (focusDetailAction === "fps") {
            setFocusFpsMultiplier(true);
            setFocusDetailAction(null);
            return;
        }
        const frame = requestAnimationFrame(() => {
            enableRef.current?.querySelector('[role="button"]')?.focus();
            setFocusDetailAction(null);
        });
        return () => cancelAnimationFrame(frame);
    }, [focusDetailAction]);
    const selectedTarget = detailAppId ? targets.find((target) => target.appid === detailAppId) : null;
    if (detailAppId === null) {
        return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
            window.SP_REACT.createElement(DFL.PanelSection, { title: title },
                window.SP_REACT.createElement(GameConfigurationSelector, { targets: targets, runningGame: runningGame, source: source, bulkOperationBusy: bulkOperationBusy, onSelect: (appid) => {
                        setFocusConfiguredToggle(false);
                        setFocusDetailAction(targets.find((target) => target.appid === appid)?.configured ? "fps" : "enable");
                        onSelect(appid);
                        setDetailAppId(appid);
                    }, onEnableAll: onEnableAll, onResetAll: onResetAll, focusConfiguredToggle: focusConfiguredToggle, onConfiguredToggleFocused: clearConfiguredToggleFocusRequest }))));
    }
    const profileLabel = selectedTarget?.name || "Game profile";
    const profileTransport = selectedTarget ? sourceLabel(selectedTarget.source) : sourceLabel(source);
    const profileDescription = selectedTarget
        ? `${profileTransport} · App ID ${selectedTarget.appid} · ${selectedTarget.configured ? "LSFG-VK Enabled" : "LSFG-VK not enabled"}${selectedTarget.source === "unknown" ? " · Bulk actions exclude this profile" : ""}`
        : "Game is no longer available";
    const enableProfile = async (appid, quitRunningGame = false) => {
        if (!(await onEnable(appid)))
            return;
        if (quitRunningGame)
            SteamClient.Apps.TerminateApp(appid, false);
        setFocusFpsMultiplier(true);
    };
    const handleProfileAction = async () => {
        if (selectedTarget?.configured) {
            await onReset();
            setFocusConfiguredToggle(true);
            closeDetails();
        }
        else if (detailAppId) {
            const isRunningUnconfigured = runningGame?.appid === detailAppId
                && runningGame.source === "steam"
                && selectedTarget?.source === "steam"
                && !runningGame.configured;
            if (isRunningUnconfigured) {
                DFL.showModal(window.SP_REACT.createElement(DFL.ConfirmModal, { strTitle: "Game is running", strDescription: "Quit the game now so LSFG-VK is used on its next launch?", strOKButtonText: "Quit and enable", strCancelButtonText: "Enable without quitting", onOK: () => void enableProfile(detailAppId, true), onCancel: () => void enableProfile(detailAppId) }));
            }
            else {
                await enableProfile(detailAppId);
            }
        }
    };
    return (window.SP_REACT.createElement(DFL.Focusable, { onCancelButton: closeDetails },
        window.SP_REACT.createElement(DFL.PanelSection, null,
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement("div", { style: { display: "flex", alignItems: "center", width: "100%" } },
                    window.SP_REACT.createElement(DFL.Focusable, { noFocusRing: true, style: { flex: "none" } },
                        window.SP_REACT.createElement(DFL.DialogButton, { "aria-label": `Back to ${title}`, onClick: closeDetails, style: {
                                width: "48px",
                                minWidth: "48px",
                                height: "24px",
                                minHeight: "24px",
                                padding: 0,
                                display: "flex",
                                alignItems: "center",
                                justifyContent: "center",
                            } },
                            window.SP_REACT.createElement(FaArrowLeft, null))),
                    window.SP_REACT.createElement("div", { className: DFL.gamepadDialogClasses.FieldLabel, style: { flex: 1, minWidth: 0, marginLeft: "8px", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" } }, profileLabel)))),
        window.SP_REACT.createElement(DFL.PanelSection, null, !selectedTarget?.configured && selectedTarget && (window.SP_REACT.createElement(DFL.PanelSectionRow, null, selectedTarget.source === "unknown" ? (window.SP_REACT.createElement(DFL.Field, { label: "Target source unavailable", description: "Refresh Steam and try again before enabling this target." })) : (window.SP_REACT.createElement(DFL.Focusable, { ref: enableRef, noFocusRing: true },
            window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: handleProfileAction }, "Enable for next launch")))))),
        selectedTarget?.configured && (window.SP_REACT.createElement(GameConfigurationControls, { config: config, onConfigChange: (field, value) => onConfigChange(field, value, selectedTarget?.source !== "unknown"), autoFocusFpsMultiplier: focusFpsMultiplier, onFpsMultiplierFocused: clearFpsFocusRequest, showWorkarounds: selectedTarget.source !== "unknown", workaroundTarget: selectedTarget.source !== "unknown" ? selectedTarget : undefined, onRepairWorkaround: selectedTarget.source !== "unknown" ? () => onRepair(selectedTarget.appid) : undefined })),
        selectedTarget?.configured && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: handleProfileAction }, "Remove profile"))),
        window.SP_REACT.createElement(ProfileDetails, { description: profileDescription })));
}

function NowPlayingSummary({ title, details }) {
    return (window.SP_REACT.createElement(DFL.PanelSection, null,
        window.SP_REACT.createElement(DFL.PanelSectionRow, null,
            window.SP_REACT.createElement(DFL.Field, { label: title, description: details.filter(Boolean).join(" | ") }))));
}

function FlatpakNowPlayingTab({ app, launcher, onConfigChange }) {
    if (!app.config)
        return null;
    const changeConfig = async (field, value) => {
        await onConfigChange(app.app_id, { ...app.config, [field]: value });
    };
    return (window.SP_REACT.createElement(DFL.Focusable, null,
        window.SP_REACT.createElement(NowPlayingSummary, { title: launcher?.name || app.app_name, details: [
                launcher ? (launcher.source === "nonSteam" ? "Steam shortcut" : "Steam") : "Flatpak",
                launcher && launcher.name !== app.app_name ? `Running in ${app.app_name}` : null,
                launcher ? "Flatpak" : null,
                `Controls: ${app.app_name} profile`,
            ].filter((detail) => detail !== null) }),
        window.SP_REACT.createElement(FpsMultiplierControl, { config: app.config, onConfigChange: changeConfig }),
        window.SP_REACT.createElement(ConfigurationSection, { config: app.config, onConfigChange: changeConfig })));
}

function targetDescription(game) {
    return sourceLabel(game.source);
}
function NowPlayingTab({ game, config, onConfigChange, }) {
    return (window.SP_REACT.createElement(DFL.Focusable, null,
        window.SP_REACT.createElement(NowPlayingSummary, { title: game.name, details: [targetDescription(game), `Controls: ${game.name} profile`] }),
        window.SP_REACT.createElement(GameConfigurationControls, { config: config, onConfigChange: onConfigChange, showWorkarounds: false })));
}

var branchSetupGif = 'http://127.0.0.1:1337/plugins/Decky LSFG-VK/assets/lsfg-vk-branch-setup-dd608696.gif';

function showBranchSetupModal() {
    let closeModal = () => { };
    const modal = DFL.showModal(window.SP_REACT.createElement(DFL.ModalRoot, { bAllowFullSize: true, closeModal: () => closeModal(), onCancel: () => closeModal() },
        window.SP_REACT.createElement("div", { style: {
                width: "100%",
                maxWidth: "900px",
                boxSizing: "border-box",
                maxHeight: "calc(100vh - 120px)",
                overflowY: "auto",
                padding: "8px 16px 16px",
                margin: "0 auto",
            } },
            window.SP_REACT.createElement("div", { style: { fontSize: "20px", fontWeight: 600, marginBottom: "8px" } }, "Switch Lossless Scaling to the lsfg-vk branch"),
            window.SP_REACT.createElement("img", { src: branchSetupGif, alt: "Steam steps for selecting the lsfg-vk branch", style: {
                    display: "block",
                    width: "100%",
                    maxWidth: "100%",
                    height: "auto",
                    boxSizing: "border-box",
                    borderRadius: "4px",
                    background: "#101418",
                } }),
            window.SP_REACT.createElement("ol", { style: { lineHeight: 1.5, margin: "14px 0 18px", paddingLeft: "24px" } },
                window.SP_REACT.createElement("li", null, "Open Lossless Scaling in your Steam library."),
                window.SP_REACT.createElement("li", null, "Open Properties, then Game Versions & Betas."),
                window.SP_REACT.createElement("li", null,
                    "Select the ",
                    window.SP_REACT.createElement("strong", null, "lsfg-vk"),
                    " branch."),
                window.SP_REACT.createElement("li", null, "Wait for Steam to finish the update and reopen Decky LSFG-VK.")),
            window.SP_REACT.createElement(DFL.DialogButton, { onClick: () => closeModal(), style: { width: "100%" } }, "Close"))), undefined, {
        strTitle: "Finish LSFG-VK setup",
        bNeverPopOut: true,
        popupWidth: 980,
        popupHeight: 760,
    });
    closeModal = modal.Close;
}

function SettingsTab(props) {
    const { isInstalled, setupComplete, installationStatus, losslessScalingInstalled, losslessScalingStatus, steamBranchStatus, isInstalling, isUninstalling, showDebugTab, onShowDebugTabChange, onInstall, onUninstall, } = props;
    const setupIncomplete = isInstalled && !setupComplete;
    const branchSetupIncomplete = Boolean(setupIncomplete && steamBranchStatus?.installed && steamBranchStatus.needs_switch);
    const selectedBranch = steamBranchStatus?.selected_branch || "the current";
    const targetBranch = steamBranchStatus?.target_branch || "lsfg-vk";
    const buttonLabel = isInstalling
        ? t("INSTALL_INSTALLING", "Installing...")
        : isUninstalling
            ? t("INSTALL_UNINSTALLING", "Uninstalling...")
            : isInstalled
                ? t("INSTALL_UNINSTALL_BTN", "Uninstall LSFG-VK")
                : t("INSTALL_INSTALL_BTN", "Install LSFG-VK");
    return (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
        window.SP_REACT.createElement(DFL.PanelSection, { title: "Settings" },
            setupIncomplete && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement("div", { role: "alert", style: {
                        width: "100%",
                        boxSizing: "border-box",
                        padding: "12px",
                        border: "1px solid #e55353",
                        borderRadius: "4px",
                        background: "rgba(128, 24, 24, 0.32)",
                        color: "#ffd7d7",
                    } },
                    window.SP_REACT.createElement("div", { style: { fontWeight: 600 } }, "Setup incomplete"),
                    window.SP_REACT.createElement("div", { style: { marginTop: "6px", lineHeight: 1.35 } }, branchSetupIncomplete ? (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
                        "Lossless Scaling is on ",
                        window.SP_REACT.createElement("strong", null, selectedBranch),
                        ". Select the ",
                        window.SP_REACT.createElement("strong", null, targetBranch),
                        " branch before launching games.")) : (window.SP_REACT.createElement(window.SP_REACT.Fragment, null, "Lossless Scaling is installed, but its LSFG-VK runtime is not ready yet."))),
                    branchSetupIncomplete && (window.SP_REACT.createElement(DFL.DialogButton, { onClick: showBranchSetupModal, style: { width: "100%", marginTop: "10px" } }, "Learn more"))))),
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Field, { label: "Lossless Scaling", description: losslessScalingInstalled ? "Installed" : losslessScalingStatus || "Not installed" })),
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Field, { label: "LSFG-VK", description: installationStatus })),
            steamBranchStatus?.installed && !setupIncomplete && (window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.Field, { label: "Steam branch", description: `${steamBranchStatus.selected_branch || "public"}${steamBranchStatus.needs_switch ? ` - ${steamBranchStatus.message}` : ""}` }))),
            window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                window.SP_REACT.createElement(DFL.ButtonItem, { layout: "below", onClick: isInstalled ? onUninstall : onInstall, disabled: isInstalling || isUninstalling }, buttonLabel))),
        isInstalled && (window.SP_REACT.createElement(window.SP_REACT.Fragment, null,
            window.SP_REACT.createElement(DFL.PanelSection, { title: "Global settings" },
                window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.Field, { label: "FP16 Acceleration", description: "Off on PB-OS: slower and stutters on Adreno" }))),
            window.SP_REACT.createElement(DFL.PanelSection, { title: "Advanced" },
                window.SP_REACT.createElement(DFL.PanelSectionRow, null,
                    window.SP_REACT.createElement(DFL.ToggleField, { label: "Show config file tab", checked: showDebugTab, onChange: onShowDebugTabChange })))))));
}

const tabIcons = {
    nowPlaying: window.SP_REACT.createElement(FaGamepad, { size: 18 }),
    steam: window.SP_REACT.createElement(FaSteam, { size: 18 }),
    nonSteam: window.SP_REACT.createElement(FaExternalLinkAlt, { size: 18 }),
    flatpak: window.SP_REACT.createElement(FaCube, { size: 18 }),
    configFile: window.SP_REACT.createElement(FaFileAlt, { size: 18 }),
    settings: window.SP_REACT.createElement(FaTools, { size: 18 }),
};
const DEBUG_TAB_VISIBILITY_KEY = "lsfg-debug-tab-visible-v1";
function tabForNowPlaying(target) {
    if (!target)
        return "Steam";
    if (target.kind === "flatpak")
        return target.launcher?.source === "nonSteam" ? "NonSteam" : "Flatpak";
    return target.game.source === "nonSteam" ? "NonSteam" : "Steam";
}
function usePersistentBoolean(key, defaultValue) {
    const [value, setValue] = SP_REACT.useState(() => {
        try {
            const stored = localStorage.getItem(key);
            return stored === null ? defaultValue : stored === "true";
        }
        catch {
            return defaultValue;
        }
    });
    SP_REACT.useEffect(() => {
        try {
            localStorage.setItem(key, String(value));
        }
        catch { }
    }, [key, value]);
    return [value, setValue];
}
function Content() {
    const { config, runningConfig, globalConfig, targets, runningGame, setSelectedAppId, save, saveFor, updateGlobal, enable, enableAll, bulkOperationBusy, repair, resetSelected, resetAll, cleanupAllWorkarounds, reload, } = useGameConfiguration();
    const { isInstalled, setupComplete, installationStatus, losslessScalingInstalled, losslessScalingStatus, steamBranchStatus, isInstalling, isUninstalling, install, uninstall, } = useInstallation(reload, cleanupAllWorkarounds);
    const flatpak = useFlatpakConfiguration(setupComplete);
    const [tab, setTab] = SP_REACT.useState("Settings");
    const [showDebugTab, setShowDebugTab] = usePersistentBoolean(DEBUG_TAB_VISIBILITY_KEY, false);
    const [contentFocused, setContentFocused] = SP_REACT.useState(false);
    const previousRunningWorkload = SP_REACT.useRef(null);
    const previousNowPlayingTab = SP_REACT.useRef("Steam");
    const runningFlatpak = flatpak.runningApp;
    const nowPlayingTarget = resolveNowPlayingTarget(runningGame, runningFlatpak);
    const hasNowPlaying = Boolean(nowPlayingTarget);
    const runningWorkload = nowPlayingTarget
        ? nowPlayingTarget.kind === "flatpak"
            ? `flatpak:${nowPlayingTarget.app.app_id}:${nowPlayingTarget.launcher?.appid ?? ""}`
            : `${nowPlayingTarget.game.source}:${nowPlayingTarget.game.appid}`
        : null;
    const steamTargets = targetsForSource(targets, "steam");
    const nonSteamTargets = targetsForSource(targets, "nonSteam");
    SP_REACT.useEffect(() => {
        if (!setupComplete) {
            setTab("Settings");
            return;
        }
        setTab((current) => current === "Settings" ? (hasNowPlaying ? "NowPlaying" : "Steam") : current);
    }, [hasNowPlaying, setupComplete]);
    SP_REACT.useEffect(() => {
        if (!setupComplete)
            return;
        const previous = previousRunningWorkload.current;
        previousRunningWorkload.current = runningWorkload;
        if (runningWorkload && runningWorkload !== previous) {
            previousNowPlayingTab.current = tabForNowPlaying(nowPlayingTarget);
            setTab("NowPlaying");
        }
        else if (!runningWorkload && previous) {
            setTab((current) => current === "NowPlaying" ? previousNowPlayingTab.current : current);
        }
    }, [runningWorkload, setupComplete]);
    SP_REACT.useEffect(() => {
        if (isInstalled) {
            void reload();
            void flatpak.reload();
        }
    }, [isInstalled, reload, flatpak.reload]);
    SP_REACT.useEffect(() => {
        if (!showDebugTab && tab === "ConfigFile")
            setTab(setupComplete ? "Steam" : "Settings");
    }, [setupComplete, showDebugTab, tab]);
    const handleConfigChange = async (fieldName, value, cleanupLaunchOptions = false) => {
        await save({ ...config, [fieldName]: value }, cleanupLaunchOptions);
    };
    const settings = (window.SP_REACT.createElement(SettingsTab, { isInstalled: isInstalled, setupComplete: setupComplete, installationStatus: installationStatus, losslessScalingInstalled: losslessScalingInstalled, losslessScalingStatus: losslessScalingStatus, steamBranchStatus: steamBranchStatus, isInstalling: isInstalling, isUninstalling: isUninstalling, globalConfig: globalConfig, showDebugTab: showDebugTab, onGlobalConfigChange: updateGlobal, onShowDebugTabChange: setShowDebugTab, onInstall: () => void install(), onUninstall: () => void uninstall() }));
    const tabContent = (content) => (window.SP_REACT.createElement("div", { className: "lsfg-vk-tab-content" }, content));
    const nowPlaying = nowPlayingTarget?.kind === "flatpak" ? (window.SP_REACT.createElement(FlatpakNowPlayingTab, { app: nowPlayingTarget.app, launcher: nowPlayingTarget.launcher, onConfigChange: flatpak.updateConfig })) : nowPlayingTarget ? (window.SP_REACT.createElement(NowPlayingTab, { game: nowPlayingTarget.game, config: runningConfig, onConfigChange: async (field, value) => {
            await saveFor(nowPlayingTarget.game.appid, { ...runningConfig, [field]: value }, true);
        } })) : null;
    const tabs = setupComplete
        ? [
            ...(nowPlaying ? [{ id: "NowPlaying", title: tabIcons.nowPlaying, content: tabContent(nowPlaying) }] : []),
            {
                id: "Steam",
                title: tabIcons.steam,
                content: tabContent(window.SP_REACT.createElement(ConfigurationTab, { title: "Steam games", source: "steam", config: config, targets: steamTargets, runningGame: runningGame, onSelect: setSelectedAppId, onConfigChange: handleConfigChange, onEnable: enable, onEnableAll: enableAll, bulkOperationBusy: bulkOperationBusy, onRepair: repair, onReset: resetSelected, onResetAll: resetAll })),
            },
            {
                id: "NonSteam",
                title: tabIcons.nonSteam,
                content: tabContent(window.SP_REACT.createElement(ConfigurationTab, { title: "Non-Steam games", source: "nonSteam", config: config, targets: nonSteamTargets, runningGame: runningGame, onSelect: setSelectedAppId, onConfigChange: handleConfigChange, onEnable: enable, onEnableAll: enableAll, bulkOperationBusy: bulkOperationBusy, onRepair: repair, onReset: resetSelected, onResetAll: resetAll })),
            },
            // PB-OS: no Flatpak tab; upstream's Flatpak extensions are x86-only
            ...(showDebugTab ? [{ id: "ConfigFile", title: tabIcons.configFile, content: tabContent(window.SP_REACT.createElement(ConfigFileTab, null)) }] : []),
            { id: "Settings", title: tabIcons.settings, content: tabContent(settings) },
        ]
        : [
            ...(isInstalled && showDebugTab
                ? [{ id: "ConfigFile", title: tabIcons.configFile, content: tabContent(window.SP_REACT.createElement(ConfigFileTab, null)) }]
                : []),
            { id: "Settings", title: tabIcons.settings, content: tabContent(settings) },
        ];
    const availableTabIds = new Set(tabs.map(({ id }) => id));
    const activeTab = availableTabIds.has(tab) ? tab : setupComplete ? "Steam" : "Settings";
    const handleFocusCapture = (event) => {
        const focusedElement = event.target;
        setContentFocused(!focusedElement?.closest?.('[role="tab"]'));
    };
    return (window.SP_REACT.createElement("div", { className: `lsfg-vk-tabs${contentFocused ? " lsfg-vk-tabs--content-focused" : ""}`, style: { height: "95%", width: "300px", position: "fixed", marginTop: "-12px", overflow: "hidden" }, onFocusCapture: handleFocusCapture },
        window.SP_REACT.createElement("style", null, tabStyles),
        window.SP_REACT.createElement(DFL.Tabs, { activeTab: activeTab, onShowTab: (nextTab) => {
                if (availableTabIds.has(nextTab))
                    setTab(nextTab);
            }, tabs: tabs })));
}

var index = definePlugin(() => {
    console.log("decky-lsfg-vk plugin initializing");
    return {
        name: "Decky LSFG-VK",
        titleView: window.SP_REACT.createElement("div", { className: DFL.staticClasses.Title }, "Decky LSFG-VK"),
        alwaysRender: true,
        content: window.SP_REACT.createElement(Content, null),
        icon: window.SP_REACT.createElement(GiPlasticDuck, null),
        onDismount() {
            console.log("decky-lsfg-vk unloading");
        }
    };
});

export { index as default };
//# sourceMappingURL=index.js.map
