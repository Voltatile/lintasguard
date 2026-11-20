# Architecture notes

See README.md sections 4 to 8 for the design. This file records deeper notes.

## Decisions log

| Date | Decision | Reason |
|---|---|---|
| 2026-11-20 | Licence: Apache-2.0 | Recommended in README section 19; detectable by GitHub. |
| 2026-11-20 | Host unit tests use plain asserts/exit codes, built by the top-level Makefile | Keeps Step 2 dependency-free. |
