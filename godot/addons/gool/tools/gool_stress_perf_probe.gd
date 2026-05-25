# gool_stress_perf_probe.gd
#
# v0.78.2 — Stress rig perf assertion helper.
#
# Drop-in module for the gool_stress_test rig. Samples Godot Performance
# monitors (the gool/* custom monitors registered by runtime_singleton.gd
# in v0.78.1) over a window of frames, then asserts pass/fail against
# per-monitor thresholds. Converts the v0.78.1 human-eyeball workflow
# (open Debugger → Monitors, run scenario, look at graphs) into a
# CI-pinnable test.
#
# Per-scenario usage:
#
#     var probe := preload("res://stress/gool_stress_perf_probe.gd").new()
#
#     # Baseline asserts that should be on EVERY scenario:
#     probe.assert_peak_below("gool/render/underruns", 1.0,
#                              "render thread missed a deadline")
#     probe.assert_peak_below("gool/runtime/update_tick_us", 2000.0,
#                              "Update tick exceeded 2ms (>12% of frame budget)")
#
#     # Scenario-specific asserts (examples; tune per scenario):
#     probe.assert_mean_below("gool/eviction/full_pool_drops_per_sec", 10.0,
#                              "sustained one-shot drops -> pool undersized")
#
#     probe.start()
#     # ... drive your scenario for N frames ...
#     await get_tree().create_timer(10.0).timeout
#     # ... or however your rig already paces itself ...
#     var result := probe.stop()
#
#     if not result.passed:
#         push_error(result.format())
#         return false
#     print(result.format())
#     return true
#
# Notes:
#   * Samples at SceneTree.process_frame cadence (~60 Hz with vsync).
#     A 10-second scenario yields ~600 samples per monitor.
#   * Rate monitors (gool/eviction/*_per_sec, gool/voice/*_per_sec, etc.)
#     report 0 on their first sample by construction — _compute_rate in
#     runtime_singleton.gd needs a prior cumulative value to delta against.
#     If your assertion window is very short (<10 frames) the leading
#     zero will skew mean() noticeably; longer windows wash it out.
#   * Self-calibration: if you don't know the right threshold for a
#     scenario, run with no asserts first, dump result.summary(), set
#     the threshold ~1.5x the observed peak, then re-run with the
#     assertion in place.

extends RefCounted

# ---------------------------------------------------------------------------
# Inner types
# ---------------------------------------------------------------------------

class _Threshold:
	var monitor:     String
	var limit:       float
	# mode is one of:
	#   "peak_lt"   - assert max(samples) < limit
	#   "mean_lt"   - assert mean(samples) < limit
	var mode:        String
	var description: String

	func _init(mon: String, lim: float, m: String, desc: String) -> void:
		monitor     = mon
		limit       = lim
		mode        = m
		description = desc

class _Series:
	var monitor: String
	var samples: PackedFloat64Array

	func _init(mon: String) -> void:
		monitor = mon
		samples = PackedFloat64Array()

	func peak() -> float:
		if samples.is_empty():
			return 0.0
		var p: float = samples[0]
		for s in samples:
			if s > p:
				p = s
		return p

	func mean() -> float:
		if samples.is_empty():
			return 0.0
		var total: float = 0.0
		for s in samples:
			total += s
		return total / float(samples.size())

class Result:
	var passed:      bool                = true
	var per_monitor: Dictionary          = {}  # name -> _Series
	var failures:    Array               = []  # human-formatted failure lines
	var frames:      int                 = 0

	# Pretty-print pass/fail outcome.
	# Wrapped in parens before the % operator because GDScript binds
	# `%` tighter than `+` — same precedence trap that ate 19 _log()
	# call sites in v0.75.3.
	func format() -> String:
		if passed:
			return ("[probe] OK (%d frames sampled, %d monitors)"
				% [frames, per_monitor.size()])
		var lines: Array = []
		lines.append("[probe] %d failure(s) over %d frames:"
			% [failures.size(), frames])
		for f in failures:
			lines.append(f)
		return "\n".join(lines)

	# Compact one-line-per-monitor table of observed peak/mean. Useful
	# for calibration runs — print result.summary() with no thresholds
	# set, eyeball the numbers, then set thresholds at ~1.5x the peak.
	func summary() -> String:
		var lines: Array = ["[probe] summary (%d frames):" % frames]
		for name in per_monitor.keys():
			var s: _Series = per_monitor[name]
			lines.append("  %-50s peak=%-12.2f mean=%-12.2f"
				% [name, s.peak(), s.mean()])
		return "\n".join(lines)

# ---------------------------------------------------------------------------
# Private state
# ---------------------------------------------------------------------------

var _thresholds:  Array       = []   # of _Threshold
var _series:      Dictionary  = {}   # monitor name -> _Series
var _running:     bool        = false
var _tree:        SceneTree   = null
var _frame_count: int         = 0

# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

# Assert that the maximum sampled value of `monitor` stays strictly
# below `limit` over the probe window. Most common form — catches
# spikes.
func assert_peak_below(monitor: String, limit: float, description: String = "") -> void:
	_thresholds.append(_Threshold.new(monitor, limit, "peak_lt", description))

# Assert that the arithmetic mean of `monitor` stays strictly below
# `limit`. Use for noisy-rate signals (e.g. eviction rates) where a
# single-frame spike is acceptable but sustained pressure isn't.
func assert_mean_below(monitor: String, limit: float, description: String = "") -> void:
	_thresholds.append(_Threshold.new(monitor, limit, "mean_lt", description))

# Watch a monitor without asserting on it. Useful in calibration runs
# where you want the value in summary() but haven't picked a limit yet.
func watch(monitor: String) -> void:
	if not _series.has(monitor):
		_series[monitor] = _Series.new(monitor)

# Begin sampling. Connects to SceneTree.process_frame; the probe will
# read every monitor it has an assertion or a watch() on, once per
# frame, until stop() is called. Idempotent: calling start() twice
# without an intervening stop() is a warning + no-op.
func start(tree: SceneTree = null) -> void:
	if _running:
		push_warning("[probe] start() called while already running; ignored")
		return
	_tree = tree if tree != null else Engine.get_main_loop()
	if _tree == null:
		push_error("[probe] no SceneTree available; pass one explicitly to start()")
		return
	# Pre-allocate Series for every monitor that has either an assertion
	# or a watch() against it. Pre-allocating means the per-frame hot
	# path only does the append.
	for th in _thresholds:
		var t: _Threshold = th
		if not _series.has(t.monitor):
			_series[t.monitor] = _Series.new(t.monitor)
	_frame_count = 0
	_running     = true
	_tree.process_frame.connect(_on_frame)

# Stop sampling and return a Result. The probe is one-shot: after
# stop() you can re-call start() to begin a fresh window.
func stop() -> Result:
	var result := Result.new()
	if not _running:
		push_warning("[probe] stop() called while not running")
		return result
	_tree.process_frame.disconnect(_on_frame)
	_running = false
	result.per_monitor = _series
	result.frames      = _frame_count

	for th in _thresholds:
		var t:   _Threshold = th
		var s:  _Series     = _series.get(t.monitor)
		if s == null or s.samples.is_empty():
			result.passed = false
			result.failures.append(("  %s: no samples captured (monitor "
				+ "missing or never polled — make sure runtime_singleton.gd "
				+ "v0.78.1 is loaded)") % t.monitor)
			continue
		var observed: float = 0.0
		match t.mode:
			"peak_lt": observed = s.peak()
			"mean_lt": observed = s.mean()
			_:
				result.passed = false
				result.failures.append(("  %s: unknown mode %s"
					% [t.monitor, t.mode]))
				continue
		if observed >= t.limit:
			result.passed = false
			var label: String = (t.description if t.description != ""
				else t.monitor)
			result.failures.append(("  %s: %s = %.2f, threshold %.2f  (%s)"
				% [label, t.mode, observed, t.limit, t.monitor]))
	return result

# ---------------------------------------------------------------------------
# Internal
# ---------------------------------------------------------------------------

func _on_frame() -> void:
	if not _running:
		return
	_frame_count += 1
	for monitor in _series.keys():
		# Performance.get_custom_monitor returns Variant; cast through
		# float() to handle both int-valued snapshot monitors and
		# float-valued rate monitors uniformly.
		var v: float = float(Performance.get_custom_monitor(monitor))
		(_series[monitor] as _Series).samples.append(v)
