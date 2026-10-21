# Steam internals the PB-OS tab relies on

The PB-OS tab in Quick Access is not a Steam feature. Like Decky Loader,
pbos-qamd puts it into Steam's own frontend from outside, and the frontend
code then finds Steam's React components and stores by searching Steam's
JavaScript. None of this is a public API, so any Steam client update can
break it. Steam on PB-OS updates itself in the client, so a break can show
up between our releases too.

**Check it before every release, and after any Steam client update you
hear about.** It takes one command on a device running Game Mode.

## The check (every release)

1. Boot the release candidate image on a test device, or in the Thor VM
   (test disk), and let Steam reach Game Mode.
2. Open Quick Access once, so the tab has been drawn.
3. Run, as root:

   ```
   sudo pbos-qamd --check
   ```

   It prints one line per item, `PASS` or `FAIL`, plus the Steam build. It
   exits with 0 when everything needed passed, 1 when something failed, and
   2 when it could not reach Steam's page. `--check --json` prints the same
   as JSON, for the release notes or a script.

   The probe runs inside the PB-OS tab that pbos-qamd already loaded, so
   pbos-qamd must be running and a user signed in to Steam (`collectionStore`
   fails while signed out). It loads nothing into Steam. Loading a second
   copy of @decky/ui into Steam's page made Steam quit (Thor VM,
   2026-10-16), so never add a check that does.
4. Also look at the device (the Thor VM's probe does the same headlessly
   with `thorvm.qam`: it opens every plugin page through
   `window.__pbosQam.open("section/id")`, reads its text, and sends a test
   toast with `window.__pbosQam.toast({...})`):
   - Quick Access shows Barry's head, and the PB-OS list opens.
   - Each plugin page opens, and B goes back to the list.
   - A toast shows. Changing the profile on the Pocket FIT or Pocket S2
     does it, so does `pbos-qamd --call tools check` while an update is
     available, or any update toast.
   - On the Thor, tapping a text field opens Barry's keyboard on the
     bottom screen.
5. Record the Steam build and the result in the release notes (the long
   notes, not the release body).

Don't release with a `FAIL` in a required item. "Quick Access embedded
view" and "running tab: drawn" are informational.

## What is used, and where

| Item in the check | What it is | Used by | Code |
|---|---|---|---|
| Steam initialized | `App.BFinishedInitStageOne()` / `BFinishedInitBeforeLogin()` | waiting for Steam before anything else | `frontend/src/loader.js` `steamReady` |
| webpack chunks | `window.webpackChunksteamui` | @decky/ui's module search (everything below) | @decky/ui `webpack.ts` |
| React, JSX runtime | Steam's React, ReactDOM and JSX modules, found by shape | rendering; set as `SP_REACT`/`SP_JSX` like Decky does | `reactGlobals` |
| Quick Access module | an export whose `type` mentions `QuickAccessMenuBrowserView` (and `QuickAccessMenuEmbedded`) | adding the PB-OS tab | `hookTabs` |
| (not in the check, needs a render) | the node with `props.onFocusNavDeactivated`, then the one with `props.tabs` | the tab list the PB-OS tab is pushed into | `hookTabs` |
| toast renderer | an export containing `controller:"notification",method:` | our own toasts when Decky is not installed | `makeToaster` |
| toast classes | the class module with `ShortTemplate` | toast look | `makeToaster` |
| NotificationStore | `ProcessNotification`, `RemoveGroupFromTray`, `m_nNextTestNotificationID` | sending toasts | `makeToaster` |
| `DFL.*` components | PanelSection, ButtonItem, ToggleField, SliderField, DropdownItem, TextField, ConfirmModal, ModalRoot, showModal, Focusable, DialogButton, … | every page | sections |
| `staticClasses.Title` | Quick Access's title style | the PB-OS header | `makePanel` |
| Router.RunningApps | running games | Barry's companion apps, per-game performance | `sections/barry.js`, `sections/tools.js` |
| `SteamClient.Apps.*`, `SteamClient.Settings.SpecifyGlobalCompatTool` | app details, overview changes, game start, launch options, compat tools; `SetShortcutExe`, `RemoveShortcut` | Proton PB-OS, game profiles; Android titles (moved to pbos-apk, stale ones removed) | `sections/tools.js` |
| `SteamClient.Apps.AddShortcut`, `RemoveShortcut`, `SetShortcutName`, `SetShortcutLaunchOptions`, `RunGame` | non-Steam shortcuts: made (returns the new app id), removed, started by game id `(appid << 32) \| 0x02000000` | the App Store's library entries, Add a Non-Steam Game | `sections/store.js` |
| `SteamClient.GameSessions.RegisterForAppLifetimeNotifications` | a game starting or ending (`unAppID`, `bRunning`) | the Performance tab's per-game mode | `sections/tools.js` |
| settingsStore, collectionStore | `m_Settings.strCompatTool`, local and all games | Proton PB-OS, game lists | `sections/tools.js`, `sections/barry.js` |
| virtual keyboard manager | `SteamUIStore.ActiveWindowInstance.VirtualKeyboardManager` with `SetVirtualKeyboardShownInternal` | Barry's keyboard instead of Steam's (Thor) | `sections/barry.js` |
| SteamUIStore.WindowStore | `SteamUIWindows` | Barry's trackpad clicks in text fields (Thor) | `sections/barry.js` |
| (not in the check) focused game | `focusin` in Steam's windows; the focused element's React fiber (`__reactFiber$…`) and its owners, for props `app`/`overview`/`appOverview` (an app overview with `appid` and `display_name`) or a numeric `appid`; else the route `/library/app/<id>` | the bottom screen's hero art (Thor) | `sections/barry.js` `focusedApp` |
| (not in the check) app overviews | `appStore.GetAppOverviewByAppID`, `collectionStore.allAppsCollection.allApps` (`app_type`, `rt_last_time_played`), `Router.MainRunningApp` | the hero art's game, and the quick settings' game profile (Thor); whether an App Store shortcut is still in the library (`GetAppOverviewByAppID`) | `sections/barry.js`, `sections/store.js` |
| running tab: … | what the loaded tab reports (patched, drawn, sections, errors) | spotting a tab that loaded but broke | `status` in `loader.js` |

Outside the frontend:

- **The DevTools port.** Steam has to run with `-cef-enable-debugging`
  (`steamos-overlay/usr/share/deckard/RUNSTEAM.sh`), which opens
  127.0.0.1:8080. pbos-qamd finds the page whose URL has
  `steamloopback.host/routes/` or `/index.html` and whose title is
  `SharedJSContext` (or the older titles Decky also accepts:
  `cdp.cpp kSharedTitles`).
- **`Runtime.addBinding` / `Runtime.bindingCalled`**: the frontend calls
  pbos-qamd through this. pbos-qamd first tries without turning on the
  Runtime domain, and turns it on if the binding stays silent (it logs
  "the binding needs the Runtime domain here"). Both work; the first keeps
  Steam's console messages off the socket.
- **`Page.domContentEventFired`**: Steam's page reloading puts the tab back
  in.
- **Steam starting**: pbos-qamd waits for a write to
  `~/.local/share/Steam/logs/*webhelper*` (or `cef*`), or for
  `~/.steam/steam.pid`. If Steam stops writing those, the tab only appears
  when pbos-qamd restarts. The journal then shows no "connected to Steam's
  …" line after Steam starts.

## When something fails

1. `journalctl -b -u pbos-qamd` shows the frontend's errors as
   `frontend: …` lines (where it failed and the stack), and the connection
   steps.
2. Most failures are a finder that no longer matches. Decky Loader runs
   into the same changes and fixes them first, in @decky/ui
   (SteamDeckHomebrew/decky-frontend-lib) and in decky-loader's
   `frontend/src/tabs-hook.tsx` and `toaster.tsx`. Our `hookTabs` and
   `makeToaster` follow those files.
   - If @decky/ui has a fix, raise the pin in `frontend/package.json`, run
     `frontend/build.sh` on the build Mac, and commit `frontend/dist/pbos-qam.js`
     and `package-lock.json`.
   - If decky-loader changed its tab hook or toaster, make the same change
     in `loader.js`.
3. A section's own Steam use (`SteamClient.*`, the stores, the keyboard
   manager) is in `sections/*.js`. They come from the Decky plugins
   unchanged, so they break when the Decky plugins would have.
4. Run `pbos-qamd --check` again, then repeat the device checks above.

## With Decky installed

The images ship Decky Loader for decky-lsfg-vk and the user's own plugins.
Decky and the PB-OS tab both patch Quick Access's tab list. Each adds only
its own tab and recognizes it again by its own mark (`decky` / `pbos`).
Toasts are different: two patches on Steam's toast renderer would fight.
So with Decky installed (pbos-qamd checks `plugin_loader.service`), PB-OS
toasts go through Decky's toaster, and the PB-OS tab only uses its own
toaster when Decky is not installed. The check's "toast renderer" item
matters for that case.

@decky/ui is Decky's own (`window.DFL`) too: with Decky installed the tab
waits for it (up to 90 s) and never starts its bundled copy. Our copy
started in a page where Decky's already ran (its webpack module cache was
enough) made Steam's UI die about a second later (React error #31, then
Steam quit; black top screen on an AYN Thor on 0.5.4.4, 2026-10-16). The VM
missed it because there the tab happened to load before Decky. The check's
"@decky/ui" item says which copy is in use; with Decky installed it must
say Decky's. Before a release, test on a device where Decky has loaded
first: restart Steam, wait until `window.DeckyPluginLoader` exists, then
start pbos-qamd.
