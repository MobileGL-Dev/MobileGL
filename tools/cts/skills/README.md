# MobileGL conformance-suite skills

Task-focused skills for running Khronos conformance suites against MobileGL.
Each skill is a self-contained package, matching the layout used by
`tools/trace_replay/skills/`:

- `SKILL.md` — the skill (frontmatter `name` + `description`, then the body). The
  directory name equals the frontmatter `name`.
- `agents/openai.yaml` — OpenAI agent descriptor (`display_name`,
  `short_description`, `default_prompt`).
- `scripts/` and/or `references/` — bundled tooling and supporting docs, when the
  skill has them.

## Skills

| Skill | What it does |
| --- | --- |
| [gl-cts-on-mobilegl](gl-cts-on-mobilegl/SKILL.md) | Build VK-GL-CTS `glcts` as a standalone Android arm64 binary against MobileGL's own EGL, run KHR-GL33, and report a per-backend OpenGL 3.3 core conformance rate. |
