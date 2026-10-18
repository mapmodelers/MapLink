# AGENTS.md: MapLink

Start here before editing. Written for AI agents; useful to people too.

## What it is

An Unreal Engine 5.7 plugin and a Blender 5.0 addon. Blender is the source of truth: `.blend` hierarchies become Level Instances (`LI_`), Blueprints (`BP_`) and static meshes (`SM_`) in Unreal. Every reimport rebuilds the asset from its `.blend`, after an integrity check. Unreal is the authority on materials.

## Layout

- `MapLink/Source/MapLinkEditor`, `MapLink/Source/MapLinkRuntime`: C++ modules
- `MapLink/blender/`: the Blender addon (Python)
- `MapLink/Scripts/`: Python run by Unreal and Blender
- `MapLink/Content/`: plugin assets
- `docs/SPECS.md`: versions and requirements

## Build

C++ project, Visual Studio 2022, Unreal Engine 5.7. Or `RunUAT BuildPlugin -Plugin=MapLink/MapLink.uplugin`.

## Rules

- The `LI_`, `BP_`, `SM_` prefixes are load-bearing; keep them.
- Keep copyright headers and license notices.
- License: `LICENSE.md`. The Blender addon is GPL-3.0-or-later.
