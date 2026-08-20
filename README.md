# Hell Run Traversal Navigation

## Overview
Hell Run Traversal Navigation is an Unreal Engine 5 navigation layer for movement that standard walkable NavMesh paths do not describe well: jumps, vaults, mantles, climbs, drops, and other traversal transitions. It combines a voxel navigation representation with generated traversal data and a runtime `UHellRunTraversalComponent` that can execute the selected traversal path. The plugin also includes dynamic-obstacle support, debug tooling, configurable navigation volumes, and automated traversal/navigation tests.

## Features
- Voxel-based navigation representation for 3D/traversal-aware path generation.
- Generated jump, vault, mantle, climb, and drop traversal transitions.
- `UHellRunTraversalComponent` for executing traversal segments at runtime.
- `AHellRunVoxelNavVolume` for defining and building voxel navigation regions.
- Dynamic obstacle component for invalidating/updating affected navigation state.
- Project settings for traversal generation, voxel resolution, movement constraints, and debug behavior.
- `AHellRunVoxelPathDebugPawn` and navigation debug logging for inspecting generated routes.
- Large-volume navigation implementation intended for non-trivial environments.
- Automated dataset and traversal-execution tests included in the runtime module.

## Architecture
`HellRunVoxelNavigation` and `HellRunVoxelNavVolume` build/query the spatial representation. `HellRunTraversalNavigation` generates and evaluates the special traversal relationships that connect otherwise separated movement regions. Once a path is selected, `HellRunTraversalComponent` owns execution of its traversal portions. Dynamic obstacle support allows runtime objects to affect navigation without treating the generated data as permanently static.

## Installation
1. Clone or copy this repository to `<Project>/Plugins/HellRunTraversalNavigation`.
2. Delete stale `Binaries` and `Intermediate` folders if it was built with another Unreal Engine version.
3. Regenerate project files and compile your Editor target.
4. Launch Unreal Editor and verify **Hell Run Traversal Navigation** is enabled under **Edit > Plugins**.
5. Review the plugin's Project Settings before generating navigation for a production map; voxel size and traversal constraints directly affect cost and path quality.

```bash
git clone https://github.com/Andressalazar005/HellRunTraversalNavigation.git <Project>/Plugins/HellRunTraversalNavigation
```

## Basic setup
1. Add/configure a `HellRunVoxelNavVolume` around the space that needs traversal-aware navigation.
2. Tune voxel/navigation and traversal-generation settings for your character dimensions and movement capabilities.
3. Build/generate the navigation data for the test area.
4. Add a `HellRunTraversalComponent` to agents that need to execute generated traversal segments.
5. Use the debug pawn/logging to inspect paths and verify generated transitions before integrating them into higher-level AI movement.
6. Add `HellRunDynamicNavObstacleComponent` to runtime obstacles that should participate in dynamic navigation updates.

## Key types
- `AHellRunVoxelNavVolume` — authored volume and generated voxel-navigation owner.
- `FHellRunVoxelNavigation` — voxel path/query implementation.
- `UHellRunTraversalComponent` — runtime traversal execution component.
- `FHellRunTraversalNavigation` — traversal link/path generation logic.
- `UHellRunDynamicNavObstacleComponent` — dynamic obstacle integration.
- `AHellRunVoxelPathDebugPawn` — path visualization/debug actor.
- `UHellRunTraversalNavigationSettings` — project-wide generation/execution tuning.

## Testing
The plugin includes automated tests for navigation datasets, traversal execution, and a subterranean navigation scenario. These are useful when changing voxel dimensions, traversal rules, or path execution behavior because regressions can otherwise be difficult to distinguish from content setup problems.

## Support
Use GitHub Issues for reproducible navigation problems. Include your Unreal Engine version, voxel/traversal settings, agent dimensions, volume dimensions, start/end positions, and any debug-path output or logs.