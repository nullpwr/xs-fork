-- plugin that emits hooks via annotate pass, verifying hook table write works
plugin.runtime.global.set("hook_table_ok", true)

-- emit a hook entry (node_id=0, kind="destructor", target="test_var")
emit_runtime_hook(0, "destructor", "test_var")

-- emit another one to verify multiple entries work
emit_runtime_hook(1, "trace", "other_var")
