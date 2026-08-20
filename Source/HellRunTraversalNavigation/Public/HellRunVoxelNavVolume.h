#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Volume.h"
#include "HellRunVoxelNavigation.h"
#include "HellRunVoxelNavVolume.generated.h"

/**
 * Immutable directed traversal edge produced by the editor bake. Search-time
 * cost profiles scale BaseCost, but may never change edge topology or mode.
 */
USTRUCT()
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunBakedVoxelEdge
{
    GENERATED_BODY()

    UPROPERTY()
    int32 ToNode = INDEX_NONE;

    UPROPERTY()
    EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;

    /** Geometric/settings cost before character-specific locomotion multipliers. */
    UPROPERTY()
    float BaseCost = 0.0f;
};

USTRUCT()
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunBakedVoxelNode
{
    GENERATED_BODY()

    UPROPERTY()
    FVector Location = FVector::ZeroVector;

    UPROPERTY()
    FVector WallNormal = FVector::ZeroVector;

    UPROPERTY()
    bool bGround = false;

    UPROPERTY()
    bool bClimb = false;

    /** Cardinal +X,-X,+Y,-Y support sampled at bake time. Prevents walking across unsampled narrow gaps. */
    UPROPERTY()
    uint8 GroundExitMask = 0;

    /** Cardinal exits blocked at capsule height but clear above, used to distinguish vaults from gaps. */
    UPROPERTY()
    uint8 ObstacleExitMask = 0;

    UPROPERTY()
    int32 CellIndex = INDEX_NONE;

    /** Contiguous range in AHellRunVoxelNavVolume::Edges. */
    UPROPERTY()
    int32 FirstEdge = INDEX_NONE;

    UPROPERTY()
    int32 EdgeCount = 0;

    /** Weakly connected grounded component assigned by the current bake. */
    UPROPERTY()
    int32 ComponentId = INDEX_NONE;
};

/**
 * Placeable, editor-baked 3D navigation volume. Runtime queries never trace or
 * voxelize geometry; they only search this serialized sparse graph.
 */
/** Read-only export row used by editor tools without exposing graph storage. */
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunBakedTraversalSegment
{
    FVector Start = FVector::ZeroVector;
    FVector End = FVector::ZeroVector;
    EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
    float BaseCost = 0.0f;
};

UCLASS(Blueprintable)
class HELLRUNTRAVERSALNAVIGATION_API AHellRunVoxelNavVolume : public AVolume
{
    GENERATED_BODY()

public:
    static constexpr int32 CurrentBakedGraphVersion = 33;

    AHellRunVoxelNavVolume(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
    virtual void PostLoad() override;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="HellRun Voxel Navigation")
    void BuildNavigationData();

    UFUNCTION(CallInEditor, BlueprintCallable, Category="HellRun Voxel Navigation")
    void ClearNavigationData();

    UFUNCTION(BlueprintPure, Category="HellRun Voxel Navigation")
    bool HasBakedNavigationData() const { return BakedGraphVersion >= 30 && Nodes.Num() > 0 && CellToNode.Num() > 0; }

    bool HasCurrentBakedNavigationData() const
    {
        return BakedGraphVersion >= CurrentBakedGraphVersion
            && Nodes.Num() > 0 && CellToNode.Num() > 0;
    }

    UFUNCTION(BlueprintPure, Category="HellRun Voxel Navigation")
    int32 GetBakedNodeCount() const { return Nodes.Num(); }

    /**
     * Returns baked ground surfaces suitable for spawning, rather than the
     * capsule-center locations used by path following. Candidates are limited
     * to ground nodes retained by the editor's grounded-agent component bake.
     * Disconnected ground islands are removed before serialization. Origin is
     * used only for the requested distance ring.
     */
    void GetSpawnSurfaceLocationsInRange(
        const FVector& Origin,
        float MinimumDistance,
        float MaximumDistance,
        TArray<FVector>& OutLocations,
        int32 MaximumSamples) const;

    /** Ground nodes retained by the capability-aware bake, in capsule-center space. */
    void GetRetainedGroundNodeLocations(TArray<FVector>& OutLocations) const;

    /** Capability-aware, bounded EQS/editor sampling from the authoritative
     * voxel graph. Returns capsule-center nodes and excludes dynamic blockers. */
    void GetQueryNodeLocationsInRange(
        const FVector& Origin,
        float MinimumDistance,
        float MaximumDistance,
        bool bIncludeGround,
        bool bIncludeClimb,
        bool bIncludeFlight,
        TArray<FVector>& OutLocations,
        int32 MaximumSamples) const;

    /** Returns reachable-space ground nodes adjacent to a wall recorded by the
     * authoritative voxel bake. These are cover locations, not a generic node
     * cloud; EQS still validates threat occlusion and path reachability. */
    void GetQueryCoverLocationsInRange(
        const FVector& Origin,
        const FVector& CoverContext,
        float MinimumDistance,
        float MaximumDistance,
        TArray<FVector>& OutLocations,
        int32 MaximumSamples,
        float MaximumFacingDot = -0.1f) const;

    bool IsCurrentQueryNodeLocation(const FVector& Location,
        bool bCanWalk,bool bCanClimb,bool bCanFly) const;

    /** Snaps an editor/EQS context to the nearest current graph node. */
    bool ResolveQueryLocation(const FVector& Location,bool bCanWalk,
        bool bCanClimb,bool bCanFly,FVector& OutLocation) const;

    /** Batched EQS path test against the current directed voxel graph. One graph
     * traversal answers every candidate, avoiding a full path search per item. */
    void TestQueryPathReachability(
        const FVector& Start,
        const TArray<FVector>& Goals,
        bool bCanWalk,
        bool bCanClimb,
        bool bCanMantle,
        bool bCanDrop,
        bool bCanJump,
        bool bCanVault,
        bool bCanFly,
        TArray<bool>& OutReachable) const;

    /** Returns voxel graph control points for rendering the selected EQS route. */
    bool BuildQueryPath(
        const FVector& Start,
        const FVector& Goal,
        bool bCanWalk,
        bool bCanClimb,
        bool bCanMantle,
        bool bCanDrop,
        bool bCanJump,
        bool bCanVault,
        bool bCanFly,
        TArray<FVector>& OutPath) const;

    /** Exports immutable baked edges for editor analysis tools. */
    void GetBakedTraversalSegments(
        TArray<struct FHellRunBakedTraversalSegment>& OutSegments,
        bool bIncludeWalkEdges = false,
        int32 MaximumSegments = 100000) const;

#if WITH_DEV_AUTOMATION_TESTS
    /** Read-only deterministic samples used by the map-backed navigation tests. */
    void GetAutomationGroundNodeLocations(
        TArray<FVector>& OutLocations,
        int32 MaximumSamples) const;

    /** Finds a deterministic real-map endpoint pair that requires the requested mode. */
    bool GetAutomationTraversalProbe(
        const ACharacter& Character,
        EHellRunVoxelSegment Mode,
        FVector& OutStart,
        FVector& OutGoal) const;

    int32 GetAutomationLastDisconnectedGroundCullCount() const
    {
        return LastBakeDisconnectedGroundCullCount;
    }
#endif
    bool HasAuthoritativeTypedEdgeGraph() const
    {
        return BakedGraphVersion >= 30 && Nodes.Num() > 0
            && FlightNeighborMasks.Num() == Nodes.Num();
    }

    bool ContainsRouteEndpoints(const FVector& Start, const FVector& Goal) const;
    FNavPathSharedPtr FindPath(const ACharacter& Character, const FVector& Start, const FVector& Goal) const;
    FNavPathSharedPtr FindSharedPath(const ACharacter& Character, const FVector& Start, const FVector& Goal) const;
    const FString& GetLastPathDiagnostic() const { return LastPathDiagnostic; }

    /** Invalidates target fields after runtime navigation-affecting geometry changes. */
    UFUNCTION(BlueprintCallable, Category="HellRun Voxel Navigation|Dynamic Obstacles")
    void InvalidateDynamicNavigation();

    void UpdateDynamicObstacle(UObject* ObstacleSource, const FBox& ObstacleBounds, bool bRemove);

    UPROPERTY(EditAnywhere, Category="HellRun Voxel Navigation|Debug")
    bool bDrawBakedGraph = false;

    UPROPERTY(EditAnywhere, Category="HellRun Voxel Navigation|Debug", meta=(ClampMin="1"))
    int32 MaximumDebugNodes = 5000;

    /** Draws directed jump/vault/mantle/drop edges; continuous walk/fly/climb edges remain hidden. */
    UPROPERTY(EditAnywhere, Category="HellRun Voxel Navigation|Debug")
    bool bDrawBakedTraversalEdges = true;

    UPROPERTY(EditAnywhere, Category="HellRun Voxel Navigation|Debug", meta=(ClampMin="1"))
    int32 MaximumDebugTraversalEdges = 5000;

    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }

private:
    /** Keeps the baked-data volume from blocking gameplay movement and projectiles. */
    void EnforceNonBlockingCollision();

    /** Clean-room regular-grid implementation used by graph version 20+. */
    void BuildNavigationDataV2();
    FNavPathSharedPtr FindPathV2(
        const ACharacter& Character,
        const FVector& Start,
        const FVector& Goal) const;
    int32 FlattenCell(const FIntVector& Cell) const;
    FIntVector UnflattenCell(int32 FlatIndex) const;
    int32 FindNearestNode(const FVector& Location, bool bCanWalk, bool bCanClimb, bool bCanFly) const;
    void BuildTypedEdgesAndComponents(
        const UHellRunTraversalNavigationSettings& Settings,
        const FCollisionObjectQueryParams& Objects,
        const FCollisionQueryParams& Params,
        float AgentRadius,
        float AgentHalfHeight);
    void DrawBakedGraph() const;
    void EnsureSpatialOctree() const;
    int32 BuildSpatialOctreeNode(const FBox& Bounds, const TArray<int32>& NodeIndices, int32 Depth) const;
    void QuerySpatialOctree(const FBox& Bounds, TArray<int32>& OutNodeIndices) const;
    void RebuildRuntimeSearchCostCache() const;
    uint32 ComputeBakedDataHash() const;

    struct FSharedFlowField
    {
        int32 GoalNode = INDEX_NONE;
        uint32 AgentSignature = 0;
        uint32 NavigationRevision = 0;
        double ExpirationTime = 0.0;
        TMap<int32, int32> NextNode;
        TMap<int32, EHellRunVoxelSegment> NextMode;
    };

    mutable TMap<uint64, FSharedFlowField> SharedFlowFields;
    uint32 DynamicNavigationRevision = 0;

    struct FSpatialOctreeNode
    {
        FBox Bounds = FBox(ForceInit);
        TArray<int32> NodeIndices;
        TArray<int32> Children;
    };

    mutable TArray<FSpatialOctreeNode> SpatialOctree;
    mutable bool bSpatialOctreeBuilt = false;
    TMap<TWeakObjectPtr<UObject>, TArray<int32>> DynamicObstacleNodes;
    TMap<int32, int32> DynamicBlockedNodeRefCounts;

    UPROPERTY(VisibleAnywhere, Category="HellRun Voxel Navigation|Baked Data")
    FVector GridOrigin = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, Category="HellRun Voxel Navigation|Baked Data")
    FIntVector GridDimensions = FIntVector::ZeroValue;

    UPROPERTY(VisibleAnywhere, Category="HellRun Voxel Navigation|Baked Data")
    float BakedVoxelSize = 0.0f;

    UPROPERTY(VisibleAnywhere, Category="HellRun Voxel Navigation|Baked Data")
    int32 BakedGraphVersion = 0;

    UPROPERTY(VisibleAnywhere, Category="HellRun Voxel Navigation|Baked Data")
    FBox BakedBounds = FBox(ForceInit);

    UPROPERTY()
    TArray<int32> CellToNode;

    UPROPERTY()
    TArray<FHellRunBakedVoxelNode> Nodes;

    UPROPERTY()
    TArray<FHellRunBakedVoxelEdge> Edges;

    /**
     * One bit for each of the 26 adjacent regular-grid cells. Flight edges
     * are derived from this compact clearance mask at query time instead of
     * serializing millions of redundant reflected edge structs.
     */
    UPROPERTY()
    TArray<uint32> FlightNeighborMasks;

    mutable TStaticArray<float, 7> MinimumBaseEdgeCostByMode;
    mutable bool bRuntimeSearchCostCacheBuilt = false;
    mutable bool bDebugTraversalEdgeCacheBuilt = false;
    mutable TArray<int32> DebugTraversalFromNodeIndices;
    mutable TArray<int32> DebugTraversalEdgeIndices;
    mutable FString LastPathDiagnostic;
    int32 LastBakeDisconnectedGroundCullCount = 0;
};
