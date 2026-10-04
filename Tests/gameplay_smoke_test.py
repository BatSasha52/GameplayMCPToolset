# End-to-end test for GameplayMCPToolset. Runs inside a real editor (not a commandlet) because it
# needs a Play-In-Editor session. Every tool is called through the Toolset Registry, the same entry
# point Unreal MCP uses. Set up the throwaway project with Tests/setup_test_project.ps1, build its
# editor target, then:
#
#   UnrealEditor.exe <TestProject>/GMCPTest.uproject -ExecutePythonScript=<plugin>/Tests/gameplay_smoke_test.py -unattended -nosplash -windowed -ResX=1280 -ResY=720
#
# The script creates fixtures under /Game/GMCPTest, starts PIE with pie_start, steps through the checks
# on Slate ticks (so the game keeps running between calls), stops PIE, runs the editor-only checks
# and quits the editor. Results go to $GMCP_SMOKE_OUT (default <Project>/Saved/gmcp_smoke.txt);
# the last line is the summary.
#
# With GMCP_MCP_BRIDGE=1 the script pauses while PIE is running, writes <Saved>/gmcp_mcp_ready.txt and
# waits for <Saved>/gmcp_mcp_done.txt, so Tests/mcp_bridge_test.py can call the tools over HTTP
# through the real Unreal MCP server.
import json, os, time, traceback, unreal

SAVED = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
OUT_PATH = os.environ.get("GMCP_SMOKE_OUT") or os.path.join(SAVED, "gmcp_smoke.txt")
OUT = open(OUT_PATH, "w", encoding="utf-8")
STATS = {"ok": 0, "fail": 0, "skip": 0}


def log(msg):
    OUT.write(msg + "\n"); OUT.flush()
    unreal.log("[gmcp_smoke] " + msg)


def check(cond, label, detail=""):
    STATS["ok" if cond else "fail"] += 1
    log("%s %s%s" % ("OK  " if cond else "FAIL", label, (" -- " + str(detail)) if detail != "" else ""))
    return cond


def skip(label):
    STATS["skip"] += 1
    log("SKIP " + label)


SCHEMAS = {}


def refresh_schemas():
    SCHEMAS.clear()
    for s in json.loads(unreal.ToolsetRegistry.get_all_toolset_json_schemas()):
        SCHEMAS[s["name"]] = s


def has_toolset(short):
    return ("GameplayMCPToolset." + short) in SCHEMAS


def call(toolset, tool, **args):
    r = unreal.ToolsetRegistry.execute_tool("GameplayMCPToolset." + toolset, tool, json.dumps(args))
    if not r.is_complete:
        return {"success": False, "error": "not complete synchronously"}
    if r.error:
        return {"success": False, "error": "registry error: " + r.error}
    return json.loads(r.value)["returnValue"]


def ok(toolset, tool, **args):
    env = call(toolset, tool, **args)
    check(env.get("success"), tool, "" if env.get("success") else env.get("error"))
    return env.get("result") if env.get("success") else None


def expect_fail(toolset, tool, contains="", **args):
    env = call(toolset, tool, **args)
    good = not env.get("success") and contains.lower() in (env.get("error") or "").lower()
    check(good, "%s rejected" % tool, env.get("error") if not env.get("success") else "unexpectedly succeeded: %s" % json.dumps(env.get("result"))[:300])
    return env.get("error")


# ---- fixtures ---------------------------------------------------------------------------------

TUT = "/Engine/Tutorial/SubEditors/TutorialAssets/Character"
ROOT = "/Game/GMCPTest"
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def create_input_action(name, value_type):
    path = "%s/Input/%s" % (ROOT, name)
    if eal.does_asset_exist(path):
        return eal.load_asset(path)
    asset = asset_tools.create_asset(name, ROOT + "/Input", unreal.InputAction, None)
    asset.set_editor_property("value_type", value_type)
    eal.save_loaded_asset(asset)
    return asset


def setup_fixtures():
    create_input_action("IA_Move", unreal.InputActionValueType.AXIS2D)
    create_input_action("IA_Jump", unreal.InputActionValueType.BOOLEAN)
    create_input_action("IA_Throttle", unreal.InputActionValueType.AXIS1D)
    log("fixture: input actions ready")

    # Animation Blueprint with an Idle/Walk state machine driven by Speed. Built with AnimMCPToolset
    # when it is available (test fixture only; GameplayMCPToolset does not depend on it).
    abp = ROOT + "/ABP_GMCPTest"
    if not eal.does_asset_exist(abp) and ("AnimMCPToolset.AnimAssetToolset" in SCHEMAS):
        def anim(toolset, tool, **args):
            r = unreal.ToolsetRegistry.execute_tool("AnimMCPToolset." + toolset, tool, json.dumps(args))
            env = json.loads(r.value)["returnValue"] if r.is_complete and not r.error else {"success": False, "error": r.error}
            if not env.get("success"):
                log("fixture: %s failed: %s" % (tool, env.get("error")))
            return env.get("result")
        anim("AnimAssetToolset", "anim_create_anim_blueprint", folder=ROOT, asset_name="ABP_GMCPTest",
             skeleton_path=TUT + "/TutorialTPP_Skeleton", add_locomotion_vars=True)
        spec = {"name": "Locomotion", "connect_to_output": True, "entry_state": "Idle",
                "states": [{"name": "Idle", "animation": TUT + "/Tutorial_Idle"},
                           {"name": "Walk", "animation": TUT + "/Tutorial_Walk_Fwd"}],
                "transitions": [{"from": "Idle", "to": "Walk", "rule": "compare", "variable": "Speed", "comparison": ">", "threshold": 10},
                                {"from": "Walk", "to": "Idle", "rule": "compare", "variable": "Speed", "comparison": "<=", "threshold": 10}]}
        anim("AnimStateMachineToolset", "anim_build_state_machine", blueprint_path=abp, spec=json.dumps(spec))
        anim("AnimAssetToolset", "anim_compile_blueprint", blueprint_path=abp)
        anim("AnimAssetToolset", "anim_save_asset", asset_path=abp)
    log("fixture: anim blueprint %s" % ("ready" if eal.does_asset_exist(abp) else "MISSING (AnimMCPToolset not available)"))

    montage = ROOT + "/AM_GMCPTest"
    if not eal.does_asset_exist(montage):
        factory = unreal.AnimMontageFactory()
        factory.set_editor_property("source_animation", eal.load_asset(TUT + "/Tutorial_Idle"))
        asset = asset_tools.create_asset("AM_GMCPTest", ROOT, unreal.AnimMontage, factory)
        eal.save_loaded_asset(asset)
    log("fixture: montage ready")

    level = ROOT + "/TestMap"
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if eal.does_asset_exist(level):
        les.load_level(level)
    else:
        les.new_level(level)
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        cube = eal.load_asset("/Engine/BasicShapes/Cube")
        floor = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, -50), unreal.Rotator(0, 0, 0))
        floor.static_mesh_component.set_static_mesh(cube)
        floor.set_actor_scale3d(unreal.Vector(60, 60, 1))
        floor.set_actor_label("Floor")
        target = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(400, 0, 50), unreal.Rotator(0, 0, 0))
        target.static_mesh_component.set_static_mesh(cube)
        target.set_actor_label("TargetCube")
        target.tags = ["GMCPTarget"]
        eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500), unreal.Rotator(-45, 30, 0))
        eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 600), unreal.Rotator(0, 0, 0))
        eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-300, 0, 100), unreal.Rotator(0, 0, 0))
        les.save_current_level()
    log("fixture: level %s ready" % level)


# ---- tick-driven test sequence ----------------------------------------------------------------

def wait_seconds(seconds):
    end = time.time() + seconds
    while time.time() < end:
        yield


def wait_until(cond, timeout, label):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        yield
    check(False, "timed out waiting for " + label)
    return False


def pie_running():
    env = call("GameplayPIEToolset", "pie_get_status")
    return env.get("success") and env["result"]["running"]


def prop(name, component="none", actor="@pawn"):
    env = call("GameplayPIEToolset", "pie_get_property", actor=actor, property_path=name, component=component)
    return env["result"]["value"] if env.get("success") else None


def test_phase1_before_pie():
    r = ok("GameplayPIEToolset", "pie_get_status")
    check(r is not None and r["running"] is False, "pie_get_status reports not running", r)
    expect_fail("GameplayPIEToolset", "pie_list_actors", contains="No Play-In-Editor session")
    expect_fail("GameplayPIEToolset", "pie_get_property", contains="No Play-In-Editor session", actor="@pawn", property_path="Health")
    expect_fail("GameplayPIEToolset", "pie_stop", contains="No Play-In-Editor session")


def test_phase1_in_pie():
    PIE = "GameplayPIEToolset"
    r = ok(PIE, "pie_get_status")
    check(r and r["running"] and r["local_players"] and r["local_players"][0]["pawn"].startswith("GMCPTestCharacter"), "status shows the test pawn", r)
    check(r and r["net_mode"] == "standalone" and r["paused"] is False, "status net_mode/paused", r)
    expect_fail(PIE, "pie_start", contains="already running")

    r = ok(PIE, "pie_list_actors", class_filter="Character")
    check(r and r["count"] == 1 and r["actors"][0]["class"] == "GMCPTestCharacter", "list_actors by parent class name", r and [a["name"] for a in r["actors"]])
    pawn_name = r["actors"][0]["name"] if r and r["count"] else "?"
    r = ok(PIE, "pie_list_actors", class_filter="/Script/Engine.StaticMeshActor")
    check(r and r["count"] == 2, "list_actors by class path", r and r["count"])
    r = ok(PIE, "pie_list_actors", tag_filter="GMCPTarget")
    check(r and r["count"] == 1 and r["actors"][0]["label"] == "TargetCube", "list_actors by tag", r)
    r = ok(PIE, "pie_list_actors", label_filter="Target*")
    check(r and r["count"] == 1, "list_actors by label wildcard", r and r["count"])
    r = ok(PIE, "pie_list_actors", limit=1)
    check(r and r["count"] == 1 and r["total_matches"] > 1, "list_actors limit", r and (r["count"], r["total_matches"]))
    expect_fail(PIE, "pie_list_actors", contains="No class found", class_filter="/Script/Engine.NoSuchClass")

    r = ok(PIE, "pie_find_actor", actor=pawn_name)
    check(r and r["is_pawn"] and r["controller"] and "Character" in r["parent_classes"], "find_actor by name", r)
    r = ok(PIE, "pie_find_actor", actor="TargetCube")
    check(r and r["name"].startswith("StaticMeshActor"), "find_actor by label", r and r["name"])
    r = ok(PIE, "pie_find_actor", actor="@controller")
    check(r and "PlayerController" in r["class"], "find_actor @controller", r and r["class"])
    expect_fail(PIE, "pie_find_actor", contains="No actor named", actor="NoSuchActor_42")
    expect_fail(PIE, "pie_find_actor", contains="Unknown shortcut", actor="@nothing")

    # Properties: nested structs, arrays by index and name key, maps, object references.
    check(prop("Health") == 100, "get Health", prop("Health"))
    check(prop("Stats.Level") == 1, "get nested struct member", prop("Stats.Level"))
    check(prop("Inventory[1].Count") == 3, "get array element by index", prop("Inventory[1].Count"))
    check(prop("Inventory[Sword].Count") == 1, "get array element by name key", prop("Inventory[Sword].Count"))
    check(prop("Ammo[Rifle]") == 30, "get map value by key", prop("Ammo[Rifle]"))
    check(prop("CharacterMovement.MaxWalkSpeed") == 600, "get through object reference", prop("CharacterMovement.MaxWalkSpeed"))
    check(prop("MaxWalkSpeed", component="CharacterMovement") == 600, "get on component", prop("MaxWalkSpeed", component="CharacterMovement"))
    check(prop("MaxWalkSpeed", component="CharacterMovementComponent") == 600, "component by class name")
    expect_fail(PIE, "pie_get_property", contains="has no property", actor="@pawn", property_path="NoSuchProp")
    expect_fail(PIE, "pie_get_property", contains="out of range", actor="@pawn", property_path="Inventory[9].Count")
    expect_fail(PIE, "pie_get_property", contains="no key", actor="@pawn", property_path="Ammo[Shotgun]")
    expect_fail(PIE, "pie_get_property", contains="has no component", actor="@pawn", property_path="Health", component="NoSuchComponent")

    ok(PIE, "pie_set_property", actor="@pawn", property_path="Health", value="55")
    check(prop("Health") == 55, "set float", prop("Health"))
    ok(PIE, "pie_set_property", actor="@pawn", property_path="Stats.Offset", value="(X=1,Y=2,Z=3)")
    ok(PIE, "pie_set_property", actor="@pawn", property_path="Stats", value='{"Level": 7}')
    st = prop("Stats")
    check(st and st["level"] == 7 and st["offset"]["x"] == 1 and st["offset"]["z"] == 3, "set struct by text then JSON patch keeps other fields", st)
    ok(PIE, "pie_set_property", actor="@pawn", property_path="Stats.Perks", value='["Dash","Roll"]')
    check(prop("Stats.Perks") == ["Dash", "Roll"], "set array from JSON", prop("Stats.Perks"))
    ok(PIE, "pie_set_property", actor="@pawn", property_path="Inventory[Potion].Count", value="9")
    check(prop("Inventory[1].Count") == 9, "set array element by name key", prop("Inventory[1].Count"))
    ok(PIE, "pie_set_property", actor="@pawn", property_path="Ammo[Pistol]", value="99")
    check(prop("Ammo[Pistol]") == 99, "set map value", prop("Ammo[Pistol]"))
    r = ok(PIE, "pie_set_property", actor="@pawn", property_path="TargetActor", value="TargetCube")
    check(r and "StaticMeshActor" in str(r["value"]), "set object reference to a PIE actor by label", r and r["value"])
    ok(PIE, "pie_set_property", actor="@pawn", property_path="MaxWalkSpeed", value="450", component="CharacterMovement")
    check(prop("CharacterMovement.MaxWalkSpeed") == 450, "set on component", prop("CharacterMovement.MaxWalkSpeed"))
    ok(PIE, "pie_set_property", actor="@pawn", property_path="MaxWalkSpeed", value="600", component="CharacterMovement")
    expect_fail(PIE, "pie_set_property", contains="not a number", actor="@pawn", property_path="Health", value="abc")
    expect_fail(PIE, "pie_set_property", contains="not a number", actor="@pawn", property_path="Health", value="12abc")
    expect_fail(PIE, "pie_set_property", contains="whole number", actor="@pawn", property_path="Stats.Level", value="2.5")
    expect_fail(PIE, "pie_set_property", contains="not a bool", actor="@pawn", property_path="bInputBound", value="maybe")
    expect_fail(PIE, "pie_set_property", contains="not a valid", actor="@pawn", property_path="Stats.Offset", value="(X=1,Q=2)")
    check(prop("Health") == 55 and prop("Stats.Level") == 7, "failed sets left values unchanged", (prop("Health"), prop("Stats.Level")))
    expect_fail(PIE, "pie_set_property", contains="not a PIE-world object", actor="TargetCube",
                property_path="StaticMeshComponent.StaticMesh.LightMapResolution", value="64")

    r = ok(PIE, "pie_list_properties", actor="@pawn", name_filter="*Health*")
    check(r and any(p["name"] == "Health" and p["value"].startswith("55") for p in r["properties"]), "list_properties filter + value", r)
    r_all = ok(PIE, "pie_list_properties", actor="@pawn", include_all=True)
    r_bp = ok(PIE, "pie_list_properties", actor="@pawn")
    check(r_all and r_bp and r_all["count"] > r_bp["count"], "include_all lists more", (r_all and r_all["count"], r_bp and r_bp["count"]))
    r = ok(PIE, "pie_list_properties", actor="@pawn", component="CharacterMovement", name_filter="MaxWalk*")
    check(r and r["count"] >= 1, "list_properties on component", r and [p["name"] for p in r["properties"]])

    r = ok(PIE, "pie_list_functions", actor="@pawn", name_filter="AddHealth")
    check(r and r["count"] == 1 and [p["name"] for p in r["functions"][0]["params"]] == ["Amount", "bClamp", "OutTimesCalled", "ReturnValue"], "list_functions params", r)

    r = ok(PIE, "pie_call_function", actor="@pawn", function_name="AddHealth", args='{"Amount": 10, "bClamp": false}')
    check(r and r["return_value"] == 65 and r["out_params"].get("OutTimesCalled") == 1, "call returns value and out param", r)
    r = ok(PIE, "pie_call_function", actor="@pawn", function_name="addhealth", args='{"Amount": 100, "bClamp": true}')
    check(r and r["return_value"] == 100 and r["out_params"].get("OutTimesCalled") == 2, "call case-insensitive, clamp", r)
    r = ok(PIE, "pie_call_function", actor="@pawn", function_name="DescribeActor", args='{"Other": "TargetCube"}')
    check(r and r["return_value"].startswith("StaticMeshActor") and r["defaulted_args"] == ["Repeat"], "object arg by label + C++ default", r)
    r = ok(PIE, "pie_call_function", actor="@pawn", function_name="DescribeActor", args='{"Other": "none", "Repeat": 2}')
    check(r and r["return_value"] == "NoneNone", "null object arg", r)
    expect_fail(PIE, "pie_call_function", contains="Unknown argument", actor="@pawn", function_name="AddHealth", args='{"Amout": 1}')
    expect_fail(PIE, "pie_call_function", contains="no function", actor="@pawn", function_name="NoSuchFunction")
    expect_fail(PIE, "pie_call_function", contains="Argument 'Amount'", actor="@pawn", function_name="AddHealth", args='{"Amount": "lots"}')
    expect_fail(PIE, "pie_call_function", contains="JSON object", actor="@pawn", function_name="AddHealth", args='not json')
    r = ok(PIE, "pie_call_function", actor="@pawn", function_name="IsMovingOnGround", component="CharacterMovement")
    check(r and r["return_value"] is True, "call on component", r)

    r = ok(PIE, "pie_get_component_tree", actor="@pawn")
    names = [c["name"] for c in (r["root"]["children"] if r else [])]
    check(r and r["root"]["name"] == "CollisionCylinder" and "CharacterMesh0" in names, "component tree root/children", names)
    check(r and any(c["class"] == "CharacterMovementComponent" for c in r["other_components"]), "component tree non-scene components", r and r["other_components"])


def test_anim_state(expect_state=None, label="anim state"):
    r = ok("GameplayPIEToolset", "pie_get_anim_state", actor="@pawn")
    if not r:
        return None
    machines = r["state_machines"]
    if not machines:
        skip("%s: no state machine fixture (AnimMCPToolset not available)" % label)
        return r
    sm = machines[0]
    check(sm["name"] == "Locomotion" and sorted(sm["states"]) == ["Idle", "Walk"], "%s: state machine found" % label, sm)
    if expect_state:
        check(sm["active_state"] == expect_state, "%s: active state is %s" % (label, expect_state), sm)
    check("Speed" in r["variables"], "%s: anim variables include Speed" % label, list(r["variables"].keys()))
    return r


def test_montage():
    r = ok("GameplayPIEToolset", "pie_call_function", actor="@pawn", component="AnimInstance", function_name="Montage_Play",
           args=json.dumps({"MontageToPlay": ROOT + "/AM_GMCPTest.AM_GMCPTest"}))
    check(r and r["return_value"] > 0, "Montage_Play via call_function with asset path arg", r)
    r = ok("GameplayPIEToolset", "pie_get_anim_state", actor="@pawn", component="CharacterMesh0")
    check(r and len(r["montages"]) == 1 and r["montages"][0]["montage"].endswith("AM_GMCPTest") and r["montages"][0]["section"] == "Default",
          "anim state lists playing montage", r and r["montages"])
    expect_fail("GameplayPIEToolset", "pie_get_anim_state", contains="not a skeletal mesh", actor="@pawn", component="CollisionCylinder")


# ---- phase 2: input simulation ----------------------------------------------------------------

INP = "GameplayInputToolset"


def job_state(job_id):
    env = call(INP, "input_status", job_id=str(job_id))
    return env["result"]["jobs"][0] if env.get("success") and env["result"]["jobs"] else None


def wait_job(job_id, timeout=30):
    yield from wait_until(lambda: (job_state(job_id) or {}).get("state") in ("completed", "cancelled", "failed"), timeout, "input job %s" % job_id)
    return job_state(job_id)


def pawn_location():
    env = call("GameplayPIEToolset", "pie_find_actor", actor="@pawn")
    return env["result"]["location"] if env.get("success") else None


def reset_counters():
    call("GameplayPIEToolset", "pie_call_function", actor="@pawn", function_name="ResetInputCounters")


def test_phase2():
    if not has_toolset(INP):
        skip("phase 2 (GameplayInputToolset not registered)")
        return
    r = ok(INP, "input_list_actions", name_filter="IA_*")
    types = {a["name"]: a["value_type"] for a in (r["actions"] if r else [])}
    check(types == {"IA_Jump": "boolean", "IA_Move": "axis2d", "IA_Throttle": "axis1d"}, "input_list_actions with value types", types)

    expect_fail(INP, "input_hold_action", contains="No Input Action named", action="IA_Nope")
    expect_fail(INP, "input_hold_action", contains="does not fit", action="IA_Move", value="1,2,3")
    expect_fail(INP, "input_hold_action", contains="not a number", action="IA_Move", value="1,up")
    expect_fail(INP, "input_hold_action", contains="Local player 3", action="IA_Jump", player_index=3)
    expect_fail(INP, "input_set_axis", contains="Boolean action", action="IA_Jump", value="1")
    expect_fail(INP, "input_tap_action", contains="count must be", action="IA_Jump", count=0)
    expect_fail(INP, "input_sequence", contains="JSON array", steps="{not json")
    expect_fail(INP, "input_sequence", contains="unknown field", steps='[{"type":"hold","action":"IA_Jump","duration":1,"speed":2}]')
    expect_fail(INP, "input_sequence", contains='needs "duration"', steps='[{"type":"wait"}]')
    expect_fail(INP, "input_sequence", contains="only apply to tap", steps='[{"type":"hold","action":"IA_Jump","duration":1,"count":2}]')
    expect_fail(INP, "input_status", contains="No input job", job_id="9999")
    expect_fail(INP, "input_status", contains="not a job id", job_id="abc")

    # Hold Move forward: the pawn walks, the anim Blueprint switches to Walk, then back to Idle.
    reset_counters()
    start = pawn_location()
    r = ok(INP, "input_hold_action", action="IA_Move", value="0,1", duration=2.0)
    job = r and r["job_id"]
    check(r and r["state"] == "queued" and abs(r["expected_seconds"] - 2.0) < 0.01, "hold returns a job id at once", r)
    yield from wait_seconds(1.0)
    st = job_state(job)
    check(st and st["state"] == "running" and st["held"] == ["IA_Move"], "job running and holding mid-way", st)
    test_anim_state("Walk", "anim state while move is held")
    expect_fail(INP, "input_hold_action", contains="already being driven", action="IA_Move", duration=0.5)
    st = yield from wait_job(job)
    check(st and st["state"] == "completed" and st["held"] == [] and 1.9 <= st["elapsed_seconds"] <= 2.3, "hold completed after ~2s", st)
    end = pawn_location()
    check(start and end and end["x"] - start["x"] > 150, "pawn moved forward", (start, end))
    check(prop("LastMoveInput") == {"x": 0, "y": 1} and prop("MoveTriggeredFrames") >= 20 and prop("MoveCompletedCount") == 1,
          "Enhanced Input delivered the held value and one release", (prop("LastMoveInput"), prop("MoveTriggeredFrames"), prop("MoveCompletedCount")))
    yield from wait_seconds(1.0)
    test_anim_state("Idle", "anim state after release")

    # Taps: three separate presses and releases.
    r = ok(INP, "input_tap_action", action="IA_Jump", count=3, interval=0.3, hold_time=0.05)
    st = yield from wait_job(r and r["job_id"])
    presses = [e for e in (st or {}).get("events", []) if "press IA_Jump" in e]
    check(st and st["state"] == "completed" and len(presses) == 3, "tap job completed with 3 presses", st and st["events"])
    check(prop("JumpPressCount") == 3 and prop("JumpReleaseCount") == 3, "game saw 3 jump presses and releases", (prop("JumpPressCount"), prop("JumpReleaseCount")))

    # Axis: constant, then ramp.
    r = ok(INP, "input_set_axis", action="IA_Throttle", value="0.5", duration=0.3)
    yield from wait_job(r and r["job_id"])
    check(abs((prop("LastThrottle") or 0) - 0.5) < 1e-4, "constant axis value delivered", prop("LastThrottle"))
    reset_counters()
    r = ok(INP, "input_set_axis", action="IA_Throttle", value="0", end_value="1", duration=1.0)
    yield from wait_seconds(0.5)
    mid = prop("LastThrottle")
    st = yield from wait_job(r and r["job_id"])
    check(st and st["state"] == "completed" and mid is not None and 0.2 < mid < 0.8 and prop("MaxThrottle") > 0.9,
          "ramp goes from 0 to 1 over the duration", (mid, prop("MaxThrottle")))

    # Sequence with an overlapping step: move while jumping.
    reset_counters()
    steps = [{"type": "hold", "action": "IA_Move", "value": [0, 1], "duration": 1.0, "wait": False},
             {"type": "wait", "duration": 0.3},
             {"type": "tap", "action": "IA_Jump"},
             {"type": "axis", "action": "IA_Throttle", "value": 0.25, "duration": 0.2}]
    r = ok(INP, "input_sequence", steps=json.dumps(steps))
    check(r and r["step_count"] == 4 and abs(r["expected_seconds"] - 1.0) < 0.01, "sequence accepted", r)
    st = yield from wait_job(r and r["job_id"])
    ev = (st or {}).get("events", [])
    idx = lambda key: next((i for i, e in enumerate(ev) if key in e), -1)
    check(st and st["state"] == "completed" and 0 <= idx("press IA_Jump") < idx("release IA_Move"), "sequence ran steps overlapped", ev)
    check(prop("JumpPressCount") == 1 and prop("MoveCompletedCount") == 1 and abs(prop("LastThrottle") - 0.25) < 1e-4, "sequence effects seen in game",
          (prop("JumpPressCount"), prop("MoveCompletedCount"), prop("LastThrottle")))

    # Cancel releases held input at once.
    reset_counters()
    r = ok(INP, "input_hold_action", action="IA_Move", value="1,0", duration=20)
    yield from wait_seconds(0.3)
    c = ok(INP, "input_cancel", job_id=str(r and r["job_id"]))
    yield from wait_seconds(0.2)
    st = job_state(r and r["job_id"])
    check(c and c["cancelled"] == 1 and st["state"] == "cancelled" and st["held"] == [], "cancel stops the job", st)
    check(prop("MoveCompletedCount") == 1, "cancel released the action in game", prop("MoveCompletedCount"))
    r = ok(INP, "input_status", job_id="*")
    check(r and len(r["jobs"]) >= 6, "input_status lists retained jobs", r and len(r["jobs"]))


# ---- phase 3: editor commands, log, live coding ------------------------------------------------

ED = "GameplayEditorToolset"


def test_phase3_editor():
    if not has_toolset(ED):
        skip("phase 3 (GameplayEditorToolset not registered)")
        return
    r = ok(ED, "editor_run_console_command", command="r.VSync")
    text = (r or {}).get("output", "") + " ".join(l["message"] for l in (r or {}).get("log", []))
    check(r and r["target"] == "editor" and "r.VSync" in text and r["recognized"], "console command output captured in editor", r)
    # Refusals are checked for the refusal only: none of these is ever executed.
    for denied in ("quit", "EXIT", "QUIT_EDITOR_TYPO_CHECK", "close_editor", "  quit", "stat fps | exit", "stat fps; quit",
                   "obj savepackage /Game/GMCPTest/TestMap", "py print(1)", "debug crash", "disconnect"):
        expect_fail(ED, "editor_run_console_command", contains="rule hard_deny", command=denied)
        expect_fail(ED, "editor_run_console_command", contains="rule hard_deny", command=denied, allow_unsafe=True)
    expect_fail(ED, "editor_run_console_command", contains="rule allowlist_miss", command="gmcp_not_a_command_xyz")
    expect_fail(ED, "editor_run_console_command", contains="allow_unsafe=true", command="transaction undo")
    r = ok(ED, "editor_run_console_command", command="stat fps; r.VSync", target="editor")
    check(r and r["rule"] == "allowed" and r["segments"] == ["stat fps", "r.VSync"], "chain split into checked segments", r and (r["rule"], r["segments"]))
    expect_fail(ED, "editor_run_console_command", contains="No Play-In-Editor session", command="stat fps", target="pie")
    expect_fail(ED, "editor_run_console_command", contains="target must be", command="stat fps", target="server")

    unreal.log_warning("GMCP_MARKER_ONE")
    r = ok(ED, "editor_get_recent_log", contains="GMCP_MARKER_ONE", min_verbosity="warning")
    check(r and r["count"] == 1 and r["lines"][0]["category"] == "LogPython" and r["lines"][0]["verbosity"] == "warning", "log line with category and verbosity", r)
    after = r and r["latest_id"]
    unreal.log("GMCP_MARKER_TWO")
    r = ok(ED, "editor_get_recent_log", contains="GMCP_MARKER", after_id=after)
    msgs = [l["message"] for l in (r or {}).get("lines", [])]
    check(r and all(l["id"] > after for l in r["lines"]) and "GMCP_MARKER_TWO" in msgs and "GMCP_MARKER_ONE" not in msgs, "after_id returns only newer lines", r)
    r = ok(ED, "editor_get_recent_log", contains="GMCP_MARKER_TWO", min_verbosity="warning")
    check(r and r["count"] == 0, "min_verbosity filters out lower levels", r)
    r = ok(ED, "editor_get_recent_log", log_category="LogPyth*", max_lines=5)
    check(r and 0 < r["count"] <= 5 and all(l["category"] == "LogPython" for l in r["lines"]), "category wildcard and max_lines", r and r["count"])
    expect_fail(ED, "editor_get_recent_log", contains="min_verbosity", min_verbosity="loud")
    expect_fail(ED, "editor_get_recent_log", contains="max_lines", max_lines=0)


def test_phase3_pie():
    if not has_toolset(ED):
        return
    r = ok(ED, "editor_run_console_command", command="slomo 0.5")
    check(r and r["target"] == "pie", "auto target uses PIE while it runs", r)
    check(prop("TimeDilation", actor="WorldSettings") == 0.5, "slomo changed the PIE world's time dilation", prop("TimeDilation", actor="WorldSettings"))
    ok(ED, "editor_run_console_command", command="slomo 1", target="pie")
    check(prop("TimeDilation", actor="WorldSettings") == 1, "slomo restored", prop("TimeDilation", actor="WorldSettings"))
    r = ok(ED, "editor_run_console_command", command="r.VSync", target="editor")
    check(r and r["target"] == "editor", "explicit editor target during PIE", r and r["target"])


def test_live_coding():
    if not has_toolset(ED):
        return
    if call(ED, "editor_live_coding_status").get("success") is False:
        check(True, "editor_live_coding_status before any compile is an error")
    r = ok(ED, "editor_live_coding_compile")
    if not r:
        return
    check(r["state"] == "compiling", "live coding compile returns a job id at once", r)
    expect_fail(ED, "editor_live_coding_compile", contains="already running")
    yield from wait_until(lambda: (call(ED, "editor_live_coding_status", job_id=str(r["job_id"])).get("result") or {}).get("state") == "completed", 600, "live coding compile")
    st = ok(ED, "editor_live_coding_status", job_id="latest")
    check(st and st["result"] in ("no_changes", "success") and st["errors"] == [], "live coding finished without errors", st)
    expect_fail(ED, "editor_live_coding_status", contains="No Live Coding job", job_id="999")

    # Break a file of the throwaway project, check the compile error comes back, then restore it.
    src = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "Source", "GMCPTest", "GMCPTest.cpp")
    original = open(src, encoding="utf-8").read()
    try:
        open(src, "w", encoding="utf-8").write(original + "\nstatic int GMCPBrokenOnPurpose = ;\n")
        r = ok(ED, "editor_live_coding_compile")
        if r:
            yield from wait_until(lambda: (call(ED, "editor_live_coding_status", job_id=str(r["job_id"])).get("result") or {}).get("state") == "completed", 600, "broken live coding compile")
            st = ok(ED, "editor_live_coding_status", job_id=str(r["job_id"]))
            check(st and st["result"] == "failure" and any("GMCP" in e or "GMCPTest.cpp" in e for e in st["errors"]), "compile error reported with file and message", st and {k: st[k] for k in ("result", "errors")})
    finally:
        open(src, "w", encoding="utf-8").write(original)
    r = ok(ED, "editor_live_coding_compile")
    if r:
        yield from wait_until(lambda: (call(ED, "editor_live_coding_status", job_id=str(r["job_id"])).get("result") or {}).get("state") == "completed", 600, "restored live coding compile")
        st = ok(ED, "editor_live_coding_status", job_id=str(r["job_id"]))
        check(st and st["result"] in ("no_changes", "success") and st["errors"] == [], "compile after restoring the file succeeds", st and {k: st[k] for k in ("result", "errors")})


# ---- phase 4: blueprint components ------------------------------------------------------------

BPT = "GameplayBlueprintToolset"


RUN_TAG = time.strftime("%H%M%S")


def make_blueprint(name, parent_class):
    # A fresh, uniquely named fixture per run (assets are never deleted, even in the test project).
    name = "%s_%s" % (name, RUN_TAG)
    path = ROOT + "/" + name
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    bp = asset_tools.create_asset(name, ROOT, unreal.Blueprint, factory)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    eal.save_loaded_asset(bp)
    return path


def components(path):
    env = call(BPT, "bp_list_components", blueprint_path=path)
    return {c["name"]: c for c in env["result"]["components"]} if env.get("success") else {}


def test_phase4():
    if not has_toolset(BPT):
        skip("phase 4 (GameplayBlueprintToolset not registered)")
        return
    A = make_blueprint("BP_GMCPActor", unreal.Actor)
    CH = make_blueprint("BP_GMCPChar", unreal.load_class(None, "/Script/GMCPTest.GMCPTestCharacter"))
    log("fixture: blueprints ready")

    r = ok(BPT, "bp_list_components", blueprint_path=A)
    check(r and r["scene_root"] == "DefaultSceneRoot" and [c["name"] for c in r["components"]] == ["DefaultSceneRoot"], "list fresh actor blueprint", r)

    r = ok(BPT, "bp_add_component", blueprint_path=A, component_class="StaticMeshComponent", name="Body")
    check(r and r["component"]["parent"] == "DefaultSceneRoot" and r["component"]["source"] == "added" and r["compile_status"] == "up_to_date" and r["dirty"],
          "add attaches to the scene root, compiles, marks dirty", r)
    r = ok(BPT, "bp_add_component", blueprint_path=A, component_class="PointLight", name="Lamp", parent="Body")
    check(r and r["component"]["class"] == "PointLightComponent" and r["component"]["parent"] == "Body", "add by short class name under a parent", r and r["component"])
    ok(BPT, "bp_add_component", blueprint_path=A, component_class="/Script/Engine.SphereComponent", name="Trigger")
    r = ok(BPT, "bp_add_component", blueprint_path=A, component_class="RotatingMovement", name="Spinner")
    check(r and r["component"]["is_scene"] is False and r["component"]["parent"] == "", "add non-scene component", r and r["component"])
    expect_fail(BPT, "bp_add_component", contains="not a scene component", blueprint_path=A, component_class="RotatingMovement", name="Spinner2", parent="Body")
    expect_fail(BPT, "bp_add_component", contains="cannot be used", blueprint_path=A, component_class="StaticMeshComponent", name="Body")
    expect_fail(BPT, "bp_add_component", contains="No class", blueprint_path=A, component_class="NotAClassAtAll", name="X1")
    expect_fail(BPT, "bp_add_component", contains="not a component class", blueprint_path=A, component_class="Actor", name="X2")
    expect_fail(BPT, "bp_add_component", contains="abstract", blueprint_path=A, component_class="PrimitiveComponent", name="X3")
    expect_fail(BPT, "bp_add_component", contains="no component named", blueprint_path=A, component_class="StaticMeshComponent", name="X4", parent="Nope")
    expect_fail(BPT, "bp_add_component", contains="cannot have children", blueprint_path=A, component_class="StaticMeshComponent", name="X5", parent="Spinner")

    r = ok(BPT, "bp_rename_component", blueprint_path=A, name="Lamp", new_name="Light")
    check(r and r["component"]["name"] == "Light" and r["component"]["parent"] == "Body", "rename keeps parent", r and r["component"])
    expect_fail(BPT, "bp_rename_component", contains="cannot be used", blueprint_path=A, name="Light", new_name="Body")
    expect_fail(BPT, "bp_rename_component", contains="already has that name", blueprint_path=A, name="Body", new_name="Body")

    r = ok(BPT, "bp_reparent_component", blueprint_path=A, name="Light", new_parent="DefaultSceneRoot")
    check(r and r["component"]["parent"] == "DefaultSceneRoot", "reparent to root", r and r["component"])
    ok(BPT, "bp_reparent_component", blueprint_path=A, name="Body", new_parent="Trigger")
    check(components(A).get("Body", {}).get("parent") == "Trigger" and "Body" in components(A).get("Trigger", {}).get("children", []), "children lists follow reparent", components(A).get("Trigger"))
    expect_fail(BPT, "bp_reparent_component", contains="own ancestor", blueprint_path=A, name="Trigger", new_parent="Body")
    expect_fail(BPT, "bp_reparent_component", contains="scene root", blueprint_path=A, name="DefaultSceneRoot", new_parent="Body")
    expect_fail(BPT, "bp_reparent_component", contains="scene components", blueprint_path=A, name="Spinner", new_parent="Body")
    expect_fail(BPT, "bp_reparent_component", contains="already attached", blueprint_path=A, name="Body", new_parent="Trigger")

    expect_fail(BPT, "bp_remove_component", contains="still has children", blueprint_path=A, name="Trigger")
    r = ok(BPT, "bp_remove_component", blueprint_path=A, name="Light")
    check(r and r["removed"] == "Light" and sorted(components(A)) == ["Body", "DefaultSceneRoot", "Spinner", "Trigger"], "remove leaf component", sorted(components(A)))

    # Every edit is one undoable transaction.
    ok(BPT, "bp_add_component", blueprint_path=A, component_class="StaticMeshComponent", name="Temp")
    if has_toolset(ED):
        ok(ED, "editor_run_console_command", command="TRANSACTION UNDO", target="editor", allow_unsafe=True)
        check("Temp" not in components(A), "undo removes the added component", sorted(components(A)))
    else:
        call(BPT, "bp_remove_component", blueprint_path=A, name="Temp")

    # Child Blueprint: components from the parent Blueprint are inherited.
    C = make_blueprint("BP_GMCPChild", unreal.EditorAssetLibrary.load_blueprint_class(A))
    comps = components(C)
    check(comps.get("Body", {}).get("source") == "inherited" and comps.get("Spinner", {}).get("source") == "inherited", "child lists inherited components", {k: v["source"] for k, v in comps.items()})
    expect_fail(BPT, "bp_remove_component", contains="inherited from the parent Blueprint", blueprint_path=C, name="Body")
    expect_fail(BPT, "bp_rename_component", contains="inherited", blueprint_path=C, name="Trigger", new_name="Zone")
    r = ok(BPT, "bp_add_component", blueprint_path=C, component_class="StaticMeshComponent", name="ChildMesh", parent="Body")
    check(r and r["component"]["parent"] == "Body" and r["component"]["source"] == "added", "add under an inherited parent", r and r["component"])

    # C++ character Blueprint: native components, native scene root.
    comps = components(CH)
    check(comps.get("CapsuleComponent", {}).get("is_root") and comps["CapsuleComponent"]["source"] == "native" and comps.get("Mesh", {}).get("source") == "native",
          "native components listed with their variable names", {k: (v["source"], v["parent"]) for k, v in comps.items()})
    r = ok(BPT, "bp_add_component", blueprint_path=CH, component_class="StaticMeshComponent", name="Weapon", parent="Mesh")
    check(r and r["component"]["parent"] == "Mesh", "add under a native component", r and r["component"])
    r = ok(BPT, "bp_add_component", blueprint_path=CH, component_class="StaticMeshComponent", name="Hat")
    r2 = ok(BPT, "bp_list_components", blueprint_path=CH)
    check(r and r["component"]["parent"] == "CapsuleComponent" and r2 and r2["scene_root"] == "CapsuleComponent", "add without parent attaches to the native root and keeps it", r and r["component"])
    expect_fail(BPT, "bp_remove_component", contains="native component", blueprint_path=CH, name="Mesh")
    expect_fail(BPT, "bp_rename_component", contains="native component", blueprint_path=CH, name="CharacterMovement", new_name="Moves")
    expect_fail(BPT, "bp_reparent_component", contains="native component", blueprint_path=CH, name="Mesh", new_parent="Weapon")
    ok(BPT, "bp_reparent_component", blueprint_path=CH, name="Hat", new_parent="Mesh")

    expect_fail(BPT, "bp_list_components", contains="No asset found", blueprint_path=ROOT + "/BP_Nope")
    expect_fail(BPT, "bp_list_components", contains="not a Blueprint", blueprint_path=ROOT + "/AM_GMCPTest")
    expect_fail(BPT, "bp_add_component", contains="only assets under /Game", blueprint_path="/Engine/BasicShapes/Cube", component_class="StaticMeshComponent", name="X")

    # Nothing is saved until bp_save_asset.
    check(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages() is not None, "dirty packages query works")
    dirty = [p.get_name() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()]
    check(A.split(".")[0] in dirty and CH.split(".")[0] in dirty, "edited blueprints are dirty, not saved", dirty)
    r = ok(BPT, "bp_save_asset", blueprint_path=A)
    check(r and r["saved"] is True, "save writes a dirty blueprint", r)
    r = ok(BPT, "bp_save_asset", blueprint_path=A)
    check(r and r["saved"] is False, "second save has nothing to do", r)
    expect_fail(BPT, "bp_save_asset", contains="only assets under /Game", blueprint_path="/Engine/BasicShapes/Cube")
    for path in (C, CH):
        call(BPT, "bp_save_asset", blueprint_path=path)


# ---- phase 5: game viewport screenshot --------------------------------------------------------

VP = "GameplayViewportToolset"


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")


def test_phase5_before_pie():
    if not has_toolset(VP):
        skip("phase 5 (GameplayViewportToolset not registered)")
        return
    expect_fail(VP, "game_screenshot_status", contains="No screenshot has been requested")
    expect_fail(VP, "game_capture_screenshot", contains="No Play-In-Editor session")


def capture(width, height, name):
    r = ok(VP, "game_capture_screenshot", width=width, height=height, file_name=name, overwrite=True)
    if not r:
        return None
    yield from wait_until(lambda: (call(VP, "game_screenshot_status", job_id=str(r["job_id"])).get("result") or {}).get("state") in ("completed", "failed"), 30, "screenshot %s" % name)
    return call(VP, "game_screenshot_status", job_id=str(r["job_id"])).get("result")


def test_phase5_in_pie():
    if not has_toolset(VP):
        return
    expect_fail(VP, "game_capture_screenshot", contains="between 64", width=10, height=720)
    expect_fail(VP, "game_capture_screenshot", contains="may only contain", file_name="../evil")
    st = yield from capture(1280, 720, "gmcp_view_1280x720")
    check(st and st["state"] == "completed" and os.path.isabs(st["file_path"]) and os.path.exists(st["file_path"]), "screenshot written to an absolute path", st)
    if st and os.path.exists(st["file_path"]):
        check(png_size(st["file_path"]) == (1280, 720) and st["file_size"] > 20000, "PNG has the requested size and real content", (png_size(st["file_path"]), st["file_size"]))
        check("Saved" in st["file_path"] and "GameplayMCP" in st["file_path"], "written under Saved/Screenshots/GameplayMCP", st["file_path"])
    st = yield from capture(640, 640, "gmcp_view_square")
    check(st and st["state"] == "completed" and png_size(st["file_path"]) == (640, 640), "non-viewport aspect ratio", st)
    expect_fail(VP, "game_capture_screenshot", contains="already exists", file_name="gmcp_view_square")
    r = ok(VP, "game_capture_screenshot", width=320, height=180)
    expect_fail(VP, "game_capture_screenshot", contains="Another screenshot", width=320, height=180)
    yield from wait_until(lambda: (call(VP, "game_screenshot_status").get("result") or {}).get("state") != "pending", 30, "auto-named screenshot")
    st = ok(VP, "game_screenshot_status", job_id="latest")
    check(st and st["state"] == "completed" and os.path.basename(st["file_path"]).startswith("Shot_"), "auto file name", st and st["file_path"])
    expect_fail(VP, "game_screenshot_status", contains="No screenshot job", job_id="999")


def bridge_wait():
    if os.environ.get("GMCP_MCP_BRIDGE") != "1":
        skip("MCP bridge test (set GMCP_MCP_BRIDGE=1 and run Tests/mcp_bridge_test.py)")
        return
    ready = os.path.join(SAVED, "gmcp_mcp_ready.txt")
    done = os.path.join(SAVED, "gmcp_mcp_done.txt")
    if os.path.exists(done):
        os.remove(done)
    open(ready, "w").write("ready")
    log("waiting for mcp_bridge_test.py ...")
    yield from wait_until(lambda: os.path.exists(done), 300, "MCP bridge test")
    if os.path.exists(done):
        result = open(done).read().strip()
        check(result.startswith("PASS"), "MCP bridge test: " + result.splitlines()[0], result)
    os.remove(ready)


def sequence():
    refresh_schemas()
    ours = sorted(n for n in SCHEMAS if n.startswith("GameplayMCPToolset."))
    log("toolsets: " + ", ".join("%s(%d)" % (n, len(SCHEMAS[n]["tools"])) for n in ours))
    setup_fixtures()
    yield

    test_phase1_before_pie()
    test_phase3_editor()
    test_phase5_before_pie()
    ok("GameplayPIEToolset", "pie_start")
    if not (yield from wait_until(pie_running, 90, "PIE to start")):
        return
    yield from wait_until(lambda: prop("bInputBound") is True, 30, "pawn possessed and input bound")
    yield from wait_seconds(1.0)

    test_phase1_in_pie()
    test_anim_state("Idle", "anim state at rest")
    test_montage()
    yield from test_phase2()
    test_phase3_pie()
    yield from test_phase5_in_pie()

    yield from bridge_wait()

    # A job still holding input when PIE ends must be cancelled and released.
    held_job = None
    if has_toolset(INP):
        r = ok(INP, "input_hold_action", action="IA_Move", value="0,1", duration=60)
        held_job = r and r["job_id"]
        yield from wait_seconds(0.3)
    ok("GameplayPIEToolset", "pie_stop")
    yield from wait_until(lambda: not pie_running(), 60, "PIE to stop")
    expect_fail("GameplayPIEToolset", "pie_get_property", contains="No Play-In-Editor session", actor="@pawn", property_path="Health")
    if held_job:
        st = job_state(held_job)
        check(st and st["state"] == "cancelled" and "PIE session ended" in st["error"] and st["held"] == [], "PIE end cancels and releases input jobs", st)
        expect_fail(INP, "input_tap_action", contains="No Play-In-Editor session", action="IA_Jump")

    test_phase4()
    yield from test_live_coding()


def run():
    gen = sequence()
    state = {"handle": None, "busy": False, "done": False}

    def finish():
        state["done"] = True
        unreal.unregister_slate_post_tick_callback(state["handle"])
        log("SUMMARY: %d ok, %d failed, %d skipped" % (STATS["ok"], STATS["fail"], STATS["skip"]))
        OUT.close()
        unreal.EditorPythonScripting.set_keep_python_script_alive(False)
        unreal.SystemLibrary.quit_editor()

    def tick(delta):
        # Loading levels or saving assets can pump Slate from inside a step; skip those nested ticks.
        if state["busy"] or state["done"]:
            return
        state["busy"] = True
        try:
            next(gen)
        except StopIteration:
            finish()
        except Exception:
            STATS["fail"] += 1
            log("FAIL exception: " + traceback.format_exc())
            finish()
        finally:
            state["busy"] = False

    state["handle"] = unreal.register_slate_post_tick_callback(tick)


# -ExecutePythonScript closes the editor on the next tick once the script returns unless it is kept
# alive; this test runs over many ticks and quits the editor itself when done.
unreal.EditorPythonScripting.set_keep_python_script_alive(True)
run()
