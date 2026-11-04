# CG Designer — AI generator and WYSIWYG editor

The CG Designer makes new CG templates two ways, and you can mix them:

* **Generate**: write a full prompt describing the graphic. You can optionally attach examples: templates from the pack
  or your own, pasted HTML, or a reference image (a screenshot of a look you like). An OpenRouter model returns a
  complete template that follows the contract below. **Refine** sends the current design back with an instruction
  ("make the plate red, add a logo on the left"). **Variations** asks for up to 3 drafts at once.
* **Edit**: a full-screen WYSIWYG editor (`/cg/designer.html`). It has click-to-select, drag and resize with snapping to
  centre and safe-area lines, double-click text editing, an inspector (text, field binding, typography, fill, border,
  shadow), a layers list, :root colour variables, an animation preview, undo/redo and a code view. Any pack template
  can be opened as a copy.

Saved designs become ordinary templates. They appear in `GET /api/v1/cg/templates` (category `custom`, `custom: true`),
in the Overlays gallery, presets, defaults, rules, cues and CG slots, and deploy through `/api/v1/overlays/{ch}/deploy`
like any pack template.

## Template contract

A template is one self-contained HTML page that headless Chromium renders at 1920x1080 over video, keyed by its alpha.

| Rule | Detail |
|---|---|
| Frame | `<!DOCTYPE html>`; `html, body` 1920x1080, margin 0, `overflow:hidden`, `background:transparent` |
| Root | Everything that animates on and off sits inside one element with `data-cg-root`. The runtime runs the in/out animation (`animIn`, `animOut` ... query params) on it |
| Text fields | `data-field="name"` (camelCase) + optional `data-label="Name"`. The element's own text is the default value |
| Multi-line | `data-type="textarea"`, styled with `white-space: pre-line` |
| Images | `<img data-field="logo" data-type="image" src="">` is hidden while empty |
| Optional | `data-hide-empty` hides the element while its value is empty |
| Colours / sizes | CSS custom properties on `:root` (`--accent: #e11d48`). A variable is a param of the same name (`accent`) |
| Scripts | End of body: `<script src="js/cg-animations.js"></script><script src="js/cg-fields.js"></script>`. Optional extra behaviour (clock, countdown, crawl) goes in one inline script after them, using `CGFields.get(name, fallback)` and `CGFields.onChange(fn)` |
| Self-contained | No external scripts, fonts or images (the playout machine may be offline). Inline SVG / data URIs only |
| Safe areas | Title-safe 96 px left/right, 54 px top/bottom; action-safe 48 / 27 |

`web/cg/js/cg-fields.js` is the runtime. On load it fills `[data-field]` elements and `:root` variables from the URL
query. `text1`/`text2`/`text3` fill the first three text fields. It defines `window.update(fields)`, which the engine's
live field push calls, and starts `CGAnimations.init('[data-cg-root]')`.

The manifest params of a designed template come from the page itself: each `data-field` in document order (type
`text`, `textarea` or `image`, label from `data-label`, default from its content), then each `:root` variable (type
`color` when the value is a colour, else `text`). `textFields` = the first three text fields.

## Storage

* Settings `cgdesigner.index` = `{ids: [...]}`; `cgdesigner.t.<id>` = the record
  `{id, name, description, category, role, params[], textFields[], html, prompt, examples[], model, note, createdMs, updatedMs, versionSeq, versions[{n, html, savedMs, note, name}]}`.
  It keeps the last 10 versions. `examples` records what a design was made from (`{kind, id?, note?, chars?}`). Image data
  is not kept. The records are cached in memory, so the manifest merge is cheap.
* The page is written to `<webRoot>/cg/<id>.html` on every save, and again at Supervisor start when it is missing (so a
  wiped `build\web` comes back). Ids are `cgd_<slug>` (`[a-z0-9_]`, at most 48 characters).
* Settings `cgdesigner.config` = `{model, temperature (0.7), maxTokens (16000), timeoutSec (240), houseStyle}`. The model
  falls back to `ai.config.model`, then the built-in default (`anthropic/claude-sonnet-5.5`). The base URL and the
  OpenRouter key are the shared ones (`OPENROUTER_API_KEY` or `ai.config.apiKey`, `ai.config.baseUrl`; see
  docs/agents.md). `houseStyle` is free text (at most 4,000 characters, for example the station's colours, faces and
  rules). When it is not empty, it is appended to the system prompt under a "Station house style" heading. The contract
  above it always wins.
* In the manifest a design is `{id, file: "<id>.html", name, category: "custom", designCategory, icon, description,
  params, textFields, role, listed: true, custom: true, updatedMs}`. It is merged in `SupervisorCore::cgManifest()` after
  the templates.json mtime cache, so a save shows up at once.

## REST

| Method | Path | Body / answer |
|---|---|---|
| GET | `/api/v1/cg/designer` | `{ai:{hasKey, keySource, model, ...}, config, templates:[summary], pack:[{id,name,category,file,role,icon,description}], categories[], roles[]}`; a summary is the record without `html` / `versions` (plus `versionCount`, `url`), newest first |
| GET / PUT | `/api/v1/cg/designer/config` | `{model?, temperature?, maxTokens?, timeoutSec?, houseStyle?}`; the answer adds `effectiveModel`. A `houseStyle` over 4,000 characters is a 400 |
| GET | `/api/v1/cg/designer/templates` | `{templates:[summary]}` |
| GET | `/api/v1/cg/designer/templates/{id}` | the full record |
| POST | `/api/v1/cg/designer/templates` | `{id?, name, html, description?, category?, role?, params?, prompt?, examples?, model?, note?}`. No `id` creates one; an `id` that exists updates it and pushes the old page to `versions`. The html is normalised (see below), then the params are re-derived. Answer: the saved record + `warnings[]` |
| DELETE | `/api/v1/cg/designer/templates/{id}` | removes the record and the page file |
| POST | `/api/v1/cg/designer/templates/{id}/restore` | `{n}`: makes version n current (the current page becomes a version) |
| POST | `/api/v1/cg/designer/generate` | `{prompt, mode:"new"\|"refine", baseHtml?, examples:[{kind:"template", id} \| {kind:"html", html, name?, note?} \| {kind:"image", dataUrl, note?}], count?:1..3, model?, kind?, brand?:{colors, font, notes}, dryRun?}`. Answer `{jobId, state:"running", model, count}`; the work runs async. At most 4 examples, of which at most 3 images (each a `data:image/...` URL of at most about 6 MB). A template example can be a pack or a designed template |
| POST | `/api/v1/cg/designer/generate` with `dryRun: true` | answers at once `{dryRun: true, model, temperature, max_tokens, maxTokens, count, messages, hasKey, url, note?}`: the exact chat/completions body that would be sent (variation 1 when `count` > 1). Each image_url is replaced by `"<image N: K bytes>"`. It makes no call and needs no key (the editor's "View full prompt") |
| GET | `/api/v1/cg/designer/jobs/{id}` | `{id, state: running\|done\|failed, mode, model, startedMs, ms, total, pending, drafts:[{variation, meta:{name, description, category, role}, html, params, textFields, warnings[], finishReason}], errors:[{variation, error, raw?}], error?, usage:{prompt_tokens, completion_tokens, total_tokens, cost?}}` |

Errors: 400 for a missing prompt / instruction / html / refine `baseHtml`, a bad example or a bad id; 404 for an unknown
template, version or job (only the last 20 jobs are kept); 409 when there is no OpenRouter key; 413 for an oversized
page or image; 429 when 20 jobs are still running. Upstream problems do not fail the request: they fail the **job**.
That covers an OpenRouter error such as `HTTP 402: Insufficient credits`, a timeout, and an unusable reply (no page; or a
page cut off before `</body>`, with "raise cgdesigner.config maxTokens" when `finish_reason` is `length`). The job then
holds `state: failed`, `error`, and per-variation `errors[]` with the first 600 characters of the reply. A job with
several variations is `done` when at least one draft came back.

### Normalisation (server side, `cgdesign::normalizeHtml`)

* It adds the doctype / `<html>` / `<head>` / `<body>` when the page lacks them.
* It removes `<script src>` and `<link rel=stylesheet>` that point off the machine (`http:`, `https:`, `//`). It also
  removes `@import url(http...)`. Each removal is reported as a warning.
* It adds the two runtime scripts when they are missing. It puts `data-cg-root` on the first element in the body when
  no element has it.
* It strips editor leftovers (`data-cge`, `data-cge-*`, `contenteditable`, `<base data-cge-base>`).
* It adds a `<style data-cg-frame>` 1920x1080 transparent frame first in the head, unless the page already sets
  `width: 1920px` somewhere. It adds `<meta charset>` when missing.
* It adds no `data-cg-root` to a page that animates itself (`CGAnimations.init(` or `data-cg-auto`), so a pack template
  opened as a copy is not animated twice. The runtime scripts go before the page's first inline script, so that script
  can use `CGFields`.
* Remote `src` / `url()` references are kept but warned about. Running it a second time changes nothing.

### What the model gets

A system prompt holds the contract, the broadcast design rules (legibility, plates behind text, safe areas, no flashing
over 3 Hz, no full-frame fill unless asked) and the output format. The model must answer with exactly one fenced
` ```json ` block of metadata `{name, description, category, role, params[]}` and one ` ```html ` block. Example
templates are sent as HTML (at most 3, 14,000 characters each) with "match the visual language, not the content".
Example images go as `image_url` parts (the model must accept images; the default does). The model is asked not to
name fields or variables `text`, `text1`-`text3` (the generic Text 1/2/3 inputs). Refine sends the current page
and asks for the whole page back with the change applied.

## Editor notes (`web/cg/designer.html`, `js/designer.js`, `css/designer.css`)

* It reads the dashboard token from `localStorage["pp.token"]`.
* The source of truth is a parsed copy of the page (DOMParser, scripts not run). Each element gets a temporary
  `data-cge` id, and the preview iframe is rendered from it (`srcdoc` with `<base href="/cg/">`). Selecting in the
  preview maps back to the source element by that id. Style and text edits go to both, so dragging is smooth.
  Structural edits re-render the preview. Elements that the page's own script creates have no id and cannot be edited
  (the inspector says so).
* Moving uses the CSS `translate` property and resizing sets `width`/`height` in px, so the template's own
  `transform` (centring, animations) is left alone.
* Save strips the editor attributes and re-derives the params. Deploy uses the normal overlays deploy on the channel
  picked in the top bar.

## Verified (2026-10-23)

* Editor, in a browser against the test Supervisor: insert, drag with centre snapping, resize, inspector edits,
  undo/redo, in-place text edit, code view, and a clean save (no `data-cge`, no `<base>`). Save of an existing design
  kept a version; the manifest entry mapped Text 1/2 to its fields. "View full prompt" showed the dry run. A real
  **Refine** ("make the name plate deep red, add a thin gold line") on the free model came back in 34 s. It was applied
  as one undo step and kept a hand-edited field value.
* `cg-fields.js` on a test page: `text1` filled the first field, a `:root` colour param applied, an empty
  `data-hide-empty` field hid, `window.update()` returned 3, and `CGFields.onChange` fired.
* Not exercised: Take / Hide from the designer on a real channel (the test Supervisor runs no engines).

* `tests/test_cgdesigner.cpp` (11 cases) covers slug/id, reply parsing (fenced, bare, truncated, refusal), normalisation
  (fragment, remote removal, editor leftovers, own-init pages, idempotence), param derivation, the prompt (house style,
  `%N` in examples not substituted) and the dry-run image redaction.
* A test Supervisor (copied DB, no engines) was checked through REST. Create / update / restore / list / get / delete all
  work, as do the manifest merge, the page served at `/cg/<id>.html`, page re-materialisation at start, the dry run and
  the house style. A deploy through `/api/v1/overlays/{ch}/deploy` was not exercised: it stops at "engine not connected"
  before the template lookup.
* OpenRouter: the default model answered `HTTP 402: Insufficient credits` (the shared key has no credit). The job failed
  cleanly with that message. A real generation with the free `nvidia/nemotron-3-ultra-550b-a55b:free` (one template
  example) took 28 s. It returned a contract-following page (data-cg-root, data-field + data-label, :root variables, the
  two runtime scripts, a shine keyframe) that normalised with no warnings.
