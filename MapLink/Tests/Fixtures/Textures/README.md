# Texture container fixtures — byte-spec reference

**Do not edit, re-save, or "optimise" these files.** They are the locked byte
specification for `FMapLinkTgaWriter` (v1.16.0) and the property specification
for Phase 2's preview-PNG encoder. Any change silently invalidates the
idempotence acceptance test in the v1.16 texture-manager spec.

Authored in Substance Designer, 64×64, flat fill.

| File | Container | Role | MD5 |
|---|---|---|---|
| `template_rgb.tga` | TGA type 2, 24 bpp | authoritative colour texture | `5caeaaf0c184687c634be3402e21ac0d` |
| `template_g.tga` | TGA type 3, 8 bpp | authoritative single-channel texture | `0fec198648e637106e565118b55a31fe` |
| `template_rgb.png` | PNG colour type 2 | Blender preview, colour | `a189a04d65fb1e3706cf7ce3aac3467e` |
| `template_g.png` | PNG colour type 0 | Blender preview, single-channel | `02e0f9d5a44ce89c62a187821620bc9e` |

## Why they live here and not in `Content/`

UAT `BuildPlugin` copies `Source`, `Content`, `Resources` and `Config` by
default, and `Config/FilterPlugin.ini` supports include patterns only — there is
no exclusion syntax. Anything under `Content/` therefore ships to marketplace
customers. Fixtures are a development artifact, so they sit under `Tests/`,
which is outside the default copy set and is not listed in `FilterPlugin.ini`.
Tracked in git, never shipped.

## Acceptance test they back

`template_rgb.tga` and `template_g.tga`, imported into UE and conformed as
MapLink-class textures, must be rewritten **byte-identically** — `fc /b` reports
no differences, and a second full conform run performs zero writes. The
descriptor byte and the TGA 2.0 footer are the two places that break this.

Constants derived from these files are recorded in the texture tests that read them.
