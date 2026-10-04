# GameplayMCPToolset

An editor-only Unreal Engine plugin that lets an AI assistant **verify gameplay itself** over MCP (Model Context Protocol) instead of handing features back untested. It can inspect a running Play-In-Editor (PIE) session, read and poke live actors, call functions, read animation state, and drive Enhanced Input like a player would.

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

## License

MIT. See [LICENSE](LICENSE).
