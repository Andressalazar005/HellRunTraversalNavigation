#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HellRunTraversalNavigationSettings.generated.h"

class ACharacter;

UENUM(BlueprintType)
enum class EHellRunNavigationMode : uint8
{
    GeneratedLinks UMETA(DisplayName="Recast + Generated Links"),
    VolumetricHybrid UMETA(DisplayName="Volumetric Traversal Preferred"),
    AutomaticHybrid UMETA(DisplayName="Automatic Per-Agent Hybrid")
};

USTRUCT(BlueprintType)
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunTraversalLinkSettings
{
    GENERATED_BODY()

    /** Recast link generation only. The voxel baker always records typed traversal topology. */
    UPROPERTY(EditAnywhere, Config, Category="Recast Link",
        meta=(DisplayName="Generate Recast Link"))
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, Config, Category="Reach", meta=(ClampMin="0.0", Units="cm"))
    float HorizontalReach = 150.0f;

    UPROPERTY(EditAnywhere, Config, Category="Reach", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float DistanceFromEdge = 10.0f;

    UPROPERTY(EditAnywhere, Config, Category="Reach", meta=(Units="cm"))
    float MaximumDepth = 150.0f;

    UPROPERTY(EditAnywhere, Config, Category="Trajectory", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float ArcHeight = 50.0f;

    UPROPERTY(EditAnywhere, Config, Category="Reach", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float EndpointHeightTolerance = 80.0f;

    UPROPERTY(EditAnywhere, Config, Category="Sampling", meta=(ClampMin="1.0", AdvancedDisplay))
    float SamplingSeparationFactor = 1.0f;

    UPROPERTY(EditAnywhere, Config, Category="Sampling", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float SimilarLinkFilterDistance = 80.0f;

    UPROPERTY(EditAnywhere, Config, Category="Recast Link", meta=(AdvancedDisplay))
    bool bCreateCenterLink = true;

    UPROPERTY(EditAnywhere, Config, Category="Recast Link", meta=(AdvancedDisplay))
    bool bCreateExtremityLinks = false;
};

/** Per-agent preferences used by the baked voxel A* planner. Base movement costs still come from the global settings below. */
USTRUCT(BlueprintType)
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunVoxelTraversalCostProfile
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float WalkMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float ClimbMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float MantleMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float DropMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float JumpMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float VaultMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Movement Multipliers", meta=(ClampMin="0.01"))
    float FlightMultiplier = 1.0f;

    /** Paid when consecutive path edges ask the locomotion FSM to change state. Prevents walk/climb/mantle thrashing. */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Route Stability", meta=(ClampMin="0.0"))
    float LocomotionStateChangePenalty = 0.35f;

    /** Paid according to the angle between consecutive edges: zero when straight and double this value for a reversal. */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Route Stability", meta=(ClampMin="0.0"))
    float TurnPenalty = 0.8f;

    /** Makes a climber prefer progressing vertically instead of wandering sideways across a wall. */
    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Route Stability", meta=(ClampMin="1.0"))
    float ClimbLateralMultiplier = 1.2f;

    UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category="Route Stability", meta=(ClampMin="1.0"))
    float ClimbDownMultiplier = 1.15f;
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="HellRun Traversal Navigation"))
class HELLRUNTRAVERSALNAVIGATION_API UHellRunTraversalNavigationSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UHellRunTraversalNavigationSettings();
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    UPROPERTY(EditAnywhere, Config, Category="General",
        meta=(DisplayName="Path Provider"))
    EHellRunNavigationMode NavigationMode = EHellRunNavigationMode::AutomaticHybrid;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Grid", meta=(ClampMin="25.0", Units="cm"))
    float VoxelSize = 100.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Endpoint Search", meta=(ClampMin="0.0", Units="cm"))
    float VoxelSearchPadding = 600.0f;

    /**
     * Maximum horizontal distance for connecting a runtime actor position to
     * the serialized graph. The connector is capsule-swept and floor-validated,
     * so this expands discovery without allowing paths through walls.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Endpoint Search",
        meta=(ClampMin="0.0", Units="cm"))
    float VoxelEndpointConnectionRadius = 2000.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Search Limits", meta=(ClampMin="100"))
    int32 MaximumVoxelSearchNodes = 2500;

    /**
     * Builds one reverse cost field per target/capability profile. This is faster,
     * but every similar agent inherits the same route and cannot distribute among
     * near-equal platform paths.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Shared Fields")
    bool bUseSharedTargetFlowFields = false;

    /** Number of deterministic near-equal route lanes available to similar agents. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Route Diversity", meta=(ClampMin="1", ClampMax="5"))
    int32 AlternativeRouteBuckets = 3;

    /**
     * Tiny heuristic tie-break used only after travel cost. It distributes agents
     * without allowing a meaningfully slower route to beat the closest fast route.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Route Diversity",
        meta=(ClampMin="0.0", ClampMax="0.01"))
    float NearEqualRouteDiversityWeight = 0.0015f;

    /** Safe lateral spread applied to traversal endpoints when collision allows it. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Route Diversity",
        meta=(ClampMin="0.0", ClampMax="60.0", Units="cm"))
    float TraversalCorridorHalfWidth = 24.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Shared Fields", meta=(ClampMin="100"))
    int32 MaximumSharedFlowFieldNodes = 30000;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Shared Fields", meta=(ClampMin="0.1", Units="s"))
    float SharedFlowFieldLifetime = 2.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Spatial Index")
    bool bUseOctreeSpatialIndex = true;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Spatial Index", meta=(ClampMin="4"))
    int32 OctreeLeafNodeCapacity = 32;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Spatial Index", meta=(ClampMin="1", ClampMax="16"))
    int32 OctreeMaximumDepth = 8;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Cache", meta=(ClampMin="0.0", Units="s"))
    float VoxelRouteCacheDuration = 1.5f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Cache", meta=(ClampMin="0.0", Units="cm"))
    float VoxelRouteCacheGoalTolerance = 150.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Provider Selection", meta=(ClampMin="0.0", Units="cm"))
    float VolumetricActivationHeight = 80.0f;

    /**
     * In Automatic mode, accept a complete Recast path without also running
     * synchronous voxel A*. Voxel search remains available for vertical,
     * partial, or unreachable routes where authored traversal is required.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Performance")
    bool bSkipVoxelSearchForDirectGroundPaths = true;

    /** Ground routes longer than this ratio remain eligible for a traversal shortcut. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Performance",
        meta=(ClampMin="1.0", ClampMax="5.0"))
    float GroundPathVoxelSearchDetourRatio = 1.2f;

    /** Small absolute detours stay on Recast even when their ratio is high. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Performance",
        meta=(ClampMin="0.0", Units="cm"))
    float GroundPathVoxelSearchMinimumExcessDistance = 200.0f;

    /** Maximum ordinary synchronous enemy path builds allowed in one game frame. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Performance|Crowd Budget",
        meta=(ClampMin="1", ClampMax="64"))
    int32 MaximumSynchronousPathRequestsPerFrame = 2;

    /**
     * Additional per-frame slots reserved for an idle enemy that has no route
     * yet or an attacker with a director commitment.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Performance|Crowd Budget",
        meta=(ClampMin="0", ClampMax="32"))
    int32 MaximumUrgentPathRequestsPerFrame = 1;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Surface Detection", meta=(ClampMin="0.0", Units="cm"))
    float ClimbSurfaceProbeDistance = 55.0f;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Surface Detection", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float ClimbSurfaceClearance = 8.0f;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Surface Detection", meta=(ClampMin="0.0", Units="cm"))
    float VoxelFloorProbeDepth = 70.0f;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Agent", meta=(ClampMin="1.0", Units="cm"))
    float VoxelBakeAgentRadius = 34.0f;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Agent", meta=(ClampMin="1.0", Units="cm"))
    float VoxelBakeAgentHalfHeight = 88.0f;

    /** Height above the sampled floor used for baked ground-node capsule centers. */
    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Agent", meta=(ClampMin="0.0", Units="cm"))
    float VoxelGroundClearance = 2.0f;

    /**
     * Allows capsule-validated diagonal ground edges. This must remain enabled
     * for authored openings narrower than two voxels: a doorway can fall
     * between cardinal grid columns depending on the volume's grid phase.
     * Every diagonal is still rejected unless its complete supported capsule
     * sweep is clear, so this does not permit corner cutting.
     */
    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Ground Connectivity")
    bool bAllowDiagonalVoxelWalk = true;

    /**
     * Extra search cost for a walk node on the boundary of a ground surface.
     * Interior nodes receive no penalty; corners and wall-adjacent lanes receive
     * progressively more so paths favor usable corridor centers.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Route Stability",
        meta=(ClampMin="0.0"))
    float GroundBoundaryAvoidancePenalty = 0.65f;

    /** Automatically rebuild every placed voxel nav volume when the editor runs Build Paths or Build All. */
    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Build Integration")
    bool bBakeVoxelVolumesWithBuildPaths = true;

    /** Hard guard against accidentally baking an impractically large volume. */
    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Limits", meta=(ClampMin="1000", AdvancedDisplay))
    int32 MaximumVoxelBakeCells = 2000000;

    /** Draw sampled free cells while an editor bake is in progress. */
    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Visualization")
    bool bVisualizeVoxelBake = true;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Visualization", meta=(ClampMin="1", AdvancedDisplay))
    int32 VoxelBakeVisualizationStride = 16;

    UPROPERTY(EditAnywhere, Config, Category="Voxel Bake|Surface Detection")
    FName NoClimbActorTag = TEXT("NoClimb");

    /** Base graph cost per voxel. Per-character WalkMultiplier is applied afterward. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Base Movement Costs",
        meta=(ClampMin="0.01", DisplayName="Walk Base Cost"))
    float VoxelWalkCost = 0.8f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Base Movement Costs",
        meta=(ClampMin="0.01", DisplayName="Climb Base Cost"))
    float VoxelClimbCost = 1.05f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Base Movement Costs",
        meta=(ClampMin="0.01", DisplayName="Mantle Base Cost"))
    float VoxelMantleCost = 1.0f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Base Movement Costs",
        meta=(ClampMin="0.01", DisplayName="Flight Base Cost"))
    float VoxelFlightCost = 1.0f;

    /** Stable three-bucket route variation. Zero makes every agent use identical traversal costs. */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Advanced|Route Diversity", meta=(ClampMin="0.0", ClampMax="0.35"))
    float TraversalRouteVariety = 0.08f;

    /**
     * Reject a route containing typed traversal when the same character
     * profile cannot find any route back to its source region. This prevents
     * drops/mantles from admitting enemies into one-way trap islands.
     */
    UPROPERTY(EditAnywhere, Config, Category="Routing|Safety")
    bool bRequireTraversalDestinationEscapeRoute = true;

    /** Used unless the character component overrides it or a matching character-class entry exists below. */
    UPROPERTY(EditAnywhere, Config, Category="Agent Cost Profiles",
        meta=(DisplayName="Default Agent Profile"))
    FHellRunVoxelTraversalCostProfile DefaultVoxelCostProfile;

    /** Most-derived loaded character class wins. Add zombie/flyer/etc Blueprint classes here. */
    UPROPERTY(EditAnywhere, Config, Category="Agent Cost Profiles",
        meta=(DisplayName="Profiles by Character Class"))
    TMap<TSoftClassPtr<ACharacter>, FHellRunVoxelTraversalCostProfile> VoxelCostProfilesByCharacterClass;

    FHellRunVoxelTraversalCostProfile GetVoxelCostProfileForCharacter(const ACharacter& Character) const;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Flight", meta=(ClampMin="1.0", Units="cm/s"))
    float FlightSpeed = 700.0f;

    UPROPERTY(EditAnywhere, Config, Category="Debug|Voxel Search")
    bool bDrawVoxelSearch = false;

    UPROPERTY(EditAnywhere, Config, Category="Debug|Voxel Search", meta=(ClampMin="0.0", Units="s"))
    float VoxelDebugLifetime = 2.0f;

    UPROPERTY(EditAnywhere, Config, Category="Traversal Generation|Jump")
    FHellRunTraversalLinkSettings Jump;

    UPROPERTY(EditAnywhere, Config, Category="Traversal Generation|Vault")
    FHellRunTraversalLinkSettings Vault;

    UPROPERTY(EditAnywhere, Config, Category="Traversal Generation|Mantle")
    FHellRunTraversalLinkSettings Mantle;

    UPROPERTY(EditAnywhere, Config, Category="Traversal Generation|Climb")
    FHellRunTraversalLinkSettings Climb;

    UPROPERTY(EditAnywhere, Config, Category="Traversal Generation|Drop")
    FHellRunTraversalLinkSettings Drop;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Jump", meta=(ClampMin="1.0", Units="cm/s"))
    float JumpSpeed = 820.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Jump", meta=(ClampMin="0.0", Units="cm"))
    float JumpMinimumArcHeight = 190.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Vault", meta=(ClampMin="1.0", Units="cm/s"))
    float VaultSpeed = 720.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Vault", meta=(ClampMin="0.0", Units="cm"))
    float VaultMinimumArcHeight = 145.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Mantle", meta=(ClampMin="1.0", Units="cm/s"))
    float MantleVerticalSpeed = 480.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Mantle", meta=(ClampMin="1.0", Units="cm/s"))
    float MantlePullOverSpeed = 560.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Climb", meta=(ClampMin="1.0", Units="cm/s"))
    float ClimbVerticalSpeed = 420.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Climb", meta=(ClampMin="1.0", Units="cm/s"))
    float ClimbPullOverSpeed = 520.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Landing", meta=(ClampMin="0.0", Units="cm"))
    float CapsuleLandingClearance = 4.0f;

    /** CharacterMovement step height used by traversal-capable agents. Keep this aligned with baked ground connectivity. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="0.0", Units="cm"))
    float GroundStepHeight = 75.0f;

    /** Full 3D reach tolerance used for climb, mantle, drop, and flight voxel segments. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="1.0", Units="cm"))
    float VoxelTraversalPointReachDistance = 28.0f;

    /** Allows a crowded ground approach to commit into its next vertical voxel edge without occupying one exact point. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="1.0", Units="cm"))
    float VoxelTraversalEntryReachDistance = 55.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="0.0", Units="cm"))
    float LowObstacleJumpMaximumHeight = 160.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="1.0", Units="cm/s"))
    float LowObstacleJumpForwardSpeed = 650.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Ground", meta=(ClampMin="1.0", Units="cm/s"))
    float LowObstacleJumpVerticalSpeed = 500.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Landing", meta=(ClampMin="0.0", Units="cm"))
    float PullOverHeight = 12.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="1.0", Units="cm/s"))
    float DropMinimumHorizontalSpeed = 650.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(Units="cm/s"))
    float DropMaximumUpwardSpeed = 160.0f;

    /** Distance the committed drop FSM moves past the ledge before enabling gravity. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="0.0", Units="cm"))
    float DropTakeoffForwardDistance = 65.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="1.0", Units="cm/s"))
    float DropTakeoffSpeed = 900.0f;

    /** Leaves the takeoff point this far short of the lower landing node's XY position. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="0.0", Units="cm"))
    float DropTakeoffLandingInset = 12.0f;

    /** Controlled fallback speed if CharacterMovement is stopped by ledge collision after launch. */
    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="1.0", Units="cm/s"))
    float DropRecoverySpeed = 900.0f;

    UPROPERTY(EditAnywhere, Config, Category="Movement|Drop", meta=(ClampMin="0.1", Units="s"))
    float DropStuckDetectionDelay = 0.25f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Recast Area Costs")
    float JumpAreaCost = 1.15f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Recast Area Costs")
    float VaultAreaCost = 1.08f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Recast Area Costs")
    float MantleAreaCost = 1.25f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Recast Area Costs")
    float ClimbAreaCost = 1.4f;

    UPROPERTY(EditAnywhere, Config, Category="Routing|Recast Area Costs")
    float DropAreaCost = 1.05f;

    UPROPERTY(EditAnywhere, Config, Category="Debug|Area Colors")
    FColor JumpAreaColor = FColor(0, 220, 255);

    UPROPERTY(EditAnywhere, Config, Category="Debug|Area Colors")
    FColor VaultAreaColor = FColor::Green;

    UPROPERTY(EditAnywhere, Config, Category="Debug|Area Colors")
    FColor MantleAreaColor = FColor(255, 70, 220);

    UPROPERTY(EditAnywhere, Config, Category="Debug|Area Colors")
    FColor ClimbAreaColor = FColor(145, 70, 255);

    UPROPERTY(EditAnywhere, Config, Category="Debug|Area Colors")
    FColor DropAreaColor = FColor(255, 145, 0);

    UPROPERTY(EditAnywhere, Config, Category="Movement|Spawning", meta=(ClampMin="0.0", Units="cm"))
    float SpawnNavSurfaceClearance = 2.0f;

    UPROPERTY(EditAnywhere, Config, Category="Debug")
    bool bDrawActualClimbPaths = true;

    UPROPERTY(EditAnywhere, Config, Category="Debug", meta=(EditCondition="bDrawActualClimbPaths", ClampMin="0.1"))
    float ClimbPathThickness = 6.0f;

    UPROPERTY(EditAnywhere, Config, Category="Debug", meta=(EditCondition="bDrawActualClimbPaths", ClampMin="0.0", Units="cm"))
    float ClimbPathPullOverHeight = 18.0f;

    UPROPERTY(EditAnywhere, Config, Category="Debug", meta=(EditCondition="bDrawActualClimbPaths"))
    FColor ClimbPathColor = FColor(145, 70, 255);

    UPROPERTY(EditAnywhere, Config, Category="Debug", meta=(EditCondition="bDrawActualClimbPaths", ClampMin="0.05", Units="s"))
    float DebugCacheRefreshInterval = 0.5f;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
