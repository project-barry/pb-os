# shelld event stream (protocol shared by both helpers — do not change without telling the lead)

Socket: AF_UNIX stream at $XDG_RUNTIME_DIR/barry_launcher/shelld.sock (dir 0700, socket 0600, owner = session user). shelld creates it at start (unlink stale first). Many clients allowed.

Server -> client: newline-delimited JSON, UTF-8, one object per line:
  {"topic": "<topic>", "data": <payload>}
On connect: one line per topic with the current payload (full snapshot), in this order: home, keyboard, top-keyboard, keyd, apps, tools, updates.
After that: a line for a topic ONLY when its serialized payload differs from the last one sent on that connection (dedupe per topic).

Topics and payloads (payload = byte-for-byte the same JSON object the existing HTTP GET returns, so QML parsing code can be shared):
  home         = GET /home
  keyboard     = GET /keyboard (as answered; curtain included)
  top-keyboard = GET /top-keyboard
  keyd         = GET /keyd
  apps         = GET /apps
                 each app's entry also has "frozen": bool, its last field (added after the first six
                 topics): the app waits frozen in the background (cgroup freezer; shelld thaws it as its
                 tile brings it back). Existing fields unchanged; clients that do not know it ignore it.
  tools        = GET /tools  (added after the first five; clients ignore topics they do not know)
                 {"serial": <int>, "leaving": {"<app>": <pid>, ...}}
                 leaving: the tool windows (trackpad, deck) shelld wants gone because Barry opens / goes
                 home, by process id; serial counts those asks. Each pid listed slides its window away
                 (~240 ms), sends {"tool_left": true} and stays up until shelld ends it (SIGTERM) as
                 Barry's window comes forward. shelld waits for every pid listed (tool_left, or its exit)
                 at most 400 ms, then ends them as before. Cleared to {} once done.
  updates      = GET /updates  (added after tools; clients ignore topics they do not know)
                 {"serial": <int>, "phase": "idle"|"offer"|"updating"|"done",
                  "apps": [{"id", "name", "installed", "version", "status": ""|"updating"|"updated"|"failed", "error"}]}
                 New versions of Project Barry's apps the user has (barry_updates), offered once after each PB-OS
                 update; serial counts the offers. The shell (AppUpdates.qml) answers with POST /updates
                 {"do": "update"|"later"|"close"}.

Client -> server: newline-delimited JSON objects, optional:
  {"keyboard_height": <int>}   same effect as GET /keyboard?h=<int> (apply_inset when it changes)
  {"tool": true}               this connection's process (SO_PEERCRED pid) is a tool window that slides
                               away when asked over "tools"; only those are asked and waited for (any other
                               tool, e.g. an older one, is ended at once as before). Sent again after a
                               reconnect.
  {"tool_left": true}          the slide has played (from a "tool" connection whose pid was in leaving)

Latency: a state change must reach clients within ~1 frame (<=16 ms) of shelld noticing it. No periodic resend. The existing HTTP API (port 47824, all GET/POST paths) stays unchanged and working.
