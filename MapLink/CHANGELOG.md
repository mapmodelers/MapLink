# MapLink changelog

## 1.21.0 - Experimental (first public release)

- Import `.blend` hierarchies as Level Instances (`LI_`), Blueprints (`BP_`) and static meshes (`SM_`), nested to any depth
- Nuke-and-recreate reimport with a pre-rebuild integrity check, and a watchdog on every Blender launch
- MapLink Manager: asset states, reimport, favorites, filters, an inspector with previews and texture health, an asset viewer
- Material Instance Manager: Unreal-authoritative materials synced to Blender libraries; material library export and import between projects
- Texture Manager: conform, import, clean up; color space and compression recorded for Blender
- World Partition expansion, plural and re-targetable, discoverable from a level's actors
- Blueprint stand-ins (BP Reference Mode)
- Optional Perforce changelist routing on both sides, with an opt-in pre-reimport shelve
- Blender addon: project scaffolding, asset shelf, vendor-in, Standardize VC / UV, batch tools, outliner cleanup, cross-root link checks
