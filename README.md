# GameplayMCPToolset

An editor-only Unreal Engine plugin that lets an AI assistant **verify gameplay itself** over MCP (Model Context Protocol) instead of handing features back untested. It can inspect a running Play-In-Editor (PIE) session, read and poke live actors, call functions, read animation state, drive Enhanced Input like a player would, run console commands, read the output log, trigger Live Coding, edit Blueprint components, and capture the game view.

It works with any project. Nothing in it is tied to a specific game, character or asset.

The tools are registered with Epic's **Toolset Registry**, and Epic's **Unreal MCP** plugin serves them to MCP clients such as Claude Code, Claude Desktop or Cursor.

---

## Requirements

| | |
|---|---|
| Engine | Unreal Engine **5.8** (built and tested against 5.8.3, Win64). Toolset Registry and Unreal MCP are *experimental* engine plugins, so their APIs may change in later versions. |
| **Toolset Registry** plugin (`ToolsetRegistry`) | Ships with UE 5.8 under `Engine/Plugins/Experimental/ToolsetRegistry`. Disabled by default. Provides the `UToolsetDefinition` base class the tools are built on. |
| **Unreal MCP** plugin (`ModelContextProtocol`) | Ships with UE 5.8 under `Engine/Plugins/Experimental/ModelContextProtocol`. Disabled by default. Runs an MCP server (HTTP) inside the editor and exposes every registered toolset to MCP clients. |
| **Enhanced Input** plugin (`EnhancedInput`) | Ships with the engine, enabled by default in new projects. |
| Platform | Editor builds only. The module is `Editor` type with `TargetAllowList: ["Editor"]`, so it never ships in a packaged game. |

`GameplayMCPToolset.uplugin` declares these plugins as dependencies, so enabling GameplayMCPToolset enables them as well. The module links against `ToolsetRegistry`; Unreal MCP discovers toolsets through the registry at runtime, so there is no link dependency on `ModelContextProtocol`.

## Setup

1. Put the plugin in your project's `Plugins/` folder, as a git submodule or a copy:
   ```bash
   git submodule add https://github.com/BatSasha52/GameplayMCPToolset.git Plugins/GameplayMCPToolset
   ```
2. Enable it in your `.uproject` (or in **Edit → Plugins**):
   ```json
   "Plugins": [
     { "Name": "GameplayMCPToolset", "Enabled": true }
   ]
   ```
3. Regenerate project files and build the editor target. The plugin is C++, so a Blueprint-only project needs to be converted to C++ first.
4. Start the editor. The log should show one line per toolset, e.g.
   ```
   LogToolsetRegistry: Display: Registering Toolset GameplayMCPToolset.GameplayPIEToolset
   ```
5. Start the MCP server and connect your client. The server is configured by the Unreal MCP plugin (**Editor Preferences → Model Context Protocol**: port, URL path, *Auto Start Server*), not by this plugin. The default endpoint is `http://127.0.0.1:8000/mcp`.

## Conventions every tool follows

- **Uniform result.** Every tool returns `{ "success": bool, "result": { ... }, "error": "..." }`. On failure `result` is `{}` and `error` says what to fix.
- **Game thread only.** Tools refuse to run off the game thread, and never block it waiting on time.
- **PIE tools need a running session.** Without one they fail with *"No Play-In-Editor session is running…"*. Only `pie_start` and `pie_stop` start or stop PIE.
- **PIE writes stay in PIE.** Set and call tools only act on objects that live in the PIE world, never on editor-world actors or assets. Property paths that would follow a reference into an asset are refused for writes.
- **Parse first, then write.** `pie_set_property` imports the value into a scratch copy; the live value is only touched if the whole value parsed. Unknown struct members, trailing text (`12abc`) and non-integers for integer properties are rejected.
- **Editor assets.** Edits run inside an `FScopedTransaction` with `Modify()` (Ctrl+Z works), only mark packages dirty, never save on their own, never delete assets, refuse assets outside `/Game` and refuse name collisions.
- **Optional string parameters** use explicit defaults (`"*"`, `"none"`, `"{}"`) instead of empty strings, because the UE 5.8 Toolset Registry only treats a parameter as optional if its schema carries a non-empty default. Empty strings are still accepted when passed explicitly.

## Tools

Toolsets appear to MCP clients as `GameplayMCPToolset.<Toolset>`. Every parameter is documented in the tool schema the client receives. See [Calling tools through Unreal MCP](#calling-tools-through-unreal-mcp) for how to name a tool when calling it.

### Addressing live objects (PIE tools)

- **Actors** by object name (`BP_Player_C_0`), editor label (`TargetCube`), full object path, or a shortcut: `@pawn`, `@controller`, `@camera`, `@hud` (append a local player index, e.g. `@pawn1`), `@gamemode`, `@gamestate`.
- **Components** (`component` parameter) by component name (`CharMoveComp`), by the actor property that holds it (`CharacterMovement`), or by class name when unique (`CharacterMovementComponent`). `AnimInstance` targets the first skeletal mesh's anim instance, `<MeshComponent>/AnimInstance` a specific one.
- **Property paths**: `Health`, `Stats.Level` (struct members), `Inventory[2].Count` (array index), `Inventory[Sword].Count` (array *name key*: matches an element's `Name`/`Id`/`Key`/`Tag` field, or an object's name), `Ammo[Rifle]` (map key), `CharacterMovement.MaxWalkSpeed` (follows object references).
- **Values**: plain text for strings/names; `true`/`false`; numbers; enum names; Unreal text like `(X=1,Y=2,Z=3)`; JSON like `{"Level": 7}` (patches only the listed struct fields) or `["Dash","Roll"]`; object references as `none`, an asset path, or a PIE actor name/shortcut (`Actor:Component` for a component).

### Phase 1 — Play-mode inspector (`GameplayPIEToolset`)

| Tool | Purpose |
|---|---|
| `pie_get_status()` | Running or not, world, map, net mode, paused, game time, local players and their pawns. Never fails when PIE is off. |
| `pie_start()` | Start PIE in the active level viewport (or a new window). Starts on the next tick; poll `pie_get_status`. |
| `pie_stop()` | End the running PIE session. |
| `pie_list_actors(class_filter="*", label_filter="*", tag_filter="*", limit=200)` | Live actors with name, label, class, location, rotation, GUID and path. Class filter matches parent classes and Blueprint `_C` names, or takes a class path. Wildcards allowed. |
| `pie_find_actor(actor)` | Resolve one actor by name, label, path or shortcut; adds parent classes, owner, controller, component count. |
| `pie_get_property(actor, property_path, component="none")` | Read a value by property path as JSON and as Unreal text. |
| `pie_set_property(actor, property_path, value, component="none")` | Write a value on a live PIE object (raw write: no setters, OnRep or PostEditChange). |
| `pie_list_properties(actor, component="none", name_filter="*", include_all=false, max_value_length=200)` | Reflected properties with current values. |
| `pie_list_functions(actor, component="none", name_filter="*", include_all=false)` | Callable UFUNCTIONs with parameter names, types, directions and C++ defaults. |
| `pie_call_function(actor, function_name, args="{}", component="none")` | Call a UFUNCTION with named JSON args; returns the return value and out params. C++ default arguments and `WorldContext` are filled in. Latent functions are refused. |
| `pie_get_anim_state(actor, component="*", include_variables=true, include_curves=true)` | Active state + time in state for every state machine, playing montages (section, position, play rate, weight), curve values, and the anim Blueprint's own variables. |
| `pie_get_component_tree(actor)` | Scene component hierarchy with relative and world transforms, plus non-scene components. |

### Phase 2 — Input simulation (`GameplayInputToolset`)

Input is injected through the local player's `UEnhancedInputLocalPlayerSubsystem` (`Start/Update/StopContinuousInputInjectionForAction`), the clean injection route Enhanced Input provides in 5.8. The action's own modifiers and triggers run exactly as for a real key, gameplay bindings fire normally, and no mapping context or UI automation is involved.

Every input tool **returns a `job_id` at once**; the job then runs on the editor's core ticker over the following frames. Poll `input_status` until `state` is `completed` before checking the result in game. Durations are **game seconds** (they respect pause and time dilation). Every press and every release lasts at least one frame, so triggers always see both edges. One job at a time may drive a given action for a given player. Cancelling a job, or PIE ending, releases everything it holds.

| Tool | Purpose |
|---|---|
| `input_list_actions(name_filter="*")` | Input Action assets with their value types (`boolean`, `axis1d`, `axis2d`, `axis3d`). |
| `input_hold_action(action, duration=1.0, value="1", player_index=0)` | Hold an action, then release. Values: `1`, `0.5`, `0,1`, `0,0,1`, `(X=0,Y=1)`, `[0,1]`. |
| `input_tap_action(action, count=1, interval=0.1, hold_time=0.05, player_index=0)` | Tap N times: press for `hold_time`, release, wait `interval`, repeat. |
| `input_set_axis(action, value, end_value="none", duration=1.0, player_index=0)` | Drive a 1D/2D/3D action with a constant value or a linear ramp to `end_value`, then release. |
| `input_sequence(steps, player_index=0)` | One job from a JSON array of `hold` / `tap` / `axis` / `wait` steps. `"wait": false` on a step starts the next one immediately, so steps can overlap (move while jumping). |
| `input_status(job_id="*")` | State, elapsed vs expected time, actions held, and a timeline of every press and release. `latest` = newest job. |
| `input_cancel(job_id="*")` | Cancel one job or all and release their actions. |

`input_sequence` example:

```json
[ { "type": "hold", "action": "IA_Move", "value": "0,1", "duration": 1.5, "wait": false },
  { "type": "wait", "duration": 0.5 },
  { "type": "tap",  "action": "IA_Jump", "count": 2, "interval": 0.2 },
  { "type": "axis", "action": "IA_Look", "value": "0,0", "end_value": "1,0", "duration": 0.5 } ]
```

### Phase 3 — Editor commands and building (`GameplayEditorToolset`)

| Tool | Purpose |
|---|---|
| `editor_run_console_command(command, target="auto")` | Run a console command and return its output plus every log line it produced. `auto` runs it through the PIE local player's controller when PIE is running (so cheats and game commands such as `slomo` work), otherwise in the editor. `pie` / `editor` force a target. Reports `recognized: false` for unknown commands. |
| `editor_get_recent_log(max_lines=100, log_category="*", min_verbosity="log", contains="*", after_id=0)` | Tail of the output log with category (wildcards), verbosity and text filters. Pass the returned `latest_id` as `after_id` to read only new lines. Holds the last 10000 lines logged after the plugin loaded. |
| `editor_live_coding_compile()` | Start a Live Coding compile (Ctrl+Alt+F11) **without blocking** and return a `job_id`. |
| `editor_live_coding_status(job_id="latest")` | `compiling` / `completed`, result (`success`, `no_changes`, `failure`, `cancelled`, `unknown`), compiler `errors` and `warnings` with file and line, and the Live Coding output. |

**Refused console commands.** The first word of every `|`-separated part is checked; these are always refused because they quit the editor or end PIE, crash on purpose, run arbitrary scripts or write packages:
`quit`, `exit`, `disconnect`, `debug` (crash/assert/hang family), `crash`, `exec`, `py`, `python`, and `obj savepackage`. Use `pie_stop` to end PIE.

**Live Coding route.** `ILiveCodingModule::Compile(ELiveCodingCompileFlags::None, …)` returns at once with `InProgress` (the `WaitForCompletion` flag would block the game thread, so it is not used). The job watches `IsCompiling()` and the result line Live Coding logs when it finishes, then reads compiler diagnostics from UnrealBuildTool's log, which lives at `%LOCALAPPDATA%/UnrealBuildTool/Log.txt` for installed engines and `Engine/Programs/UnrealBuildTool/Log.txt` for source builds.

### Phase 4 — Blueprint components (`GameplayBlueprintToolset`)

Edits an Actor Blueprint's Simple Construction Script through `USubobjectDataSubsystem`, the same layer the Blueprint editor's Components panel uses. Components are identified by **name** (their variable name, e.g. `Mesh`, `DefaultSceneRoot`), never by index. Each one has a `source`:

- `added` — in this Blueprint's construction script: can be renamed, reparented and removed.
- `inherited` — added by a parent Blueprint: edit it in that Blueprint.
- `native` — declared in C++ (e.g. a Character's `CapsuleComponent`, `Mesh`, `CharacterMovement`): can be used as a parent, never renamed, moved or removed.

Every change is one undoable transaction (`FScopedTransaction` + `Modify()`), then the Blueprint is compiled and marked dirty. Nothing is written to disk until `bp_save_asset`. Only Blueprints under `/Game` can be changed.

| Tool | Purpose |
|---|---|
| `bp_list_components(blueprint_path)` | Every component with class, parent, children, source, scene/root flags, socket, and what the editor allows (`can_rename`, `can_remove`, `can_reparent`). |
| `bp_add_component(blueprint_path, component_class, name, parent="none")` | Add a component. Class by name (`StaticMeshComponent`, `PointLight`), `/Script/…` path or Blueprint component path. `parent="none"` attaches scene components to the existing scene root (native, inherited or added) and never replaces it; non-scene components take no parent. Rejects abstract, deprecated and non-spawnable classes and used names. |
| `bp_remove_component(blueprint_path, name)` | Remove an `added` component. Refuses inherited and native components, and components that still have children. |
| `bp_rename_component(blueprint_path, name, new_name)` | Rename an `added` component (name validated by the editor's own rules). |
| `bp_reparent_component(blueprint_path, name, new_parent)` | Attach an `added` scene component to another scene component (added, inherited or native), keeping its relative transform. Refuses the scene root, cycles and non-scene components. |
| `bp_save_asset(blueprint_path)` | Save the asset. The only tool in this plugin that writes asset files. |

### Phase 5 — Game viewport screenshot (`GameplayViewportToolset`)

| Tool | Purpose |
|---|---|
| `game_capture_screenshot(width=1280, height=720, file_name="auto", overwrite=false)` | Capture the PIE game view at the requested resolution and return a `job_id` at once. The PNG goes to `<Project>/Saved/Screenshots/GameplayMCP/<file_name>.png`; the result carries the absolute `file_path` and resolution, never the image data. |
| `game_screenshot_status(job_id="latest")` | `pending` / `completed` / `failed`, file path, size in pixels and bytes. |

It uses the engine's high-res screenshot path (`GScreenshotResolutionX/Y` + `FViewport::TakeHighResScreenShot()`): on the next frame the PIE game viewport client renders into an offscreen viewport of that size and hands the pixels to `UGameViewportClient::OnScreenshotCaptured`, which this plugin writes as PNG. There is no editor UI in the image. Slate widgets (including UMG) are not captured either; HUD canvas drawing is. The camera's field of view is kept, so a different aspect ratio than the viewport shows more or less of the scene. File names may only use letters, digits, `_`, `-` and `.`; existing files are not replaced unless `overwrite=true`.

## Calling tools through Unreal MCP

With Unreal MCP's default tool search (`bEnableToolSearch`), the client sees three meta-tools: `list_toolsets`, `describe_toolset` and `call_tool`. Call a tool like this:

```json
{ "toolset_name": "GameplayMCPToolset.GameplayPIEToolset", "tool_name": "pie_get_status", "arguments": {} }
```

`tool_name` must be the **short** name. `describe_toolset` prints fully qualified names such as `GameplayMCPToolset.GameplayPIEToolset.pie_get_status`, but passing that as `tool_name` fails with `Unknown tool …`. This is engine behaviour in UE 5.8.3, not this plugin's, and is not worked around here; `call_tool`'s own description says to pass the name "without toolset prefix". `Tests/mcp_bridge_test.py` checks both behaviours over HTTP.

## Example prompts

**Phase 1 — Play-mode inspector**
> Start PIE, find my player pawn, and tell me its health, its CharacterMovement max walk speed and which locomotion state its anim Blueprint is in. Then set Health to 10, call `ApplyDamage` with 5 and confirm the pawn reports 5.

**Phase 2 — Input simulation**
> In PIE, hold `IA_Move` forward for 2 seconds, then double-tap `IA_Jump`. Wait for the input job to finish and tell me how far the pawn moved, whether it left the ground, and which anim state it was in while moving.

**Phase 3 — Editor commands and building**
> I changed `AMyCharacter::Jump` in C++. Live-compile it, show me any compiler errors, and if it succeeded start PIE, run `slomo 0.25`, tap jump once and give me the last 20 warnings from the output log.

**Phase 4 — Blueprint components**
> In `/Game/Blueprints/BP_Door`, add a `BoxComponent` called `OpenTrigger` under `DoorFrame` and a `PointLight` called `Lamp` under the scene root, rename `StaticMesh1` to `Panel`, then list the components and save the Blueprint.

**Phase 5 — Game viewport screenshot**
> Start PIE, hold `IA_Move` forward for one second, then take a 1920x1080 screenshot of the game view called `after_move` and tell me where the file is.

**All together**
> Implement the double-jump I described, live-compile, start PIE, double-tap `IA_Jump`, and verify with `pie_get_property` that `JumpCurrentCount` reached 2 and with `pie_get_anim_state` that the anim Blueprint entered `DoubleJump`. Attach a screenshot of the apex and stop PIE.

## Testing

Everything is tested end to end in a throwaway project (not part of any game), with the plugin loaded in a real editor:

- `Tests/TestProject/` is a minimal C++ project: a game mode and a trivial character (`AGMCPTestCharacter`) with Enhanced Input bindings that record what they receive, nested struct/array/map/object properties, and a couple of `BlueprintCallable` functions. Its `Target.cs`/`Build.cs` files are stored as `*.cs.in`, because UnrealBuildTool scans every `*.cs` file under a plugin folder and real ones would break the projects that use this plugin.
- `Tests/setup_test_project.ps1` materialises that project (default `%TEMP%\GMCPTest`) with this plugin linked in as a junction. If the sibling AnimMCPToolset plugin is available it is linked too, and the test uses it **only** to build a fixture Animation Blueprint with an Idle/Walk state machine. GameplayMCPToolset does not depend on it.
- `Tests/gameplay_smoke_test.py` runs inside the editor (`UnrealEditor.exe GMCPTest.uproject -ExecutePythonScript=<plugin>/Tests/gameplay_smoke_test.py -unattended -nosplash -windowed`). It creates fixtures (input actions, montage, level, Blueprints), starts PIE with `pie_start`, steps through every tool on Slate ticks so the game keeps running between calls, stops PIE, runs the editor-only checks and quits. Every call goes through the Toolset Registry, the same entry point Unreal MCP uses. Results go to `<Project>/Saved/gmcp_smoke.txt`.
- `Tests/mcp_bridge_test.py` (plain Python 3) talks to the **real Unreal MCP HTTP server** while the editor test is paused in PIE (`GMCP_MCP_BRIDGE=1`): `initialize`, `tools/list`, `list_toolsets`, `describe_toolset` and `call_tool` for PIE, input, console and screenshot tools, the error envelope, and the dotted-name failure.

**Results for 0.1.0** (UE 5.8.3, Win64): `RunUAT BuildPlugin` compiles with 0 errors and 0 warnings. 269 checks with 0 failures in the editor test, and 14 with 0 failures over MCP HTTP. Highlights of what is checked against real game state rather than just tool output:

- Holding `IA_Move` moves the pawn and switches the anim Blueprint from Idle to Walk while held, then back to Idle.
- Taps produce exactly N `Started`/`Completed` events in game, and an axis ramp delivers mid values.
- Cancelling a job, or PIE ending mid-hold, releases the action (the game sees `Completed`).
- `slomo` through the console tool changes the PIE world's time dilation.
- A deliberately broken source file makes Live Coding report `failure` with the compiler error (`file(line,col): error C2059`); after the file is restored it compiles cleanly.
- Blueprint edits compile, mark dirty without saving, and `TRANSACTION UNDO` reverts them.
- Screenshots have the requested pixel size and no editor UI.

### Not tested

These could not be covered by the scripted run and have only been reasoned about against the 5.8.3 sources:

- A real MCP client application (Claude Code, Cursor…). The bridge test speaks MCP JSON-RPC over HTTP itself.
- Multiplayer PIE (several clients, listen/dedicated server) and `player_index` > 0 actually driving a second local player. The PIE tools act on the first PIE instance.
- Simulate-In-Editor sessions.
- Input actions with their own triggers and modifiers (e.g. a Hold trigger) and `axis3d` actions. The fixtures use plain boolean/1D/2D actions.
- `pie_call_function` refusing a latent function (the fixture has none); RPCs.
- HDR viewports (the `OnHDRScreenshotCaptured` conversion path), and the screenshot timeout when the editor window is minimized.
- Live Coding when it is disabled or unavailable, and the UBT log location of source-built engines.
- Undo of rename, reparent and remove (undo is checked for add). Blueprint component classes from `/Game` as `component_class`, child actor components, and sockets.
- Platforms other than Win64.

## Limitations and notes

- **Experimental engine APIs.** Toolset Registry and Unreal MCP are experimental in UE 5.8.3 and may change.
- **First PIE instance only.** With several PIE clients, the PIE tools see the first instance (`GetPIEWorldContext(0)`).
- **Raw property writes.** `pie_set_property` writes memory directly: no setters, `OnRep`, `PostEditChangeProperty` or construction script reruns. Use `pie_call_function` for setters. Fixed-size C arrays are read-only. Sets are not supported in paths.
- **Input timing is game time**, so a hold lasts longer while the game is paused or slowed down. A job is limited to 300 game seconds. While PIE is paused, input jobs keep waiting.
- **Console output.** Many commands print to the log rather than the output device; both are returned. Some take effect only on the next frame.
- **Log tail** only holds lines logged after the plugin module loaded (it is a `PostEngineInit` module), up to the last 10000.
- **Screenshots** use the engine's shared screenshot delegate for one frame; another screenshot taken at that exact moment by something else would be routed here. Only one capture runs at a time.
- **Live Coding** needs a Win64 editor with Live Coding enabled and a C++ project. Diagnostics come from UnrealBuildTool's log, which Epic's own LiveCodingToolset in 5.8.3 reads from `Engine/Programs`, the wrong place for installed engines. This plugin reads `%LOCALAPPDATA%/UnrealBuildTool/Log.txt` for installed engines, as UBT itself does.
- **Running the test while another editor of the same engine has Live Coding active**: UnrealBuildTool refuses to build any editor target of that engine install. Build the test project with `-NoHotReloadFromIDE` only if that other editor does not load the binaries you are building.

## Changelog

**0.1.0** — first release: play-mode inspector, Enhanced Input simulation, console/log/Live Coding, Blueprint component tools, game viewport screenshots.

## License

MIT. See [LICENSE](LICENSE).
