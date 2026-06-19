# Offline / Semi-Online Player-Message (Blood Message) Mod - Research & Design

_Reverse-engineering notes + design, produced 2026-05-31 against eldenring.exe build 2026-05-29 (ImageBase 0x140000000) and ERR v2.2.1.2. All .text RVAs shift per game patch - AOB-resolve them. Status: RESEARCH/DESIGN, not yet implemented._

---

**Target:** eldenring.exe build 2026-05-29, ImageBase `0x140000000`. All RVAs below are in the **first (low-VA) MSVC `.text`** section `[0x1000 .. 0x29a3000]`. The second `.text` at `0x4c0e000` is VMProtect-packed and not cleanly hookable at fixed RVAs. **RVAs shift every game patch - resolve by string/RTTI xref each build.**

**Naming correction up front:** the task framing calls the manager `CSWorldMsgMan`. That class name does **not** exist in this binary (zero hits for `WorldMsg`/`CSWorldMsg`/`WorldMessage`). The real, RTTI-confirmed manager is **`CS::BloodMessageInsMan`**. Everywhere "the message manager" is meant, it is this class. This is a confirmed finding, not a guess.

**ERSC is irrelevant** to this design and is not referenced. Messages are 100% vanilla game code; ERR only redirects the endpoint.

---

## 1. Vanilla message flow (manager, funcs, per-message struct)

The subsystem splits into four layers, all vanilla `eldenring.exe`:

**(a) In-world entities & manager** - what renders messages on the ground.
- `CS::BloodMessageInsMan` - manager. ctor `0x1b8940` (allocs `0x2b0`, vtable `0x29c8e68`, sub-object at `this+0x28`), dtor `0x1b8be0`. **Confidence: high.**
- It is **not** a standalone singleton. A parent object holds **two** instances: `parent+0x60` = *Others*, `parent+0x68` = *Self*, built via `0x1c19f0` with `edx=0`/`edx=1`. Matches FMG strings `SelfBloodMessage`/`OthersBloodMessage`/`StaticBloodMessage`. **Confidence: high.**
- `CS::BloodMessageIns` - on-ground message entity. ctor `0x1b7028`, vtable `0x29c89e8`. **Confidence: high.**

**(b) Local received-message DB** - cache of what the network downloaded.
- `CS::CSNetBloodMessageDb` (vtable `0x29c9c68`) / `CSNetBloodMessageDbItem` (vtable `0x29c9e88`). **Confidence: high.**

**(c) Network client-servant** - issues RPCs.
- `FromNet::FNBloodMessageImpl` (vtable `0x30804f0`) / `FNBloodMessage` (vtable `0x30804a8`). Eight virtual methods at `0x1df40d0, 0x1df4100, 0x1df4120, 0x1df4140 (op 0x10 Create), 0x1df41a0 (op 0x14), 0x1df4200, 0x1df4220, 0x1df4240`. Each tags a request opcode and calls the dispatch router. **Confidence: high.** (The exact method↔RPC mapping beyond Create=0x10 is **medium**.)
- Workers: GetList `0x1df3be0` (returns vector, elem size `0x18`), `0x1df3ca0`, `0x1df3d60`, `0x1df3e00`.

**(d) Jobs & UI** (drive the workflow, lower-priority hooks): `BloodMessageCreateJob`, `BloodMessageDownloadJob`, `BloodMessageEvaluateJob`, `BloodMessageRemoveJob`, `BloodMessageReentryJob`, `CSBloodMessageLoginEntrySequenceJob`, `LoginJob`; `MenuBloodMessage*`, `BloodMessageEditDialog`, `CSMsbPointHintBloodMessage`. **Confidence: high** (existence), **medium** (exact roles).

**Per-message content struct - `CS::BLOODMSG_MSG_PARAM`** (mangled `UBLOODMSG_MSG_PARAM@CS@@` present). The `BloodMessageIns` ctor copies **10 dwords** from `param+0x28..0x4c` → `ins+0x48..0x6c`. These are the template/word/conjunction/gesture IDs. The struct existence is **high** confidence; the *exact field-by-field semantics* (which dword is template1 vs word1 vs conjunction vs gesture) is **medium** - see open questions.

**`BloodMessageIns` recovered layout** (from ctor `0x1b7028`, **high**):
```
+0x00 vtable (0x29c89e8)      +0x3c message-data/param ptr
+0x10 owner/handle qword       +0x44 u32 lookup id (from param+0x200 via sub_a12410)
+0x18 u32                      +0x48..0x6c  10×u32 content (= BLOODMSG_MSG_PARAM+0x28..0x4c)
+0x1c float2 pos-a             +0x70 flag dword (low nibble = 0x30 | state&7)
+0x24/+0x28 u32                +0x80 sub-object (sound/anim)
+0x2c float2 pos-b             +0x4a0 sub-object (text/format buffer)
+0x34 u32   +0x38 u32 (r9 arg)
```

**Transport (vanilla):** WinHTTP WebSocket to `wss://gr-prod-steam.fromsoftware-game.net:10901` (string @ VA `0x142b3bf78`). The exe imports `WinHttpConnect/OpenRequest/SendRequest/ReceiveResponse/WebSocketCompleteUpgrade/WebSocketSend/WebSocketReceive/WebSocketClose`. **Confidence: high.** Serialization is bincode-style (waygate uses bincode 1.3.3 + chacha20/aes). **Important caveat:** the WSS endpoint string has **zero LEA xrefs in the MSVC `.text`** - the connection/TLS/serialization setup lives **inside VMProtect**. This is why the IAT (imported WinHTTP functions) is the only VMProtect-safe code anchor.

---

## 2. Interception points - our blueprint

Ranked by robustness, three tiers exist at three different layers. Pick based on whether we want zero-network (in-process) or server-backed.

**Tier A - In-process, no network (best for a pure local bank):**
1. **`BloodMessageIns::ctor` @ `0x1b7028`** - *primary local-bank inject point.* Build a `BLOODMSG_MSG_PARAM` in memory and drive a spawn through the manager; the ctor copies our 10 content dwords. Simplest path to "messages on the ground with no server." **Risk:** requires a valid live `BloodMessageInsMan` instance + correct spawn caller args (`r9`, positions). Needs the manager pointer chain (open question).
2. **Populate `CSNetBloodMessageDb` / `CSNetBloodMessageDbItem`** (vtables `0x29c9c68`/`0x29c9e88`) and let `BloodMessageDownloadJob` spawn from it. More faithful to the game's own flow; more state to satisfy.

**Tier B - Request-dispatch chokepoint (one hook for all message/sign/bloodstain RPCs):**
3. **Request dispatch router @ `0x1deffa0`.** Does `mov rax,[table + opcode*8]; jmp rax`. Table @ `.data 0x47ff410` (lazily filled; once-guard `.data 0x47ff698`; TLS slot `.data 0x4856f10`). Hook here to intercept by opcode and synthesize responses without parsing WS frames or touching crypto. **This is the cleanest single in-`.text` redirect for the whole online request path.** **Confidence in the chokepoint mechanics: high.** The exact opcode→handler table contents landed partly in VMProtect bytes, so the per-opcode mapping beyond Create=`0x10` needs live validation (open question).
4. **`FNBloodMessageImpl` vtable methods** (`0x30804f0`) - hook the per-RPC virtuals (GetList/Create/Evaluate/Remove) individually. Finer-grained than the router but more hooks.

**Tier C - IAT / WinHTTP layer (VMProtect-safe, the same layer ERR uses):**
5. **IAT-hook `WinHttpWebSocketReceive` @ IAT `0x4c0ccb4`** - return locally-built `Response*Params` so the game's own parser feeds the DB.
6. **IAT-hook `WinHttpWebSocketSend` @ IAT `0x4c0ccbc`** - capture outbound `RequestCreate*` (submit) / `RequestEvaluate*` (rate), store locally, synthesize the matching response on next Receive.
7. **IAT-hook `WinHttpConnect` @ IAT `0x4c0cc84`** - rewrite host to a localhost server (literally ERR's technique). Other WinHTTP IAT slots: `OpenRequest 0x4c0cc9c`, `SendRequest 0x4c0ccc4`, `WebSocketCompleteUpgrade 0x4c0cccc`.

**The crypto problem with Tier C:** if WS payloads are encrypted (waygate uses a session-nonce key exchange; reforged.dll bundles chacha20/aes/sha256), a Send/Receive hook sees ciphertext. Then we must either replicate the crypto, run a real server that speaks it, or move the hook **above** the crypto (Tier B router `0x1deffa0`, which handles plaintext request objects). **This is the single biggest unknown gating Tier C.**

**Recommended blueprint:** start Tier A (#1) for a visible offline result with no crypto/network risk; graduate to Tier B (#3) for full request/response control once the manager pointer chain and opcode table are validated live.

---

## 3. How ERR redirects online - and what our server must speak

**Mechanism (high confidence, from reforged.dll strings; it's an unpacked Rust PE, ImageBase `0x180000000`):**
- ERR statically links **Waygate** (`vswarte/waygate-server`'s client side; paths `deps\waygate-client\src\...`).
- On startup (`src\features\waygate\mod.rs`, `"Initializing Waygate (pre)..."`) it **hooks `winhttp.dll!WinHttpConnect`** (`"Hooked winhttp"`, `deps\waygate-client\src\eac\hook.rs`; `"WinHttpConnect: Redirecting request from "`, `"Swapping details for request open"`, `winhttp.rs`).
- It rewrites the host from `fromsoftware-game.net` → **`online.erreforged.com:443`** and injects auth headers.
- The message **logic stays vanilla**; only the endpoint moves. `reforged.toml` line 1: `enable_online = true`.

**Embedded config / what our server must present:**
- Config fields (positional bincode): `host, port, enable_ssl, client_secret_key, server_public_key, regulation_hash, native_hash, files_hash, dll_hashes, access_key`.
- Server identity baked in: `host=online.erreforged.com`, `port=443`, `server_public_key (X25519/sodium, base64) = 8MCuznpxj+EzrLY4bwOlfyGYKK2Stmo7CKyAAJeWrmg=`.
- Login headers: `X-STEAM-ID`, `X-STEAM-SESSION-TICKET`, `X-WAYGATE-CLIENT-VERSION: 0.1.0`, `X-WAYGATE-ACCESS-KEY`, `X-REFORGED-REGULATION-HASH/-NATIVE-HASH/-FILES-HASH/-DLL-HASHES`.
- Steam auth via `SteamAPI_ISteamUser_GetAuthSessionTicket`; P2P over `SteamAPI_ISteamNetworkingMessages_*`, libsodium-encrypted after an X25519 nonce exchange (`"Swapping sodium keys"`, `"Nonce message"`).

**What OUR server must speak:** the Waygate protocol - HTTPS login carrying the headers above, then bincode-serialized, sodium-encrypted Waygate RPCs. The message RPCs (and their wire shape) are documented open-source in `vswarte/waygate-server` (`message/src/eldenring/bloodmessage.rs`). See §4 for the exact fields. **Key consequence:** standing up our own server is *not* novel work - it is the existing open-source waygate-server, optionally with our endpoints. The harder part (crypto handshake, Steam ticket validation) is already implemented there.

---

## 4. Message content data model

A stored message holds **no free text** - only integer IDs into `BloodMsg.fmg` (file id=2 inside `menu.msgbnd.dcx`; ERR override = `mod/msg/engus/menu_dlc02.msgbnd.dcx`, a **full merged bank**). **Confidence: high.**

**Logical record (language-independent):**
```
{ msgType:u8 (0..3),         // 1-part vs 2-part selection
  templateId, wordId,        // part 1 → word substituted into template's <?bmsg?>
  conjunctionId,             // join via <?belongMsg?>
  template2Id, word2Id }     // part 2 (when msgType selects 2-part)
```
Every `*Id` indexes `BloodMsg.fmg` of the active locale. Plus a **gesture id** (separate field in the message detail, *not* in BloodMsg.fmg - its presence/range is an open question). The renderer expands `<?bmsg?>` (word slot) and `<?belongMsg?>` (conjunction slot). These map onto the 10 content dwords at `BLOODMSG_MSG_PARAM+0x28..0x4c`; the precise dword↔field assignment is **medium** confidence.

**Wire model (waygate; maps onto FromNet structs):**
- **FETCH** `RequestGetBloodMessageListParams { search_areas: Vec<PlayRegionArea{play_region:i32, area:i32}>, group_passwords }` → entries `{ player_id, character_id, identifier:ObjectIdentifier(i64), rating_good:i32, rating_bad:i32, data:Vec<u8>, area, group_passwords }`. GetList returns ~64 entries near a random pivot per play-region.
- **SUBMIT** `RequestCreateBloodMessageParams { area, character_id, data:Vec<u8>, unk, group_passwords }` → `{ identifier }`.
- **RATE** `RequestEvaluateBloodMessageParams { identifier, rating:u32 }` → empty (server bumps `rating_good`/`rating_bad`).
- **REMOVE** `{ identifier }`; **REENTRY** `{ identifiers:Vec, unk }` → `{ identifiers }` (re-announce own messages after relogin).
- `data: Vec<u8>` = the **serialized `BLOODMSG_MSG_PARAM`** (the IDs above). Server treats it as opaque. Position is `PlayRegionArea` (play_region/area cell IDs, e.g. `1100000`), **not** raw coords.

**Opcodes (from FN method constants):** Create=`0x10`; other blood-message ops use `0x13`, `0x14`; full table resolved at runtime into `.data 0x47ff410[opcode]`.

**FMG binary format** (for reading the word/template text - only needed for UI/composing, not for storing the bank): ER FMG v2 LE; DCX = Oodle Kraken (`KRAK` magic ~`0x28`, decompress via `oo2core_*`). Tooling already exists in `tools/get_fmg_ids.py` and the scratch scripts below.

**ERR vocabulary delta (high confidence):** ERR re-densified vanilla's sparse IDs into contiguous ranges **and added vocabulary**, so a bank for ERR must use **ERR's IDs**, not vanilla's. Examples: templates moved to `387-417` with 6 new (`412 "it is surely <?bmsg?>!"` … `417 "are you a <?bmsg?>?"`); new People words `505-524`; new Things `571-578`; new conjunctions `878-893` (belongMsg join at `889`); new phrases `869-877`. `NetworkMessage.fmg` (id=31) is **notification** text, not message words - do not confuse the two.

---

## 5. Proposed mod architecture

**Form factor:** a ModEngine2-loaded DLL (coexists with reforged.dll). Pure offline by default; optional semi-online via our server.

**5.1 Hook strategy (AOB-resolve, since `.text` shifts):**
- Do **not** hardcode `.text` RVAs. The stable anchors across patches are: (1) the **RTTI/mangled-name strings** (`.?AVBloodMessageInsMan@CS@@`, `UBLOODMSG_MSG_PARAM@CS@@`, `FromNet::Request*BloodMessage*Params`), and (2) the **WinHTTP IAT slots**.
- Resolution recipe per launch:
  1. Find the mangled-name string → its `TypeDescriptor` (16 bytes before) → scan `.rdata` for the `CompleteObjectLocator` (sig==1) referencing it → vtable = COL-ptr + 8. This yields `BloodMessageInsMan`/`Ins`/`CSNetBloodMessageDb`/`FNBloodMessageImpl` vtables generically.
  2. From a vtable, the ctor is found by xref to the vtable-store `lea`/`mov [rcx],rax` (e.g. `BloodMessageIns` ctor stores `0x29c89e8` - locate that store, the enclosing function is the ctor).
  3. For the router `0x1deffa0`: AOB on the `mov rax,[r8+rsi*8]; jmp rax` epilogue plus the `gs:[0x58]` TLS prologue.
  4. WinHTTP IAT: walk the import table by name (`WinHttpWebSocketReceive`, etc.) - name-keyed, fully patch-stable.
- This is exactly the methodology that produced the RVAs in this doc; the scratch scripts (§artifacts) are the reference implementation.

**5.2 Local bank file format** (proposed; keep it explicit and append-friendly):
```
bank.json (or bincode)  - one object per stored message:
{
  id:        u64,            // local identifier (our ObjectIdentifier)
  area:      { play_region:i32, area:i32 },
  pos:       { x:f32, y:f32, z:f32 },   // for in-world spawn placement
  msg: { msgType:u8, templateId, wordId, conjunctionId, template2Id, word2Id, gestureId },
  rating_good: i32,
  rating_bad:  i32,
  source:    "local" | "server",
  author:    string (optional)
}
```
Store the *logical IDs*, not text → no translation, locale-invariant. Keep `rating_good/rating_bad` locally because Evaluate has an empty response (ratings must be mirrored client-side). Bank lives **inside the sandbox dir**, never in game/mod dirs (per project rules).

**5.3 Populating the bank:**
- **Seed:** ship a curated `bank.json` (hand-authored ERR-valid IDs).
- **Pull:** optional `GET /messages?play_region=&area=` from our server returns a list in the same schema; merge into bank keyed by `id`.
- **Import:** allow scraping real exported messages if available later.

**5.4 Coexistence with offline mode (no FromSoft matchmaking):**
- Tier A path needs **no** login/session. Confirm whether the game gates message *display* on `CSSessionManager` state `Active` (enum `Inactive/Requested/Loading/Active` present). If it does, we must satisfy that state machine or inject far enough downstream (the `Ins` ctor / DB) to bypass the gate. **Open question - needs the running game.**
- If we instead use Tier C with a localhost server, we must also stub the login (`CSBloodMessageLoginEntrySequenceJob`) - i.e. our server answers the HTTPS login so the session reaches `Active`. This is what waygate-server already does.

**5.5 Optional semi-online server (our own):**
- Reuse `vswarte/waygate-server` (Rust + PostgreSQL) - it already implements create/get_list/evaluate/remove/reentry with the correct crypto/handshake. Point either (a) reforged.toml's waygate-client at our host, or (b) our DLL's `WinHttpConnect` hook at `127.0.0.1`.
- Minimal custom endpoints if we *don't* run full waygate: `GET /messages` (pull-by-area), `POST /messages` (submit), `POST /messages/{id}/rate`. These feed/serve the bank; the DLL does all game-side work. This avoids reimplementing the FromSoft crypto entirely - but then the game's own netcode is **not** used (we're in Tier A/B in-process), and "server" just means "bank source over HTTP," which is the pragmatic choice.

---

## 6. Open questions - and exactly what resolves each

| # | Question | What's needed |
|---|---|---|
| 1 | Stable pointer chain to **live `BloodMessageInsMan`** (parent holds it at `+0x60`/`+0x68`; parent singleton slot unresolved). Required for Tier A spawn. | **Running game** + debugger: xref the get-instance accessor, or scan which singleton getter is followed by a `[+0x60]` read. Try `CSNetMan`/`CSSessionManager` as the parent. |
| 2 | **Exact field semantics** of the 10 dwords in `BLOODMSG_MSG_PARAM+0x28..0x4c` (which is template1/word1/conj/gesture). | Capture a live param when submitting a *known* message (e.g. "Be wary of X") and diff; or disassemble the `BloodMessageEditDialog` commit (`MenuBloodMessage` `AEBUBLOODMSG_MSG_PARAM` lambda). |
| 3 | **Serialization** of `BLOODMSG_MSG_PARAM` → `data: Vec<u8>` (byte order/endianness). | Disassemble the Create worker reachable from FN method `0x1df4240` (op `0x10`); compare to waygate's `message/test/data/RequestCreateBloodMessage.bin`. |
| 4 | Are WS payloads **encrypted** at the Send/Receive boundary? (Gates whether Tier C IAT hooks are viable.) | **Network capture** of the live ERR/vanilla session + check against waygate's session crypto. If encrypted → use Tier B router `0x1deffa0`. |
| 5 | The **opcode→handler table** contents at `.data 0x47ff410` (only Create=`0x10` validated; rest partly in VMProtect). | **Running game**: dump the table after lazy-init (read `.data 0x47ff410` once the once-guard `0x47ff698` is set). |
| 6 | Inner servant via `FNBloodMessageImpl [this+8]` (holds cached received list; method `[rax+0xf0]` used by GetList worker). Natural local-bank inject point for GetList. | **Running game**: resolve the object + its vtable live. |
| 7 | Does message **display gate on `CSSessionManager == Active`** offline? | **Running game** test: load offline, try to spawn via Tier A, observe. |
| 8 | World-position → `PlayRegionArea` conversion (`CSPlayRegion` @ `0x142b448a0`). Needed to key the bank by area. | Disassemble `CSPlayRegion`/`PlayRegionID` logic; cross-check ERR's extracted play_region data (MapForGoblins). |
| 9 | Does ERR's waygate fork match **upstream** message schema? | Read reforged.dll matchmaking serializers, or diff against upstream `waygate-server`. |
| 10 | WinHTTP **hook ordering** vs reforged.dll (both hook WinHttp). | Test load order under ModEngine2; or avoid the conflict by pointing reforged's client at our localhost waygate instead of double-hooking. |

---

## 7. Pragmatic phased plan (smallest first)

**Phase 0 - Anchors & resolver (no game behavior change).**
Write the AOB/RTTI resolver: from mangled names → vtables → ctors; from imports → WinHTTP IAT. Validate it reproduces the RVAs in this doc on the current build. Reuse scratch scripts. *Deliverable: a resolver lib + a log dumping resolved addresses.*

**Phase 1 - Inject ONE hardcoded message (Tier A).**
Resolve live `BloodMessageInsMan` (Q1). Build one `BLOODMSG_MSG_PARAM` with known ERR IDs (e.g. template `387` + a word), drive `BloodMessageIns::ctor @ 0x1b7028` at a fixed world position. *Deliverable: one readable message on the ground, fully offline.* This proves the content model + spawn path and resolves Q2/Q7 empirically.

**Phase 2 - Load a LOCAL BANK.**
Define `bank.json` (§5.2), spawn all in-area entries on area load (needs Q8 for area keying, or spawn all near player initially). Mirror ratings locally. *Deliverable: a curated offline message bank that displays in-world.*

**Phase 3 - PULL from our server.**
Add `GET /messages?play_region=&area=` client in the DLL; merge into bank. Server is a thin HTTP service (not full waygate). *Deliverable: semi-online bank, offline-by-default, refreshable.*

**Phase 4 - SUBMIT & RATE.**
Hook the compose-commit (or `WinHttpWebSocketSend` / router op `0x10`/`0x14`) to capture submits/ratings → write to bank + `POST` to server. Synthesize identifiers locally. *Deliverable: full create/rate loop without FromSoft.*

**Phase 5 (optional) - Full waygate parity.**
If we want the game's *own* netcode to do the work, run `vswarte/waygate-server` locally and point the client at `127.0.0.1` (Q4/Q9/Q10). Heaviest path; only if Tier A/B prove insufficient.

---

### Confidence summary
- **High:** all listed vtables/ctor RVAs in the low `.text`; the router `0x1deffa0` + table `0x47ff410` mechanics; WinHTTP transport + IAT slots; ERR's WinHttpConnect-redirect mechanism + endpoint/key/headers; the waygate wire schema; BloodMsg.fmg content model + ERR ID shift.
- **Medium:** `BLOODMSG_MSG_PARAM` exact field order; FN method↔RPC mapping beyond Create=`0x10`; whether IAT-layer payloads are plaintext.
- **Unverified (needs running game / capture):** live manager pointer chain; opcode table contents beyond `0x10`; session-state gating; on-wire byte order/crypto.

### Reference artifacts (absolute paths)
- `G:\Games\Elden Ring\ERR saves piece\scratch\msg_strscan.py`, `msg_strscan2.py` - string/RTTI scans
- `G:\Games\Elden Ring\ERR saves piece\scratch\rtti1.py`, `rtti2.py`, `_rtti.py` - RTTI→vtable resolution
- `G:\Games\Elden Ring\ERR saves piece\scratch\insctor.py`, `insctor2.py` - `BloodMessageIns` ctor layout
- `G:\Games\Elden Ring\ERR saves piece\scratch\fnmethods.py`, `sendfn.py` - FN vtable + dispatch router
- `G:\Games\Elden Ring\ERR saves piece\scratch\vtables.json` - resolved vtable RVAs
- `G:\Games\Elden Ring\ERR saves piece\tools\get_fmg_ids.py` - FMG/DCX decode (Oodle)
- ERR redirect source-of-truth: `reforged.dll` (Rust, ImageBase `0x180000000`); config in `...\internals\modengine\dll\reforged.toml`
- Server precedent: `github.com/vswarte/waygate-server` - `message/src/eldenring/bloodmessage.rs`, `handler/eldenring/bloodmessage.rs`
---

## 8. Live scan addendum (2026-05-31, game running, online, messages nearby)

Confirmed against the live process (PID-scanned via ReadProcessMemory; base ASLR 0x7FF76A960000; RVAs = build 2026-05-29). Scripts: `scratch/scripts/_live_bloodmsg_scan*.py`.

**Confirmed live:**
- **Opcode dispatch table** `base+0x47ff410` is populated (resolves the §2/Q5 unknown). Live entries:
  `table[0x10]=rva 0x1DE7700, [0x11]=0x1DE6C20, [0x12]=0x1DE6610, [0x13]=0x1DE5EE0, [0x14]=0x1DE5AA0, [0x15]=0x1DE8B80, [0x16]=0x1DE5D20, [0x17]=0x1DE5CC0`. (Create=0x10 → 0x1DE7700.) **High.**
- **~60 live `BloodMessageIns`** (vtable `base+0x29c89e8`) and **103 live `CSNetBloodMessageDbItem`** (vtable `base+0x29c9e88`) - messages are loaded (online session). **High.**
- **Ins → DbItem ownership:** a populated `BloodMessageIns` holds its owning `CSNetBloodMessageDbItem` at **`Ins+0x10`**. **Corrects §1:** the renderable content is NOT inline in `BloodMessageIns+0x48..0x6c` - for live instances that inline block is empty (all `0xFFFFFFFF`, `look44=-1`). The real per-message data lives in the **CSNetBloodMessageDbItem**. **High.**
- **Message content is an OPAQUE BLOB, not inline FMG ids.** Scanning all 103 DbItems for the ERR template-id range (380-420) found NO fixed offset holding a template id (only coincidental 1-of-103 hits). This confirms the waygate `data: Vec<u8>` model: the 6 ids (msgType/template/word/conjunction/template2/word2) are **inside a serialized blob referenced by the DbItem**, not stored as inline struct fields. A local bank therefore stores/reconstructs these **blobs**; decoding to template/word ids requires parsing the blob format. **High (negative result).**
- **`CSNetBloodMessageDb` root** (vtable `base+0x29c9c68`) found as a heap object (cluster ~`0x1c9821e5xxx`, adjacent to a `BloodMessageInsMan` and an internal `{vtable,u32,u32}` registry array). **No static-image pointer points directly at it** → it is reached via an accessor/getter, exactly like the marker-anchor chain (needs the getter xref to pin a static chain). **Medium.**

**Still open after this scan:**
- The **blob (`data: Vec<u8>`) serialization format** - needed to decode existing messages and to build new ones. Resolve via the Create-worker disasm (table[0x10]→0x1DE7700) or waygate test vectors.
- The **DbItem field layout**: where the `data`-vector pointer (ptr/len/cap), the `ObjectIdentifier` (i64), and `rating_good/rating_bad` live. (DbItem is ~0xC0+ bytes; raw dumps captured in scratch but not yet field-mapped - heap state turns over between runs as messages stream, so a single-shot capture with the player stationary at a known message is needed.)
- The **static accessor chain** to the DB/manager cluster (pointer-chase like the marker anchor).
- The **`BloodMessageInsMan` parent pairing**: the static-guess `+0x60/+0x68` (Others/Self) did NOT match live (the live manager pair is ~0x100/0xC0 apart, not 8) - re-derive offsets live.

**Implication for the mod:** Tier A (inject a `BLOODMSG_MSG_PARAM` and spawn) still needs the blob/param field map; the cleanest near-term path may be **populating `CSNetBloodMessageDbItem` entries** (the DB cache the game already renders from) with blobs, which sidesteps inline-field guessing - once the blob format and DbItem `data`-pointer offset are pinned.

### 8.1 Content field map - CRACKED (2026-05-31, live-decoded against rusRU BloodMsg.fmg)

The opaque-blob worry was wrong for the in-memory DbItem: the renderable ids are stored as plain dwords in `CSNetBloodMessageDbItem`. Correlated by matching a known on-ground message ("Впереди птица") + own message ("только начало") to memory, then decoding all live items through `BloodMsg.fmg` (file id=2 in `msg/<locale>/menu_dlc02.msgbnd.dcx`; locale-specific text, language-independent ids).

- **`DbItem+0x30` = primary content id (BloodMsg.fmg id).** CONFIRMED - decoded 28 live messages to correct Russian text ("Впереди руна", "Впереди монстр", "молодец", "только начало", ERR-custom "reforged", …).
  - ids `40000-41999` = **standalone phrases** (`+0x30` is the whole message, e.g. 41016 "только начало", 41005 "молодец", 40069 "reforged").
  - ids `30000-36999` = **nouns/words** that combine with a template → "Впереди <word>" etc.
- **`DbItem+0x34` = second word id** (2-part messages; `0` in all loaded 1-part samples). INFERRED.
- **Template / conjunction / msgType fields: NOT yet pinned** - every message loaded near the test spot rendered as template 0 ("Впереди <?bmsg?>") or a standalone phrase, so the template field couldn't be distinguished from a default-0 (brute-forcing `combo = 10000000 + tmplIdx*100000 + word` trivially matches `tmplIdx=0`). Needs a loaded message with a non-"Впереди" template (e.g. "<?bmsg?>, but…" / "<?bmsg?>? неожиданно…") to locate it. Candidate region: the small dwords at `+0x38/+0x3c/+0x40/+0x44`.
- **FMG composed-combo table:** `BloodMsg.fmg` pre-renders every template×word at id `10000000 + tmplIdx*100000 + wordId` (e.g. 10030029 = "Впереди птица", 10130029 = "впереди птица отсутствует"). Useful to render without manual template substitution.

**Decode recipe (works now):** read `DbItem+0x30` → look up in `BloodMsg.fmg` of the active locale → if it's a noun, prepend the template (default "Впереди "). Reproduce with `scratch/fmg_parse.py` + `scratch/scripts/_live_bloodmsg_scan*.py`.

**Implication for the mod:** a local bank entry needs just `{ contentId (+0x30), word2 (+0x34, opt), templateIdx, msgType, area, pos, rating }`. Populating `CSNetBloodMessageDbItem` (or its DB) with these is the cleanest offline path - the ids are plain dwords, no blob (de)serialization needed for the in-memory path. (The on-WIRE form to/from a server is still the waygate `data: Vec<u8>` blob - only relevant if we speak the network protocol rather than inject in-process.)

### 8.2 2-part messages: only word1 is inline; full structure is in the wire blob

Tested with two live 2-part messages ("ах, роскошный вид... так что горшок сейчас не помешает"; "Впереди так держать и тогда руна сейчас не помешает").
- `DbItem+0x30` holds ONLY **word1** (e.g. 35035 "роскошный вид", 41018 "так держать"); `+0x34`=0. The part-2 components (word2 32052/32048, conjunction 50003/50000, template2 10014) are **NOT** present as inline dwords anywhere in the DbItem's first ~0x120 bytes, and no pointer field led to them. ⇒ the full multi-part record (msgType, template1, word1, conjunction, template2, word2) lives in the **opaque network blob (`data: Vec<u8>`)**, not in scannable inline fields. So `+0x30` is a cached "primary word" (good for gist/identification) - 1-part messages decode fully from it because they ARE just word1; 2-part need the blob.
- Scanning for conjunction ids (50000/50003) in memory hits the **compose-UI category tables**, not received-message records - confirming the received content isn't stored as plain (t1,w1,conj,t2,w2) dwords.
- **Template-index ↔ FMG-id rule (confirmed):** template combo index = `templateFmgId − 10000` (10000 "Впереди <?>"→0, 10001 "впереди <?> отсутствует"→1, 10014 "<?> сейчас не помешает"→14, 10020 "ах, <?>..."→20). Pre-rendered combo id = `10000000 + idx*100000 + wordId`. FMG category bands: 30000-39999 words, 40000 "Понятия"/41000 "Фразы" phrases, 10000+ templates, 50000+ conjunctions.
- **Consequence for the bank:** to faithfully store/replay 2-part messages we must capture the **wire blob** per message (or decode its format). Decoding the blob layout needs the Create-serializer disasm (dispatch table[0x10] → live RVA 0x1DE7700, see §8) or waygate test vectors. For 1-part messages the inline `+0x30` word is sufficient.

### 8.3 "Download all messages" + which function - answer

- **There is NO bulk "get all" RPC.** The game fetches **per area**: `FromNet::RequestGetBloodMessageListParams` (the GetList call; FN worker RVA `0x1df3be0`, dispatched via the router `0x1deffa0` / live opcode table). GetList returns ~64 messages near a random pivot inside one `PlayRegionArea`. To enumerate everything you iterate every play_region/area and repeat GetList (random pivot ⇒ page by accumulating + dedup on the `ObjectIdentifier`). High volume, eventually-complete, not exact.
- **You normally don't CALL it - you ANSWER it.** Two practical mod shapes:
  1. **Own server (recommended for "semi-online bank"):** run waygate-server (we control its DB) and point the game at it (WinHttpConnect host-swap, exactly ERR's trick). The game auto-calls GetList per area as the player moves; our server replies from the bank, and "download all" is just a DB query on the server side. Submit/rate ride the same path (Create=op 0x10, Evaluate).
  2. **In-process inject (pure offline, no server):** populate the game's received-message DB (`CSNetBloodMessageDb` / add a `CSNetBloodMessageDbItem`) directly so the world spawns them. The "add received item" function is the missing piece to pin (reachable from `BloodMessageDownloadJob` / the GetList-response consumer) - that's the one function to hook/call for a serverless bank.
- **To scrape ERR's existing messages** specifically: speak ERR's waygate GetList per area (needs their auth headers + sodium session crypto - see §3) and walk all areas; or intercept live GetList responses (Tier C `WinHttpWebSocketReceive`) while playing online and log them into the bank.

### 8.4 Blob format CRACKED - full multi-part decode works

The full multi-part content IS in a heap blob (u16 ids, not u32 - that's why earlier u32 scans failed). Layout (relative to the blob's 8-byte header; verified by decoding the live message "ах, роскошный вид... так что горшок сейчас не помешает" byte-for-byte):
```
blob+0x00..0x07  header (8 bytes; not yet fully mapped - varies per instance)
blob+0x08  u16 templateId1     (FMG 10000..11999; 10000="Впереди <?>", 10014="<?> сейчас не помешает", 10020="ах,<?>...")
blob+0x0A  u16 UNKNOWN         (deterministic from content; role TBD - gesture? word-category? flags)
blob+0x0C  u16 wordId1         (the noun/phrase substituted into template1's <?bmsg?>)
blob+0x0E  u16 conjunctionId   (0 = 1-part; else 50000..50003: 50000="и тогда", 50003="так что")
blob+0x10  u16 templateId2     (0 if 1-part)
blob+0x12  u16 wordId2         (0 if 1-part)
```
**Render:** `template1.replace("<?bmsg?>", word1)` + (if conj) ` conj template2.replace("<?bmsg?>", word2)`. Decoded the user's exact 2-part message verbatim. Template-index↔FMG rule (§8.1) still holds; pre-rendered combos exist too. Validity filter to isolate real blobs from memory noise: `conj ∈ {0,50000..50003}` AND `t2 ∈ {0,10000..11999}` AND `w2` a valid word or 0.

**This closes the data model.** A bank entry = `{templateId1, wordId1, conjunctionId, templateId2, wordId2, (+0x0A unk), area, pos, identifier, rating_good, rating_bad}` - six u16 ids fully define the renderable text in any locale.

### 8.5 Filling the bank from the ERR server - harvest, don't call

The game ALREADY fetches messages for the player's area from the ERR server (they're live in memory as the DbItems + these blobs). So the bank is populated by **harvesting in-memory blobs while online**, not by us calling any RPC:
- Walk the world online; periodically enumerate live message blobs (anchor via the DbItem→blob pointer, or structural u16 scan + validity filter) and decode the six ids → append to the bank (dedup by identifier).
- Calling `GetList` ourselves is possible but **unnecessary and risky** (needs correct `this`/args/calling thread; the game already calls it per area). Not recommended.
- For a complete sweep you still must visit every area (GetList is per play-region, ~64 near a random pivot) - coverage grows as you explore.
- Remaining for a clean harvester: pin the **DbItem→blob pointer offset** (so we enumerate blobs via the DB instead of a noisy memory scan) and the `+0x0A` field's meaning. Both are small follow-ups; the format itself is done.

### 8.6 Gestures, the +0x0A field, AOBs, and automation

**Gestures (from 2 known gesture-messages):** "кровотечение является в видениях…" + gesture "Скрестить ноги"; "бег? неожиданно…" + gesture "Растянуться". Gesture names live in **GoodsName.fmg** (Скрестить ноги = goods 9033, Растянуться = goods 9037). In the message blob, neither the goods id (9033/9037) nor its goods-rank (33/37) appears. The only small, per-gesture-message-varying field is **blob+0x1C = 26 (Скрестить ноги) / 7 (Растянуться)** → leading candidate for the gesture, almost certainly the **EquipParamGesture row id** (a different numbering from the goods id), NOT the goods id. CONFIDENCE: medium - confirm by parsing regulation `EquipParamGesture` (does Растянуться=row 7, Скрестить ноги=row 26?). Text content for both decoded correctly via §8.4.
- **blob+0x0A** (47708 / 26459 / 52831 across three messages) stays UNKNOWN - not a gesture, not a goods id, not an FMG id; varies per message. Likely a content hash / dedup token. Non-essential for rendering.
- Blob tail (`+0x20`…) is a run of `0xFFFF` (empty fixed-size slots).

**AOBs (build 2026-05-29; for update-resilience):**
- `BloodMessageIns::ctor` - **UNIQUE** (1 hit): `97 52 0B 40 01 00 00 00 48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50`
- `FN create (op 0x13)` @ ~0x1df4140 - near-unique (2 hits), ends `... C7 84 24 00 04 00 00 13 00 00 00` (the opcode constant).
- `dispatch_router` (0x1deffa0), `BloodMessageInsMan::ctor` (0x1b8940), `GetList_worker` (0x1df3be0) have generic MSVC prologues (63/15/6 hits) → **do NOT AOB by prologue**. Resolve them by **RTTI mangled-name strings** in `.rdata` (`.?AVBloodMessageInsMan@CS@@`, `.?AVBloodMessageIns@CS@@`, `FromNet::FNBloodMessageImpl`, `UBLOODMSG_MSG_PARAM@CS@@`) → TypeDescriptor → CompleteObjectLocator → vtable → ctor via the vtable-store xref. RTTI/.rdata did NOT move on the 2026-05-29 update, so this is the patch-stable anchor (same method that produced every RVA here).

**Automation (no manual walking; respect rate limit to avoid ban):**
- **MITM is NOT viable** - traffic is WSS + the waygate sodium/chacha session crypto, so a proxy sees ciphertext (keys are per-session). Ruled out.
- **Recommended: in-process driver DLL.** Enumerate every `PlayRegionArea` (we already have the area list from the mod's map data), and for each programmatically trigger the game's own fetch - either call `FNBloodMessage` GetList (resolved via RTTI) with `this`+area, or enqueue `BloodMessageDownloadJob`. Let the game's async job + crypto do the network; hook the GetList **response consumer** to capture the resulting `CSNetBloodMessageDbItem`s, decode (§8.4), and append to `bank.json` (dedup by `ObjectIdentifier`). GetList returns ~64 near a random pivot per region ⇒ call each area several times.
- **Rate limiting (ban-safety):** throttle to a human-plausible cadence (≈1 GetList every 2-5 s), randomize pivots, never spam Create/Evaluate, mimic normal client timing. A full sweep then runs unattended over ~hours but with zero manual interaction.
- **Pure-offline alternative:** once harvested, replay the bank by populating `CSNetBloodMessageDb`/spawning `BloodMessageIns` from `bank.json` with NO network at all.

### 8.7 Phantom appearance is NOT in the list blob - it's in GetBloodMessageDetail

Good catch: reading a gesture message spawns a translucent **phantom dressed as the author**, performing the gesture. Checked whether that appearance (armor/weapon ids) is in the on-ground/list blob: the `+0x14/+0x18/+0x1A` fields (15535, 50258, 39443 / 4653, 18949, 39375) do NOT resolve to ProtectorName/WeaponName ids (real protector ids are ×100: 200, 1000, 1100…). So **appearance is NOT in the lightweight GetList entry.**
- By design the system is two-tier: **GetList** returns lightweight entries (text ids + gesture + position + rating + identifier); **GetBloodMessageDetail** (`RequestGetBloodMessageDetailParams`, op among the FN methods) is fetched on INTERACTION and carries the full payload incl. the **phantom equipment** that dresses the ghost. The `0xFFFF` run at blob `+0x20…` is most likely the empty appearance/equipment slots, filled by the Detail fetch when the message is read.
- The `+0x14/+0x18/+0x1A` non-armor fields are then the identifier/rating/hash, and `+0x1C` (26/7) the gesture (EquipParamGesture row) - all list-level metadata.
- **Consequence for a faithful bank:** to replay phantoms in the author's gear we must also capture the **Detail response** (intercept it on read, or read the populated detail/phantom-ChrIns equipment after interacting). For plain text+gesture display (the core goal) the GetList blob is sufficient; appearance is an optional fidelity add-on via the Detail path.
- Next experiment to crack appearance: read one gesture message in-game (phantom appears) and scan for the now-populated Detail struct / phantom ChrIns equipment, or hook the GetBloodMessageDetail response.

### 8.8 Appearance crack attempt (live, message read + phantom shown) - needs the Detail payload, not memory archaeology

Read "красиво…" (BloodMsg 41028) + gesture "Скрестить ноги"; phantom sat down, shield on back. Tried to locate the phantom's gear in memory:
- Equipment ids are large u32 (ProtectorName 200..7004200; WeaponName 1000..68510000; shields base 30000000+, e.g. greatshield "Деревянный большой щит"=32290000). So appearance is a u32 loadout array, separate from the small text-ids.
- **Loose memory scan is unreliable here:** the equipment id space is so dense that almost any `3xxxxxxx`/`Nxxxxxx` u32 "resolves" to some weapon/armor when rounded to base - produced false clusters (e.g. a bogus `32297029`→greatshield match). No clean contiguous ChrAsm loadout block was isolated near the message.
- **Conclusion:** the phantom appearance is the full **ChrAsm loadout of a separate ghost ChrIns**, populated from the **GetBloodMessageDetail** response on interaction. Cracking it cleanly needs a structured anchor, NOT memory archaeology:
  1. **Hook the GetBloodMessageDetail response** (WinHttpWebSocketReceive, or the Detail consumer) → the raw appearance payload in exact serialized form (best - also gives the bank-storable format).
  2. Or find the phantom **ChrIns** and read its ChrAsm/equip module (needs the ghost ChrIns vtable).
- **Priority:** appearance is OPTIONAL fidelity. Core offline display = text (§8.4) + gesture (§8.6). Appearance is a later add-on via the Detail path.

### 8.9 FULL message+appearance layout CRACKED (CSNetBloodMessageDbItem) - live-confirmed

The whole message (text + position + gesture-link + phantom appearance) lives in ONE object: **CSNetBloodMessageDbItem** (vtable = base+0x29c9e88). The "blob" is embedded here, not a separate allocation. Live-confirmed on the read phantom "красиво…" (BloodMsg 41028): two different DbItems for the same text carried DIFFERENT gear/face ⇒ it is the **message author's real loadout**, NOT a static CharaInitParam preset (the earlier CharaInitParam match was a false lead - coincidental).

**CSNetBloodMessageDbItem field map (u16 unless noted; offsets from object base):**
```
+0x1C..+0x2B  world position: float X, Y, Z (+ a 4th float)
+0x2C  u16 templateId1        (FMG 10000-11999; idx = id-10000)
+0x2E  u16 token              (per-message SALT/dedup id - NOT content, NOT a hash of the ids;
                               uniform-random, varies for identical text; ignore for rendering. == old "+0x0A")
+0x30  u16 wordId1            (primary content; 30000-41999. 40000-41999 = standalone phrase)
+0x32  u16 conjunctionId      (0 = 1-part; 50000-50012 for 2-part: 50000 "и тогда", 50003 "так что")
+0x34  u16 templateId2        (0 if 1-part)
+0x36  u16 wordId2            (0 if 1-part)
+0x38  word/type count        +0x40 word id (aux)   +0x48 online-message id
--- appearance (populated by GetBloodMessageDetail on interaction) ---
+0x4C..+0x60  u32 weapons[6]   (RightHand[0..2], LeftHand[0..2]; the on-back shield is a LeftHand slot)
+0x64..+0x70  u32 ammo[4]      (arrow1, bolt1, arrow2, bolt2)
+0x7C head  +0x80 body  +0x84 arms  +0x88 legs   (u32 ProtectorParam ids)
+0xBC  'FACE' (0x45434146) facegen/character-creator block  (author's face/body sliders)
```
Live decode example (msg 41028): weapons "Посох королевы полулюдей / Рапира Рожера / Факел", armor "Повязка простолюдина / Туника из Кайдена / …", FACE block present. Equipment ids resolve strictly via ProtectorName/WeaponName (weapon id = base + affinity*100 + reinforce; base ×10000).

**Network:** GetBloodMessageDetail = opcode **0x14**, `FNBloodMessageImpl` vtable slot 6 @ RVA 0x1df41a0. FN methods: 0x1df3d60(GetList 0x12), 0x1df3ca0(Create 0xF), 0x1df4140(Evaluate 0x13), 0x1df41a0(GetDetail 0x14), 0x1df4240(Remove 0x10), 0x1df3e00(Reentry 0x11). Router 0x1deffa0 → table .data 0x47ff410[opcode]. **The response-parse/phantom-build handlers are in the obfuscated 2nd .text (0x4c0e000+) - not statically disassemblable**, so to capture appearance, READ the Detail-populated DbItem (above) rather than the parser.

**Gesture:** keyed by the **EquipParamGoods id** (9033 "Скрестить ноги", 9037 "Растянуться") → GestureParam (9033→row 91 msgAnimId 80910; 9037→row 95 msgAnimId 80950). The earlier blob+0x1C=26/7 candidate was REFUTED (not a GestureParam row/goods/sortId). GESTURE_PARAM_ST has NO armor field - the "gesture needs matching armor" is purely the author's own loadout (the appearance block above), the gesture row doesn't enforce gear. Where the gesture goods id is stored in the DbItem is the one small remaining unknown (likely a field near the appearance block, populated by Detail).

**This fully closes the data model:** a bank entry can store text (5 ids) + position + gesture goods id + the full appearance (weapons[6]/ammo[4]/armor[4] param ids + FACE block) → a faithful phantom (author's gear, face, gesture) reproducible offline.

### 8.10 Gesture field FOUND - DbItem+0x52 = GestureParam row id

Live-confirmed on the read phantom (msg 41028, gesture "Скрестить ноги"): **`CSNetBloodMessageDbItem+0x52` (u16) = 91 = the GestureParam ROW id for goods 9033** (Растянуться would be 95). Exact match to the activated gesture's GestureParam row ⇒ this is the gesture field (not the goods id, not the refuted +0x1C). Populated on Detail/interaction (the gesture plays when the message is read). Full chain: `DbItem+0x52 (row 91)` → `GestureParam.itemId = 9033` → `GoodsName "Скрестить ноги"` + `msgAnimId 80910` (the played animation). `0`/sentinel = no gesture. **The message data model is now 100% mapped** (text + position + gesture + full appearance gear/face).

### 8.11 CORRECTION - appearance(+0x4C)/gesture(+0x52) offsets are NOT confirmed

Building the harvester forced a reconciliation that overturns §8.9/§8.10's appearance/gesture offsets:
- On 4 FACE-present DbItems read in a single snapshot, `+0x52` = 61/137/61/61 (not 91), and `+0x4C..` did NOT contain a clean weapon array. So **gesture@+0x52 = 91 and weapons@+0x4C were COINCIDENCES** - products of volatile-heap single samples + an over-loose equipment resolver (rounding any `Nxxxxxx` u32 to a base id false-positives). The earlier "live-confirmed" gear names ("Посох королевы полулюдей" etc.) were likely loose-resolution noise too.
- **Still SOLID (confirmed across dozens of items):** text fields `+0x2C..+0x36`, position `+0x1C`, localId `+0x40`, and FACE-present (`u32@+0xBC == 'FACE'`). The appearance DOES exist in the DbItem (FACE marker proves it) - but the exact equipment slot offsets and the gesture field are **not yet pinned**.
- **Correct way to pin them (planned):** the MsgBank harvester captures the RAW bytes of `+0x38..+0xC0` per record. With many records accumulated (varied known messages/gestures/gear), the true layout falls out statistically offline (stable columns vs noise) - no more single-sample archaeology. Resolution must use STRICT equipment matching (exact param id, no loose base-rounding).

Net: the message-text + position + identifier model is production-solid; appearance/gesture layout is pending the raw-capture analysis. See MsgBank/ (the harvester).

## EventTextForMap (600M band) - reserved, not implemented

Marker labels are offset-encoded by source FMG: weapon +100M, protector +200M,
accessory +300M, gem +400M, goods +500M, **EventTextForMap +600M**, NpcName +700M,
ActionButtonText +800M, TutorialTitle +900M, BloodMsg +950M (see the encoding
comment in `src/goblin_messages.cpp`).

**EventTextForMap (+600M) is the only band no marker uses.** A scan of all four
profiles' baked `goblin_map_data.cpp` (textId1..8 positions) found ZERO markers
with a textId in [600M, 700M). EventTextForMap holds map-event strings like
"Closed with an imp's seal", but our interactable markers label from
ActionButtonText instead ("Examine seal" / "Light flame", +800M, set in
`generate_seal_puzzles.py`) or from PlaceName - never EventTextForMap.

So the runtime classifier no longer carries a 600M branch (removed 2026-06-19; it
was dead defensive code + a never-firing warning). The GoodsName branch is bounded
to `< 600M` so a stray 600M id can't be misclassified as goods - it simply stays
unresolved (`?PlaceName?`), the same outcome the dead branch produced.

**If EventTextForMap is ever needed** (a future profile emits +600M textIds):
EventTextForMap lives in the **menu msgbnd** bank (FMG slots 34 / 367 / 467), a
separate `MsgRepository` bank from the item msgbnd the copier walks. ActionButtonText
(slot 32) happens to be reachable via the global-BND-id slot array directly, but
34/367/467 need the menu-bank pointer to be located first. Add a `+600M ->
event_text_ids_needed` branch back in the classifier, find the menu-bank pointer,
and copy those slots the same way `copy_fmg_*` handles the item banks.
