# The readable code: where it stands

Interim, kept current as the run goes (2026-11-04).

- **View:** 16,965 of 16,966 functions decompiled, no bad instruction data. One function missing (`game_task 9A70370`).
- **Accuracy:** three audits against the generated C. Neutral samples, misleading views: 5 of 20, then 2 of 20 after signatures
  moved to machine-code analysis. A fourth, of the final view, is still to come. Remaining problems: docs/readable.md.
- **Evidence:** 22 recordings traced; 3,763 functions seen running. Replays with the tracing build match the normal build's RAM.
- **Names:** batches over EBOOT, hunter and combat done so far. Each batch checked by a skeptic: mechanics are reliable, the weak
  point is game words spread by inheritance. Batch results and corrections are in the git history of `tools/readable/names/`.

To be completed: final numbers, the game-unchanged check, and what's left.
