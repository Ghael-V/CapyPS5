<!--
SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
SPDX-License-Identifier: GPL-2.0-only
-->

# AI per-game settings schema

The list of settings the "Recommended settings" feature may change, with type, default, range or enum values and
a description. The suggestions server puts it in the model prompt and checks the answer against it; the app checks
again before it writes the game's settings. The server keeps a copy as `schemas/capyps5-<schema_version>.json`.

Regenerate after adding, removing or changing a core option (`Settings.kt`) or a `debug.capy.*` switch:

```sh
python -I android/ai-schema/gen_schema.py . android/app/src/main/res/raw/ai_settings_schema.json
python -I android/ai-schema/gen_schema.py . --list     # every switch, UNTIERED marks the ones tiers.json lacks
```

Sources:

- The `CoreOption` enum in `Settings.kt`: the per-game options of the settings screens (key = the property name
  without `debug.capy.`, values and English titles from the enum and `strings.xml`).
- Every `CachedValue x("debug.capy.<name>", default)` under `core/src`: the hidden switches. The default comes from
  the declaration, the description from `tiers.json` or else the comment next to it. A switch only ever compared
  with 0 is a bool; any other needs a type with min/max in `tiers.json`.

`tiers.json` decides what the AI may touch:

- `allow`: the AI may change it (per-game compatibility and performance switches that are safe to flip).
- `risky`: only with a curated answer on the server, shown as experimental (can crash a game or break its start).
- `preference`: the player's taste (frame limit, frame generation, language, 3D audio). Never changed.
- Anything else is `never` (logging, dumps, traces, bisection aids, diagnosis). `never_reasons` says why for the
  ones that could look useful.

Only `allow` and `risky` are exported. When the schema changes in a way an older app cannot read, raise
`schema_version`, and give the server the new file next to the old one.
