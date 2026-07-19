# MobileGL trace-replay skills

Task-focused skills for capturing, replaying, debugging, and authoring MobileGL
apitrace fixtures. Each skill is a self-contained package:

- `SKILL.md` — the skill (frontmatter `name` + `description`, then the body). The
  directory name equals the frontmatter `name`.
- `agents/openai.yaml` — OpenAI agent descriptor (`display_name`,
  `short_description`, `default_prompt`).
- `scripts/` and/or `references/` — bundled tooling and supporting docs, when the
  skill has them.

## Skills

| Skill | What it does |
| --- | --- |
| [trace-fixture-authoring-on-android-fcl](trace-fixture-authoring-on-android-fcl/SKILL.md) | Capture an on-device Android apitrace from FCL's MobileGL renderers (DirectGLES / Magma / SimpleFPEWrapper), mark the defect frame, and pull `full.trace`. |
| [renderdoc-debug-on-trace-replay](renderdoc-debug-on-trace-replay/SKILL.md) | Capture and validate an exact frame from a MobileGL retrace on a connected Android device with RenderDoc / rdc-cli. |
| [mismatch-retrace-debugging](mismatch-retrace-debugging/SKILL.md) | Localize the first divergent render pass and draw call when a fixture replays correctly in a golden environment but renders differently on a target backend. |
| [trace-fixture-authoring](trace-fixture-authoring/SKILL.md) | Author a deterministic trace-replay fixture — trim, golden, package under the size budget, register in `trace_cases.json`, and validate on Linux and Android. |
