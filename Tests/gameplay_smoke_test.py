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
    ok("GameplayPIEToolset", "pie_start")
    if not (yield from wait_until(pie_running, 90, "PIE to start")):
        return
    yield from wait_until(lambda: prop("bInputBound") is True, 30, "pawn possessed and input bound")
    yield from wait_seconds(1.0)

    test_phase1_in_pie()
    test_anim_state("Idle", "anim state at rest")
    test_montage()

    yield from bridge_wait()

    ok("GameplayPIEToolset", "pie_stop")
    yield from wait_until(lambda: not pie_running(), 60, "PIE to stop")
    expect_fail("GameplayPIEToolset", "pie_get_property", contains="No Play-In-Editor session", actor="@pawn", property_path="Health")


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
