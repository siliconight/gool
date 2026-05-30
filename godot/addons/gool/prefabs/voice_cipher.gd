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

# addons/gool/prefabs/voice_cipher.gd
#
## Application-layer voice encryption. AES-256-CBC + HMAC-SHA256 in
## encrypt-then-MAC composition. No C++ engine changes, no
## certificate management, no external dependencies — uses only
## Godot's built-in `AESContext`, `HMACContext`, and `Crypto`
## classes. Closes the lightweight path of roadmap item 2.1.
#
## WHEN TO USE THIS
## ================
#
## Use VoiceCipher when:
##   - You're using a multiplayer transport that doesn't encrypt
##     (raw ENet without DTLS, custom UDP, WebRTC data channels in
##     some configs).
##   - You want voice encrypted but the rest of your game traffic
##     left in the clear (debug-friendly, e.g. for replay analysis
##     of game state without needing crypto keys).
##   - You want crypto independent of transport choice so swapping
##     transports later doesn't require rewiring.
#
## WHEN NOT TO USE THIS
## ====================
#
## Prefer Godot's DTLS for the whole connection if:
##   - You're using ENetMultiplayerPeer (DTLS is supported natively).
##     Set up: host generates a self-signed cert
##     (`Crypto.generate_self_signed_certificate`), clients call
##     `set_dtls_verify_enabled(false)` if you don't want to manage
##     PKI. Encrypts ALL traffic — voice, chat, game events — with
##     one transport-level config block.
##   - You want forward secrecy (DTLS does ephemeral key exchange;
##     VoiceCipher uses a static pre-shared key).
##   - You're shipping over Steam GameNetworkingSockets, EOS Voice,
##     or any transport that already encrypts at the network layer.
##     Layering app-level crypto on top of network-level crypto is
##     defense in depth but rarely worth the per-packet overhead.
#
## THREAT MODEL
## ============
#
## VoiceCipher protects against:
##   - **Passive eavesdropping** on the network (neighbor on the
##     same Wi-Fi sniffing your voice packets). Without the
##     session key, ciphertext is unreadable.
##   - **Packet tampering** (HMAC verifies integrity; tampered
##     packets are dropped on decrypt).
#
## VoiceCipher does NOT protect against:
##   - **Replay attacks**: an attacker who captured an encrypted
##     packet can re-send it; the receiver will decrypt successfully.
##     For voice this is mostly harmless (Opus duplicate detection
##     + jitter buffer dedup handles most cases). If you need
##     replay rejection, prepend a sequence number to the plaintext
##     before calling encrypt() and verify it on decrypt().
##   - **Compromised session key**: one leaked key compromises all
##     past and future sessions using that key. No forward secrecy.
##     For long-running production sessions, rotate the key
##     periodically and re-distribute via your lobby.
##   - **Attacker who can MITM the key distribution**: if your
##     lobby handshake itself is unencrypted, the attacker captures
##     the session key during join and reads everything. Use this
##     with a transport that protects your lobby handshake (DTLS,
##     TLS, or Steam GNS).
##   - **Side-channel attacks**: timing of HMAC verification is
##     not constant-time in GDScript. Not a practical concern for
##     friendly cooperative play; relevant if attackers have
##     local network access and time to mount sophisticated
##     attacks.
#
## CRYPTOGRAPHIC CONSTRUCTION
## ==========================
#
## Encrypt-then-MAC, the standard secure composition for AES-CBC +
## HMAC. Per RFC 7518 section 5.2.2:
#
##   1. Derive separate enc_key (32 bytes) and mac_key (32 bytes)
##      from the 32-byte session_key via HMAC-SHA256:
##        enc_key = HMAC-SHA256(session_key, "gool-enc-v1")
##        mac_key = HMAC-SHA256(session_key, "gool-mac-v1")
#
##   2. Encrypt:
##        iv = random(16)
##        ciphertext = AES-256-CBC(enc_key, iv, PKCS7-pad(plaintext))
##        tag = HMAC-SHA256(mac_key, iv || ciphertext)
##        packet = iv || ciphertext || tag
#
##   3. Decrypt:
##        Split packet into iv, ciphertext, tag.
##        Verify tag = HMAC-SHA256(mac_key, iv || ciphertext).
##        If mismatch, drop (return empty).
##        plaintext = PKCS7-unpad(AES-256-CBC-decrypt(enc_key, iv, ciphertext))
#
## Per-packet overhead: 16-byte IV + up to 15 bytes PKCS7 padding
## + 32-byte tag = ~48-63 bytes. For a 20ms Opus packet at ~80
## bytes encoded, that's ~60% size increase. Acceptable for the
## "lightweight + works without certificate setup" use case.
#
## USAGE
## =====
#
## Setup at game start:
##
##     # Host generates a fresh session key
##     var key := VoiceCipher.generate_session_key()
##
##     # Host distributes the key to joining peers via your lobby
##     # handshake. CRITICAL: the handshake itself must be on a
##     # secure channel (DTLS-enabled multiplayer, TLS HTTP, Steam
##     # GNS, etc.) — otherwise the attacker captures the key
##     # during distribution and the whole thing is moot.
##     _lobby.broadcast_session_key.rpc_id(peer_id, key)
##
##     # On each peer (host and client), wire the cipher
##     $VoiceCipher.session_key = key
##
## Encrypting outgoing voice:
##
##     $VoiceCaptureSource.pcm_frame_ready.connect(_on_voice_frame)
##
##     func _on_voice_frame(frame: PackedFloat32Array) -> void:
##         var opus := _encoder.encode(frame)
##         var encrypted := $VoiceCipher.encrypt(opus)
##         if not encrypted.is_empty():
##             _network.send_voice_packet.rpc(encrypted)
##
## Decrypting incoming voice:
##
##     @rpc("any_peer", "unreliable_ordered", "call_remote")
##     func send_voice_packet(encrypted: PackedByteArray) -> void:
##         var opus := $VoiceCipher.decrypt(encrypted)
##         if opus.is_empty():
##             # HMAC verification failed — packet dropped silently.
##             return
##         var sender := multiplayer.get_remote_sender_id()
##         Gool.submit_voice_packet(sender, opus, ...)

@tool
class_name VoiceCipher
extends Node


# ---------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------

## 32-byte (256-bit) pre-shared session key. All peers in the session
## must have the same key. Generate one via the static helper
## `VoiceCipher.generate_session_key()` and distribute via your
## lobby handshake.
##
## SETTING THIS VIA THE INSPECTOR IS DEVELOPMENT/TESTING ONLY.
## In production, set it programmatically after lobby join — a key
## hardcoded into a .tscn file is a key checked into your repo.
@export var session_key: PackedByteArray = PackedByteArray():
	set(value):
		session_key = value
		if not Engine.is_editor_hint():
			_apply_session_key()


# ---------------------------------------------------------------------
# State
# ---------------------------------------------------------------------

# Derived subkeys (HKDF-like construction from session_key).
var _enc_key: PackedByteArray = PackedByteArray()
var _mac_key: PackedByteArray = PackedByteArray()

# Reusable Crypto instance for random IV generation. Created once;
# making a new one per encrypt() call would be wasteful.
var _crypto: Crypto = null

# True once subkeys have been derived AND self-test has passed.
# encrypt() and decrypt() require this; calls before initialization
# return empty arrays with a clear error log.
var _ready_to_cipher: bool = false


# ---------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------

const IV_SIZE      := 16  # AES block size = AES-CBC IV size
const MAC_SIZE     := 32  # SHA-256 output size
const BLOCK_SIZE   := 16  # AES block size for PKCS7 padding
const KEY_SIZE     := 32  # AES-256 key size

# KDF info strings. If a future version changes the construction,
# bump these and the old keys won't accidentally decrypt new packets.
const _ENC_KDF_INFO := "gool-enc-v1"
const _MAC_KDF_INFO := "gool-mac-v1"


# ---------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_crypto = Crypto.new()
	if session_key.is_empty():
		push_warning("[VoiceCipher] session_key is empty. encrypt() " +
				"and decrypt() will fail until session_key is set. " +
				"Typically: host calls VoiceCipher.generate_session_key() " +
				"then RPCs the result to peers via the lobby; each peer " +
				"assigns the result to $VoiceCipher.session_key.")
		return
	_apply_session_key()


# ---------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------

## Generate a fresh 32-byte session key. Call this once on the host
## at session start; distribute the result to peers via your lobby
## handshake. Each call returns a cryptographically random key from
## the OS's secure RNG (via Godot's `Crypto.generate_random_bytes`).
static func generate_session_key() -> PackedByteArray:
	return Crypto.new().generate_random_bytes(KEY_SIZE)


## Encrypt a plaintext packet. Returns the encrypted packet
## (iv || ciphertext || tag) or an empty array if the cipher
## isn't initialized.
func encrypt(plaintext: PackedByteArray) -> PackedByteArray:
	if not _ready_to_cipher:
		push_error("[VoiceCipher.encrypt] cipher not initialized — " +
				"session_key not set or invalid.")
		return PackedByteArray()

	# Fresh random IV per packet. Reusing an IV with the same key
	# under CBC mode catastrophically breaks security; the
	# Crypto.generate_random_bytes is OS-secure RNG.
	var iv: PackedByteArray = _crypto.generate_random_bytes(IV_SIZE)
	var padded: PackedByteArray = _pkcs7_pad(plaintext, BLOCK_SIZE)

	var aes := AESContext.new()
	aes.start(AESContext.MODE_CBC_ENCRYPT, _enc_key, iv)
	var ciphertext: PackedByteArray = aes.update(padded)
	aes.finish()

	# MAC covers iv || ciphertext. Encrypt-then-MAC composition
	# (RFC 7518 section 5.2.2). Tampering with either iv or
	# ciphertext invalidates the tag on the receiver side.
	var mac_input := iv.duplicate()
	mac_input.append_array(ciphertext)
	var tag: PackedByteArray = _hmac_sha256(_mac_key, mac_input)

	# Output packet: iv (16) || ciphertext (16N) || tag (32)
	var packet := iv.duplicate()
	packet.append_array(ciphertext)
	packet.append_array(tag)
	return packet


## Decrypt and verify a packet. Returns the original plaintext, or
## an empty array if the cipher isn't initialized, if the packet is
## malformed, or if HMAC verification fails (which means the packet
## was tampered with, encrypted under a different key, or isn't
## actually a VoiceCipher packet). Callers should check
## `result.is_empty()` and silently drop on failure.
func decrypt(packet: PackedByteArray) -> PackedByteArray:
	if not _ready_to_cipher:
		push_error("[VoiceCipher.decrypt] cipher not initialized.")
		return PackedByteArray()

	# Minimum size: iv + at least one ciphertext block + tag
	var min_size: int = IV_SIZE + BLOCK_SIZE + MAC_SIZE
	if packet.size() < min_size:
		# Too short to be valid. Could be noise or a wrong-version
		# packet. Drop silently.
		return PackedByteArray()

	var ciphertext_end: int = packet.size() - MAC_SIZE
	if ((ciphertext_end - IV_SIZE) % BLOCK_SIZE) != 0:
		# Ciphertext length not a multiple of AES block size.
		# Malformed.
		return PackedByteArray()

	var iv: PackedByteArray = packet.slice(0, IV_SIZE)
	var ciphertext: PackedByteArray = packet.slice(IV_SIZE, ciphertext_end)
	var received_tag: PackedByteArray = packet.slice(ciphertext_end, packet.size())

	# Verify MAC BEFORE decrypting. If the tag doesn't match,
	# the packet was tampered with or encrypted under a different
	# key — either way, don't waste cycles decrypting garbage.
	var mac_input := iv.duplicate()
	mac_input.append_array(ciphertext)
	var computed_tag: PackedByteArray = _hmac_sha256(_mac_key, mac_input)
	if computed_tag != received_tag:
		# Authentication failure. Drop silently (a log here would
		# enable denial-of-service via flooding with garbage packets).
		return PackedByteArray()

	var aes := AESContext.new()
	aes.start(AESContext.MODE_CBC_DECRYPT, _enc_key, iv)
	var padded: PackedByteArray = aes.update(ciphertext)
	aes.finish()

	return _pkcs7_unpad(padded, BLOCK_SIZE)


## True if the cipher is fully initialized and ready to encrypt /
## decrypt. False during the brief window between scene-load and
## session_key assignment, or permanently if a misconfigured key
## was supplied.
func is_ready() -> bool:
	return _ready_to_cipher


# ---------------------------------------------------------------------
# Internals — key handling
# ---------------------------------------------------------------------

# Called when session_key is set (either inspector or programmatic).
# Derives the subkeys and runs the self-test. On failure, sets
# _ready_to_cipher = false so encrypt/decrypt refuse to run.
func _apply_session_key() -> void:
	if _crypto == null:
		_crypto = Crypto.new()
	if session_key.size() != KEY_SIZE:
		push_error("[VoiceCipher] session_key must be exactly %d bytes (got %d). " %
				[KEY_SIZE, session_key.size()] +
				"Use VoiceCipher.generate_session_key() to produce a valid key.")
		_ready_to_cipher = false
		return
	_derive_subkeys()
	if not _self_test():
		_ready_to_cipher = false
		return
	_ready_to_cipher = true


# HKDF-like construction. Derives separate enc_key and mac_key from
# the single session_key. Using different keys for encryption and
# authentication is a security best practice — even if one is
# somehow leaked, the other remains protective of its function.
func _derive_subkeys() -> void:
	_enc_key = _hmac_sha256(session_key, _ENC_KDF_INFO.to_utf8_buffer())
	_mac_key = _hmac_sha256(session_key, _MAC_KDF_INFO.to_utf8_buffer())


# Self-test: encrypt-decrypt round-trip with a known input. Catches
# misconfigurations (wrong key length, missing Crypto class, broken
# Godot API) at session_key set time, not at first packet.
func _self_test() -> bool:
	# Temporarily set ready flag so encrypt/decrypt work during test.
	# (They check _ready_to_cipher; we'll restore on failure.)
	_ready_to_cipher = true
	var test_input := "gool-cipher-self-test".to_utf8_buffer()
	var encrypted := encrypt(test_input)
	if encrypted.is_empty():
		push_error("[VoiceCipher] self-test FAILED at encrypt — " +
				"Godot AESContext or HMACContext may not be available.")
		_ready_to_cipher = false
		return false
	var decrypted := decrypt(encrypted)
	if decrypted != test_input:
		push_error("[VoiceCipher] self-test FAILED — round-trip " +
				"input/output mismatch. encrypted.size()=%d decrypted.size()=%d" %
				[encrypted.size(), decrypted.size()])
		_ready_to_cipher = false
		return false
	# Pass. _ready_to_cipher stays true.
	return true


# ---------------------------------------------------------------------
# Internals — HMAC
# ---------------------------------------------------------------------

func _hmac_sha256(key: PackedByteArray, data: PackedByteArray) -> PackedByteArray:
	var ctx := HMACContext.new()
	ctx.start(HashingContext.HASH_SHA256, key)
	ctx.update(data)
	return ctx.finish()


# ---------------------------------------------------------------------
# Internals — PKCS7 padding
# ---------------------------------------------------------------------

# Pad input to a multiple of block_size using PKCS7. If input is
# already a multiple, add a full block of padding (the unpad
# function relies on at least 1 byte of padding always being present).
func _pkcs7_pad(data: PackedByteArray, block_size: int) -> PackedByteArray:
	var pad_len: int = block_size - (data.size() % block_size)
	if pad_len == 0:
		pad_len = block_size
	var out := data.duplicate()
	for i in range(pad_len):
		out.append(pad_len)
	return out


# Strip PKCS7 padding. Validates that:
#   - the input is non-empty
#   - the last byte is a valid pad length (1..block_size)
#   - all pad_len trailing bytes equal pad_len
# On validation failure, returns empty array. This catches corrupted
# input and wrong-key decryptions where the bytes look random.
func _pkcs7_unpad(data: PackedByteArray, block_size: int) -> PackedByteArray:
	if data.is_empty():
		return PackedByteArray()
	var pad_len: int = data[data.size() - 1]
	if pad_len < 1 or pad_len > block_size or pad_len > data.size():
		return PackedByteArray()
	# Verify every padding byte equals pad_len.
	var start: int = data.size() - pad_len
	for i in range(start, data.size()):
		if data[i] != pad_len:
			return PackedByteArray()
	return data.slice(0, start)
