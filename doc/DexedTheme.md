# Themes and user data

AV Dexed draws its interface with JUCE vector graphics and embedded SVG assets. It does not load the upstream `DexedTheme.xml` format or raster skin files.

New application settings, user presets, and cartridge banks are stored in the product-specific `AV Vibes/AV Dexed` data folder. On first launch, AV Dexed checks supported legacy data locations and copies existing settings and banks into the new folder; it leaves the originals untouched.
