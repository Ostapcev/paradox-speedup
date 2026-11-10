"""Structural regression checks for the lightweight optimize path."""
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "src" / "eu4_modifier_patch.cpp").read_text(
    encoding="utf-8"
)

fast_return = "if (optimization_enabled) return original(destination, source);"
assert fast_return in SOURCE, "optimize must return directly to the original merge"
merge_body = SOURCE.split("void* profile_merge", 1)[1].split("void* add_hook", 1)[0]
assert merge_body.index(fast_return) < merge_body.index("QueryPerformanceCounter"), (
    "the fast return must precede all per-call timing"
)
assert "const bool diagnostic_hooks = !optimization_enabled;" in SOURCE
for index in range(2, 10):
    assert f"if (prepared && diagnostic_hooks) prepared = prepare({index}," in SOURCE, (
        f"diagnostic hook {index} must not be prepared in optimize mode"
    )
assert "const auto patch_count = optimization_enabled ? std::size_t{2} : patches.size();" in SOURCE
assert "index < patch_count" in SOURCE, "commit must be limited to prepared hooks"
assert "mode=%ls hooks=%zu search_sse2=%u" in SOURCE
assert "version, mode_name, patch_count, optimization_enabled ? 2U : 0U," in SOURCE
print("lightweight_path_tests: all checks passed")
