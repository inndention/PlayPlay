# uispec: the button system's single source of truth

Design: [docs/plans/ux-buttons.md](../../docs/plans/ux-buttons.md) section 3.3.

| file | role |
|---|---|
| `assets/ui/buttons.json` | tokens (sizes, roles, fills, state layers, timing, LEDs), the theme name table, swatches, the icon catalogue (`iconGroups`), the glyph to icon map used by the shims, reserved hotkeys and hotkey rules, the safety levels and per-action safety defaults, editor limits. Its `about` field explains the conventions. |
| `assets/ui/icons.json` | the in-house icons, drawn on a 24x24 grid: `{ "name": { "d": [path...], "fd": [filled path...], "fill": bool, "sw": 1.75, "tags": [...], "origin": "original" } }` |
| `tools/uispec/gen_uispec.cmake` | the generator (a `cmake -P` script: no Python or Node) |

Generated outputs. They are committed, so never edit them by hand:

| output | for |
|---|---|
| `Play_Play_Native/qml/PlayPlay/Buttons/spec.js` | the QML module (`.pragma library`; one `var` per buttons.json key, plus `icons`) |
| `web/ui-spec.js` | web helpers (an ES module with the same values as exports) |
| `web/icons.svg` | the web sprite: `<symbol id="i-NAME" viewBox="0 0 24 24">` |
| `web/ui-tokens.css` | `:root` custom properties: `--info`, `--role-*`, `--btn-*`, `--led-*` |

## Changing a token or an icon

1. Edit `assets/ui/buttons.json` or `assets/ui/icons.json`.
2. Regenerate the outputs: `cmake -P tools/uispec/gen_uispec.cmake` from the repo root, or build the `gen_uispec` target. Only files
   whose content changed are rewritten.
3. Run `test_uispec`. It fails while any output differs from a fresh generation, and it also checks:
   - the JSON: no duplicate keys, names, fields and cross references;
   - that the theme hex values equal `Theme.qml` and `web/style.css`;
   - the safety floors;
   - the hotkey forms;
   - the icon path syntax (Qt-safe: separate arc flags, no exponents, no `1.5.5` runs; inside the 24x24 grid), with no
     zero-length segment (one under 0.1 units, such as an arc back to its own start) and no subpath that draws nothing: Qt skips
     those, while Chromium draws a round-cap dot there;
   - that every `iconGroups` name is drawn (the set is complete since UXB-2) and that each icon's first tag is its group id;
   - the icon geometry: ink (path bounds plus half the stroke) inside 1.5..22.5, its centre within 2.25 of (12, 12), no arc with a
     radius under `sw / 2`, and filled parts of one icon that do not overlap;
   - that the outputs load in QML, as an ES module, as XML and as CSS;
   - that every icon name used in QML / web sources is drawn, and that every `IconButton` has a `tip`.
4. Run `test_buttons_qml` too. Its `tst_iconset.qml` builds every icon of the generated `spec.js` as a Qt `Shape` (`PathSvg`, round
   caps and joins) and compares it with the path data as read in JS:
   - bounds: `Shape.boundingRect` (the bounding box of Qt's parse, widened by `sw / 2`) must match the JS bounds. This catches a
     path Qt stops parsing early or reads with other numbers, but not a difference inside the box.
   - ink: Qt's raster, grabbed at 8x, must be inked at every sampled point of every path. This catches a piece Qt drops.

   The runner is headless (offscreen platform, software renderer), so it does not exercise CurveRenderer. It also cannot see ink
   that only a browser draws, or browser stroking quirks; the data rules in step 3 cover those. Check a new icon in a browser and in
   a CurveRenderer window as well.

## Drawing icons

- Use the 24x24 grid with content in about 3..21, round caps and joins, and stroke width 1.75. `sw` takes quarter steps from 1 to 2.5.
- Absolute commands with spaces between all numbers keep the paths readable. Write arc flags as separate `0` / `1`.
- A `fill: true` icon (play, stop, record ...) is filled **and** stroked at `sw` with round joins, which rounds its corners. Draw it
  inset by `sw / 2`. To fill part of a stroked icon (the program monitor's dot, the wipe's filled half), use `fd` paths, which are
  rendered the same way.
- Filled parts of one icon must not overlap each other: the web sprite fills nonzero, a Qt `ShapePath` odd-even by default, and
  only disjoint parts look the same under both.
- Keep details at least about 2 units apart (one stroke plus a gap): the s / m buttons draw icons at 13 / 15 px, where a unit is
  about 0.6 px. Check a new icon at 12-15 px, not only large.
- Draw every icon in-house: `origin` is `"original"`, and nothing is copied from Lucide, Feather or similar sets. Add the name to a
  group in `buttons.json` `iconGroups`, and give it tags, because the icon picker searches them. The first tag is the group id.
- No segment may have zero length, and no subpath may be a bare moveto. When cutting a shape away (the slash of `bolt-off` and
  `eye-off`), drop the pieces that vanish entirely: `eye-off` has no pupil, because the 2.75-unit gap swallows the whole r 2.75
  circle.
- No arc may be tighter than `sw / 2` (0.875 at sw 1.75), in a filled or a stroked part. A tighter arc folds the stroke's inner
  edge over. Chromium (Skia) then leaves a hole that Qt fills: a filled r 0.25 dot came out as a ring on the web.
- Shared shapes keep the set coherent:
  - screens are the rounded frame 3.5..20.5 x 4.5..19.5 (r 1.5);
  - monitors add the stand of `preview`;
  - arrow heads are 45-degree chevrons;
  - small dots (the i, ! and ? dots, calendar days, remote buttons) are short stroked segments 0.25 long, such as
    `M12 16 V16.25`, which ink to 1.75 x 2;
  - larger solid dots (`more`, `drag`, the agent's eyes) are filled circles of r 1, inking to 3.75.
