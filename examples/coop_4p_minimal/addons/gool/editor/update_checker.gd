# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

# addons/gool/editor/update_checker.gd
#
# v0.79.2: Editor-side update notification helper.
# v0.80.23: full diagnostic rewrite (#19) — every return path now
#           logs to console, writes the cache, and emits the signal.
#           Pre-v0.80.23 there were 6 silent-return paths and the
#           cache was written ONLY on the full happy path, making
#           the feature invisible to any user whose check failed
#           (which, per field reports, was effectively everyone).
#
# Queries the GitHub releases API for the latest stable gool release
# and compares against the running version. Emits a signal with the
# result so the update banner can render.
#
# DESIGN
# ======
#   - Editor-only. The runtime singleton (Gool autoload) never calls
#     this. Shipped games have zero update-check overhead.
#   - 24h cache for successful results; 5min retry window for failed
#     attempts. See _is_cache_fresh.
#   - Opt-out via project setting `audio/gool/check_for_updates`
#     (default true). Setting is registered in plugin.gd's _enter_tree
#     so it appears in Project Settings → audio → gool.
#   - NO silent failures: every return path logs and writes a cache
#     entry with `last_attempt_status` so users (and bug reports) have
#     a known on-disk artifact to inspect.
#   - GitHub's /releases/latest endpoint already filters out
#     prereleases and drafts (per their REST API docs), so we get
#     stable-only releases by default. Belt-and-suspenders: also
#     skip tags with hyphens (`v0.80.0-rc1`-style).
#
# CACHE SCHEMA (v0.80.23)
# =======================
# {
#   "latest":               "0.80.x" or "",
#   "checked_at":           <unix ts of last attempt>,
#   "last_attempt_status":  "ok" | "cache_hit" | "opt_out" |
#                           "no_scene_root" | "request_failed" |
#                           "result_<n>" | "http_<code>" |
#                           "parse_failed" | "tag_invalid",
#   "last_attempt_message": "human-readable description"
# }
#
# Backward compatibility: old caches (pre-v0.80.23) lack the status
# fields. `_load_cache` provides defaults so they still work — they
# read as `last_attempt_status: "ok"` and serve as a normal success
# cache until they age out.
#
# RATE LIMITS
# ===========
#   GitHub allows 60 unauthenticated requests per hour per IP. The
#   24h success cache + 5min failure retry window means at most ~12
#   requests per hour even when the API is consistently down.
#
# THREADING
# =========
#   HTTPRequest is asynchronous. The signal fires on the main thread
#   via request_completed. No locking required from callers.

@tool
extends RefCounted

const CACHE_PATH := "user://gool_update_check.json"
const CACHE_TTL_SECONDS := 86400           # 24h for successful results
const FAILURE_RETRY_SECONDS := 300         # 5min retry window for failures
const GITHUB_API := "https://api.github.com/repos/siliconight/gool/releases/latest"
const SETTING_PATH := "audio/gool/check_for_updates"

# Status codes used in cache + console logs.
const STATUS_OK := "ok"
const STATUS_CACHE_HIT := "cache_hit"
const STATUS_OPT_OUT := "opt_out"
const STATUS_NO_SCENE_ROOT := "no_scene_root"
const STATUS_REQUEST_FAILED := "request_failed"
const STATUS_PARSE_FAILED := "parse_failed"
const STATUS_TAG_INVALID := "tag_invalid"
# result_<n> and http_<code> are formatted at the call site since
# they include dynamic values.

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
		_finish_with_status("", STATUS_OPT_OUT,
				"User has disabled audio/gool/check_for_updates.")
		return

	# Cache hit? Serve stored result if it's fresh.
	var cached := _load_cache()
	if _is_cache_fresh(cached):
		var cached_latest: String = str(cached.get("latest", ""))
		var cached_status: String = str(cached.get("last_attempt_status",
				STATUS_OK))
		var age: int = int(Time.get_unix_time_from_system()) \
				- int(cached.get("checked_at", 0))
		if cached_latest != "":
			# Fresh successful cache — serve the version.
			print("[gool] update check: cache hit, latest=%s, age=%ds"
					% [cached_latest, age])
			emit_signal("update_check_complete", cached_latest,
					_is_newer(cached_latest, current_version))
		else:
			# Fresh failure cache — don't hammer the API, but also
			# don't pretend we have a version. Re-emit the failure.
			print(("[gool] update check: recent failure (%ds/%ds ago), "
					+ "skipping retry. Status: %s")
					% [age, FAILURE_RETRY_SECONDS, cached_status])
			emit_signal("update_check_complete", "", false)
		return

	# Fresh network check.
	if scene_root == null or not is_instance_valid(scene_root):
		_finish_with_status("", STATUS_NO_SCENE_ROOT,
				"check() called with null/invalid scene_root; "
				+ "HTTPRequest needs a tree parent.")
		return

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
		# Request didn't even leave the gate — possible HTTPS init
		# failure, bad URL, network unavailable, etc.
		_cleanup()
		_finish_with_status("", STATUS_REQUEST_FAILED,
				("HTTPRequest.request() returned err=%d for %s. "
				+ "Possible causes: HTTPS/TLS not available in this "
				+ "Godot build, network unavailable, malformed URL.")
				% [err, GITHUB_API])


func _on_response(result: int, response_code: int,
				   _headers: PackedStringArray,
				   body: PackedByteArray) -> void:
	_cleanup()
	if result != HTTPRequest.RESULT_SUCCESS:
		_finish_with_status("", "result_%d" % result,
				("HTTPRequest completed with non-SUCCESS result=%d. "
				+ "Common values: 1=cant_connect, 2=cant_resolve, "
				+ "3=connection_error, 4=tls_handshake_error, "
				+ "5=no_response, 6=body_size_limit_exceeded, "
				+ "7=body_decompress_failed, 8=request_failed, "
				+ "9=download_file_cant_open, 10=download_file_write_error, "
				+ "11=redirect_limit_reached, 12=timeout. "
				+ "See Godot's HTTPRequest.Result enum.")
				% result)
		return
	if response_code != 200:
		_finish_with_status("", "http_%d" % response_code,
				("GitHub API returned HTTP %d. Common cases: "
				+ "403=rate-limited (60/hr unauthenticated), "
				+ "404=repo moved/renamed, 5xx=GitHub outage.")
				% response_code)
		return
	var json := JSON.new()
	var body_str := body.get_string_from_utf8()
	if json.parse(body_str) != OK:
		_finish_with_status("", STATUS_PARSE_FAILED,
				"GitHub API response was HTTP 200 but body didn't "
				+ "parse as JSON. First 200 chars: %s"
				% body_str.substr(0, 200))
		return
	var data: Variant = json.data
	if not (data is Dictionary):
		_finish_with_status("", STATUS_PARSE_FAILED,
				"GitHub API response parsed as JSON but root wasn't "
				+ "a Dictionary (got %s)."
				% typeof(data))
		return
	var tag: String = str(data.get("tag_name", ""))
	# GitHub's /releases/latest excludes pre-releases per their docs,
	# but defense-in-depth: also skip tags with hyphens.
	if tag == "":
		_finish_with_status("", STATUS_TAG_INVALID,
				"GitHub response had no tag_name field — possibly "
				+ "an empty repo or API schema change.")
		return
	if "-" in tag:
		_finish_with_status("", STATUS_TAG_INVALID,
				"Latest release tag '%s' looks like a pre-release "
				+ "(contains hyphen); skipping." % tag)
		return
	# Tags ship as "v0.79.2"; strip the v for comparison.
	var latest := tag.lstrip("v")
	_finish_with_status(latest, STATUS_OK,
			"Fresh check succeeded; latest=%s, current=%s."
			% [latest, _current_version])


# Single point of exit for every check() outcome. Writes cache,
# logs to console, emits the signal. `latest` is "" on any failure;
# `status` is one of the STATUS_* constants (or a dynamically-built
# `result_<n>` / `http_<code>`); `message` is the human-readable
# detail surfaced via the cache file and (for failures) push_warning.
func _finish_with_status(latest: String, status: String,
		message: String) -> void:
	_save_cache(latest, status, message)
	if status == STATUS_OK:
		print("[gool] update check: ok, latest=%s, current=%s"
				% [latest, _current_version])
	elif status == STATUS_OPT_OUT:
		print("[gool] update check: skipped (%s). %s"
				% [status, message])
	else:
		# Real failure — louder log so it shows up in the user's
		# Output panel without them having to inspect the cache file.
		push_warning("[gool] update check: failed [%s] — %s"
				% [status, message])
	var is_newer: bool = false
	if latest != "":
		is_newer = _is_newer(latest, _current_version)
	emit_signal("update_check_complete", latest, is_newer)


# Returns true if `cached` is recent enough that we shouldn't fire a
# fresh network check. Logic:
#   - Successful caches (latest != ""): 24h TTL.
#   - Failure caches (latest == ""):    5min retry window. This
#     prevents hammering GitHub when it's persistently down, while
#     still letting the user retry quickly after a transient issue.
func _is_cache_fresh(cached: Dictionary) -> bool:
	if not cached.has("checked_at"):
		return false
	var now: int = int(Time.get_unix_time_from_system())
	var age: int = now - int(cached.get("checked_at", 0))
	if age < 0:
		# Clock skew — treat as not fresh. Avoid the case where a
		# future-dated cache locks out checks forever.
		return false
	var latest: String = str(cached.get("latest", ""))
	if latest != "":
		return age < CACHE_TTL_SECONDS
	return age < FAILURE_RETRY_SECONDS


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


# v0.80.23: enriched cache. The previous schema (just latest +
# checked_at) was load-bearing for the silent-failure problem —
# without the status fields, there was no way to tell a clean
# cache miss from a quietly-failed network check from a never-
# attempted check.
func _save_cache(latest: String, status: String, message: String) -> void:
	var f := FileAccess.open(CACHE_PATH, FileAccess.WRITE)
	if f == null:
		# Even the cache write failed — log it loudly. Without the
		# cache file, the next check has no record of this attempt,
		# but at least the console captures it.
		push_warning(("[gool] update check: could not open %s for "
				+ "write (FileAccess.get_open_error()=%d). "
				+ "Status was [%s]: %s")
				% [CACHE_PATH, FileAccess.get_open_error(), status,
				message])
		return
	var payload := {
		"latest": latest,
		"checked_at": int(Time.get_unix_time_from_system()),
		"last_attempt_status": status,
		"last_attempt_message": message,
	}
	f.store_string(JSON.stringify(payload, "  "))
	f.close()


func _cleanup() -> void:
	if _http != null and is_instance_valid(_http):
		_http.queue_free()
	_http = null
