# UAsset Importer

Import external Unreal Engine .uasset packages into your project and choose which dependencies to bring with them.

Open one or more assets in the Content Browser import dialog. Review their package paths and references, choose a destination for the assets and their dependencies, then import the selection. Keeping the source project's Content folder structure makes referenced packages easier to locate.

[Русский](README.ru.md)

## Contents

- [Features](#features)
- [Using the plugin](#using-the-plugin)
- [Limitations](#limitations)
- [Building from source](#building-from-source)

## Features

- Review a dependency tree before copying packages. See which references are available in the source, already present in the project, or missing.
- Preserve package paths or organize imported assets and their dependencies under a chosen folder.
- Control replacement of existing assets and keep recovery copies during import.

## Using the plugin

### Enable the plugin

Open **Edit → Plugins**, search for **UAsset Importer**, enable it and restart the editor if prompted.

### Select assets and dependencies

1. Open the destination folder in the **Content Browser**. Drag one or more external `.uasset` files into the asset view, or click **Import** and select the files.
2. In **Assets to Import**, select the assets you want. Use **Load selected previews** to load the selected packages and render available thumbnails.
3. Expand **Dependencies**. Review missing references and select the source packages you want to copy. Leaving out a referenced package requires a compatible copy at the path the asset expects.

### Choose where packages go

**Destination** controls the selected assets. Use the current Content Browser folder, the project Content root, a custom folder, or the original package paths. **Dependency placement** lets you keep original paths, preserve the source structure inside a Dependencies folder, flatten dependencies into one folder, or place them beside the imported assets. Preserving original package paths also keeps dependencies at their original paths.

### Import and check the result

By default, missing required dependencies stop the import. Missing soft references can be reported as warnings. **Overwrite existing assets** is off and **Roll back copied files if import fails** is on. Enable overwrite only when you intend to replace the destination packages.

Click **Import**. By default the Content Browser selects the imported assets. Loading the selected assets after import is a separate option. Check **UAsset Importer** in the Message Log for reported problems and the Output Log for the summary. If recovery is incomplete, keep the backup directory named in the error.

## Limitations

- Select external `.uasset` files. Maps (`.umap`) are outside this workflow.
- Keep package sidecars beside the corresponding asset and preserve the source Content hierarchy when possible. The importer cannot recover missing source data.
- The editor must be able to read the packages and resolve the classes they use. Cooked, corrupt or incompatible packages may fail. Install required project plugins before opening assets that depend on their classes.

## Building from source

### Build in a project

Close the editor. From the root of a C++ project, run:

```shell
git clone https://github.com/DontFWithTheJ/UE-UAsset-Importer.git Plugins/UAssetImporter
```

Check that `Plugins/UAssetImporter/UAssetImporter.uplugin` exists. Refresh project files if your IDE requires it. Build the project's **Development Editor** target for your platform, then open the project and enable the plugin.

### Package with Unreal Automation Tool

On Windows, run the following in Command Prompt with paths for your installation. Choose an empty output directory outside the source tree.

```bat
"C:\UE\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin -Plugin="D:\Project\Plugins\UAssetImporter\UAssetImporter.uplugin" -Package="D:\Packages\UAssetImporter" -TargetPlatforms=Win64
```

Use the engine installation you intend to target. `-Plugin` points to the plugin descriptor, `-Package` sets the output directory, and `-TargetPlatforms` selects the target platform.

### Use the packaged plugin

Copy the output folder containing `UAssetImporter.uplugin` to `YourProject/Plugins/UAssetImporter/` for one project, or to `Engine/Plugins/Marketplace/UAssetImporter/` for projects using that engine installation. Enable the plugin under **Edit → Plugins**. Rebuild and check API compatibility when changing engine versions.
