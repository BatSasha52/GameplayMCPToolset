# Calls GameplayMCPToolset tools over HTTP through the real Unreal MCP server, the way an MCP client
# does. Run it with a normal Python 3 (outside the editor) while gameplay_smoke_test.py is paused in
# PIE with GMCP_MCP_BRIDGE=1:
#
#   python Tests/mcp_bridge_test.py --saved <TestProject>/Saved [--url http://127.0.0.1:8765/mcp]
#
# The test project's config starts the MCP server on port 8765 with tool search enabled, so the client
# sees list_toolsets / describe_toolset / call_tool. Results are written to <Saved>/gmcp_mcp_done.txt
# (first line PASS or FAIL), which also releases the paused editor test.
import argparse, json, os, sys, time, urllib.request, urllib.error

parser = argparse.ArgumentParser()
parser.add_argument("--saved", required=True)
parser.add_argument("--url", default="http://127.0.0.1:8765/mcp")
parser.add_argument("--timeout", type=float, default=600)
opts = parser.parse_args()

ready = os.path.join(opts.saved, "gmcp_mcp_ready.txt")
done = os.path.join(opts.saved, "gmcp_mcp_done.txt")
end = time.time() + opts.timeout
while not os.path.exists(ready):
    if time.time() > end:
        sys.exit("editor never became ready")
    time.sleep(1)

session = {"id": None, "next": 1}
lines = []
failures = []


def check(cond, label, detail=""):
    lines.append("%s %s%s" % ("OK  " if cond else "FAIL", label, (" -- " + str(detail)[:400]) if detail != "" else ""))
    if not cond:
        failures.append(label)
    return cond


def rpc(method, params=None, notify=False):
    body = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        body["params"] = params
    if not notify:
        body["id"] = session["next"]
        session["next"] += 1
    req = urllib.request.Request(opts.url, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if session["id"]:
        req.add_header("Mcp-Session-Id", session["id"])
    with urllib.request.urlopen(req, timeout=60) as resp:
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            session["id"] = sid
        raw = resp.read().decode("utf-8")
    if notify or not raw.strip():
        return None
    if raw.lstrip().startswith("event:") or raw.lstrip().startswith("data:"):
        data = [l[5:].strip() for l in raw.splitlines() if l.startswith("data:")]
        raw = data[-1]
    return json.loads(raw)


def call_tool(toolset, tool, **arguments):
    """Returns (envelope dict or None, raw MCP result)."""
    resp = rpc("tools/call", {"name": "call_tool", "arguments": {"toolset_name": toolset, "tool_name": tool, "arguments": arguments}})
    result = resp.get("result") or {}
    text = "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
    envelope = None
    try:
        parsed = json.loads(text)
        envelope = parsed.get("returnValue", parsed) if isinstance(parsed, dict) else None
    except ValueError:
        pass
    if envelope is None and isinstance(result.get("structuredContent"), dict):
        sc = result["structuredContent"]
        envelope = sc.get("returnValue", sc)
    return envelope, resp


PIE = "GameplayMCPToolset.GameplayPIEToolset"
try:
    init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "gmcp-bridge-test", "version": "0.1.0"}})
    check(init and "result" in init, "initialize", init and init.get("result", {}).get("serverInfo"))
    rpc("notifications/initialized", notify=True)

    tools = rpc("tools/list", {})
    names = [t["name"] for t in tools["result"]["tools"]]
    check({"list_toolsets", "describe_toolset", "call_tool"} <= set(names), "tools/list exposes tool-search meta tools", names)

    listed = rpc("tools/call", {"name": "list_toolsets", "arguments": {}})
    listed_text = json.dumps(listed)
    check(PIE in listed_text, "list_toolsets includes GameplayPIEToolset")

    described = rpc("tools/call", {"name": "describe_toolset", "arguments": {"toolset_name": PIE}})
    described_text = json.dumps(described)
    check("pie_get_property" in described_text and "property_path" in described_text, "describe_toolset shows tools and parameter docs")

    env, raw = call_tool(PIE, "pie_get_status")
    check(env and env.get("success") and env["result"]["running"], "call_tool pie_get_status (short name)", env or raw)

    env, raw = call_tool(PIE, "pie_get_property", actor="@pawn", property_path="Inventory[Sword].Count")
    check(env and env.get("success") and env["result"]["value"] == 1, "call_tool pie_get_property with defaults omitted", env or raw)

    env, raw = call_tool(PIE, "pie_list_actors", tag_filter="GMCPTarget")
    check(env and env.get("success") and env["result"]["count"] == 1, "call_tool pie_list_actors", env or raw)

    env, raw = call_tool(PIE, "pie_find_actor", actor="NoSuchActor_42")
    check(env and env.get("success") is False and "No actor named" in env.get("error", ""), "error envelope comes through MCP", env or raw)

    env, raw = call_tool(PIE, PIE + ".pie_get_status")
    check(not (env and env.get("success")), "dotted full tool name is rejected by the bridge (documented engine behaviour)", raw)

    INPUT = "GameplayMCPToolset.GameplayInputToolset"
    if INPUT in listed_text:
        env, raw = call_tool(INPUT, "input_tap_action", action="IA_Jump", count=1)
        check(env and env.get("success") and env["result"].get("job_id"), "call_tool input_tap_action returns a job id", env or raw)
        job = env["result"]["job_id"] if env and env.get("success") else None
        state = None
        for _ in range(100):
            env, raw = call_tool(INPUT, "input_status", job_id=str(job))
            state = env and env.get("success") and env["result"]["jobs"][0]["state"]
            if state in ("completed", "failed", "cancelled"):
                break
            time.sleep(0.1)
        check(state == "completed", "input job completes when polled over MCP", state)

    EDITOR = "GameplayMCPToolset.GameplayEditorToolset"
    if EDITOR in listed_text:
        env, raw = call_tool(EDITOR, "editor_run_console_command", command="stat fps")
        check(env and env.get("success"), "call_tool editor_run_console_command", env or raw)

    VIEW = "GameplayMCPToolset.GameplayViewportToolset"
    if VIEW in listed_text:
        env, raw = call_tool(VIEW, "game_capture_screenshot", width=640, height=360, file_name="bridge_shot")
        job = env["result"]["job_id"] if check(env and env.get("success"), "call_tool game_capture_screenshot", env or raw) else None
        state = None
        for _ in range(100 if job else 0):
            env, raw = call_tool(VIEW, "game_screenshot_status", job_id=str(job))
            state = env and env.get("success") and env["result"]["state"]
            if state in ("completed", "failed"):
                break
            time.sleep(0.1)
        check(state == "completed" and os.path.exists(env["result"]["file_path"]), "screenshot over MCP written to disk", env and env.get("result"))
except Exception as exc:  # report, don't hang the editor
    import traceback
    check(False, "exception", traceback.format_exc())

summary = "PASS" if not failures else "FAIL (%d): %s" % (len(failures), ", ".join(failures))
with open(done, "w", encoding="utf-8") as f:
    f.write(summary + "\n" + "\n".join(lines) + "\n")
print(summary)
print("\n".join(lines))
