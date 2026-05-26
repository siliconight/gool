# addons/gool/editor/update_checker.gd
#
# v0.79.2: Editor-side update notification helper.
#
# Queries the GitHub releases API for the latest stable gool release
# and compares against the running version. Emits a signal with the
# result so the update banner can render.
#
# DESIGN
# ======
#   - Editor-only. The runtime singleton (Gool autoload) never calls
#     this. Shipped games have zero update-check overhead.
#   - 24h cache in user://gool_update_check.json. We're not hammering
#     GitHub on every editor restart.
#   - Opt-out via project setting `audio/gool/check_for_updates`
#     (default true). Setting is registered in plugin.gd's _enter_tree
#     so it appears in Project Settings → audio → gool.
#   - Silently no-ops on any network failure. The check is a nicety,
#     not a requirement.
#   - GitHub's /releases/latest endpoint already filters out
#     prereleases and drafts (per their REST API docs), so we get
#     stable-only releases by default. Belt-and-suspenders: also
#     skip tags with hyphens (`v0.80.0-rc1`-style).
#
# RATE LIMITS
# ===========
#   GitHub allows 60 unauthenticated requests per hour per IP. The
#   24h cache means at most one request per editor session per
#   developer — well under the limit even on shared CI runners.
#
# THREADING
# =========
#   HTTPRequest is asynchronous. The signal fires on the main thread
#   via request_completed. No locking required from callers.

@tool
extends RefCounted

const CACHE_PATH := "user://gool_update_check.json"
const CACHE_TTL_SECONDS := 86400  # 24h
const GITHUB_API := "https://api.github.com/repos/siliconight/gool/releases/latest"
const SETTING_PATH := "audio/gool/check_for_updates"

# Emitted once per check() call (whether served from cache, fresh
# network, or no-op due to opt-out / network failure where we still
# want to signal completion for the caller's bookkeeping). Always
# arrives on the main thread.
#
# latest_version: e.g. "0.79.3". Empty string if check failed.
# is_newer:       true if latest is strictly newer than the version
#                 passed to check().
signal update_check_complete(latest_version: String, is_newer: bool)

var _http: HTTPRequest = null
var _current_version: String = ""


# Trigger a check.
#   current_version: e.g. "0.79.2" — the version we're running now.
#   scene_root: any Node in the scene tree where the temporary
#               HTTPRequest can be parented (it self-frees on
#               completion). Caller typically passes get_tree().root
#               from the mixer dock.
func check(current_version: String, scene_root: Node) -> void:
	_current_version = current_version

	# Opt-out: user has unticked the project setting.
	if not bool(ProjectSettings.get_setting(SETTING_PATH, true)):
		return

	# Cache hit? Serve stored result if it's fresh.
	var cached := _load_cache()
	var now := int(Time.get_unix_time_from_system())
	if cached.has("latest") and \
			(now - int(cached.get("checked_at", 0))) < CACHE_TTL_SECONDS:
		var cached_latest: String = str(cached["latest"])
		if cached_latest != "":
			emit_signal("update_check_complete", cached_latest,
				_is_newer(cached_latest, current_version))
		return

	# Fresh network check.
	if scene_root == null or not is_instance_valid(scene_root):
		return  # Can't make an HTTPRequest without a parent node.
	_http = HTTPRequest.new()
	_http.timeout = 5.0
	_http.request_completed.connect(_on_response)
	scene_root.add_child(_http)
	var headers := PackedStringArray([
		"User-Agent: gool-editor-plugin",
		"Accept: application/vnd.github+json",
	])
	var err := _http.request(GITHUB_API, headers)
	if err != OK:
		_cleanup()


func _on_response(result: int, response_code: int,
				   _headers: PackedStringArray,
				   body: PackedByteArray) -> void:
	_cleanup()
	if result != HTTPRequest.RESULT_SUCCESS or response_code != 200:
		return
	var json := JSON.new()
	if json.parse(body.get_string_from_utf8()) != OK:
		return
	var data: Variant = json.data
	if not (data is Dictionary):
		return
	var tag: String = str(data.get("tag_name", ""))
	# GitHub's /releases/latest excludes pre-releases per their docs,
	# but defense-in-depth: also skip tags with hyphens.
	if tag == "" or "-" in tag:
		return
	# Tags ship as "v0.79.2"; strip the v for comparison.
	var latest := tag.lstrip("v")
	_save_cache(latest)
	emit_signal("update_check_complete", latest,
		_is_newer(latest, _current_version))


# Element-wise numeric comparison: "0.79.10" > "0.79.9" > "0.79.2".
# Splits on dots, parses each segment as int. Returns true if `latest`
# is strictly newer than `current`. Equal-length sequences where all
# segments match return false (same version, not "newer").
func _is_newer(latest: String, current: String) -> bool:
	var l_parts := latest.split(".")
	var c_parts := current.split(".")
	var n: int = mini(l_parts.size(), c_parts.size())
	for i in range(n):
		var l_n := int(l_parts[i])
		var c_n := int(c_parts[i])
		if l_n > c_n:
			return true
		if l_n < c_n:
			return false
	# All compared segments equal; the longer sequence is newer
	# (e.g. "0.79.2" > "0.79").
	return l_parts.size() > c_parts.size()


func _load_cache() -> Dictionary:
	if not FileAccess.file_exists(CACHE_PATH):
		return {}
	var f := FileAccess.open(CACHE_PATH, FileAccess.READ)
	if f == null:
		return {}
	var content := f.get_as_text()
	f.close()
	var json := JSON.new()
	if json.parse(content) != OK:
		return {}
	var data: Variant = json.data
	return data if (data is Dictionary) else {}


func _save_cache(latest: String) -> void:
	var f := FileAccess.open(CACHE_PATH, FileAccess.WRITE)
	if f == null:
		return
	var payload := {
		"latest": latest,
		"checked_at": int(Time.get_unix_time_from_system()),
	}
	f.store_string(JSON.stringify(payload))
	f.close()


func _cleanup() -> void:
	if _http != null and is_instance_valid(_http):
		_http.queue_free()
	_http = null
