MapLink plugin resources.

Every file here is MapLink-authored. No engine art, no third-party art, no
downloaded logos -- FAB submission has to be able to answer "where did this
come from" for every image in the package, and "we grabbed it off a website"
is not an answer.

Sources live in MapLink/authoring_icons/src/ue/ and are built with:

    cd MapLink\authoring_icons
    build_icons.cmd

That writes the four files below directly. Nothing else belongs in this folder.

    Icon128.png           Plugins browser. Name fixed by UE convention.
    MapLinkLogo_32.png    Viewport toolbar button and menu entries.
    ProxyAsset_64.png     ClassThumbnail.MapLinkProxyAsset (Content Browser tile).
    ProxyAsset_16.png     ClassIcon.MapLinkProxyAsset (Content Browser list).

Missing files are fine and are the correct state for unauthored art.
FMapLinkEditorModule::RegisterStyle registers a brush only when its PNG exists,
so the editor falls back to FSlateIconFinder and the stock plugin icon instead
of drawing an empty box. Do not drop a blank placeholder PNG in here to fill a
gap: it loads successfully, renders nothing, and defeats that fallback.

Engine types are never re-drawn. StaticMesh, Blueprint and World keep
FSlateIconFinder::FindIconForClass so the plugin follows the editor theme.
Never hardcode an engine brush path.
