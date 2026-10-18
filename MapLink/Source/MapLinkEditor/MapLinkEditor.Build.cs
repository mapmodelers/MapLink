using UnrealBuildTool;

public class MapLinkEditor : ModuleRules
{
    public MapLinkEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "MapLinkRuntime",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd",
            "AssetTools",
            "AssetRegistry",
            "ContentBrowser",
            "EditorScriptingUtilities",
            "LevelEditor",
            "BlueprintGraph",
            "KismetCompiler",
            "LevelInstanceEditor",
            "Kismet",
            "PropertyEditor",
            "ToolMenus",
            "Blutility",
            "Json",
            "JsonUtilities",
            "Projects",
            "Slate",
            "SlateCore",
            "InputCore",
            "SourceControl",
            "DesktopPlatform",
            // v1.10 (MIM Drop 2) — embedded material preview viewport
            "AdvancedPreviewScene",
            "RenderCore",
            // v1.20 — MapLinkMaterialExporter renders material-instance
            // thumbnails offscreen. That reaches two RHI globals directly:
            // GMaxRHIFeatureLevel (the FCanvas feature level) and GRHIGlobals
            // (pulled in by the readback path). Both are RHI_API, so the
            // module has to declare RHI or it compiles and fails at link.
            "RHI",
            // v1.10 (MIM Drop 2.1) — texture-parameter thumbnails
            "EditorWidgets",
            // v1.13 — SCustomDialog (labeled-button modals)
            "ToolWidgets",
            // v1.13 — clipboard for Copy Diagnostics
            "ApplicationCore",
            // v1.16 — Texture Manager: FImage / FImageView for FTextureSource
            // pixel access (mip 0), independent of the on-disk import format.
            "ImageCore",
            // v1.16 — Texture Manager: preview-PNG encoding. Added now, first
            // used by FMapLinkTexturePreviewGen in Phase 2.
            "ImageWrapper"
        });
    }
}
