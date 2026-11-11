# UAsset Importer (UE 5.7)

Editor-only Unreal Engine plugin for importing external `.uasset` packages into the current project. It opens a single import dialog from **File → Import .uasset Files...** and keeps successful imports silent apart from the Output Log summary. Warnings or errors are also written to the UAsset Importer Message Log, which opens automatically when there are problems.

## Installation

1. Close Unreal Editor.
2. Copy the `UAssetImporter` folder into `<Project>/Plugins/`.
3. Reopen the project and enable **UAsset Importer** under Edit → Plugins if required.
4. Compile the project Editor target.
5. Use **File → Import .uasset Files...**.

## Import workflow

1. Select one or more `.uasset` files in the native file picker.
2. Review the asset list, thumbnails, class metadata, source path, and available saved-engine-version metadata.
3. Select which top-level assets to import and configure the import options.
4. Start the import. Dependency packages discovered through the Asset Registry are included automatically when their source files can be resolved.
5. On success, only a short summary is written to Output Log. No success dialog or summary window appears. When warnings or errors occur, the UAsset Importer Message Log opens automatically.

## Options

- **Abort if a required dependency is missing**: on by default. Missing required dependencies stop the operation before file changes.
- **Ignore missing soft references**: on by default. Missing soft references are logged as warnings.
- **Overwrite existing assets**: off by default.
- **Rollback changes on failure**: on by default. Overwritten package files are backed up for the duration of the operation.
- **Sync selected assets in Content Browser**: on by default.
- **Load selected assets after import**: off by default. Existing assets are not force-reloaded.

Option values are saved in the project's editor-per-project config, rather than exposed a second time in Editor Preferences.

## Important initial-version limits

- Source directories are discovered by locating the nearest ancestor named `Content`. When no `Content` ancestor exists, the selected file's containing directory is treated as the root.
- `/Game` packages preserve their relative path under the project's `Content` directory. Plugin-mounted packages require the corresponding content plugin to exist in the destination project. Importing engine-mounted content directly is blocked.
- Dependency discovery relies on metadata that Unreal can read from the source package. Cooked, corrupt, unsupported, or newer-incompatible packages may not be readable. Packages with missing sidecar files or missing external dependencies may remain unusable.
- This first test build exposes the native file-picker workflow. Direct Windows Explorer drag-and-drop into Content Browser is not wired up in this initial source drop; it needs a dedicated Content Browser drag/drop integration and should be added only after the core copy/rollback path has been validated.
- The code has been prepared against UE 5.7 editor APIs but cannot be compiled in this environment because an Unreal Engine installation and project are not available here. Treat it as a test build and send back the first compiler errors from Rider/UBT if any appear.
