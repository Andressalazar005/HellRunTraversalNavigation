# HellRun Traversal Navigation

Generated traversal navigation for Unreal Engine 5 supporting jump, vault, mantle, climb, and drop links with runtime traversal execution.

**Version:** 1.0  
**Category:** AI  
**Module:** `HellRunTraversalNavigation` (Runtime)

## Installation

From your Unreal project's `Plugins` directory:

```bash
git clone https://github.com/Andressalazar005/HellRunTraversalNavigation.git HellRunTraversalNavigation
```

Expected layout:

```text
YourProject/
  Plugins/
    HellRunTraversalNavigation/
      HellRunTraversalNavigation.uplugin
      Source/
```

Then:

1. Close Unreal Editor.
2. Delete `Binaries/` and `Intermediate/` if the plugin was previously compiled with another Unreal Engine version.
3. Regenerate project files if your C++ workflow requires it.
4. Build the project's **Development Editor** target, or launch Unreal and allow it to compile the source plugin.
5. Open **Edit > Plugins** and confirm **HellRun Traversal Navigation** is enabled.
6. Restart the editor if prompted.

## Requirements

- Unreal Engine 5 project with a working C++ toolchain.
- No additional plugin dependency is declared by `HellRunTraversalNavigation.uplugin`.

## What the plugin provides

- Generated traversal links for jump, vault, mantle, climb, and drop movement.
- Runtime traversal execution support.
- A reusable navigation layer intended to extend normal movement/navigation with authored or generated traversal opportunities.

## Verify the installation

- Confirm the `HellRunTraversalNavigation` runtime module compiles and loads.
- Check the Output Log for navigation/module initialization errors.
- Test traversal generation and execution in a small representative level before applying it to large navigation spaces.

## Used by

`HellRunTacticalLab` declares this plugin as a required dependency. Install Traversal Navigation before compiling Tactical AI Lab.

## Updating

```bash
cd YourProject/Plugins/HellRunTraversalNavigation
git pull
```

When changing Unreal Engine versions, delete `Binaries/` and `Intermediate/` before rebuilding.

## Support

Use GitHub Issues for reproducible traversal-generation, navigation, or runtime execution problems. Include your Unreal Engine version, level/navigation setup, reproduction steps, and relevant logs.
