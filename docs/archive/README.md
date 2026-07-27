# Retired planning context

The active documentation set is intentionally small. Historical planning and handoff files that were written before PR #7 and PR #8 were removed from the working tree after their useful decisions were incorporated into the canonical roadmap and detailed design.

Removed from the active tree:

- `STREAMING_PRIORITY_AND_PERFORMANCE_POLICY.md`
- `STEAMOS_ADAPTIVE_STREAMING_DESIGN.md`
- `STEAMOS_VIRTUAL_SESSION_PLAN.md`
- `STEAMOS_CONTROL_PLANE_DESIGN.md`
- `SESSION_PRESENTATION_AND_CLIENT_PROFILES_GOAL_PLAN.md`
- `WEB_UI_COEXISTENCE_AND_VALIDATION_PLAN.md`
- `WEB_UI_REDESIGN_HANDOFF.md`

Their complete contents remain available in Git history and the pull requests that introduced them. They should not be loaded into routine Codex context.

Use these active documents instead:

1. `../PROJECT_ROADMAP.md`
2. `../STREAM_NEGOTIATION_HDR_QUALITY_DESIGN.md`
3. `../CODEX_GOAL_MODE_STREAMING.md`
4. `../IMPLEMENTATION_STATUS.md`
5. implementation-specific records such as `../STEAMOS_AUTO_VIRTUAL_DISPLAY_IMPLEMENTATION.md`

When a new plan supersedes an active plan:

- transfer still-valid decisions into the canonical documents;
- update implementation status with measured evidence;
- remove the obsolete plan from the active index;
- rely on Git history rather than retaining multiple contradictory copies.
