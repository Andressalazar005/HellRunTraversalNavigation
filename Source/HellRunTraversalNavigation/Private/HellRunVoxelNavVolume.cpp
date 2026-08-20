#include "HellRunVoxelNavVolume.h"

#include "Components/BrushComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "HellRunTraversalComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

namespace
{
    bool IsNavigationTopology(
        const AActor* Actor,
        const UPrimitiveComponent* Component)
    {
        // Pawns are transient occupants. They must never create or remove baked
        // graph topology; crowd avoidance handles them at execution time.
        if (Actor && Actor->IsA<APawn>()) return false;

        // Collision and navigation relevance are separate authoring controls
        // in Unreal. Decorative/blocking collision with "Can Ever Affect
        // Navigation" disabled must not remove voxels or traversal edges.
        return !Component || Component->CanEverAffectNavigation();
    }

    bool CapsuleOverlapsTopology(
        const UWorld& World,
        const FVector& Location,
        const FCollisionObjectQueryParams& Objects,
        const FCollisionQueryParams& Params,
        float Radius,
        float HalfHeight)
    {
        TArray<FOverlapResult> Overlaps;
        World.OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity, Objects,
            FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);
        return Overlaps.ContainsByPredicate([](const FOverlapResult& Overlap)
        {
            return IsNavigationTopology(
                Overlap.GetActor(),
                Overlap.GetComponent());
        });
    }

    bool TraceTopology(
        const UWorld& World,
        FHitResult& OutHit,
        const FVector& Start,
        const FVector& End,
        const FCollisionObjectQueryParams& Objects,
        const FCollisionQueryParams& Params)
    {
        TArray<FHitResult> Hits;
        World.LineTraceMultiByObjectType(Hits, Start, End, Objects, Params);
        for (const FHitResult& Hit : Hits)
        {
            if (IsNavigationTopology(
                    Hit.GetActor(),
                    Hit.GetComponent()))
            {
                OutHit = Hit;
                return true;
            }
        }
        return false;
    }

    bool SweepTopologyClear(
        const UWorld& World,
        const FVector& Start,
        const FVector& End,
        const FCollisionObjectQueryParams& Objects,
        const FCollisionQueryParams& Params,
        const FCollisionShape& Shape)
    {
        TArray<FHitResult> Hits;
        World.SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, Objects, Shape, Params);
        return !Hits.ContainsByPredicate([](const FHitResult& Hit)
        {
            return IsNavigationTopology(
                Hit.GetActor(),
                Hit.GetComponent());
        });
    }

    uint8 CardinalDirectionBit(int32 DX, int32 DY)
    {
        if (DX > 0) return 1u << 0;
        if (DX < 0) return 1u << 1;
        if (DY > 0) return 1u << 2;
        if (DY < 0) return 1u << 3;
        return 0;
    }

    uint8 OppositeCardinalDirectionBit(int32 DX, int32 DY)
    {
        return CardinalDirectionBit(-DX, -DY);
    }

    bool IsDiscreteTraversal(EHellRunVoxelSegment Mode)
    {
        return Mode == EHellRunVoxelSegment::Jump
            || Mode == EHellRunVoxelSegment::Vault
            || Mode == EHellRunVoxelSegment::Mantle
            || Mode == EHellRunVoxelSegment::Drop;
    }

    void GetTraversalBoundaryPoints(
        const FVector& FromNode,
        const FVector& ToNode,
        float VoxelSize,
        EHellRunVoxelSegment Mode,
        FVector& OutEntry,
        FVector& OutExit)
    {
        OutEntry = FromNode;
        OutExit = ToNode;
        const FVector HorizontalDelta(
            ToNode.X - FromNode.X,
            ToNode.Y - FromNode.Y,
            0.0f);
        const float HorizontalDistance = HorizontalDelta.Size();
        if (HorizontalDistance <= UE_SMALL_NUMBER) return;

        const FVector Direction = HorizontalDelta / HorizontalDistance;
        const float Inset = FMath::Min(
            VoxelSize * 0.5f,
            HorizontalDistance * 0.5f);
        OutEntry += Direction * Inset;
        // Mantle and Drop execution own the complete arrival onto the landing
        // surface. Ending those actions at the geometric mid-edge creates a
        // synthetic Walk connector whose capsule begins inside the ledge face.
        // Jump/Vault still use paired takeoff/landing boundaries.
        if (Mode == EHellRunVoxelSegment::Jump
            || Mode == EHellRunVoxelSegment::Vault)
        {
            OutExit -= Direction * Inset;
        }
    }

    bool IsTraversalEdgeClear(const ACharacter& Character, const FVector& From, const FVector& To,
        EHellRunVoxelSegment Mode, const UHellRunTraversalNavigationSettings& Settings)
    {
        if (Mode != EHellRunVoxelSegment::Jump
            && Mode != EHellRunVoxelSegment::Vault)
        {
            // Mantles intentionally travel beside the obstacle being climbed.
            // A free-flight capsule sweep therefore rejects the supporting wall
            // and disconnects otherwise valid platform layers. Their endpoints
            // are capsule-validated when baked, and execution performs swept
            // vertical/pull-over movement so unexpected geometry still blocks
            // the character and requests a new route.
            return true;
        }

        const UWorld* World = Character.GetWorld();
        const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
        if (!World || !Capsule) return false;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunTraversalEdgeClearance), false, &Character);
        const FCollisionShape Shape = FCollisionShape::MakeCapsule(
            Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
        auto SegmentClear = [&](
            const FVector& A,
            const FVector& B,
            bool bAllowTakeoffSupport,
            bool bAllowLandingSupport)
        {
            TArray<FHitResult> Hits;
            World->SweepMultiByObjectType(Hits, A, B, FQuat::Identity, Objects, Shape, Params);
            for (const FHitResult& Hit : Hits)
            {
                if (IsNavigationTopology(
                        Hit.GetActor(),
                        Hit.GetComponent())
                    && !Cast<AHellRunVoxelNavVolume>(Hit.GetActor()))
                {
                    const bool bUpwardSupport =
                        Hit.ImpactNormal.Z > 0.7f
                        || Hit.Normal.Z > 0.7f;
                    const bool bEndpointNumericalContact =
                        (bAllowTakeoffSupport && Hit.Time <= 0.05f)
                        || (bAllowLandingSupport && Hit.Time >= 0.95f);
                    if (bEndpointNumericalContact
                        || (bUpwardSupport
                            && (bAllowTakeoffSupport
                                || bAllowLandingSupport)))
                    {
                        continue;
                    }
                    return false;
                }
            }
            return true;
        };

        // Baked ground nodes place the capsule exactly on its supporting
        // surface. Sweep queries starting at that contact report the floor as
        // an initial overlap and would reject every airborne transition.
        // Lift only the validation trajectory; executable endpoints remain
        // grounded.
        constexpr float GroundContactTolerance = 2.0f;
        const FVector ClearanceFrom =
            From + FVector(0.0f, 0.0f, GroundContactTolerance);
        const FVector ClearanceTo =
            To + FVector(0.0f, 0.0f, GroundContactTolerance);
        const float MinimumArc = Mode == EHellRunVoxelSegment::Jump
            ? Settings.JumpMinimumArcHeight : Settings.VaultMinimumArcHeight;
        const float Arc = FMath::Max(
            MinimumArc,
            FMath::Abs(ClearanceTo.Z - ClearanceFrom.Z) + 70.0f);
        FVector Previous = ClearanceFrom;
        constexpr int32 ArcSamples = 8;
        for (int32 Sample = 1; Sample <= ArcSamples; ++Sample)
        {
            const float Alpha = static_cast<float>(Sample) / ArcSamples;
            FVector Point = FMath::Lerp(
                ClearanceFrom, ClearanceTo, Alpha);
            Point.Z += FMath::Sin(Alpha * UE_PI) * Arc;
            if (!SegmentClear(
                Previous,
                Point,
                Sample == 1,
                Sample == ArcSamples))
            {
                return false;
            }
            Previous = Point;
        }
        return true;
    }
}

AHellRunVoxelNavVolume::AHellRunVoxelNavVolume(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    SetCanBeDamaged(false);
    EnforceNonBlockingCollision();
}

void AHellRunVoxelNavVolume::PostLoad()
{
    Super::PostLoad();
    // Level instances can contain serialized collision settings that override
    // constructor defaults. This actor stores baked navigation data and must
    // never behave as physical world geometry at runtime.
    EnforceNonBlockingCollision();
    RebuildRuntimeSearchCostCache();
}

void AHellRunVoxelNavVolume::EnforceNonBlockingCollision()
{
    SetActorEnableCollision(false);
    if (UBrushComponent* VolumeBrush = GetBrushComponent())
    {
        VolumeBrush->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        VolumeBrush->SetGenerateOverlapEvents(false);
        VolumeBrush->SetCollisionResponseToAllChannels(ECR_Ignore);
    }
}

void AHellRunVoxelNavVolume::RebuildRuntimeSearchCostCache() const
{
    for (float& MinimumCost : MinimumBaseEdgeCostByMode)
    {
        MinimumCost = BIG_NUMBER;
    }
    for (const FHellRunBakedVoxelEdge& Edge : Edges)
    {
        const int32 ModeIndex = static_cast<int32>(Edge.Mode);
        if (ModeIndex >= 0 && ModeIndex < MinimumBaseEdgeCostByMode.Num()
            && Edge.BaseCost > 0.0f)
        {
            MinimumBaseEdgeCostByMode[ModeIndex] = FMath::Min(
            MinimumBaseEdgeCostByMode[ModeIndex], Edge.BaseCost);
        }
    }
    if (FlightNeighborMasks.Num() == Nodes.Num())
    {
        const UHellRunTraversalNavigationSettings* Settings =
            GetDefault<UHellRunTraversalNavigationSettings>();
        MinimumBaseEdgeCostByMode[
            static_cast<int32>(EHellRunVoxelSegment::Fly)] =
                Settings->VoxelFlightCost;
    }
    bRuntimeSearchCostCacheBuilt = true;
}

FIntVector AHellRunVoxelNavVolume::UnflattenCell(int32 FlatIndex) const
{
    if (FlatIndex < 0 || GridDimensions.X <= 0 || GridDimensions.Y <= 0) return FIntVector::ZeroValue;
    const int32 XY = GridDimensions.X * GridDimensions.Y;
    const int32 Z = FlatIndex / XY;
    const int32 Remainder = FlatIndex - Z * XY;
    return FIntVector(Remainder % GridDimensions.X, Remainder / GridDimensions.X, Z);
}

int32 AHellRunVoxelNavVolume::FlattenCell(const FIntVector& Cell) const
{
    if (Cell.X < 0 || Cell.Y < 0 || Cell.Z < 0 || Cell.X >= GridDimensions.X || Cell.Y >= GridDimensions.Y || Cell.Z >= GridDimensions.Z)
    {
        return INDEX_NONE;
    }
    return Cell.X + GridDimensions.X * (Cell.Y + GridDimensions.Y * Cell.Z);
}

void AHellRunVoxelNavVolume::ClearNavigationData()
{
    Modify();
    GridOrigin = FVector::ZeroVector;
    GridDimensions = FIntVector::ZeroValue;
    BakedVoxelSize = 0.0f;
    BakedGraphVersion = 0;
    BakedBounds = FBox(ForceInit);
    CellToNode.Reset();
    Nodes.Reset();
    Edges.Reset();
    FlightNeighborMasks.Reset();
    for (float& MinimumCost : MinimumBaseEdgeCostByMode)
    {
        MinimumCost = BIG_NUMBER;
    }
    bRuntimeSearchCostCacheBuilt = false;
    DebugTraversalFromNodeIndices.Reset();
    DebugTraversalEdgeIndices.Reset();
    bDebugTraversalEdgeCacheBuilt = false;
    SpatialOctree.Reset();
    bSpatialOctreeBuilt = false;
    DynamicObstacleNodes.Reset();
    DynamicBlockedNodeRefCounts.Reset();
    SharedFlowFields.Reset();
    ++DynamicNavigationRevision;
    MarkPackageDirty();
}

void AHellRunVoxelNavVolume::BuildNavigationData()
{
    UPackage* MapPackage = GetOutermost();
    const bool bPackageWasDirty = MapPackage && MapPackage->IsDirty();
    const uint32 PreviousDataHash = ComputeBakedDataHash();

    // Version 24 is the sole production builder. Keep the legacy body below
    // temporarily for binary history, but never produce its incompatible
    // query-time-adjacency dataset from the editor button.
    BuildNavigationDataV2();

    // Editor navigation generation may run again while opening an already
    // baked map. Do not make that map look modified when the resulting graph
    // is identical to the graph that was loaded from disk.
    if (MapPackage
        && !bPackageWasDirty
        && PreviousDataHash == ComputeBakedDataHash())
    {
        MapPackage->SetDirtyFlag(false);
    }
    return;

    UWorld* World = GetWorld();
    if (!World) return;
    Modify();
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    BakedVoxelSize = FMath::Max(25.0f, S->VoxelSize);
    BakedGraphVersion = 2;
    BakedBounds = GetComponentsBoundingBox(true);
    GridOrigin = BakedBounds.Min;
    const FVector Size = BakedBounds.GetSize();
    GridDimensions = FIntVector(
        FMath::Max(1, FMath::CeilToInt(Size.X / BakedVoxelSize)),
        FMath::Max(1, FMath::CeilToInt(Size.Y / BakedVoxelSize)),
        FMath::Max(1, FMath::CeilToInt(Size.Z / BakedVoxelSize)));
    const int64 CellCount64 = static_cast<int64>(GridDimensions.X) * GridDimensions.Y * GridDimensions.Z;
    if (CellCount64 <= 0 || CellCount64 > MAX_int32 || CellCount64 > S->MaximumVoxelBakeCells)
    {
        UE_LOG(LogTemp, Error, TEXT("Voxel navigation volume %s requires %lld cells; the configured limit is %d. Increase Voxel Size or Maximum Voxel Bake Cells."),
            *GetName(), CellCount64, S->MaximumVoxelBakeCells);
        ClearNavigationData();
        return;
    }

#if WITH_EDITOR
    FNotificationInfo NotificationInfo(FText::Format(
        NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeStarted", "Building voxel navigation: {0}"),
        FText::FromString(GetActorLabel())));
    NotificationInfo.bFireAndForget = false;
    NotificationInfo.bUseThrobber = true;
    const TSharedPtr<SNotificationItem> BuildNotification = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
    FScopedSlowTask BuildProgress(static_cast<float>(CellCount64 * 2),
        NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeProgress", "Building HellRun voxel navigation"));
    BuildProgress.MakeDialogDelayed(0.5f, false);
#endif

    CellToNode.Init(INDEX_NONE, static_cast<int32>(CellCount64));
    Nodes.Reset();
    Edges.Reset();
    SpatialOctree.Reset();
    bSpatialOctreeBuilt = false;
    DynamicObstacleNodes.Reset();
    DynamicBlockedNodeRefCounts.Reset();
    SharedFlowFields.Reset();
    ++DynamicNavigationRevision;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunVoxelBake), false, this);
    const float Radius = S->VoxelBakeAgentRadius;
    const float HalfHeight = S->VoxelBakeAgentHalfHeight;
    const float EffectiveFloorProbeDepth = FMath::Max(
        S->VoxelFloorProbeDepth, BakedVoxelSize + S->VoxelGroundClearance);
    const FVector Directions[] = { FVector::ForwardVector, FVector::BackwardVector, FVector::RightVector, FVector::LeftVector };
    TMap<FIntVector, int32> GroundSurfaceNodes;

    for (int32 Z = 0; Z < GridDimensions.Z; ++Z)
    for (int32 Y = 0; Y < GridDimensions.Y; ++Y)
    for (int32 X = 0; X < GridDimensions.X; ++X)
    {
#if WITH_EDITOR
        BuildProgress.EnterProgressFrame(1.0f, NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeSampling", "Sampling navigable voxel cells"));
#endif
        const FIntVector Cell(X, Y, Z);
        const FVector Center = GridOrigin + FVector(X + 0.5f, Y + 0.5f, Z + 0.5f) * BakedVoxelSize;
        if (!EncompassesPoint(Center)) continue;
        if (CapsuleOverlapsTopology(*World, Center, Objects, Params, Radius, HalfHeight)) continue;

        FHellRunBakedVoxelNode Node;
        Node.Location = Center;
        FHitResult FloorHit;
        Node.bGround = TraceTopology(*World, FloorHit, Center,
            Center - FVector(0, 0, HalfHeight + EffectiveFloorProbeDepth), Objects, Params)
            && FloorHit.ImpactNormal.Z >= 0.55f;
        if (Node.bGround)
        {
            // A ground voxel represents the character capsule center, not the
            // arbitrary center of its 3D cell. Keeping the cell-center Z made
            // walkers float and made adjacent floor cells look like climbs.
            const FVector GroundLocation(Center.X, Center.Y,
                FloorHit.ImpactPoint.Z + HalfHeight + S->VoxelGroundClearance);
            if (!CapsuleOverlapsTopology(
                *World, GroundLocation, Objects, Params, Radius, HalfHeight))
            {
                Node.Location = GroundLocation;
            }
            else
            {
                Node.bGround = false;
            }
        }
        if (Node.bGround)
        {
            // Surface topology must not depend on the Z phase of the 3D grid.
            // Several free voxel layers can project onto the same floor; alias
            // all of them to one canonical ground node instead of creating
            // coincident, disconnected nodes.
            const float SurfaceBandHeight = FMath::Max(10.0f, S->GroundStepHeight * 0.5f);
            const FIntVector SurfaceKey(
                X, Y, FMath::RoundToInt(FloorHit.ImpactPoint.Z / SurfaceBandHeight));
            if (const int32* ExistingNode = GroundSurfaceNodes.Find(SurfaceKey))
            {
                CellToNode[FlattenCell(Cell)] = *ExistingNode;
                continue;
            }
            GroundSurfaceNodes.Add(SurfaceKey, Nodes.Num());
        }
        float BestWallDistance = BIG_NUMBER;
        for (const FVector& Direction : Directions)
        {
            FHitResult WallHit;
            if (TraceTopology(*World, WallHit, Node.Location,
                Node.Location + Direction * (Radius + S->ClimbSurfaceProbeDistance), Objects, Params)
                && FMath::Abs(WallHit.ImpactNormal.Z) < 0.35f
                && (!WallHit.GetActor() || !WallHit.GetActor()->ActorHasTag(S->NoClimbActorTag))
                && WallHit.Distance < BestWallDistance)
            {
                BestWallDistance = WallHit.Distance;
                Node.bClimb = true;
                Node.WallNormal = WallHit.ImpactNormal.GetSafeNormal2D();
            }
        }
        Node.CellIndex = FlattenCell(Cell);
        const int32 NodeIndex = Nodes.Add(Node);
        CellToNode[Node.CellIndex] = NodeIndex;
#if WITH_EDITOR
        if (S->bVisualizeVoxelBake && NodeIndex % FMath::Max(1, S->VoxelBakeVisualizationStride) == 0)
        {
            const FColor Color = Node.bGround ? FColor::Cyan : (Node.bClimb ? FColor(145, 70, 255) : FColor::Blue);
            DrawDebugPoint(World, Node.Location, 4.0f, Color, false, 10.0f, 5);
        }
#endif
    }

    // Serialize cardinal ground continuity. Center-only floor samples made two
    // ground nodes on opposite sides of a narrow gap look walk-connected.
    const FIntVector CardinalCells[] = {
        FIntVector(1,0,0), FIntVector(-1,0,0), FIntVector(0,1,0), FIntVector(0,-1,0) };
    for (FHellRunBakedVoxelNode& Node : Nodes)
    {
        if (!Node.bGround) continue;
        for (const FIntVector& Direction : CardinalCells)
        {
            const FVector DirectionVector(static_cast<double>(Direction.X), static_cast<double>(Direction.Y), 0.0);
            const uint8 DirectionBit = CardinalDirectionBit(Direction.X, Direction.Y);
            bool bSupported = true;
            for (const float SampleAlpha : { 0.25f, 0.5f })
            {
                const FVector SampleXY = Node.Location + DirectionVector * (BakedVoxelSize * SampleAlpha);
                FHitResult SupportHit;
                const FVector TraceStart(SampleXY.X, SampleXY.Y, Node.Location.Z + S->GroundStepHeight);
                const FVector TraceEnd = TraceStart - FVector(0.0f, 0.0f,
                    HalfHeight + S->GroundStepHeight + S->VoxelFloorProbeDepth);
                if (!TraceTopology(*World, SupportHit, TraceStart, TraceEnd, Objects, Params)
                    || SupportHit.ImpactNormal.Z < 0.55f
                    || FMath::Abs((Node.Location.Z - HalfHeight - S->VoxelGroundClearance)
                        - SupportHit.ImpactPoint.Z) > S->GroundStepHeight)
                {
                    bSupported = false;
                    break;
                }
            }
            if (bSupported)
            {
                Node.GroundExitMask |= DirectionBit;
            }

            const FVector BoundaryCenter = Node.Location + DirectionVector * (BakedVoxelSize * 0.5f);
            const bool bBlockedAtCapsule = CapsuleOverlapsTopology(
                *World, BoundaryCenter, Objects, Params, Radius, HalfHeight);
            const bool bClearAbove = !CapsuleOverlapsTopology(
                *World, BoundaryCenter + FVector(0.0f, 0.0f, BakedVoxelSize),
                Objects, Params, Radius, HalfHeight);
            if (bBlockedAtCapsule && bClearAbove)
            {
                Node.ObstacleExitMask |= DirectionBit;
            }
        }
    }

    // The bake owns compact spatial truth and surface semantics. Traversal
    // connectivity remains capability-dependent and is classified by the
    // query, so no single agent profile is frozen into the serialized graph.
    BakedGraphVersion = 2;
    Edges.Reset();

#if WITH_EDITOR
    BuildProgress.EnterProgressFrame(static_cast<float>(CellCount64),
        NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeFinalize", "Finalizing compact voxel graph"));
#endif
    MarkPackageDirty();
    UE_LOG(LogTemp, Display, TEXT("Baked compact voxel navigation volume %s: %d nodes, %d cells (typed adjacency generated during queries)."),
        *GetName(), Nodes.Num(), CellToNode.Num());
#if WITH_EDITOR
    if (BuildNotification.IsValid())
    {
        BuildNotification->SetText(FText::Format(
            NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeComplete", "Voxel navigation built: {0} nodes"),
            FText::AsNumber(Nodes.Num())));
        BuildNotification->SetCompletionState(SNotificationItem::CS_Success);
        BuildNotification->ExpireAndFadeout();
    }
#endif
}

void AHellRunVoxelNavVolume::BuildTypedEdgesAndComponents(
    const UHellRunTraversalNavigationSettings& S,
    const FCollisionObjectQueryParams& Objects,
    const FCollisionQueryParams& Params,
    float AgentRadius,
    float AgentHalfHeight)
{
    UWorld* World = GetWorld();
    Edges.Reset();
    if (!World || Nodes.IsEmpty()) return;

    TArray<TArray<FHellRunBakedVoxelEdge>> NodeEdges;
    NodeEdges.SetNum(Nodes.Num());
    auto AddEdge = [&NodeEdges](int32 FromNode, int32 ToNode, EHellRunVoxelSegment Mode, float BaseCost)
    {
        if (FromNode == ToNode || !NodeEdges.IsValidIndex(FromNode)) return;
        TArray<FHellRunBakedVoxelEdge>& Outgoing = NodeEdges[FromNode];
        if (FHellRunBakedVoxelEdge* Existing = Outgoing.FindByPredicate(
            [ToNode, Mode](const FHellRunBakedVoxelEdge& Edge)
            {
                return Edge.ToNode == ToNode && Edge.Mode == Mode;
            }))
        {
            Existing->BaseCost = FMath::Min(Existing->BaseCost, BaseCost);
            return;
        }
        FHellRunBakedVoxelEdge& Edge = Outgoing.AddDefaulted_GetRef();
        Edge.ToNode = ToNode;
        Edge.Mode = Mode;
        Edge.BaseCost = BaseCost;
    };
    auto TraversalClear = [World, &Objects, &Params, AgentRadius, AgentHalfHeight, &S](
        const FVector& From, const FVector& To, EHellRunVoxelSegment Mode)
    {
        const FCollisionShape Shape = FCollisionShape::MakeCapsule(AgentRadius, AgentHalfHeight);
        auto SegmentClear = [World, &Objects, &Params, &Shape](const FVector& A, const FVector& B)
        {
            return SweepTopologyClear(*World, A, B, Objects, Params, Shape);
        };
        if (Mode == EHellRunVoxelSegment::Walk
            || Mode == EHellRunVoxelSegment::Climb
            || Mode == EHellRunVoxelSegment::Fly)
        {
            return SegmentClear(From, To);
        }
        if (Mode == EHellRunVoxelSegment::Mantle)
        {
            // Match UHellRunTraversalComponent exactly: back away from the
            // supporting face, rise beside it, then pull over to the landing.
            // Validating a vertical sweep at the un-offset edge position
            // incorrectly rejected every real wall-backed mantle.
            const FVector MantleForward = (To - From).GetSafeNormal2D();
            const FVector LiftBase = From - MantleForward
                * FMath::Max(S.Mantle.DistanceFromEdge, 12.0f);
            const FVector VerticalTarget(
                LiftBase.X, LiftBase.Y, To.Z + S.PullOverHeight);
            return SegmentClear(From, LiftBase)
                && SegmentClear(LiftBase, VerticalTarget)
                && SegmentClear(VerticalTarget, To);
        }

        const float MinimumArc = Mode == EHellRunVoxelSegment::Jump
            ? S.JumpMinimumArcHeight
            : (Mode == EHellRunVoxelSegment::Vault
                ? S.VaultMinimumArcHeight
                : FMath::Max(20.0f, S.DropTakeoffForwardDistance * 0.35f));
        const float Arc = Mode == EHellRunVoxelSegment::Drop
            ? MinimumArc
            : FMath::Max(MinimumArc, FMath::Abs(To.Z - From.Z) + 70.0f);
        FVector Previous = From;
        for (int32 Sample = 1; Sample <= 8; ++Sample)
        {
            const float Alpha = static_cast<float>(Sample) / 8.0f;
            FVector Point = FMath::Lerp(From, To, Alpha);
            Point.Z += FMath::Sin(Alpha * UE_PI) * Arc;
            if (!SegmentClear(Previous, Point))
            {
                return false;
            }
            Previous = Point;
        }
        return true;
    };

    auto AddValidatedEdge = [&AddEdge, &TraversalClear](
        int32 FromNode,
        int32 ToNode,
        EHellRunVoxelSegment Mode,
        float BaseCost,
        const FVector& FromLocation,
        const FVector& ToLocation)
    {
        if (TraversalClear(FromLocation, ToLocation, Mode))
        {
            AddEdge(FromNode, ToNode, Mode, BaseCost);
        }
    };

    // Grounded topology is a projection of the 3D clearance field onto
    // supported surfaces. It must therefore be connected by neighboring XY
    // surface columns and actual projected heights, never by the arbitrary Z
    // layer of the free voxel that discovered the surface. The latter changes
    // phase on ramps and used to split a continuous slope into directed islands.
    TMap<int32, TArray<int32>> GroundNodesByColumn;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        if (!Nodes[NodeIndex].bGround) continue;
        const FIntVector Cell = UnflattenCell(Nodes[NodeIndex].CellIndex);
        GroundNodesByColumn.FindOrAdd(Cell.X + GridDimensions.X * Cell.Y).Add(NodeIndex);
    }
    const FIntVector SurfaceNeighborOffsets[] = {
        FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
        FIntVector(0, 1, 0), FIntVector(0, -1, 0),
        FIntVector(1, 1, 0), FIntVector(1, -1, 0),
        FIntVector(-1, 1, 0), FIntVector(-1, -1, 0)
    };
    auto SurfaceWalkClear =
        [World, &Objects, &Params, AgentRadius, AgentHalfHeight, &S](
            const FVector& From, const FVector& To)
    {
        // Walking follows the projected support surface; it is not a straight
        // free-space chord and it must not depend on a reciprocal bit mask
        // sampled earlier from only one voxel layer. Reconstruct the local
        // surface profile between the two canonical nodes and validate both
        // support and capsule clearance along that profile.
        const FCollisionShape AgentShape =
            FCollisionShape::MakeCapsule(AgentRadius, AgentHalfHeight);
        FVector PreviousCenter = From;
        constexpr int32 SupportSampleCount = 4;
        for (int32 Sample = 1; Sample <= SupportSampleCount; ++Sample)
        {
            const float Alpha =
                static_cast<float>(Sample) / SupportSampleCount;
            const FVector ExpectedCenter = FMath::Lerp(From, To, Alpha);
            const FVector ExpectedFloor = ExpectedCenter
                - FVector::UpVector
                    * (AgentHalfHeight + S.VoxelGroundClearance);
            FHitResult FloorHit;
            const float ProbeHeight = FMath::Max(
                S.GroundStepHeight, S.VoxelFloorProbeDepth);
            if (!TraceTopology(*World, FloorHit,
                    ExpectedFloor + FVector::UpVector * S.GroundStepHeight,
                    ExpectedFloor - FVector::UpVector * ProbeHeight,
                    Objects, Params)
                || FloorHit.ImpactNormal.Z < 0.55f
                || FMath::Abs(FloorHit.ImpactPoint.Z - ExpectedFloor.Z)
                    > S.GroundStepHeight)
            {
                return false;
            }
            const FVector SupportedCenter(
                ExpectedCenter.X,
                ExpectedCenter.Y,
                FloorHit.ImpactPoint.Z + AgentHalfHeight
                    + S.VoxelGroundClearance);
            if (!SweepTopologyClear(*World, PreviousCenter, SupportedCenter,
                    Objects, Params, AgentShape))
            {
                return false;
            }
            PreviousCenter = SupportedCenter;
        }
        return true;
    };
    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        if (!From.bGround) continue;
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        for (const FIntVector& Offset : SurfaceNeighborOffsets)
        {
            const bool bDiagonal = Offset.X != 0 && Offset.Y != 0;
            if (bDiagonal && !S.bAllowDiagonalVoxelWalk) continue;
            const int32 NeighborX = FromCell.X + Offset.X;
            const int32 NeighborY = FromCell.Y + Offset.Y;
            if (NeighborX < 0 || NeighborY < 0
                || NeighborX >= GridDimensions.X || NeighborY >= GridDimensions.Y) continue;
            const TArray<int32>* NeighborSurfaces = GroundNodesByColumn.Find(
                NeighborX + GridDimensions.X * NeighborY);
            if (!NeighborSurfaces) continue;

            for (const int32 ToIndex : *NeighborSurfaces)
            {
                if (ToIndex == FromIndex || !Nodes.IsValidIndex(ToIndex)) continue;
                const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
                // Do not classify a continuous slope as a vertical step from
                // endpoint delta. SurfaceWalkClear samples the actual support
                // profile and walkable normals; a real riser or gap fails that
                // test, while a ramp remains connected regardless of how much
                // Z it gains over one XY voxel.
                if (!SurfaceWalkClear(From.Location, To.Location)) continue;
                const float DistanceInVoxels = FVector::Distance(
                    From.Location, To.Location) / FMath::Max(BakedVoxelSize, 1.0f);
                AddEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Walk,
                    S.VoxelWalkCost * DistanceInVoxels);
            }
        }
    }

    const int32 MaximumDropCells = FMath::Max(1,
        FMath::CeilToInt(S.Drop.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
    const int32 MaximumMantleCells = FMath::Max(1,
        FMath::CeilToInt(S.Mantle.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        for (int32 DZ = -MaximumDropCells; DZ <= MaximumMantleCells; ++DZ)
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            if (DX == 0 && DY == 0 && DZ == 0) continue;
            const int32 ToFlat = FlattenCell(FromCell + FIntVector(DX, DY, DZ));
            const int32 ToIndex = CellToNode.IsValidIndex(ToFlat) ? CellToNode[ToFlat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(ToIndex)) continue;
            const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
            const float HeightDelta = To.Location.Z - From.Location.Z;
            const int32 HorizontalSteps = FMath::Abs(DX) + FMath::Abs(DY);
            const bool bCardinal = HorizontalSteps == 1 && FMath::Abs(DZ) <= 1;
            const uint8 ExitBit = CardinalDirectionBit(DX, DY);
            const bool bContinuousGround = ExitBit != 0
                && (From.GroundExitMask & ExitBit) != 0
                && (To.GroundExitMask & OppositeCardinalDirectionBit(DX, DY)) != 0;
            const float Distance = FVector(DX, DY, DZ).Size();

            if (From.bGround && To.bGround && bCardinal && !bContinuousGround
                && FMath::Abs(HeightDelta) <= S.GroundStepHeight)
            {
                const bool bLowObstacle = ((From.ObstacleExitMask & ExitBit) != 0)
                    || ((To.ObstacleExitMask & OppositeCardinalDirectionBit(DX, DY)) != 0);
                const EHellRunVoxelSegment Mode = bLowObstacle
                    ? EHellRunVoxelSegment::Vault : EHellRunVoxelSegment::Jump;
                const bool bEnabled = bLowObstacle ? S.Vault.bEnabled : S.Jump.bEnabled;
                const float Reach = bLowObstacle ? S.Vault.HorizontalReach : S.Jump.HorizontalReach;
                const float HeightTolerance = bLowObstacle
                    ? S.Vault.EndpointHeightTolerance : S.Jump.EndpointHeightTolerance;
                if (bEnabled && BakedVoxelSize <= Reach && FMath::Abs(HeightDelta) <= HeightTolerance
                    && TraversalClear(From.Location, To.Location, Mode))
                {
                    AddValidatedEdge(FromIndex, ToIndex, Mode,
                        (bLowObstacle ? S.VaultAreaCost : S.JumpAreaCost) * Distance,
                        From.Location, To.Location);
                }
            }
            if (From.bGround && To.bGround && HorizontalSteps == 1
                && HeightDelta < -S.GroundStepHeight && FMath::Abs(HeightDelta) <= S.Drop.MaximumDepth)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Drop,
                    S.DropAreaCost * Distance, From.Location, To.Location);
            }
            if (From.bGround && To.bGround && HorizontalSteps == 1
                && HeightDelta > S.GroundStepHeight && HeightDelta <= S.Mantle.MaximumDepth)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Mantle,
                    S.VoxelMantleCost * Distance, From.Location, To.Location);
            }
            if (!From.bGround && From.bClimb && To.bGround && FMath::Abs(DZ) <= 1)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Mantle,
                    S.VoxelMantleCost * Distance, From.Location, To.Location);
            }
            if (From.bGround && !To.bGround && To.bClimb && FMath::Abs(DZ) <= 1)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Climb,
                    S.VoxelClimbCost * Distance, From.Location, To.Location);
            }
            if (From.bClimb && To.bClimb && FMath::Abs(DZ) <= 1
                && FVector::DotProduct(From.WallNormal, To.WallNormal) >= 0.25f)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Climb,
                    S.VoxelClimbCost * Distance, From.Location, To.Location);
            }
            if (FMath::Abs(DZ) <= 1)
            {
                AddValidatedEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Fly,
                    S.VoxelFlightCost * Distance, From.Location, To.Location);
            }
        }

        if (!From.bGround) continue;

        // Surface portals connect separately projected surface regions. Search
        // neighboring XY columns, then classify the transition from actual
        // projected positions. Raw voxel Z indices are deliberately irrelevant.
        const float MaximumPortalReach = FMath::Max(
            FMath::Max(S.Jump.HorizontalReach, S.Vault.HorizontalReach),
            FMath::Max(S.Mantle.HorizontalReach, S.Drop.HorizontalReach));
        const int32 MaximumPortalSpanCells = FMath::Max(1,
            FMath::CeilToInt(MaximumPortalReach / FMath::Max(BakedVoxelSize, 1.0f)));
        const FCollisionShape AgentShape =
            FCollisionShape::MakeCapsule(AgentRadius, AgentHalfHeight);
        for (int32 OffsetX = -MaximumPortalSpanCells;
            OffsetX <= MaximumPortalSpanCells; ++OffsetX)
        for (int32 OffsetY = -MaximumPortalSpanCells;
            OffsetY <= MaximumPortalSpanCells; ++OffsetY)
        {
            if (OffsetX == 0 && OffsetY == 0) continue;
            const int32 ColumnX = FromCell.X + OffsetX;
            const int32 ColumnY = FromCell.Y + OffsetY;
            if (ColumnX < 0 || ColumnY < 0
                || ColumnX >= GridDimensions.X || ColumnY >= GridDimensions.Y) continue;
            const TArray<int32>* LandingSurfaces = GroundNodesByColumn.Find(
                ColumnX + GridDimensions.X * ColumnY);
            if (!LandingSurfaces) continue;
            for (const int32 LandingIndex : *LandingSurfaces)
            {
                if (LandingIndex == FromIndex || !Nodes.IsValidIndex(LandingIndex)) continue;
                const FHellRunBakedVoxelNode& Landing = Nodes[LandingIndex];
                const float HorizontalDistance =
                    FVector::Dist2D(From.Location, Landing.Location);
                if (HorizontalDistance > MaximumPortalReach) continue;
                const float HeightDelta = Landing.Location.Z - From.Location.Z;
                const float DistanceInVoxels = FVector::Distance(
                    From.Location, Landing.Location) / FMath::Max(BakedVoxelSize, 1.0f);

                if (S.Mantle.bEnabled
                    && HeightDelta > S.GroundStepHeight
                    && HeightDelta <= S.Mantle.MaximumDepth
                    && HorizontalDistance <= S.Mantle.HorizontalReach)
                {
                    AddValidatedEdge(FromIndex, LandingIndex,
                        EHellRunVoxelSegment::Mantle,
                        S.VoxelMantleCost * DistanceInVoxels,
                        From.Location, Landing.Location);
                }
                if (S.Drop.bEnabled
                    && HeightDelta < -S.GroundStepHeight
                    && FMath::Abs(HeightDelta) <= S.Drop.MaximumDepth
                    && HorizontalDistance <= S.Drop.HorizontalReach)
                {
                    const float HorizontalTime = HorizontalDistance
                        / FMath::Max(1.0f, S.DropMinimumHorizontalSpeed);
                    const float FallTime = FMath::Sqrt(
                        2.0f * FMath::Abs(HeightDelta) / 980.0f);
                    AddValidatedEdge(FromIndex, LandingIndex,
                        EHellRunVoxelSegment::Drop,
                        FMath::Max(HorizontalTime, FallTime) * 7.0f * S.DropAreaCost,
                        From.Location, Landing.Location);
                }

                bool bMissingFloorSupport = false;
                const int32 SupportSamples = FMath::Max(2, FMath::CeilToInt(
                    HorizontalDistance / FMath::Max(BakedVoxelSize * 0.5f, 1.0f)));
                for (int32 Sample = 1; Sample < SupportSamples; ++Sample)
                {
                    const float Alpha = static_cast<float>(Sample) / SupportSamples;
                    const FVector SampleCenter =
                        FMath::Lerp(From.Location, Landing.Location, Alpha);
                    const FVector ExpectedFloor = SampleCenter - FVector::UpVector
                        * (AgentHalfHeight + S.VoxelGroundClearance);
                    FHitResult FloorHit;
                    const bool bSupported = TraceTopology(
                        *World, FloorHit,
                        ExpectedFloor + FVector::UpVector * S.GroundStepHeight,
                        ExpectedFloor - FVector::UpVector
                            * FMath::Max(S.VoxelFloorProbeDepth, BakedVoxelSize),
                        Objects, Params)
                        && FloorHit.ImpactNormal.Z >= 0.55f
                        && FMath::Abs(FloorHit.ImpactPoint.Z - ExpectedFloor.Z)
                            <= S.GroundStepHeight;
                    if (!bSupported)
                    {
                        bMissingFloorSupport = true;
                        break;
                    }
                }
                if (bMissingFloorSupport && S.Jump.bEnabled
                    && HorizontalDistance <= S.Jump.HorizontalReach
                    && FMath::Abs(HeightDelta) <= S.Jump.EndpointHeightTolerance)
                {
                    AddValidatedEdge(FromIndex, LandingIndex,
                        EHellRunVoxelSegment::Jump,
                        S.JumpAreaCost * DistanceInVoxels,
                        From.Location, Landing.Location);
                }

                const bool bDirectCorridorBlocked = !SweepTopologyClear(
                    *World, From.Location, Landing.Location,
                    Objects, Params, AgentShape);
                if (bDirectCorridorBlocked && S.Vault.bEnabled
                    && HorizontalDistance <= S.Vault.HorizontalReach
                    && FMath::Abs(HeightDelta) <= S.Vault.EndpointHeightTolerance)
                {
                    AddValidatedEdge(FromIndex, LandingIndex,
                        EHellRunVoxelSegment::Vault,
                        S.VaultAreaCost * DistanceInVoxels,
                        From.Location, Landing.Location);
                }
            }
        }
    }

    TArray<int32> Parent;
    Parent.SetNumUninitialized(Nodes.Num());
    for (int32 Index = 0; Index < Parent.Num(); ++Index) Parent[Index] = Index;
    auto FindRoot = [&Parent](int32 Node)
    {
        int32 Root = Node;
        while (Parent[Root] != Root) Root = Parent[Root];
        while (Parent[Node] != Node)
        {
            const int32 Next = Parent[Node];
            Parent[Node] = Root;
            Node = Next;
        }
        return Root;
    };
    for (int32 FromIndex = 0; FromIndex < NodeEdges.Num(); ++FromIndex)
    {
        for (const FHellRunBakedVoxelEdge& Edge : NodeEdges[FromIndex])
        {
            const int32 A = FindRoot(FromIndex);
            const int32 B = FindRoot(Edge.ToNode);
            if (A != B) Parent[B] = A;
        }
    }
    TMap<int32, int32> ComponentIds;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        Nodes[NodeIndex].ComponentId = ComponentIds.FindOrAdd(
            FindRoot(NodeIndex), ComponentIds.Num());
        Nodes[NodeIndex].FirstEdge = Edges.Num();
        Nodes[NodeIndex].EdgeCount = NodeEdges[NodeIndex].Num();
        Edges.Append(NodeEdges[NodeIndex]);
    }

    // Validate the graph product at bake time. A successful voxel sampling pass
    // is not a successful navigation build if its supported surfaces were
    // fragmented before A* ever sees them.
    int32 ModeCounts[7] = {};
    int32 GroundNodeCount = 0;
    int32 GroundNodesWithoutTraversal = 0;
    TArray<int32> GroundParent;
    GroundParent.SetNumUninitialized(Nodes.Num());
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        GroundParent[NodeIndex] = NodeIndex;
        if (Nodes[NodeIndex].bGround) ++GroundNodeCount;
    }
    auto FindGroundRoot = [&GroundParent](int32 Node)
    {
        int32 Root = Node;
        while (GroundParent[Root] != Root) Root = GroundParent[Root];
        while (GroundParent[Node] != Node)
        {
            const int32 Next = GroundParent[Node];
            GroundParent[Node] = Root;
            Node = Next;
        }
        return Root;
    };
    auto IsGroundTraversalMode = [](EHellRunVoxelSegment Mode)
    {
        return Mode == EHellRunVoxelSegment::Walk
            || Mode == EHellRunVoxelSegment::Jump
            || Mode == EHellRunVoxelSegment::Vault
            || Mode == EHellRunVoxelSegment::Mantle
            || Mode == EHellRunVoxelSegment::Drop;
    };
    for (int32 FromIndex = 0; FromIndex < NodeEdges.Num(); ++FromIndex)
    {
        bool bHasGroundTraversal = false;
        for (const FHellRunBakedVoxelEdge& Edge : NodeEdges[FromIndex])
        {
            const int32 ModeIndex = static_cast<int32>(Edge.Mode);
            if (ModeIndex >= 0 && ModeIndex < UE_ARRAY_COUNT(ModeCounts))
            {
                ++ModeCounts[ModeIndex];
            }
            if (!Nodes[FromIndex].bGround
                || !Nodes.IsValidIndex(Edge.ToNode)
                || !Nodes[Edge.ToNode].bGround
                || !IsGroundTraversalMode(Edge.Mode)) continue;
            bHasGroundTraversal = true;
            const int32 A = FindGroundRoot(FromIndex);
            const int32 B = FindGroundRoot(Edge.ToNode);
            if (A != B) GroundParent[B] = A;
        }
        if (Nodes[FromIndex].bGround && !bHasGroundTraversal)
        {
            ++GroundNodesWithoutTraversal;
        }
    }
    TMap<int32, int32> GroundIslandSizes;
    int32 LargestGroundIsland = 0;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        if (!Nodes[NodeIndex].bGround) continue;
        const int32 Size = ++GroundIslandSizes.FindOrAdd(
            FindGroundRoot(NodeIndex));
        LargestGroundIsland = FMath::Max(LargestGroundIsland, Size);
    }
    UE_LOG(LogTemp, Display,
        TEXT("VOXEL_GRAPH_AUDIT | version=%d nodes=%d ground=%d edges=%d ")
        TEXT("walk=%d climb=%d mantle=%d drop=%d fly=%d jump=%d vault=%d ")
        TEXT("groundIslands=%d largestGroundIsland=%d groundWithoutTraversal=%d"),
        BakedGraphVersion, Nodes.Num(), GroundNodeCount, Edges.Num(),
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Walk)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Climb)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Mantle)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Drop)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Jump)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Vault)],
        GroundIslandSizes.Num(), LargestGroundIsland,
        GroundNodesWithoutTraversal);
}

bool AHellRunVoxelNavVolume::ContainsRouteEndpoints(const FVector& Start, const FVector& Goal) const
{
    return HasBakedNavigationData() && BakedBounds.IsInsideOrOn(Start) && BakedBounds.IsInsideOrOn(Goal);
}

void AHellRunVoxelNavVolume::GetSpawnSurfaceLocationsInRange(
    const FVector& Origin,
    float MinimumDistance,
    float MaximumDistance,
    TArray<FVector>& OutLocations,
    int32 MaximumSamples) const
{
    OutLocations.Reset();
    // Older maps remain valid for pathfinding, but may still contain sealed
    // interior cells. Fail closed until Build Paths produces the current bake;
    // never reconstruct enclosure reachability on the game thread.
    if (MaximumSamples <= 0
        || !HasBakedNavigationData()
        || !HasCurrentBakedNavigationData())
    {
        return;
    }

    const UHellRunTraversalNavigationSettings* Settings =
        GetDefault<UHellRunTraversalNavigationSettings>();
    const float MinimumDistanceSq =
        FMath::Square(FMath::Max(0.0f, MinimumDistance));
    const float MaximumDistanceSq = MaximumDistance > 0.0f
        ? FMath::Square(MaximumDistance)
        : TNumericLimits<float>::Max();

    // Prefer the walkable elevation occupied by the survivor. In stacked
    // layouts a uniform reservoir is dominated by roofs and upper floors, so a
    // survivor in a basement can otherwise cause every accepted spawn to be on
    // the floor above despite valid connected ground beside them.
    const float PreferredLayerHalfHeight = FMath::Max(
        Settings->VoxelSize * 2.0f,
        Settings->GroundStepHeight * 2.0f);

    // Fixed-memory reservoir sampling avoids allocating and then walking a
    // potentially multi-thousand-node temporary array on the game thread.
    OutLocations.Reserve(MaximumSamples);
    int32 MatchingCount = 0;
    TArray<FVector> FallbackLocations;
    FallbackLocations.Reserve(MaximumSamples);
    int32 FallbackMatchingCount = 0;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        const FHellRunBakedVoxelNode& Node = Nodes[NodeIndex];
        if (!Node.bGround
            || DynamicBlockedNodeRefCounts.Contains(NodeIndex))
        {
            continue;
        }
        const FVector SurfaceLocation =
            Node.Location
            - FVector::UpVector
                * Settings->VoxelBakeAgentHalfHeight;
        const float DistanceSq =
            FVector::DistSquared(Origin, SurfaceLocation);
        if (DistanceSq < MinimumDistanceSq || DistanceSq > MaximumDistanceSq)
        {
            continue;
        }

        auto ReservoirAdd = [MaximumSamples](
            TArray<FVector>& Locations,
            int32& MatchCount,
            const FVector& Location)
        {
            ++MatchCount;
            if (Locations.Num() < MaximumSamples)
            {
                Locations.Add(Location);
            }
            else
            {
                const int32 ReplacementIndex =
                    FMath::RandRange(0, MatchCount - 1);
                if (ReplacementIndex < MaximumSamples)
                {
                    Locations[ReplacementIndex] = Location;
                }
            }
        };
        ReservoirAdd(
            FallbackLocations, FallbackMatchingCount, SurfaceLocation);
        if (FMath::Abs(SurfaceLocation.Z - Origin.Z)
            <= PreferredLayerHalfHeight)
        {
            ReservoirAdd(OutLocations, MatchingCount, SurfaceLocation);
        }
    }
    if (OutLocations.IsEmpty())
    {
        OutLocations = MoveTemp(FallbackLocations);
    }
}

void AHellRunVoxelNavVolume::GetRetainedGroundNodeLocations(
    TArray<FVector>& OutLocations) const
{
    OutLocations.Reset();
    OutLocations.Reserve(Nodes.Num());
    for (const FHellRunBakedVoxelNode& Node : Nodes)
    {
        if (Node.bGround)
        {
            OutLocations.Add(Node.Location);
        }
    }
}

void AHellRunVoxelNavVolume::GetQueryNodeLocationsInRange(
    const FVector& Origin,const float MinimumDistance,const float MaximumDistance,
    const bool bIncludeGround,const bool bIncludeClimb,const bool bIncludeFlight,
    TArray<FVector>& OutLocations,const int32 MaximumSamples) const
{
    OutLocations.Reset();
    if(MaximumSamples<=0||!HasCurrentBakedNavigationData())return;
    const float MinimumSq=FMath::Square(FMath::Max(0.0f,MinimumDistance));
    const float MaximumSq=MaximumDistance>0.0f
        ?FMath::Square(MaximumDistance):TNumericLimits<float>::Max();
    OutLocations.Reserve(MaximumSamples);
    int32 MatchCount=0;
    const uint32 OriginSeed=HashCombineFast(GetTypeHash(FMath::RoundToInt(Origin.X)),
        HashCombineFast(GetTypeHash(FMath::RoundToInt(Origin.Y)),GetTypeHash(FMath::RoundToInt(Origin.Z))));
    for(int32 NodeIndex=0;NodeIndex<Nodes.Num();++NodeIndex)
    {
        const FHellRunBakedVoxelNode& Node=Nodes[NodeIndex];
        const bool bSupported=(bIncludeGround&&Node.bGround)||
            (bIncludeClimb&&Node.bClimb)||
            (bIncludeFlight&&!Node.bGround&&!Node.bClimb);
        if(!bSupported||DynamicBlockedNodeRefCounts.Contains(NodeIndex))continue;
        const float DistanceSq=FVector::DistSquared(Origin,Node.Location);
        if(DistanceSq<MinimumSq||DistanceSq>MaximumSq)continue;
        ++MatchCount;
        if(OutLocations.Num()<MaximumSamples)OutLocations.Add(Node.Location);
        else
        {
            const uint32 StableHash=HashCombineFast(OriginSeed,GetTypeHash(NodeIndex));
            const int32 Replacement=static_cast<int32>(StableHash%static_cast<uint32>(MatchCount));
            if(Replacement<MaximumSamples)OutLocations[Replacement]=Node.Location;
        }
    }
}

void AHellRunVoxelNavVolume::GetQueryCoverLocationsInRange(
    const FVector& Origin,const FVector& CoverContext,const float MinimumDistance,
    const float MaximumDistance,TArray<FVector>& OutLocations,
    const int32 MaximumSamples,const float MaximumFacingDot) const
{
    OutLocations.Reset();
    if(MaximumSamples<=0||!HasCurrentBakedNavigationData())return;
    const float MinimumSq=FMath::Square(FMath::Max(0.0f,MinimumDistance));
    const float MaximumSq=MaximumDistance>0.0f
        ?FMath::Square(MaximumDistance):TNumericLimits<float>::Max();
    OutLocations.Reserve(MaximumSamples);
    int32 MatchCount=0;
    const uint32 OriginSeed=HashCombineFast(GetTypeHash(FMath::RoundToInt(Origin.X)),
        HashCombineFast(GetTypeHash(FMath::RoundToInt(Origin.Y)),GetTypeHash(FMath::RoundToInt(Origin.Z))));
    for(int32 NodeIndex=0;NodeIndex<Nodes.Num();++NodeIndex)
    {
        const FHellRunBakedVoxelNode& Node=Nodes[NodeIndex];
        if(!Node.bGround||Node.WallNormal.IsNearlyZero()||
            DynamicBlockedNodeRefCounts.Contains(NodeIndex))continue;
        const FVector ToContext=(CoverContext-Node.Location).GetSafeNormal2D();
        // The context must lie through the wall from the candidate. This dot
        // test is invariant to wall rotation and rejects the exposed side.
        if(ToContext.IsNearlyZero()||FVector::DotProduct(
            Node.WallNormal.GetSafeNormal2D(),ToContext)>MaximumFacingDot)continue;
        const float DistanceSq=FVector::DistSquared(Origin,Node.Location);
        if(DistanceSq<MinimumSq||DistanceSq>MaximumSq)continue;
        ++MatchCount;
        if(OutLocations.Num()<MaximumSamples)OutLocations.Add(Node.Location);
        else
        {
            const uint32 StableHash=HashCombineFast(OriginSeed,GetTypeHash(NodeIndex));
            const int32 Replacement=static_cast<int32>(StableHash%static_cast<uint32>(MatchCount));
            if(Replacement<MaximumSamples)OutLocations[Replacement]=Node.Location;
        }
    }
}

bool AHellRunVoxelNavVolume::IsCurrentQueryNodeLocation(const FVector& Location,
    const bool bCanWalk,const bool bCanClimb,const bool bCanFly) const
{
    if(!HasCurrentBakedNavigationData())return false;
    const int32 NodeIndex=FindNearestNode(Location,bCanWalk,bCanClimb,bCanFly);
    return Nodes.IsValidIndex(NodeIndex)&&!DynamicBlockedNodeRefCounts.Contains(NodeIndex)&&
        FVector::DistSquared(Nodes[NodeIndex].Location,Location)<=FMath::Square(
            FMath::Max(10.0f,BakedVoxelSize*.55f));
}

bool AHellRunVoxelNavVolume::ResolveQueryLocation(const FVector& Location,
    const bool bCanWalk,const bool bCanClimb,const bool bCanFly,
    FVector& OutLocation) const
{
    if(!HasCurrentBakedNavigationData())return false;
    const int32 NodeIndex=FindNearestNode(Location,bCanWalk,bCanClimb,bCanFly);
    if(!Nodes.IsValidIndex(NodeIndex)||DynamicBlockedNodeRefCounts.Contains(NodeIndex))
        return false;
    OutLocation=Nodes[NodeIndex].Location;
    return true;
}

void AHellRunVoxelNavVolume::TestQueryPathReachability(
    const FVector& Start,
    const TArray<FVector>& Goals,
    const bool bCanWalk,
    const bool bCanClimb,
    const bool bCanMantle,
    const bool bCanDrop,
    const bool bCanJump,
    const bool bCanVault,
    const bool bCanFly,
    TArray<bool>& OutReachable) const
{
    OutReachable.Init(false,Goals.Num());
    if(Goals.IsEmpty()||!HasCurrentBakedNavigationData()||
        !ContainsRouteEndpoints(Start,Start)||
        !HasAuthoritativeTypedEdgeGraph())return;

    const int32 StartNode=FindNearestNode(Start,bCanWalk,bCanClimb,bCanFly);
    if(!Nodes.IsValidIndex(StartNode)||DynamicBlockedNodeRefCounts.Contains(StartNode))return;

    TBitArray<> Visited(false,Nodes.Num());
    TArray<int32> Pending;
    Pending.Reserve(FMath::Min(Nodes.Num(),4096));
    Pending.Add(StartNode);Visited[StartNode]=true;
    for(int32 Cursor=0;Cursor<Pending.Num();++Cursor)
    {
        const int32 NodeIndex=Pending[Cursor];
        const FHellRunBakedVoxelNode& Node=Nodes[NodeIndex];
        const int32 EdgeEnd=Node.FirstEdge+Node.EdgeCount;
        for(int32 EdgeIndex=Node.FirstEdge;EdgeIndex<EdgeEnd;++EdgeIndex)
        {
            if(!Edges.IsValidIndex(EdgeIndex))continue;
            const FHellRunBakedVoxelEdge& Edge=Edges[EdgeIndex];
            if(!Nodes.IsValidIndex(Edge.ToNode)||Visited[Edge.ToNode]||
                DynamicBlockedNodeRefCounts.Contains(Edge.ToNode))continue;
            bool bModeAllowed=false;
            switch(Edge.Mode)
            {
            case EHellRunVoxelSegment::Walk:bModeAllowed=bCanWalk;break;
            case EHellRunVoxelSegment::Climb:bModeAllowed=bCanClimb;break;
            case EHellRunVoxelSegment::Mantle:bModeAllowed=bCanMantle;break;
            case EHellRunVoxelSegment::Drop:bModeAllowed=bCanDrop;break;
            case EHellRunVoxelSegment::Jump:bModeAllowed=bCanJump;break;
            case EHellRunVoxelSegment::Vault:bModeAllowed=bCanVault;break;
            case EHellRunVoxelSegment::Fly:bModeAllowed=bCanFly;break;
            default:break;
            }
            if(!bModeAllowed)continue;
            const FHellRunBakedVoxelNode& Destination=Nodes[Edge.ToNode];
            if(!(bCanFly||(bCanWalk&&Destination.bGround)||
                (bCanClimb&&Destination.bClimb)))continue;
            Visited[Edge.ToNode]=true;
            Pending.Add(Edge.ToNode);
        }
    }

    for(int32 GoalIndex=0;GoalIndex<Goals.Num();++GoalIndex)
    {
        const int32 GoalNode=FindNearestNode(Goals[GoalIndex],
            bCanWalk,bCanClimb,bCanFly);
        OutReachable[GoalIndex]=Nodes.IsValidIndex(GoalNode)&&Visited[GoalNode]&&
            !DynamicBlockedNodeRefCounts.Contains(GoalNode);
    }
}

bool AHellRunVoxelNavVolume::BuildQueryPath(const FVector& Start,const FVector& Goal,
    const bool bCanWalk,const bool bCanClimb,const bool bCanMantle,
    const bool bCanDrop,const bool bCanJump,const bool bCanVault,const bool bCanFly,
    TArray<FVector>& OutPath) const
{
    OutPath.Reset();
    if(!HasCurrentBakedNavigationData()||!HasAuthoritativeTypedEdgeGraph()||
        !ContainsRouteEndpoints(Start,Start))return false;
    const int32 StartNode=FindNearestNode(Start,bCanWalk,bCanClimb,bCanFly);
    const int32 GoalNode=FindNearestNode(Goal,bCanWalk,bCanClimb,bCanFly);
    if(!Nodes.IsValidIndex(StartNode)||!Nodes.IsValidIndex(GoalNode)||
        DynamicBlockedNodeRefCounts.Contains(StartNode)||
        DynamicBlockedNodeRefCounts.Contains(GoalNode))return false;

    TArray<int32> Parent;Parent.Init(INDEX_NONE,Nodes.Num());
    TBitArray<> Visited(false,Nodes.Num());
    TArray<int32> Pending;Pending.Reserve(FMath::Min(Nodes.Num(),4096));
    Pending.Add(StartNode);Visited[StartNode]=true;
    for(int32 Cursor=0;Cursor<Pending.Num()&&!Visited[GoalNode];++Cursor)
    {
        const int32 NodeIndex=Pending[Cursor];
        const FHellRunBakedVoxelNode& Node=Nodes[NodeIndex];
        for(int32 EdgeIndex=Node.FirstEdge;EdgeIndex<Node.FirstEdge+Node.EdgeCount;++EdgeIndex)
        {
            if(!Edges.IsValidIndex(EdgeIndex))continue;
            const FHellRunBakedVoxelEdge& Edge=Edges[EdgeIndex];
            if(!Nodes.IsValidIndex(Edge.ToNode)||Visited[Edge.ToNode]||
                DynamicBlockedNodeRefCounts.Contains(Edge.ToNode))continue;
            bool bAllowed=false;
            switch(Edge.Mode)
            {
            case EHellRunVoxelSegment::Walk:bAllowed=bCanWalk;break;
            case EHellRunVoxelSegment::Climb:bAllowed=bCanClimb;break;
            case EHellRunVoxelSegment::Mantle:bAllowed=bCanMantle;break;
            case EHellRunVoxelSegment::Drop:bAllowed=bCanDrop;break;
            case EHellRunVoxelSegment::Jump:bAllowed=bCanJump;break;
            case EHellRunVoxelSegment::Vault:bAllowed=bCanVault;break;
            case EHellRunVoxelSegment::Fly:bAllowed=bCanFly;break;
            default:break;
            }
            const FHellRunBakedVoxelNode& Destination=Nodes[Edge.ToNode];
            if(!bAllowed||!(bCanFly||(bCanWalk&&Destination.bGround)||
                (bCanClimb&&Destination.bClimb)))continue;
            Visited[Edge.ToNode]=true;Parent[Edge.ToNode]=NodeIndex;Pending.Add(Edge.ToNode);
        }
    }
    if(!Visited[GoalNode])return false;
    TArray<int32> Reverse;
    for(int32 Node=GoalNode;Node!=INDEX_NONE;Node=Parent[Node])
    {Reverse.Add(Node);if(Node==StartNode)break;}
    if(Reverse.IsEmpty()||Reverse.Last()!=StartNode)return false;
    OutPath.Reserve(Reverse.Num()+2);OutPath.Add(Start);
    for(int32 Index=Reverse.Num()-1;Index>=0;--Index)
        if(FVector::DistSquared(OutPath.Last(),Nodes[Reverse[Index]].Location)>1.0f)
            OutPath.Add(Nodes[Reverse[Index]].Location);
    if(FVector::DistSquared(OutPath.Last(),Goal)>1.0f)OutPath.Add(Goal);

    // Keep bends and traversal transitions, discard dense collinear voxel samples.
    for(int32 Index=OutPath.Num()-2;Index>0;--Index)
    {
        const FVector A=(OutPath[Index]-OutPath[Index-1]).GetSafeNormal();
        const FVector B=(OutPath[Index+1]-OutPath[Index]).GetSafeNormal();
        if(FVector::DotProduct(A,B)>.995f)OutPath.RemoveAt(Index,1,EAllowShrinking::No);
    }
    return OutPath.Num()>1;
}

void AHellRunVoxelNavVolume::GetBakedTraversalSegments(
    TArray<FHellRunBakedTraversalSegment>& OutSegments,
    const bool bIncludeWalkEdges, const int32 MaximumSegments) const
{
    OutSegments.Reset();
    const int32 Limit = FMath::Max(0, MaximumSegments);
    if (Limit == 0) return;
    for (const FHellRunBakedVoxelNode& Node : Nodes)
    {
        if (Node.FirstEdge == INDEX_NONE || Node.EdgeCount <= 0) continue;
        for (int32 Offset = 0; Offset < Node.EdgeCount; ++Offset)
        {
            const int32 EdgeIndex = Node.FirstEdge + Offset;
            if (!Edges.IsValidIndex(EdgeIndex)) continue;
            const FHellRunBakedVoxelEdge& Edge = Edges[EdgeIndex];
            if (!Nodes.IsValidIndex(Edge.ToNode)
                || (!bIncludeWalkEdges
                    && Edge.Mode == EHellRunVoxelSegment::Walk))
                continue;
            FHellRunBakedTraversalSegment& Export =
                OutSegments.AddDefaulted_GetRef();
            Export.Start = Node.Location;
            Export.End = Nodes[Edge.ToNode].Location;
            Export.Mode = Edge.Mode;
            Export.BaseCost = Edge.BaseCost;
            if (OutSegments.Num() >= Limit) return;
        }
    }
}

uint32 AHellRunVoxelNavVolume::ComputeBakedDataHash() const
{
    uint32 Hash = GetTypeHash(GridOrigin);
    Hash = HashCombineFast(Hash, GetTypeHash(GridDimensions));
    Hash = HashCombineFast(Hash, GetTypeHash(BakedVoxelSize));
    Hash = HashCombineFast(Hash, GetTypeHash(BakedGraphVersion));
    Hash = HashCombineFast(Hash, GetTypeHash(BakedBounds.Min));
    Hash = HashCombineFast(Hash, GetTypeHash(BakedBounds.Max));
    for (const int32 Cell : CellToNode)
    {
        Hash = HashCombineFast(Hash, GetTypeHash(Cell));
    }
    for (const FHellRunBakedVoxelNode& Node : Nodes)
    {
        Hash = HashCombineFast(Hash, GetTypeHash(Node.Location));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.WallNormal));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.bGround));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.bClimb));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.GroundExitMask));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.ObstacleExitMask));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.CellIndex));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.FirstEdge));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.EdgeCount));
        Hash = HashCombineFast(Hash, GetTypeHash(Node.ComponentId));
    }
    for (const uint32 FlightMask : FlightNeighborMasks)
    {
        Hash = HashCombineFast(Hash, GetTypeHash(FlightMask));
    }
    for (const FHellRunBakedVoxelEdge& Edge : Edges)
    {
        Hash = HashCombineFast(Hash, GetTypeHash(Edge.ToNode));
        Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(Edge.Mode)));
        Hash = HashCombineFast(Hash, GetTypeHash(Edge.BaseCost));
    }
    return Hash;
}

#if WITH_DEV_AUTOMATION_TESTS
void AHellRunVoxelNavVolume::GetAutomationGroundNodeLocations(
    TArray<FVector>& OutLocations,
    int32 MaximumSamples) const
{
    OutLocations.Reset();
    if (MaximumSamples <= 0) return;
    int32 GroundCount = 0;
    for (const FHellRunBakedVoxelNode& Node : Nodes)
    {
        if (Node.bGround) ++GroundCount;
    }
    if (GroundCount <= 0) return;
    const int32 Stride = FMath::Max(1,
        FMath::CeilToInt(static_cast<float>(GroundCount) / MaximumSamples));
    int32 GroundOrdinal = 0;
    for (const FHellRunBakedVoxelNode& Node : Nodes)
    {
        if (!Node.bGround) continue;
        if ((GroundOrdinal++ % Stride) == 0)
        {
            OutLocations.Add(Node.Location);
            if (OutLocations.Num() >= MaximumSamples) break;
        }
    }
}

bool AHellRunVoxelNavVolume::GetAutomationTraversalProbe(
    const ACharacter& Character,
    EHellRunVoxelSegment Mode,
    FVector& OutStart,
    FVector& OutGoal) const
{
    const UHellRunTraversalNavigationSettings* S =
        GetDefault<UHellRunTraversalNavigationSettings>();
    int32 TopologyCandidateCount = 0;
    int32 ClearanceRejectedCount = 0;
    const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
    const float AgentZAdjustment = Capsule
        ? Capsule->GetScaledCapsuleHalfHeight()
            - S->VoxelBakeAgentHalfHeight
        : 0.0f;
    auto AgentLocation = [AgentZAdjustment](
        const FHellRunBakedVoxelNode& Node)
    {
        FVector Result = Node.Location;
        if (Node.bGround) Result.Z += AgentZAdjustment;
        return Result;
    };
    if (HasAuthoritativeTypedEdgeGraph())
    {
        if (Mode == EHellRunVoxelSegment::Fly)
        {
            for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
            {
                if (!FlightNeighborMasks.IsValidIndex(FromIndex)) continue;
                const uint32 FlightMask = FlightNeighborMasks[FromIndex];
                if (FlightMask == 0) continue;
                const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
                const FIntVector FromCell = UnflattenCell(From.CellIndex);
                int32 NeighborBit = 0;
                for (int32 DZ = -1; DZ <= 1; ++DZ)
                for (int32 DY = -1; DY <= 1; ++DY)
                for (int32 DX = -1; DX <= 1; ++DX)
                {
                    if (DX == 0 && DY == 0 && DZ == 0) continue;
                    const int32 Bit = NeighborBit++;
                    if ((FlightMask & (1u << Bit)) == 0) continue;
                    const int32 FlatIndex = FlattenCell(
                        FromCell + FIntVector(DX, DY, DZ));
                    const int32 ToIndex = CellToNode.IsValidIndex(FlatIndex)
                        ? CellToNode[FlatIndex] : INDEX_NONE;
                    if (!Nodes.IsValidIndex(ToIndex)) continue;
                    OutStart = AgentLocation(From);
                    OutGoal = AgentLocation(Nodes[ToIndex]);
                    return true;
                }
            }
            UE_LOG(LogTemp, Warning,
                TEXT("VOXEL_AUTOMATION_PROBE_FAILED | mode=%d flightMasks=%d"),
                static_cast<int32>(Mode), FlightNeighborMasks.Num());
            return false;
        }
        for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
        {
            const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
            const int32 EdgeEnd = From.FirstEdge + From.EdgeCount;
            for (int32 EdgeIndex = From.FirstEdge;
                EdgeIndex < EdgeEnd; ++EdgeIndex)
            {
                if (!Edges.IsValidIndex(EdgeIndex)) continue;
                const FHellRunBakedVoxelEdge& Edge = Edges[EdgeIndex];
                if (Edge.Mode != Mode
                    || !Nodes.IsValidIndex(Edge.ToNode)) continue;
                OutStart = AgentLocation(From);
                OutGoal = AgentLocation(Nodes[Edge.ToNode]);
                return true;
            }
        }
        UE_LOG(LogTemp, Warning,
            TEXT("VOXEL_AUTOMATION_PROBE_FAILED | mode=%d typedEdges=%d"),
            static_cast<int32>(Mode), Edges.Num());
        return false;
    }
    const FIntVector CardinalDirections[] = {
        FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
        FIntVector(0, 1, 0), FIntVector(0, -1, 0)
    };
    auto ResolveGroundNode = [this](const FIntVector& Cell)
    {
        const int32 Flat = FlattenCell(Cell);
        const int32 NodeIndex =
            CellToNode.IsValidIndex(Flat)
            ? CellToNode[Flat] : INDEX_NONE;
        return Nodes.IsValidIndex(NodeIndex) && Nodes[NodeIndex].bGround
            ? NodeIndex : INDEX_NONE;
    };

    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        if (!From.bGround) continue;
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        for (const FIntVector& Direction : CardinalDirections)
        {
            if (Mode == EHellRunVoxelSegment::Mantle
                || Mode == EHellRunVoxelSegment::Drop)
            {
                const int32 MaximumCells = FMath::Max(
                    1,
                    FMath::CeilToInt(
                        (Mode == EHellRunVoxelSegment::Mantle
                            ? S->Mantle.MaximumDepth
                            : S->Drop.MaximumDepth)
                        / FMath::Max(BakedVoxelSize, 1.0f)));
                for (int32 CellDelta = 1;
                    CellDelta <= MaximumCells;
                    ++CellDelta)
                {
                    const int32 SignedDelta =
                        Mode == EHellRunVoxelSegment::Mantle
                        ? CellDelta : -CellDelta;
                    const int32 CandidateIndex = ResolveGroundNode(
                        FromCell + Direction
                            + FIntVector(0, 0, SignedDelta));
                    if (!Nodes.IsValidIndex(CandidateIndex)) continue;
                    const float HeightDelta =
                        Nodes[CandidateIndex].Location.Z
                        - From.Location.Z;
                    const bool bMatches =
                        Mode == EHellRunVoxelSegment::Mantle
                        ? HeightDelta > S->GroundStepHeight
                            && HeightDelta
                                <= S->Mantle.MaximumDepth
                        : HeightDelta < -S->GroundStepHeight
                            && -HeightDelta
                                <= S->Drop.MaximumDepth;
                    if (bMatches)
                    {
                        OutStart = From.Location;
                        OutGoal = Nodes[CandidateIndex].Location;
                        return true;
                    }
                }
                continue;
            }

            if (Mode == EHellRunVoxelSegment::Jump)
            {
                const int32 LandingIndex =
                    ResolveGroundNode(FromCell + Direction * 2);
                if (!Nodes.IsValidIndex(LandingIndex)) continue;
                const int32 MiddleFlat =
                    FlattenCell(FromCell + Direction);
                const int32 MiddleIndex =
                    CellToNode.IsValidIndex(MiddleFlat)
                    ? CellToNode[MiddleFlat] : INDEX_NONE;
                const FHellRunBakedVoxelNode& Landing =
                    Nodes[LandingIndex];
                const float ActionDistance = FMath::Max(
                    0.0f,
                    FVector::Dist2D(From.Location, Landing.Location)
                        - BakedVoxelSize);
                if (Nodes.IsValidIndex(MiddleIndex)
                    && !Nodes[MiddleIndex].bGround
                    && ActionDistance <= S->Jump.HorizontalReach
                    && FMath::Abs(Landing.Location.Z - From.Location.Z)
                        <= S->Jump.EndpointHeightTolerance)
                {
                    ++TopologyCandidateCount;
                    FVector Entry;
                    FVector Exit;
                    GetTraversalBoundaryPoints(
                        AgentLocation(From),
                        AgentLocation(Landing),
                        BakedVoxelSize,
                        Mode,
                        Entry,
                        Exit);
                    if (IsTraversalEdgeClear(
                        Character, Entry, Exit, Mode, *S))
                    {
                        OutStart = From.Location;
                        OutGoal = Landing.Location;
                        return true;
                    }
                    ++ClearanceRejectedCount;
                }
                continue;
            }

            const int32 ToIndex =
                ResolveGroundNode(FromCell + Direction);
            if (!Nodes.IsValidIndex(ToIndex)) continue;
            const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
            const float HeightDelta = To.Location.Z - From.Location.Z;
            const uint8 ExitBit =
                CardinalDirectionBit(Direction.X, Direction.Y);
            const bool bContinuous =
                (From.GroundExitMask & ExitBit) != 0
                && (To.GroundExitMask
                    & OppositeCardinalDirectionBit(
                        Direction.X, Direction.Y)) != 0;
            bool bMatches = false;
            switch (Mode)
            {
            case EHellRunVoxelSegment::Walk:
                bMatches = bContinuous
                    && FMath::Abs(HeightDelta)
                        <= S->GroundStepHeight;
                break;
            case EHellRunVoxelSegment::Vault:
                bMatches = !bContinuous
                    && (((From.ObstacleExitMask & ExitBit) != 0)
                        || ((To.ObstacleExitMask
                            & OppositeCardinalDirectionBit(
                                Direction.X, Direction.Y)) != 0))
                    && FMath::Abs(HeightDelta)
                        <= S->Vault.EndpointHeightTolerance;
                break;
            case EHellRunVoxelSegment::Mantle:
                bMatches = HeightDelta > S->GroundStepHeight
                    && HeightDelta <= S->Mantle.MaximumDepth;
                break;
            case EHellRunVoxelSegment::Drop:
                bMatches = HeightDelta < -S->GroundStepHeight
                    && -HeightDelta <= S->Drop.MaximumDepth;
                break;
            default:
                break;
            }
            if (bMatches)
            {
                ++TopologyCandidateCount;
                FVector Entry = From.Location;
                FVector Exit = To.Location;
                if (IsDiscreteTraversal(Mode))
                {
                    GetTraversalBoundaryPoints(
                        AgentLocation(From),
                        AgentLocation(To),
                        BakedVoxelSize,
                        Mode,
                        Entry,
                        Exit);
                }
                if (IsTraversalEdgeClear(
                    Character, Entry, Exit, Mode, *S))
                {
                    OutStart = From.Location;
                    OutGoal = To.Location;
                    return true;
                }
                ++ClearanceRejectedCount;
            }
        }
    }
    UE_LOG(
        LogTemp,
        Warning,
        TEXT("VOXEL_AUTOMATION_PROBE_FAILED | mode=%d topology=%d clearanceRejected=%d"),
        static_cast<int32>(Mode),
        TopologyCandidateCount,
        ClearanceRejectedCount);
    return false;
}
#endif

int32 AHellRunVoxelNavVolume::FindNearestNode(const FVector& Location, bool bCanWalk, bool bCanClimb, bool bCanFly) const
{
    if (BakedVoxelSize <= 0.0f) return INDEX_NONE;
    const FVector Local = (Location - GridOrigin) / BakedVoxelSize;
    const FIntVector Center(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
    int32 BestNode = INDEX_NONE;
    float BestDistanceSq = BIG_NUMBER;
    for (int32 R = 0; R <= 4; ++R)
    {
        for (int32 Z = -R; Z <= R; ++Z)
        for (int32 Y = -R; Y <= R; ++Y)
        for (int32 X = -R; X <= R; ++X)
        {
            const int32 Flat = FlattenCell(Center + FIntVector(X, Y, Z));
            const int32 NodeIndex = CellToNode.IsValidIndex(Flat) ? CellToNode[Flat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(NodeIndex)) continue;
            if (DynamicBlockedNodeRefCounts.Contains(NodeIndex)) continue;
            const FHellRunBakedVoxelNode& Node = Nodes[NodeIndex];
            if (!bCanFly && !(bCanWalk && Node.bGround) && !(bCanClimb && Node.bClimb)) continue;
            const float DistanceSq = FVector::DistSquared(Node.Location, Location);
            if (DistanceSq < BestDistanceSq) { BestDistanceSq = DistanceSq; BestNode = NodeIndex; }
        }
    }
    return BestNode;
}

void AHellRunVoxelNavVolume::EnsureSpatialOctree() const
{
    if (bSpatialOctreeBuilt) return;
    SpatialOctree.Reset();
    bSpatialOctreeBuilt = true;
    if (Nodes.IsEmpty() || !BakedBounds.IsValid) return;

    TArray<int32> AllNodes;
    AllNodes.Reserve(Nodes.Num());
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) AllNodes.Add(NodeIndex);
    BuildSpatialOctreeNode(BakedBounds.ExpandBy(FMath::Max(1.0f, BakedVoxelSize * 0.5f)), AllNodes, 0);
}

int32 AHellRunVoxelNavVolume::BuildSpatialOctreeNode(const FBox& Bounds, const TArray<int32>& NodeIndices, int32 Depth) const
{
    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    const int32 TreeNodeIndex = SpatialOctree.AddDefaulted();
    SpatialOctree[TreeNodeIndex].Bounds = Bounds;
    if (NodeIndices.Num() <= Settings->OctreeLeafNodeCapacity || Depth >= Settings->OctreeMaximumDepth)
    {
        SpatialOctree[TreeNodeIndex].NodeIndices = NodeIndices;
        return TreeNodeIndex;
    }

    const FVector Center = Bounds.GetCenter();
    TArray<int32> ChildNodes[8];
    for (const int32 NodeIndex : NodeIndices)
    {
        const FVector& Location = Nodes[NodeIndex].Location;
        const int32 Octant = (Location.X >= Center.X ? 1 : 0)
            | (Location.Y >= Center.Y ? 2 : 0)
            | (Location.Z >= Center.Z ? 4 : 0);
        ChildNodes[Octant].Add(NodeIndex);
    }

    SpatialOctree[TreeNodeIndex].Children.Init(INDEX_NONE, 8);
    for (int32 Octant = 0; Octant < 8; ++Octant)
    {
        if (ChildNodes[Octant].IsEmpty()) continue;
        FVector ChildMin = Bounds.Min;
        FVector ChildMax = Bounds.Max;
        if (Octant & 1) ChildMin.X = Center.X; else ChildMax.X = Center.X;
        if (Octant & 2) ChildMin.Y = Center.Y; else ChildMax.Y = Center.Y;
        if (Octant & 4) ChildMin.Z = Center.Z; else ChildMax.Z = Center.Z;
        SpatialOctree[TreeNodeIndex].Children[Octant] = BuildSpatialOctreeNode(
            FBox(ChildMin, ChildMax), ChildNodes[Octant], Depth + 1);
    }
    return TreeNodeIndex;
}

void AHellRunVoxelNavVolume::QuerySpatialOctree(const FBox& Bounds, TArray<int32>& OutNodeIndices) const
{
    if (!GetDefault<UHellRunTraversalNavigationSettings>()->bUseOctreeSpatialIndex)
    {
        for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
        {
            if (Bounds.IsInsideOrOn(Nodes[NodeIndex].Location)) OutNodeIndices.Add(NodeIndex);
        }
        return;
    }
    EnsureSpatialOctree();
    if (SpatialOctree.IsEmpty()) return;
    TArray<int32, TInlineAllocator<64>> Pending;
    Pending.Add(0);
    while (!Pending.IsEmpty())
    {
        const int32 TreeNodeIndex = Pending.Pop(EAllowShrinking::No);
        if (!SpatialOctree.IsValidIndex(TreeNodeIndex)) continue;
        const FSpatialOctreeNode& TreeNode = SpatialOctree[TreeNodeIndex];
        if (!TreeNode.Bounds.Intersect(Bounds)) continue;
        if (!TreeNode.NodeIndices.IsEmpty())
        {
            for (const int32 NodeIndex : TreeNode.NodeIndices)
            {
                if (Nodes.IsValidIndex(NodeIndex) && Bounds.IsInsideOrOn(Nodes[NodeIndex].Location))
                {
                    OutNodeIndices.Add(NodeIndex);
                }
            }
        }
        else
        {
            for (const int32 ChildIndex : TreeNode.Children)
            {
                if (ChildIndex != INDEX_NONE) Pending.Add(ChildIndex);
            }
        }
    }
}

void AHellRunVoxelNavVolume::UpdateDynamicObstacle(UObject* ObstacleSource, const FBox& ObstacleBounds, bool bRemove)
{
    if (!ObstacleSource) return;
    const TWeakObjectPtr<UObject> SourceKey(ObstacleSource);
    bool bChanged = false;
    if (TArray<int32>* PreviousNodes = DynamicObstacleNodes.Find(SourceKey))
    {
        bChanged = true;
        for (const int32 NodeIndex : *PreviousNodes)
        {
            if (int32* Count = DynamicBlockedNodeRefCounts.Find(NodeIndex))
            {
                if (--(*Count) <= 0) DynamicBlockedNodeRefCounts.Remove(NodeIndex);
            }
        }
        DynamicObstacleNodes.Remove(SourceKey);
    }

    if (!bRemove && ObstacleBounds.IsValid && BakedBounds.Intersect(ObstacleBounds))
    {
        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
        const FVector Clearance(Settings->VoxelBakeAgentRadius, Settings->VoxelBakeAgentRadius,
            Settings->VoxelBakeAgentHalfHeight);
        TArray<int32> BlockedNodes;
        QuerySpatialOctree(ObstacleBounds.ExpandBy(Clearance), BlockedNodes);
        for (const int32 NodeIndex : BlockedNodes) ++DynamicBlockedNodeRefCounts.FindOrAdd(NodeIndex);
        DynamicObstacleNodes.Add(SourceKey, MoveTemp(BlockedNodes));
        bChanged = true;
    }
    if (bChanged) InvalidateDynamicNavigation();
}

FNavPathSharedPtr AHellRunVoxelNavVolume::FindPath(const ACharacter& Character, const FVector& Start, const FVector& Goal) const
{
    // Version 24 is built as an authoritative typed-edge graph and has its own
    // matching anchor/search implementation. Sending that data through the
    // legacy query path mixes incompatible endpoint and graph assumptions,
    // which can make identical baked topology appear intermittently
    // unreachable as an endpoint moves between neighboring cells.
    if (HasAuthoritativeTypedEdgeGraph())
    {
        return FindPathV2(Character, Start, Goal);
    }

    LastPathDiagnostic.Reset();
    const UHellRunTraversalComponent* C = Character.FindComponentByClass<UHellRunTraversalComponent>();
    const bool bWalk = !C || C->CanWalkNavigation();
    const bool bClimb = !C || C->CanClimbNavigation();
    const bool bMantle = !C || C->CanMantleNavigation();
    const bool bDrop = !C || C->CanDropNavigation();
    const bool bJump = !C || C->CanJumpNavigation();
    const bool bVault = !C || C->CanVaultNavigation();
    const bool bWallClimb = C && C->CanWallClimbNavigation();
    const bool bFly = C && C->CanFlyNavigation();
    const UHellRunTraversalNavigationSettings* S =
        GetDefault<UHellRunTraversalNavigationSettings>();
    // Endpoint projection is a set, not a single nearest node. On ramps,
    // stacked floors and platform lips the Euclidean-nearest surface cell can
    // be a directed dead end while an adjacent cell is the correct graph
    // entrance. Let A* choose the cheapest reachable anchored candidate.
    auto CollectEndpointCandidates =
        [this, bWalk, bWallClimb, bFly, S](const FVector& Location)
    {
        TArray<int32> Candidates;
        if (BakedVoxelSize <= 0.0f) return Candidates;
        const FVector Local = (Location - GridOrigin) / BakedVoxelSize;
        const FIntVector Center(
            FMath::FloorToInt(Local.X),
            FMath::FloorToInt(Local.Y),
            FMath::FloorToInt(Local.Z));
        auto ConsiderCell = [this, bWalk, bWallClimb, bFly, S, Location, &Candidates](
            const FIntVector& Cell)
        {
            if (Cell.X < 0 || Cell.Y < 0 || Cell.Z < 0
                || Cell.X >= GridDimensions.X
                || Cell.Y >= GridDimensions.Y
                || Cell.Z >= GridDimensions.Z) return;
            const int32 Flat = FlattenCell(Cell);
            const int32 NodeIndex =
                CellToNode.IsValidIndex(Flat) ? CellToNode[Flat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(NodeIndex)
                || DynamicBlockedNodeRefCounts.Contains(NodeIndex)
                || Candidates.Contains(NodeIndex)) return;
            const FHellRunBakedVoxelNode& Node = Nodes[NodeIndex];
            if (!bFly && !(bWalk && Node.bGround)
                && !(bWallClimb && Node.bClimb)) return;
            const float HorizontalAnchorDistance =
                FVector::Dist2D(Node.Location, Location);
            const float VerticalAnchorDistance =
                FMath::Abs(Node.Location.Z - Location.Z);
            const bool bValidAnchor = bFly
                ? FVector::Distance(Node.Location, Location)
                    <= BakedVoxelSize * 1.5f
                : HorizontalAnchorDistance <= BakedVoxelSize * 1.5f
                    && VerticalAnchorDistance <= S->VoxelSearchPadding;
            if (bValidAnchor) Candidates.Add(NodeIndex);
        };

        if (bFly)
        {
            // A flying endpoint is a true local 3D occupancy query.
            for (int32 Z = -2; Z <= 2; ++Z)
            for (int32 Y = -2; Y <= 2; ++Y)
            for (int32 X = -2; X <= 2; ++X)
            {
                ConsiderCell(Center + FIntVector(X, Y, Z));
            }
        }
        else
        {
            // A grounded endpoint is a query against nearby surface columns,
            // not a cube around an arbitrary voxel Z phase. Inspect every
            // baked layer in the local XY footprint so stacked floors remain
            // explicit candidates.
            for (int32 Y = -2; Y <= 2; ++Y)
            for (int32 X = -2; X <= 2; ++X)
            for (int32 Z = 0; Z < GridDimensions.Z; ++Z)
            {
                ConsiderCell(FIntVector(Center.X + X, Center.Y + Y, Z));
            }

            // Grounded projection is column-first. An endpoint in open air
            // falls onto support beneath its own XY position; it must not snap
            // sideways onto a geometrically closer but disconnected ledge.
            Candidates.RemoveAll([this, Location, S](const int32 Candidate)
            {
                return Nodes[Candidate].Location.Z
                    > Location.Z + S->GroundStepHeight;
            });
            float ClosestHorizontalDistance = BIG_NUMBER;
            for (const int32 Candidate : Candidates)
            {
                ClosestHorizontalDistance = FMath::Min(
                    ClosestHorizontalDistance,
                    FVector::Dist2D(Nodes[Candidate].Location, Location));
            }
            const float ColumnTierTolerance =
                FMath::Max(1.0f, BakedVoxelSize * 0.1f);
            Candidates.RemoveAll([
                this,
                Location,
                ClosestHorizontalDistance,
                ColumnTierTolerance](const int32 Candidate)
            {
                return FVector::Dist2D(Nodes[Candidate].Location, Location)
                    > ClosestHorizontalDistance + ColumnTierTolerance;
            });

            // Within that surface column, retain the closest vertical layer.
            // This preserves an occupied elevated floor while avoiding a lower
            // stacked floor in the same column.
            float ClosestVerticalDistance = BIG_NUMBER;
            for (const int32 Candidate : Candidates)
            {
                ClosestVerticalDistance = FMath::Min(ClosestVerticalDistance,
                    FMath::Abs(Nodes[Candidate].Location.Z - Location.Z));
            }
            const float SurfaceTierTolerance =
                FMath::Max(S->GroundStepHeight, BakedVoxelSize * 0.5f);
            Candidates.RemoveAll([this, Location, ClosestVerticalDistance, SurfaceTierTolerance](
                const int32 Candidate)
            {
                return FMath::Abs(Nodes[Candidate].Location.Z - Location.Z)
                    > ClosestVerticalDistance + SurfaceTierTolerance;
            });
        }
        Candidates.Sort([this, Location, bFly](int32 A, int32 B)
        {
            if (!bFly)
            {
                const float HorizontalA =
                    FVector::Dist2D(Nodes[A].Location, Location);
                const float HorizontalB =
                    FVector::Dist2D(Nodes[B].Location, Location);
                if (!FMath::IsNearlyEqual(HorizontalA, HorizontalB, 0.1f))
                {
                    return HorizontalA < HorizontalB;
                }
            }
            return FVector::DistSquared(Nodes[A].Location, Location)
                < FVector::DistSquared(Nodes[B].Location, Location);
        });
        if (bFly && !Candidates.IsEmpty())
        {
            const float ClosestDistance = FVector::Distance(
                Nodes[Candidates[0]].Location,
                Location);
            const float AnchorTierTolerance =
                FMath::Max(1.0f, BakedVoxelSize * 0.1f);
            Candidates.RemoveAll([
                this,
                Location,
                ClosestDistance,
                AnchorTierTolerance](int32 Candidate)
            {
                return FVector::Distance(
                    Nodes[Candidate].Location,
                    Location)
                    > ClosestDistance + AnchorTierTolerance;
            });
        }
        constexpr int32 MaximumEndpointCandidates = 32;
        if (Candidates.Num() > MaximumEndpointCandidates)
        {
            Candidates.SetNum(MaximumEndpointCandidates, EAllowShrinking::No);
        }
        return Candidates;
    };
    const TArray<int32> StartCandidates = CollectEndpointCandidates(Start);
    const TArray<int32> GoalCandidates = CollectEndpointCandidates(Goal);
    const int32 StartNode = StartCandidates.IsEmpty() ? INDEX_NONE : StartCandidates[0];
    int32 GoalNode = GoalCandidates.IsEmpty() ? INDEX_NONE : GoalCandidates[0];
    if (StartNode == INDEX_NONE || GoalNode == INDEX_NONE)
    {
        LastPathDiagnostic = FString::Printf(
            TEXT("ENDPOINT FAILED | startNode %d | goalNode %d"), StartNode, GoalNode);
        FHellRunNavigationDebugLog::Write(&Character, TEXT("VOXEL_GRAPH_ENDPOINT_FAILED"),
            FString::Printf(TEXT("start=%s startNode=%d goal=%s goalNode=%d graphVersion=%d nodes=%d edges=%d"),
                *Start.ToCompactString(), StartNode, *Goal.ToCompactString(), GoalNode,
                BakedGraphVersion, Nodes.Num(), Edges.Num()));
        return nullptr;
    }
    const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
    const float AgentHalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : S->VoxelBakeAgentHalfHeight;
    const float GroundNodeZAdjustment = AgentHalfHeight - S->VoxelBakeAgentHalfHeight;
    auto GetAgentNodeLocation = [this, GroundNodeZAdjustment](int32 NodeIndex)
    {
        FVector Location = Nodes[NodeIndex].Location;
        if (Nodes[NodeIndex].bGround) Location.Z += GroundNodeZAdjustment;
        return Location;
    };
    FHellRunVoxelTraversalCostProfile CostProfile = S->GetVoxelCostProfileForCharacter(Character);
    if (C && C->bOverrideVoxelCostProfile)
    {
        CostProfile = C->VoxelCostProfileOverride;
    }
    struct FRecord
    {
        float G = BIG_NUMBER;
        float F = BIG_NUMBER;
        int32 Parent = INDEX_NONE;
        EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
        FVector TraversalEntry = FVector::ZeroVector;
        FVector TraversalExit = FVector::ZeroVector;
        bool bClosed = false;
    };
    TMap<int32, FRecord> Records;
    TArray<int32> Open;
    auto Push = [&Open, &Records](int32 Index)
    {
        int32 P = Open.Add(Index);
        while (P > 0) { const int32 Parent = (P - 1) / 2; if (Records.FindChecked(Open[Parent]).F <= Records.FindChecked(Open[P]).F) break; Open.Swap(Parent, P); P = Parent; }
    };
    auto Pop = [&Open, &Records]()
    {
        const int32 Result = Open[0]; Open[0] = Open.Last(); Open.Pop(EAllowShrinking::No); int32 P = 0;
        while (Open.IsValidIndex(P)) { const int32 L=P*2+1,R=L+1; if(!Open.IsValidIndex(L)) break; int32 B=L; if(Open.IsValidIndex(R)&&Records.FindChecked(Open[R]).F<Records.FindChecked(Open[L]).F)B=R; if(Records.FindChecked(Open[P]).F<=Records.FindChecked(Open[B]).F)break; Open.Swap(P,B);P=B; }
        return Result;
    };
    const FVector SearchGoal = Goal;
    float MinimumCostPerVoxel =
        S->VoxelWalkCost * CostProfile.WalkMultiplier;
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->VoxelClimbCost * CostProfile.ClimbMultiplier);
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->VoxelMantleCost * CostProfile.MantleMultiplier);
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->JumpAreaCost * CostProfile.JumpMultiplier);
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->VaultAreaCost * CostProfile.VaultMultiplier);
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->DropAreaCost * CostProfile.DropMultiplier);
    MinimumCostPerVoxel = FMath::Min(MinimumCostPerVoxel,
        S->VoxelFlightCost * CostProfile.FlightMultiplier);
    MinimumCostPerVoxel = FMath::Max(0.0001f, MinimumCostPerVoxel);
    auto ScoreNode = [this, SearchGoal, MinimumCostPerVoxel](
        int32 NodeIndex, float G)
    {
        const FVector NodeLocation = Nodes[NodeIndex].Location;
        const float H = FVector::Distance(NodeLocation, SearchGoal)
            / FMath::Max(BakedVoxelSize, 1.0f) * MinimumCostPerVoxel;
        return G + H;
    };
    for (const int32 Candidate : StartCandidates)
    {
        FRecord& StartRecord = Records.Add(Candidate);
        StartRecord.G = FVector::Distance(Start, GetAgentNodeLocation(Candidate))
            / FMath::Max(BakedVoxelSize, 1.0f);
        StartRecord.F = ScoreNode(Candidate, StartRecord.G);
        Push(Candidate);
    }
    TSet<int32> GoalCandidateSet;
    GoalCandidateSet.Reserve(GoalCandidates.Num());
    for (const int32 Candidate : GoalCandidates) GoalCandidateSet.Add(Candidate);
    bool bReachedGoal = false;
    // Route existence is a graph property. A tuning budget cannot turn an
    // unfinished search into "unreachable"; A* must exhaust its open set before
    // making that claim for either baked-edge or query-time adjacency.
    int32 ExpandedNodes = 0;
    int32 ClosestExpandedNode = INDEX_NONE;
    float ClosestExpandedDistanceSq = BIG_NUMBER;
    while (!Open.IsEmpty())
    {
        ++ExpandedNodes;
        const int32 Current = Pop();
        FRecord& CurrentRecord = Records.FindChecked(Current);
        if (CurrentRecord.bClosed) continue;
        CurrentRecord.bClosed = true;
        const float GoalDistanceSq = FVector::DistSquared(
            GetAgentNodeLocation(Current), Goal);
        if (GoalDistanceSq < ClosestExpandedDistanceSq)
        {
            ClosestExpandedDistanceSq = GoalDistanceSq;
            ClosestExpandedNode = Current;
        }
        if (GoalCandidateSet.Contains(Current))
        {
            GoalNode = Current;
            bReachedGoal = true;
            break;
        }
        const FHellRunBakedVoxelNode& From = Nodes[Current];
        if (HasAuthoritativeTypedEdgeGraph())
        {
            const int32 EdgeEnd = From.FirstEdge + From.EdgeCount;
            for (int32 EdgeIndex = From.FirstEdge; EdgeIndex < EdgeEnd; ++EdgeIndex)
            {
                if (!Edges.IsValidIndex(EdgeIndex)) continue;
                const FHellRunBakedVoxelEdge& Edge = Edges[EdgeIndex];
                if (!Nodes.IsValidIndex(Edge.ToNode)
                    || DynamicBlockedNodeRefCounts.Contains(Edge.ToNode)) continue;

                const FVector EdgeDelta =
                    Nodes[Edge.ToNode].Location - From.Location;
                const float EdgeHorizontal = EdgeDelta.Size2D();
                bool bGeometricallyValid = true;
                if (Edge.Mode == EHellRunVoxelSegment::Mantle)
                {
                    bGeometricallyValid =
                        EdgeDelta.Z >= -S->GroundStepHeight
                        && EdgeDelta.Z <= S->Mantle.MaximumDepth
                        && EdgeHorizontal <= S->Mantle.HorizontalReach;
                }
                else if (Edge.Mode == EHellRunVoxelSegment::Drop)
                {
                    bGeometricallyValid =
                        EdgeDelta.Z < -S->GroundStepHeight
                        && -EdgeDelta.Z <= S->Drop.MaximumDepth
                        && EdgeHorizontal <= S->Drop.HorizontalReach;
                }
                if (!bGeometricallyValid) continue;

                bool bCapabilityAllowed = false;
                float Multiplier = 1.0f;
                switch (Edge.Mode)
                {
                case EHellRunVoxelSegment::Walk:
                    bCapabilityAllowed = bWalk;
                    Multiplier = CostProfile.WalkMultiplier;
                    break;
                case EHellRunVoxelSegment::Climb:
                    bCapabilityAllowed = bWallClimb;
                    Multiplier = CostProfile.ClimbMultiplier;
                    break;
                case EHellRunVoxelSegment::Mantle:
                    bCapabilityAllowed = bMantle;
                    Multiplier = CostProfile.MantleMultiplier;
                    break;
                case EHellRunVoxelSegment::Drop:
                    bCapabilityAllowed = bDrop;
                    Multiplier = CostProfile.DropMultiplier;
                    break;
                case EHellRunVoxelSegment::Jump:
                    bCapabilityAllowed = bJump;
                    Multiplier = CostProfile.JumpMultiplier;
                    break;
                case EHellRunVoxelSegment::Vault:
                    bCapabilityAllowed = bVault;
                    Multiplier = CostProfile.VaultMultiplier;
                    break;
                case EHellRunVoxelSegment::Fly:
                    bCapabilityAllowed = bFly;
                    Multiplier = CostProfile.FlightMultiplier;
                    break;
                default:
                    break;
                }
                if (!bCapabilityAllowed) continue;

                float EdgeCost = Edge.BaseCost * Multiplier;
                if (CurrentRecord.Parent != INDEX_NONE)
                {
                    if (CurrentRecord.Mode != Edge.Mode)
                    {
                        EdgeCost += CostProfile.LocomotionStateChangePenalty;
                    }
                    const FVector PreviousDirection =
                        (From.Location - Nodes[CurrentRecord.Parent].Location).GetSafeNormal();
                    const FVector NextDirection =
                        (Nodes[Edge.ToNode].Location - From.Location).GetSafeNormal();
                    EdgeCost += CostProfile.TurnPenalty
                        * (1.0f - FVector::DotProduct(PreviousDirection, NextDirection));
                }

                FRecord& Next = Records.FindOrAdd(Edge.ToNode);
                if (Next.bClosed) continue;
                const float NewG = CurrentRecord.G + EdgeCost;
                if (NewG < Next.G)
                {
                    Next.G = NewG;
                    Next.Parent = Current;
                    Next.Mode = Edge.Mode;
                    if (IsDiscreteTraversal(Edge.Mode))
                    {
                        GetTraversalBoundaryPoints(
                            GetAgentNodeLocation(Current),
                            GetAgentNodeLocation(Edge.ToNode),
                            BakedVoxelSize,
                            Edge.Mode,
                            Next.TraversalEntry,
                            Next.TraversalExit);
                    }
                    Next.F = ScoreNode(Edge.ToNode, NewG);
                    Push(Edge.ToNode);
                }
            }
            continue;
        }
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        const int32 MaximumDropCells = FMath::Max(1,
            FMath::CeilToInt(S->Drop.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
        const int32 MaximumMantleCells = FMath::Max(1,
            FMath::CeilToInt(S->Mantle.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
        for (int32 DZ = -MaximumDropCells; DZ <= MaximumMantleCells; ++DZ)
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            if (DX == 0 && DY == 0 && DZ == 0) continue;
            const int32 HorizontalSteps = FMath::Abs(DX) + FMath::Abs(DY);
            const bool bCardinalHorizontal = FMath::Abs(DZ) <= 1 && HorizontalSteps == 1;
            const bool bDiagonalHorizontal = FMath::Abs(DZ) <= 1 && FMath::Abs(DX) == 1 && FMath::Abs(DY) == 1;
            const bool bTraversalAcrossEdge = HorizontalSteps == 1;
            const int32 ToFlat = FlattenCell(FromCell + FIntVector(DX, DY, DZ));
            const int32 ToNode = CellToNode.IsValidIndex(ToFlat) ? CellToNode[ToFlat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(ToNode)) continue;
            if (DynamicBlockedNodeRefCounts.Contains(ToNode)) continue;
            const FHellRunBakedVoxelNode& To = Nodes[ToNode];
            const float GroundHeightDelta = To.Location.Z - From.Location.Z;
            const float HorizontalDistance = FVector::Dist2D(From.Location, To.Location);
            const float TraversalHorizontalDistance =
                FMath::Max(0.0f, HorizontalDistance - BakedVoxelSize);
            const uint8 ExitBit = CardinalDirectionBit(DX, DY);
            const bool bContinuousGround = ExitBit != 0
                && (From.GroundExitMask & ExitBit) != 0
                && (To.GroundExitMask & OppositeCardinalDirectionBit(DX, DY)) != 0;
            const float Distance = FVector(DX, DY, DZ).Size();
            EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
            float EdgeCost = BIG_NUMBER;
            auto Consider = [&Mode, &EdgeCost](EHellRunVoxelSegment CandidateMode, float CandidateCost)
            {
                if (CandidateCost < EdgeCost) { Mode = CandidateMode; EdgeCost = CandidateCost; }
            };
            if (bWalk && From.bGround && To.bGround
                && FMath::Abs(GroundHeightDelta) <= S->GroundStepHeight
                && (bDiagonalHorizontal || bContinuousGround)
                && (bCardinalHorizontal || (S->bAllowDiagonalVoxelWalk && bDiagonalHorizontal)))
            {
                Consider(EHellRunVoxelSegment::Walk, S->VoxelWalkCost * CostProfile.WalkMultiplier * Distance);
            }
            if (bClimb && From.bGround && To.bGround && bCardinalHorizontal && !bContinuousGround
                && FMath::Abs(GroundHeightDelta) <= S->GroundStepHeight)
            {
                const bool bLowObstacle = ((From.ObstacleExitMask & ExitBit) != 0)
                    || ((To.ObstacleExitMask & OppositeCardinalDirectionBit(DX, DY)) != 0);
                if (bLowObstacle && S->Vault.bEnabled && BakedVoxelSize <= S->Vault.HorizontalReach
                    && FMath::Abs(GroundHeightDelta) <= S->Vault.EndpointHeightTolerance)
                {
                    Consider(EHellRunVoxelSegment::Vault,
                        S->VaultAreaCost * CostProfile.VaultMultiplier * Distance);
                }
                else if (!bLowObstacle && S->Jump.bEnabled && BakedVoxelSize <= S->Jump.HorizontalReach
                    && FMath::Abs(GroundHeightDelta) <= S->Jump.EndpointHeightTolerance)
                {
                    Consider(EHellRunVoxelSegment::Jump,
                        S->JumpAreaCost * CostProfile.JumpMultiplier * Distance);
                }
            }
            if (bClimb && From.bGround && To.bGround
                && GroundHeightDelta < -S->GroundStepHeight
                && FMath::Abs(GroundHeightDelta) <= S->Drop.MaximumDepth
                && TraversalHorizontalDistance <= S->Drop.HorizontalReach
                && bTraversalAcrossEdge)
            {
                Consider(EHellRunVoxelSegment::Drop, S->DropAreaCost * CostProfile.DropMultiplier * Distance);
            }
            else if (bClimb && From.bGround && To.bGround
                && GroundHeightDelta > S->GroundStepHeight
                && GroundHeightDelta <= S->Mantle.MaximumDepth
                && TraversalHorizontalDistance <= S->Mantle.HorizontalReach
                && bTraversalAcrossEdge)
            {
                Consider(EHellRunVoxelSegment::Mantle, S->VoxelMantleCost * CostProfile.MantleMultiplier * Distance);
            }
            else if (bWallClimb && !From.bGround && From.bClimb && To.bGround && FMath::Abs(DZ) <= 1)
            {
                Consider(EHellRunVoxelSegment::Mantle, S->VoxelMantleCost * CostProfile.MantleMultiplier * Distance);
            }
            else if (bWallClimb && From.bGround && !To.bGround && To.bClimb && FMath::Abs(DZ) <= 1)
            {
                Consider(EHellRunVoxelSegment::Climb,
                    S->VoxelClimbCost * CostProfile.ClimbMultiplier * Distance);
            }
            else if (bWallClimb && From.bClimb && To.bClimb && FMath::Abs(DZ) <= 1
                && FVector::DotProduct(From.WallNormal, To.WallNormal) >= 0.25f)
            {
                const float HorizontalAmount = FVector2D(static_cast<double>(DX), static_cast<double>(DY)).Size();
                const float VerticalAmount = FMath::Abs(static_cast<float>(DZ));
                const float LateralAlpha = HorizontalAmount / FMath::Max(HorizontalAmount + VerticalAmount, UE_SMALL_NUMBER);
                const float DirectionCost = FMath::Lerp(1.0f, CostProfile.ClimbLateralMultiplier, LateralAlpha)
                    * (DZ < 0 ? CostProfile.ClimbDownMultiplier : 1.0f);
                Consider(EHellRunVoxelSegment::Climb,
                    S->VoxelClimbCost * CostProfile.ClimbMultiplier * DirectionCost * Distance);
            }
            if (bFly && FMath::Abs(DZ) <= 1)
            {
                Consider(EHellRunVoxelSegment::Fly, S->VoxelFlightCost * CostProfile.FlightMultiplier * Distance);
            }
            if (EdgeCost == BIG_NUMBER) continue;

            if (CurrentRecord.Parent != INDEX_NONE)
            {
                if (CurrentRecord.Mode != Mode)
                {
                    EdgeCost += CostProfile.LocomotionStateChangePenalty;
                }

                const FHellRunBakedVoxelNode& ParentNode = Nodes[CurrentRecord.Parent];
                const FVector PreviousDirection = (From.Location - ParentNode.Location).GetSafeNormal();
                const FVector NextDirection = (To.Location - From.Location).GetSafeNormal();
                EdgeCost += CostProfile.TurnPenalty * (1.0f - FVector::DotProduct(PreviousDirection, NextDirection));
            }
            FVector TraversalEntry = From.Location;
            FVector TraversalExit = To.Location;
            if (IsDiscreteTraversal(Mode))
            {
                GetTraversalBoundaryPoints(
                    GetAgentNodeLocation(Current),
                    GetAgentNodeLocation(ToNode),
                    BakedVoxelSize,
                    Mode,
                    TraversalEntry,
                    TraversalExit);
            }
            if (!IsTraversalEdgeClear(
                Character,
                TraversalEntry,
                TraversalExit,
                Mode,
                *S))
            {
                continue;
            }
            FRecord& Next = Records.FindOrAdd(ToNode);
            if (Next.bClosed) continue;
            const float NewG = CurrentRecord.G + EdgeCost;
            if (NewG < Next.G)
            {
                Next.G = NewG;
                Next.Parent = Current;
                Next.Mode = Mode;
                Next.TraversalEntry = TraversalEntry;
                Next.TraversalExit = TraversalExit;
                Next.F = ScoreNode(ToNode, NewG);
                Push(ToNode);
            }
        }

        // Straight cardinal gap edges are explicit locomotion actions. Free airborne
        // intermediate cells form a jump; blocked cells with free space one voxel
        // above form a low-obstacle vault. Never synthesize diagonal zig-zag spans.
        if (bClimb && From.bGround)
        {
            const FIntVector Directions[] = {
                FIntVector(1,0,0), FIntVector(-1,0,0), FIntVector(0,1,0), FIntVector(0,-1,0),
                FIntVector(1,1,0), FIntVector(1,-1,0), FIntVector(-1,1,0), FIntVector(-1,-1,0)
            };
            const int32 MaximumDropSpanCells = FMath::Max(1, FMath::CeilToInt(
                S->Drop.HorizontalReach / FMath::Max(BakedVoxelSize, 1.0f)));
            const int32 MaximumDropDepthCells = FMath::Max(1, FMath::CeilToInt(
                S->Drop.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
            for (const FIntVector& Direction : Directions)
            for (int32 SpanCells = 1; SpanCells <= MaximumDropSpanCells; ++SpanCells)
            for (int32 LandingDZ = -1; LandingDZ >= -MaximumDropDepthCells; --LandingDZ)
            {
                const FIntVector LandingCell = FromCell + Direction * SpanCells + FIntVector(0, 0, LandingDZ);
                const int32 LandingFlat = FlattenCell(LandingCell);
                const int32 LandingNode = CellToNode.IsValidIndex(LandingFlat) ? CellToNode[LandingFlat] : INDEX_NONE;
                if (!Nodes.IsValidIndex(LandingNode) || !Nodes[LandingNode].bGround
                    || DynamicBlockedNodeRefCounts.Contains(LandingNode)) continue;
                const FHellRunBakedVoxelNode& Landing = Nodes[LandingNode];
                const float HeightDelta = Landing.Location.Z - From.Location.Z;
                const float HorizontalDistance = FVector::Dist2D(From.Location, Landing.Location);
                const float TraversalHorizontalDistance =
                    FMath::Max(0.0f, HorizontalDistance - BakedVoxelSize);
                if (HeightDelta >= -S->GroundStepHeight
                    || FMath::Abs(HeightDelta) > S->Drop.MaximumDepth
                    || TraversalHorizontalDistance > S->Drop.HorizontalReach)
                {
                    continue;
                }

                const uint8 DirectionBit = CardinalDirectionBit(Direction.X, Direction.Y);
                const int32 FirstFlat = FlattenCell(FromCell + Direction);
                const int32 FirstNode = CellToNode.IsValidIndex(FirstFlat) ? CellToNode[FirstFlat] : INDEX_NONE;
                const bool bLeavesGround = (From.GroundExitMask & DirectionBit) == 0
                    || !Nodes.IsValidIndex(FirstNode) || !Nodes[FirstNode].bGround
                    || (SpanCells == 1
                        && (Landing.GroundExitMask & OppositeCardinalDirectionBit(Direction.X, Direction.Y)) == 0);
                if (!bLeavesGround) continue;
                FVector TraversalEntry;
                FVector TraversalExit;
                GetTraversalBoundaryPoints(
                    GetAgentNodeLocation(Current),
                    GetAgentNodeLocation(LandingNode),
                    BakedVoxelSize,
                    EHellRunVoxelSegment::Drop,
                    TraversalEntry,
                    TraversalExit);
                if (!IsTraversalEdgeClear(
                    Character,
                    TraversalEntry,
                    TraversalExit,
                    EHellRunVoxelSegment::Drop,
                    *S))
                {
                    continue;
                }

                const float HorizontalTime = TraversalHorizontalDistance
                    / FMath::Max(1.0f, S->DropMinimumHorizontalSpeed);
                const float FallTime = FMath::Sqrt(2.0f * FMath::Abs(HeightDelta) / 980.0f);
                float EdgeCost = FMath::Max(HorizontalTime, FallTime) * 7.0f
                    * S->DropAreaCost * CostProfile.DropMultiplier + CostProfile.LocomotionStateChangePenalty;
                FRecord& Next = Records.FindOrAdd(LandingNode);
                if (Next.bClosed) continue;
                const float NewG = CurrentRecord.G + EdgeCost;
                if (NewG < Next.G)
                {
                    Next.G = NewG;
                    Next.Parent = Current;
                    Next.Mode = EHellRunVoxelSegment::Drop;
                    Next.TraversalEntry = TraversalEntry;
                    Next.TraversalExit = TraversalExit;
                    Next.F = ScoreNode(LandingNode, NewG);
                    Push(LandingNode);
                }
            }

            const int32 MaximumSpanCells = FMath::Max(2, FMath::CeilToInt(
                FMath::Max(S->Jump.HorizontalReach, S->Vault.HorizontalReach) / FMath::Max(BakedVoxelSize, 1.0f)));
            for (int32 OffsetX = -MaximumSpanCells; OffsetX <= MaximumSpanCells; ++OffsetX)
            for (int32 OffsetY = -MaximumSpanCells; OffsetY <= MaximumSpanCells; ++OffsetY)
            for (int32 LandingDZ = -1; LandingDZ <= 1; ++LandingDZ)
            {
                const FIntVector SpanOffset(OffsetX, OffsetY, 0);
                const int32 SpanCells = FMath::Max(FMath::Abs(OffsetX), FMath::Abs(OffsetY));
                const float NodeSpanDistance =
                    FVector(SpanOffset).Size() * BakedVoxelSize;
                const float TraversalSpanDistance =
                    FMath::Max(0.0f, NodeSpanDistance - BakedVoxelSize);
                if (SpanCells < 2
                    || TraversalSpanDistance
                        > FMath::Max(
                            S->Jump.HorizontalReach,
                            S->Vault.HorizontalReach))
                {
                    continue;
                }
                const FIntVector LandingCell = FromCell + SpanOffset + FIntVector(0, 0, LandingDZ);
                const int32 LandingFlat = FlattenCell(LandingCell);
                const int32 LandingNode = CellToNode.IsValidIndex(LandingFlat) ? CellToNode[LandingFlat] : INDEX_NONE;
                if (!Nodes.IsValidIndex(LandingNode) || !Nodes[LandingNode].bGround
                    || DynamicBlockedNodeRefCounts.Contains(LandingNode)) continue;

                bool bAllGap = true;
                bool bAllLowObstacle = true;
                for (int32 Step = 1; Step < SpanCells; ++Step)
                {
                    const float Alpha = static_cast<float>(Step) / SpanCells;
                    const FIntVector MiddleCell = FromCell + FIntVector(
                        FMath::RoundToInt(OffsetX * Alpha), FMath::RoundToInt(OffsetY * Alpha), 0);
                    const int32 MiddleFlat = FlattenCell(MiddleCell);
                    const int32 MiddleNode = CellToNode.IsValidIndex(MiddleFlat) ? CellToNode[MiddleFlat] : INDEX_NONE;
                    bAllGap &= Nodes.IsValidIndex(MiddleNode) && !Nodes[MiddleNode].bGround;

                    const int32 AboveFlat = FlattenCell(MiddleCell + FIntVector(0, 0, 1));
                    const int32 AboveNode = CellToNode.IsValidIndex(AboveFlat) ? CellToNode[AboveFlat] : INDEX_NONE;
                    bAllLowObstacle &= !Nodes.IsValidIndex(MiddleNode) && Nodes.IsValidIndex(AboveNode);
                }
                const float HeightDelta = Nodes[LandingNode].Location.Z - From.Location.Z;
                EHellRunVoxelSegment SpanMode;
                float BaseCost;
                const bool bCardinalSpan = OffsetX == 0 || OffsetY == 0;
                if (bAllGap && S->Jump.bEnabled
                    && TraversalSpanDistance <= S->Jump.HorizontalReach
                    && FMath::Abs(HeightDelta) <= S->Jump.EndpointHeightTolerance)
                {
                    SpanMode = EHellRunVoxelSegment::Jump;
                    BaseCost = S->JumpAreaCost * CostProfile.JumpMultiplier;
                }
                else if (bCardinalSpan && bAllLowObstacle && S->Vault.bEnabled
                    && TraversalSpanDistance <= S->Vault.HorizontalReach
                    && FMath::Abs(HeightDelta) <= S->Vault.EndpointHeightTolerance)
                {
                    SpanMode = EHellRunVoxelSegment::Vault;
                    BaseCost = S->VaultAreaCost * CostProfile.VaultMultiplier;
                }
                else continue;
                FVector TraversalEntry;
                FVector TraversalExit;
                GetTraversalBoundaryPoints(
                    GetAgentNodeLocation(Current),
                    GetAgentNodeLocation(LandingNode),
                    BakedVoxelSize,
                    SpanMode,
                    TraversalEntry,
                    TraversalExit);
                if (!IsTraversalEdgeClear(
                    Character,
                    TraversalEntry,
                    TraversalExit,
                    SpanMode,
                    *S))
                {
                    continue;
                }

                float EdgeCost = BaseCost * FVector::Distance(From.Location, Nodes[LandingNode].Location)
                    / FMath::Max(BakedVoxelSize, 1.0f) + CostProfile.LocomotionStateChangePenalty;
                if (CurrentRecord.Parent != INDEX_NONE)
                {
                    const FVector PreviousDirection = (From.Location - Nodes[CurrentRecord.Parent].Location).GetSafeNormal();
                    const FVector NextDirection = (Nodes[LandingNode].Location - From.Location).GetSafeNormal();
                    EdgeCost += CostProfile.TurnPenalty * (1.0f - FVector::DotProduct(PreviousDirection, NextDirection));
                }
                FRecord& Next = Records.FindOrAdd(LandingNode);
                if (Next.bClosed) continue;
                const float NewG = CurrentRecord.G + EdgeCost;
                if (NewG < Next.G)
                {
                    Next.G = NewG;
                    Next.Parent = Current;
                    Next.Mode = SpanMode;
                    Next.TraversalEntry = TraversalEntry;
                    Next.TraversalExit = TraversalExit;
                    Next.F = ScoreNode(LandingNode, NewG);
                    Push(LandingNode);
                }
            }
        }
    }
    const FRecord* GoalRecord = Records.Find(GoalNode);
    if (!bReachedGoal || !GoalRecord)
    {
        LastPathDiagnostic = FString::Printf(
            TEXT("NO ROUTE | open set exhausted | start %d %s C%d | goal %d %s C%d | expanded %d | closest %d distance %.1f delta %s"),
            StartNode, *GetAgentNodeLocation(StartNode).ToCompactString(),
            Nodes[StartNode].ComponentId,
            GoalNode, *GetAgentNodeLocation(GoalNode).ToCompactString(),
            Nodes[GoalNode].ComponentId, ExpandedNodes,
            ClosestExpandedNode,
            FMath::Sqrt(ClosestExpandedDistanceSq),
            ClosestExpandedNode != INDEX_NONE
                ? *(GetAgentNodeLocation(ClosestExpandedNode) - Goal)
                    .ToCompactString()
                : TEXT("none"));
        FHellRunNavigationDebugLog::Write(&Character, TEXT("VOXEL_GRAPH_SEARCH_FAILED"),
            FString::Printf(TEXT("startNode=%d startComponent=%d startEdges=%d goalNode=%d goalComponent=%d goalEdges=%d expanded=%d limit=%d"),
                StartNode, Nodes[StartNode].ComponentId, Nodes[StartNode].EdgeCount,
                GoalNode, Nodes[GoalNode].ComponentId, Nodes[GoalNode].EdgeCount,
                ExpandedNodes, Nodes.Num()));
        return nullptr;
    }
    TArray<int32> Reverse;
    for (int32 N = GoalNode; N != INDEX_NONE; )
    {
        Reverse.Add(N);
        const FRecord* Record = Records.Find(N);
        N = Record ? Record->Parent : INDEX_NONE;
    }
    Algo::Reverse(Reverse);

    struct FRoutePoint
    {
        FVector Location = FVector::ZeroVector;
        EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
        float CumulativeCost = 0.0f;
    };

    TArray<FRoutePoint> RawRoute;
    RawRoute.Reserve(Reverse.Num() * 3 + 2);
    RawRoute.Add({Start, EHellRunVoxelSegment::Walk, 0.0f});
    for (const int32 NodeIndex : Reverse)
    {
        const FRecord& Record = Records.FindChecked(NodeIndex);
        const FVector NodeLocation = GetAgentNodeLocation(NodeIndex);
        if (Record.Parent != INDEX_NONE && IsDiscreteTraversal(Record.Mode))
        {
            const FRecord& ParentRecord =
                Records.FindChecked(Record.Parent);
            const float EntryCost =
                FMath::IsFinite(ParentRecord.G) ? ParentRecord.G : 0.0f;
            if (!RawRoute.Last().Location.Equals(
                Record.TraversalEntry, 1.0f))
            {
                RawRoute.Add({
                    Record.TraversalEntry,
                    EHellRunVoxelSegment::Walk,
                    EntryCost
                });
            }
            RawRoute.Add({
                Record.TraversalExit,
                Record.Mode,
                Record.G
            });
            if (!Record.TraversalExit.Equals(NodeLocation, 1.0f))
            {
                RawRoute.Add({
                    NodeLocation,
                    EHellRunVoxelSegment::Walk,
                    Record.G
                });
            }
        }
        else if (!RawRoute.Last().Location.Equals(NodeLocation, 1.0f))
        {
            RawRoute.Add({
                NodeLocation,
                Record.Mode,
                Record.G
            });
        }
    }

    // Preserve the exact validated edge chain. Iteratively removing
    // near-collinear nodes turns a gradual curve into a long unvalidated chord
    // and can cut through collision. Smoothing belongs in a separate
    // capsule-validated string-pulling pass.
    TArray<FRoutePoint> Route = RawRoute;

    const float ExactGoalAnchorDistance =
        FVector::Distance(Route.Last().Location, Goal);
    if (!Route.Last().Location.Equals(Goal, 1.0f)
        && ExactGoalAnchorDistance <= BakedVoxelSize * 1.1f)
    {
        const EHellRunVoxelSegment GoalMode = bWalk && Nodes[GoalNode].bGround
            ? EHellRunVoxelSegment::Walk
            : (bFly ? EHellRunVoxelSegment::Fly : EHellRunVoxelSegment::Climb);
        Route.Add({Goal, GoalMode, GoalRecord ? GoalRecord->G : Route.Last().CumulativeCost});
    }

    TArray<FVector> Locations;
    Locations.Reserve(Route.Num());
    for (const FRoutePoint& Point : Route)
    {
        Locations.Add(Point.Location);
    }
    TSharedPtr<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe> VoxelPath =
        MakeShared<FHellRunVoxelNavigationPath>(Locations, nullptr);
    FNavPathSharedPtr Path = VoxelPath;
    TArray<FNavPathPoint>& Points = Path->GetPathPoints();
    VoxelPath->SegmentCosts.SetNumZeroed(Route.Num());
    for (int32 PointIndex = 0; PointIndex < Route.Num(); ++PointIndex)
    {
        if (PointIndex > 0)
        {
            VoxelPath->SegmentCosts[PointIndex] = FMath::Max(
                0.0f, Route[PointIndex].CumulativeCost - Route[PointIndex - 1].CumulativeCost);
        }
        Points[PointIndex].Flags = HellRunVoxelPath::MakeFlags(
            Route[PointIndex].Mode, VoxelPath->SegmentCosts[PointIndex]);
    }
    VoxelPath->TotalCost = Route.IsEmpty() ? 0.0f : Route.Last().CumulativeCost;
    Path->MarkReady();
    LastPathDiagnostic = FString::Printf(
        TEXT("COMPLETE | start %d C%d | goal %d C%d | expanded %d"),
        StartNode, Nodes[StartNode].ComponentId,
        GoalNode, Nodes[GoalNode].ComponentId, ExpandedNodes);
    return Path;
}

FNavPathSharedPtr AHellRunVoxelNavVolume::FindSharedPath(const ACharacter& Character, const FVector& Start, const FVector& Goal) const
{
    // Version 14 edges are authoritative. Until the reverse shared-flow cache is
    // converted to consume those exact directed edges, use the same typed A*
    // route rather than rebuilding a second, contradictory adjacency model.
    if (HasAuthoritativeTypedEdgeGraph())
    {
        return FindPath(Character, Start, Goal);
    }

    const UHellRunTraversalComponent* Traversal = Character.FindComponentByClass<UHellRunTraversalComponent>();
    const bool bWalk = !Traversal || Traversal->CanWalkNavigation();
    const bool bClimb = !Traversal || Traversal->CanClimbNavigation();
    const bool bWallClimb = Traversal && Traversal->CanWallClimbNavigation();
    const bool bFly = Traversal && Traversal->CanFlyNavigation();
    const bool bPreferFly = Traversal && Traversal->PrefersFlyingNavigation();
    const int32 StartNode = FindNearestNode(Start, bWalk, bWallClimb, bFly);
    const int32 GoalNode = FindNearestNode(Goal, bWalk, bWallClimb, bFly);
    if (StartNode == INDEX_NONE || GoalNode == INDEX_NONE)
    {
        return nullptr;
    }

    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
    const float AgentHalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : Settings->VoxelBakeAgentHalfHeight;
    const float GroundNodeZAdjustment = AgentHalfHeight - Settings->VoxelBakeAgentHalfHeight;
    auto GetAgentNodeLocation = [this, GroundNodeZAdjustment](int32 NodeIndex)
    {
        FVector Location = Nodes[NodeIndex].Location;
        if (Nodes[NodeIndex].bGround) Location.Z += GroundNodeZAdjustment;
        return Location;
    };
    FHellRunVoxelTraversalCostProfile Profile = Settings->GetVoxelCostProfileForCharacter(Character);
    if (Traversal && Traversal->bOverrideVoxelCostProfile)
    {
        Profile = Traversal->VoxelCostProfileOverride;
    }
    uint32 AgentSignature = bWalk ? 1u : 0u;
    AgentSignature = HashCombine(AgentSignature, bClimb ? 1u : 0u);
    AgentSignature = HashCombine(AgentSignature, bWallClimb ? 1u : 0u);
    AgentSignature = HashCombine(AgentSignature, bFly ? 1u : 0u);
    AgentSignature = HashCombine(AgentSignature, bPreferFly ? 1u : 0u);
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.WalkMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.ClimbMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.MantleMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.DropMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.JumpMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.VaultMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.FlightMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.LocomotionStateChangePenalty));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.ClimbLateralMultiplier));
    AgentSignature = HashCombine(AgentSignature, ::GetTypeHash(Profile.ClimbDownMultiplier));

    const uint64 FieldKey = (static_cast<uint64>(static_cast<uint32>(GoalNode)) << 32) | AgentSignature;
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
    FSharedFlowField* Field = SharedFlowFields.Find(FieldKey);
    if (!Field || Field->NavigationRevision != DynamicNavigationRevision || Field->ExpirationTime < Now)
    {
        FSharedFlowField NewField;
        NewField.GoalNode = GoalNode;
        NewField.AgentSignature = AgentSignature;
        NewField.NavigationRevision = DynamicNavigationRevision;
        NewField.ExpirationTime = Now + Settings->SharedFlowFieldLifetime;

        struct FFlowRecord
        {
            float Cost = BIG_NUMBER;
            bool bClosed = false;
        };
        TMap<int32, FFlowRecord> Records;
        TArray<int32> Open;
        auto Push = [&Open, &Records](int32 NodeIndex)
        {
            int32 Position = Open.Add(NodeIndex);
            while (Position > 0)
            {
                const int32 ParentPosition = (Position - 1) / 2;
                if (Records.FindChecked(Open[ParentPosition]).Cost <= Records.FindChecked(Open[Position]).Cost) break;
                Open.Swap(ParentPosition, Position);
                Position = ParentPosition;
            }
        };
        auto Pop = [&Open, &Records]()
        {
            const int32 Result = Open[0];
            Open[0] = Open.Last();
            Open.Pop(EAllowShrinking::No);
            int32 Position = 0;
            while (Open.IsValidIndex(Position))
            {
                const int32 Left = Position * 2 + 1;
                const int32 Right = Left + 1;
                if (!Open.IsValidIndex(Left)) break;
                int32 Best = Left;
                if (Open.IsValidIndex(Right)
                    && Records.FindChecked(Open[Right]).Cost < Records.FindChecked(Open[Left]).Cost) Best = Right;
                if (Records.FindChecked(Open[Position]).Cost <= Records.FindChecked(Open[Best]).Cost) break;
                Open.Swap(Position, Best);
                Position = Best;
            }
            return Result;
        };

        Records.Add(GoalNode).Cost = 0.0f;
        Push(GoalNode);
        int32 ExpandedNodes = 0;
        while (!Open.IsEmpty() && ExpandedNodes++ < Settings->MaximumSharedFlowFieldNodes)
        {
            const int32 CurrentNode = Pop();
            FFlowRecord& CurrentRecord = Records.FindChecked(CurrentNode);
            if (CurrentRecord.bClosed) continue;
            CurrentRecord.bClosed = true;
            const FHellRunBakedVoxelNode& To = Nodes[CurrentNode];
            const FIntVector ToCell = UnflattenCell(To.CellIndex);

            const int32 MaximumDropCells = FMath::Max(1,
                FMath::CeilToInt(Settings->Drop.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
            const int32 MaximumMantleCells = FMath::Max(1,
                FMath::CeilToInt(Settings->Mantle.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
            for (int32 DZ = -MaximumDropCells; DZ <= MaximumMantleCells; ++DZ)
            for (int32 DY = -1; DY <= 1; ++DY)
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                if (DX == 0 && DY == 0 && DZ == 0) continue;
                const FIntVector Delta(DX, DY, DZ);
                const int32 HorizontalSteps = FMath::Abs(DX) + FMath::Abs(DY);
                const bool bCardinalHorizontal = FMath::Abs(DZ) <= 1 && HorizontalSteps == 1;
                const bool bDiagonalHorizontal = FMath::Abs(DZ) <= 1 && FMath::Abs(DX) == 1 && FMath::Abs(DY) == 1;
                const bool bTraversalAcrossEdge = HorizontalSteps == 1;
                const int32 FromFlat = FlattenCell(ToCell - Delta);
                const int32 FromNode = CellToNode.IsValidIndex(FromFlat) ? CellToNode[FromFlat] : INDEX_NONE;
                if (!Nodes.IsValidIndex(FromNode)) continue;
                if (DynamicBlockedNodeRefCounts.Contains(FromNode)
                    || DynamicBlockedNodeRefCounts.Contains(CurrentNode)) continue;
                const FHellRunBakedVoxelNode& From = Nodes[FromNode];
                const float GroundHeightDelta = To.Location.Z - From.Location.Z;
                const uint8 ExitBit = CardinalDirectionBit(Delta.X, Delta.Y);
                const bool bContinuousGround = ExitBit != 0
                    && (From.GroundExitMask & ExitBit) != 0
                    && (To.GroundExitMask & OppositeCardinalDirectionBit(Delta.X, Delta.Y)) != 0;
                const float Distance = FVector(Delta).Size();
                EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
                float EdgeCost = BIG_NUMBER;
                auto Consider = [&Mode, &EdgeCost](EHellRunVoxelSegment CandidateMode, float CandidateCost)
                {
                    if (CandidateCost < EdgeCost)
                    {
                        Mode = CandidateMode;
                        EdgeCost = CandidateCost;
                    }
                };

                if (bWalk && From.bGround && To.bGround
                    && FMath::Abs(GroundHeightDelta) <= Settings->GroundStepHeight
                    && (bDiagonalHorizontal || bContinuousGround)
                    && (bCardinalHorizontal || (Settings->bAllowDiagonalVoxelWalk && bDiagonalHorizontal)))
                {
                    Consider(EHellRunVoxelSegment::Walk, Settings->VoxelWalkCost * Profile.WalkMultiplier * Distance);
                }
                if (bClimb && From.bGround && To.bGround && bCardinalHorizontal && !bContinuousGround
                    && FMath::Abs(GroundHeightDelta) <= Settings->GroundStepHeight)
                {
                    const bool bLowObstacle = ((From.ObstacleExitMask & ExitBit) != 0)
                        || ((To.ObstacleExitMask & OppositeCardinalDirectionBit(Delta.X, Delta.Y)) != 0);
                    if (bLowObstacle && Settings->Vault.bEnabled && BakedVoxelSize <= Settings->Vault.HorizontalReach
                        && FMath::Abs(GroundHeightDelta) <= Settings->Vault.EndpointHeightTolerance)
                    {
                        Consider(EHellRunVoxelSegment::Vault,
                            Settings->VaultAreaCost * Profile.VaultMultiplier * Distance);
                    }
                    else if (!bLowObstacle && Settings->Jump.bEnabled && BakedVoxelSize <= Settings->Jump.HorizontalReach
                        && FMath::Abs(GroundHeightDelta) <= Settings->Jump.EndpointHeightTolerance)
                    {
                        Consider(EHellRunVoxelSegment::Jump,
                            Settings->JumpAreaCost * Profile.JumpMultiplier * Distance);
                    }
                }
                if (bClimb && From.bGround && To.bGround
                    && GroundHeightDelta < -Settings->GroundStepHeight
                    && FMath::Abs(GroundHeightDelta) <= Settings->Drop.MaximumDepth && bTraversalAcrossEdge)
                {
                    Consider(EHellRunVoxelSegment::Drop,
                        Settings->DropAreaCost * Profile.DropMultiplier * Distance
                        + Profile.LocomotionStateChangePenalty);
                }
                else if (bClimb && From.bGround && To.bGround
                    && GroundHeightDelta > Settings->GroundStepHeight
                    && GroundHeightDelta <= Settings->Mantle.MaximumDepth && bTraversalAcrossEdge)
                {
                    Consider(EHellRunVoxelSegment::Mantle,
                        Settings->VoxelMantleCost * Profile.MantleMultiplier * Distance
                        + Profile.LocomotionStateChangePenalty);
                }
                else if (bWallClimb && !From.bGround && From.bClimb && To.bGround && FMath::Abs(Delta.Z) <= 1)
                {
                    Consider(EHellRunVoxelSegment::Mantle,
                        Settings->VoxelMantleCost * Profile.MantleMultiplier * Distance
                        + Profile.LocomotionStateChangePenalty);
                }
                else if (bWallClimb && From.bGround && !To.bGround && To.bClimb && FMath::Abs(Delta.Z) <= 1)
                {
                    Consider(EHellRunVoxelSegment::Climb,
                        Settings->VoxelClimbCost * Profile.ClimbMultiplier * Distance
                        + Profile.LocomotionStateChangePenalty);
                }
                else if (bWallClimb && From.bClimb && To.bClimb && FMath::Abs(Delta.Z) <= 1
                    && FVector::DotProduct(From.WallNormal, To.WallNormal) >= 0.25f)
                {
                    const float Horizontal = FVector2D(static_cast<double>(Delta.X), static_cast<double>(Delta.Y)).Size();
                    const float Vertical = FMath::Abs(static_cast<float>(Delta.Z));
                    const float LateralAlpha = Horizontal / FMath::Max(Horizontal + Vertical, UE_SMALL_NUMBER);
                    const float DirectionMultiplier = FMath::Lerp(1.0f, Profile.ClimbLateralMultiplier, LateralAlpha)
                        * (Delta.Z < 0 ? Profile.ClimbDownMultiplier : 1.0f);
                    Consider(EHellRunVoxelSegment::Climb,
                        Settings->VoxelClimbCost * Profile.ClimbMultiplier * DirectionMultiplier * Distance);
                }
                if (bFly && FMath::Abs(Delta.Z) <= 1)
                {
                    const float GroundPreference = bPreferFly ? 1.0f : 1.2f;
                    Consider(EHellRunVoxelSegment::Fly,
                        Settings->VoxelFlightCost * Profile.FlightMultiplier * GroundPreference * Distance);
                }
                if (EdgeCost == BIG_NUMBER) continue;
                if (!IsTraversalEdgeClear(Character, From.Location, To.Location, Mode, *Settings)) continue;

                FFlowRecord& FromRecord = Records.FindOrAdd(FromNode);
                if (FromRecord.bClosed) continue;
                const float NewCost = CurrentRecord.Cost + EdgeCost;
                bool bPreferCandidate = NewCost < FromRecord.Cost - KINDA_SMALL_NUMBER;
                if (!bPreferCandidate && FMath::IsNearlyEqual(NewCost, FromRecord.Cost, KINDA_SMALL_NUMBER))
                {
                    const FVector DesiredDirection = (Nodes[GoalNode].Location - From.Location).GetSafeNormal();
                    const float CandidateAlignment = FVector::DotProduct((To.Location - From.Location).GetSafeNormal(), DesiredDirection);
                    const int32* ExistingNext = NewField.NextNode.Find(FromNode);
                    const float ExistingAlignment = ExistingNext && Nodes.IsValidIndex(*ExistingNext)
                        ? FVector::DotProduct((Nodes[*ExistingNext].Location - From.Location).GetSafeNormal(), DesiredDirection)
                        : -1.0f;
                    bPreferCandidate = CandidateAlignment > ExistingAlignment;
                }
                if (bPreferCandidate)
                {
                    FromRecord.Cost = NewCost;
                    NewField.NextNode.Add(FromNode, CurrentNode);
                    NewField.NextMode.Add(FromNode, Mode);
                    Push(FromNode);
                }
            }

            // Reverse expansion of the same straight jump/vault spans used by A*.
            if (bClimb && To.bGround)
            {
                const FIntVector Directions[] = {
                    FIntVector(1,0,0), FIntVector(-1,0,0), FIntVector(0,1,0), FIntVector(0,-1,0),
                    FIntVector(1,1,0), FIntVector(1,-1,0), FIntVector(-1,1,0), FIntVector(-1,-1,0)
                };
                const int32 MaximumDropSpanCells = FMath::Max(1, FMath::CeilToInt(
                    Settings->Drop.HorizontalReach / FMath::Max(BakedVoxelSize, 1.0f)));
                const int32 MaximumDropDepthCells = FMath::Max(1, FMath::CeilToInt(
                    Settings->Drop.MaximumDepth / FMath::Max(BakedVoxelSize, 1.0f)));
                for (const FIntVector& Direction : Directions)
                for (int32 SpanCells = 1; SpanCells <= MaximumDropSpanCells; ++SpanCells)
                for (int32 LandingDZ = -1; LandingDZ >= -MaximumDropDepthCells; --LandingDZ)
                {
                    const FIntVector FromCell = ToCell - Direction * SpanCells - FIntVector(0, 0, LandingDZ);
                    const int32 FromFlat = FlattenCell(FromCell);
                    const int32 FromNode = CellToNode.IsValidIndex(FromFlat) ? CellToNode[FromFlat] : INDEX_NONE;
                    if (!Nodes.IsValidIndex(FromNode) || !Nodes[FromNode].bGround
                        || DynamicBlockedNodeRefCounts.Contains(FromNode)) continue;
                    const FHellRunBakedVoxelNode& From = Nodes[FromNode];
                    const float HeightDelta = To.Location.Z - From.Location.Z;
                    if (HeightDelta >= -Settings->GroundStepHeight
                        || FMath::Abs(HeightDelta) > Settings->Drop.MaximumDepth) continue;

                    const uint8 DirectionBit = CardinalDirectionBit(Direction.X, Direction.Y);
                    const int32 FirstFlat = FlattenCell(FromCell + Direction);
                    const int32 FirstNode = CellToNode.IsValidIndex(FirstFlat) ? CellToNode[FirstFlat] : INDEX_NONE;
                    const bool bLeavesGround = (From.GroundExitMask & DirectionBit) == 0
                        || !Nodes.IsValidIndex(FirstNode) || !Nodes[FirstNode].bGround
                        || (SpanCells == 1
                            && (To.GroundExitMask & OppositeCardinalDirectionBit(Direction.X, Direction.Y)) == 0);
                    if (!bLeavesGround) continue;

                    const float HorizontalTime = FVector::Dist2D(From.Location, To.Location)
                        / FMath::Max(1.0f, Settings->DropMinimumHorizontalSpeed);
                    const float FallTime = FMath::Sqrt(2.0f * FMath::Abs(HeightDelta) / 980.0f);
                    const float EdgeCost = FMath::Max(HorizontalTime, FallTime) * 7.0f
                        * Settings->DropAreaCost * Profile.DropMultiplier + Profile.LocomotionStateChangePenalty;
                    FFlowRecord& FromRecord = Records.FindOrAdd(FromNode);
                    if (FromRecord.bClosed) continue;
                    const float NewCost = CurrentRecord.Cost + EdgeCost;
                    if (NewCost < FromRecord.Cost)
                    {
                        FromRecord.Cost = NewCost;
                        NewField.NextNode.Add(FromNode, CurrentNode);
                        NewField.NextMode.Add(FromNode, EHellRunVoxelSegment::Drop);
                        Push(FromNode);
                    }
                }

                const int32 MaximumSpanCells = FMath::Max(2, FMath::CeilToInt(
                    FMath::Max(Settings->Jump.HorizontalReach, Settings->Vault.HorizontalReach)
                    / FMath::Max(BakedVoxelSize, 1.0f)));
                for (int32 OffsetX = -MaximumSpanCells; OffsetX <= MaximumSpanCells; ++OffsetX)
                for (int32 OffsetY = -MaximumSpanCells; OffsetY <= MaximumSpanCells; ++OffsetY)
                for (int32 LandingDZ = -1; LandingDZ <= 1; ++LandingDZ)
                {
                    const FIntVector SpanOffset(OffsetX, OffsetY, 0);
                    const int32 SpanCells = FMath::Max(FMath::Abs(OffsetX), FMath::Abs(OffsetY));
                    const float SpanDistance = FVector(SpanOffset).Size() * BakedVoxelSize;
                    if (SpanCells < 2
                        || SpanDistance > FMath::Max(Settings->Jump.HorizontalReach, Settings->Vault.HorizontalReach))
                    {
                        continue;
                    }
                    const FIntVector FromCell = ToCell - SpanOffset - FIntVector(0, 0, LandingDZ);
                    const int32 FromFlat = FlattenCell(FromCell);
                    const int32 FromNode = CellToNode.IsValidIndex(FromFlat) ? CellToNode[FromFlat] : INDEX_NONE;
                    if (!Nodes.IsValidIndex(FromNode) || !Nodes[FromNode].bGround
                        || DynamicBlockedNodeRefCounts.Contains(FromNode)) continue;

                    bool bAllGap = true;
                    bool bAllLowObstacle = true;
                    for (int32 Step = 1; Step < SpanCells; ++Step)
                    {
                        const float Alpha = static_cast<float>(Step) / SpanCells;
                        const FIntVector MiddleCell = FromCell + FIntVector(
                            FMath::RoundToInt(OffsetX * Alpha), FMath::RoundToInt(OffsetY * Alpha), 0);
                        const int32 MiddleFlat = FlattenCell(MiddleCell);
                        const int32 MiddleNode = CellToNode.IsValidIndex(MiddleFlat) ? CellToNode[MiddleFlat] : INDEX_NONE;
                        bAllGap &= Nodes.IsValidIndex(MiddleNode) && !Nodes[MiddleNode].bGround;
                        const int32 AboveFlat = FlattenCell(MiddleCell + FIntVector(0, 0, 1));
                        const int32 AboveNode = CellToNode.IsValidIndex(AboveFlat) ? CellToNode[AboveFlat] : INDEX_NONE;
                        bAllLowObstacle &= !Nodes.IsValidIndex(MiddleNode) && Nodes.IsValidIndex(AboveNode);
                    }
                    const FHellRunBakedVoxelNode& From = Nodes[FromNode];
                    const float HeightDelta = To.Location.Z - From.Location.Z;
                    EHellRunVoxelSegment SpanMode;
                    float BaseCost;
                    const bool bCardinalSpan = OffsetX == 0 || OffsetY == 0;
                    if (bAllGap && Settings->Jump.bEnabled && SpanDistance <= Settings->Jump.HorizontalReach
                        && FMath::Abs(HeightDelta) <= Settings->Jump.EndpointHeightTolerance)
                    {
                        SpanMode = EHellRunVoxelSegment::Jump;
                        BaseCost = Settings->JumpAreaCost * Profile.JumpMultiplier;
                    }
                    else if (bCardinalSpan && bAllLowObstacle && Settings->Vault.bEnabled
                        && SpanDistance <= Settings->Vault.HorizontalReach
                        && FMath::Abs(HeightDelta) <= Settings->Vault.EndpointHeightTolerance)
                    {
                        SpanMode = EHellRunVoxelSegment::Vault;
                        BaseCost = Settings->VaultAreaCost * Profile.VaultMultiplier;
                    }
                    else continue;
                    if (!IsTraversalEdgeClear(Character, From.Location, To.Location, SpanMode, *Settings)) continue;

                    const float EdgeCost = BaseCost * FVector::Distance(From.Location, To.Location)
                        / FMath::Max(BakedVoxelSize, 1.0f) + Profile.LocomotionStateChangePenalty;
                    FFlowRecord& FromRecord = Records.FindOrAdd(FromNode);
                    if (FromRecord.bClosed) continue;
                    const float NewCost = CurrentRecord.Cost + EdgeCost;
                    if (NewCost < FromRecord.Cost)
                    {
                        FromRecord.Cost = NewCost;
                        NewField.NextNode.Add(FromNode, CurrentNode);
                        NewField.NextMode.Add(FromNode, SpanMode);
                        Push(FromNode);
                    }
                }
            }
        }

        SharedFlowFields.Add(FieldKey, MoveTemp(NewField));
        Field = SharedFlowFields.Find(FieldKey);
        if (SharedFlowFields.Num() > 24)
        {
            for (auto It = SharedFlowFields.CreateIterator(); It; ++It)
            {
                if (It.Value().ExpirationTime < Now && It.Key() != FieldKey) It.RemoveCurrent();
            }
        }
    }

    if (!Field || (StartNode != GoalNode && !Field->NextNode.Contains(StartNode)))
    {
        return nullptr;
    }

    TArray<FVector> Locations;
    TArray<EHellRunVoxelSegment> Modes;
    Locations.Add(Start);
    Modes.Add(EHellRunVoxelSegment::Walk);
    if (!Start.Equals(GetAgentNodeLocation(StartNode), 1.0f))
    {
        Locations.Add(GetAgentNodeLocation(StartNode));
        Modes.Add(bWalk && Nodes[StartNode].bGround ? EHellRunVoxelSegment::Walk
            : (bFly ? EHellRunVoxelSegment::Fly : EHellRunVoxelSegment::Climb));
    }
    int32 CurrentNode = StartNode;
    int32 Guard = 0;
    while (CurrentNode != GoalNode && Guard++ < Nodes.Num())
    {
        const int32* NextNode = Field->NextNode.Find(CurrentNode);
        const EHellRunVoxelSegment* NextMode = Field->NextMode.Find(CurrentNode);
        if (!NextNode || !NextMode || !Nodes.IsValidIndex(*NextNode)) return nullptr;
        Locations.Add(GetAgentNodeLocation(*NextNode));
        Modes.Add(*NextMode);
        CurrentNode = *NextNode;
    }
    if (CurrentNode != GoalNode) return nullptr;

    if (!Locations.Last().Equals(Goal, 1.0f))
    {
        Locations.Add(Goal);
        Modes.Add(bWalk && Nodes[GoalNode].bGround ? EHellRunVoxelSegment::Walk
            : (bFly ? EHellRunVoxelSegment::Fly : EHellRunVoxelSegment::Climb));
    }

    // The shared field stores exact graph successors, but path following must
    // consume corridors rather than treating every voxel center as a terminal
    // movement goal. Collapse only collinear edges with the same typed mode.
    // This preserves the selected topology and every traversal boundary while
    // preventing braking at each cell in a straight walk run.
    TArray<FVector> CorridorLocations;
    TArray<EHellRunVoxelSegment> CorridorModes;
    CorridorLocations.Reserve(Locations.Num());
    CorridorModes.Reserve(Modes.Num());
    CorridorLocations.Add(Locations[0]);
    CorridorModes.Add(Modes[0]);
    for (int32 PointIndex = 1; PointIndex < Locations.Num() - 1; ++PointIndex)
    {
        const FVector Incoming = (Locations[PointIndex] - CorridorLocations.Last()).GetSafeNormal();
        const FVector Outgoing = (Locations[PointIndex + 1] - Locations[PointIndex]).GetSafeNormal();
        const bool bSameMovementMode = Modes[PointIndex] == Modes[PointIndex + 1];
        const bool bContinuousLocomotion =
            Modes[PointIndex] == EHellRunVoxelSegment::Walk
            || Modes[PointIndex] == EHellRunVoxelSegment::Climb
            || Modes[PointIndex] == EHellRunVoxelSegment::Fly;
        const bool bCollinear = FVector::DotProduct(Incoming, Outgoing) > 0.9999f;
        if (!bContinuousLocomotion || !bSameMovementMode || !bCollinear)
        {
            CorridorLocations.Add(Locations[PointIndex]);
            CorridorModes.Add(Modes[PointIndex]);
        }
    }
    CorridorLocations.Add(Locations.Last());
    CorridorModes.Add(Modes.Last());

    FNavPathSharedPtr Path = MakeShared<FNavigationPath>(CorridorLocations, nullptr);
    TArray<FNavPathPoint>& Points = Path->GetPathPoints();
    for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex)
    {
        Points[PointIndex].Flags = HellRunVoxelPath::MakeFlags(CorridorModes[PointIndex]);
    }
    Path->MarkReady();
    return Path;
}

void AHellRunVoxelNavVolume::InvalidateDynamicNavigation()
{
    ++DynamicNavigationRevision;
    SharedFlowFields.Reset();
}

void AHellRunVoxelNavVolume::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bDrawBakedGraph) DrawBakedGraph();
}

void AHellRunVoxelNavVolume::DrawBakedGraph() const
{
    UWorld* World = GetWorld(); if (!World) return;
    const int32 Count = FMath::Min(MaximumDebugNodes, Nodes.Num());
    for (int32 I=0; I<Count; ++I)
    {
        const FHellRunBakedVoxelNode& Node = Nodes[I];
        const FColor Color = Node.bGround ? FColor::Cyan : (Node.bClimb ? FColor(145,70,255) : FColor::Blue);
        DrawDebugPoint(World, Node.Location, 5.0f, Color, false, 0.0f, 5);
    }
    if (!bDrawBakedTraversalEdges) return;
    if (!bDebugTraversalEdgeCacheBuilt)
    {
        DebugTraversalFromNodeIndices.Reset();
        DebugTraversalEdgeIndices.Reset();
        for (int32 FromIndex = 0;
            FromIndex < Nodes.Num();
            ++FromIndex)
        {
            const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
            for (int32 EdgeIndex = From.FirstEdge;
                EdgeIndex < From.FirstEdge + From.EdgeCount;
                ++EdgeIndex)
            {
                if (Edges.IsValidIndex(EdgeIndex)
                    && IsDiscreteTraversal(
                        Edges[EdgeIndex].Mode))
                {
                    DebugTraversalFromNodeIndices.Add(FromIndex);
                    DebugTraversalEdgeIndices.Add(EdgeIndex);
                }
            }
        }
        bDebugTraversalEdgeCacheBuilt = true;
    }

    const int32 DrawCount = FMath::Min3(
        MaximumDebugTraversalEdges,
        DebugTraversalFromNodeIndices.Num(),
        DebugTraversalEdgeIndices.Num());
    for (int32 DebugIndex = 0;
        DebugIndex < DrawCount;
        ++DebugIndex)
    {
        const int32 FromIndex =
            DebugTraversalFromNodeIndices[DebugIndex];
        const int32 EdgeIndex =
            DebugTraversalEdgeIndices[DebugIndex];
        if (!Nodes.IsValidIndex(FromIndex)
            || !Edges.IsValidIndex(EdgeIndex))
        {
            continue;
        }
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        const FHellRunBakedVoxelEdge& Edge = Edges[EdgeIndex];
        if (!Nodes.IsValidIndex(Edge.ToNode)) continue;
        const FColor Color =
            Edge.Mode == EHellRunVoxelSegment::Jump
                ? FColor(40, 140, 255)
                : Edge.Mode == EHellRunVoxelSegment::Vault
                    ? FColor::Green
                    : Edge.Mode == EHellRunVoxelSegment::Mantle
                        ? FColor(220, 0, 255)
                        : FColor::Orange;
        DrawDebugDirectionalArrow(
            World,
            From.Location,
            Nodes[Edge.ToNode].Location,
            20.0f,
            Color,
            false,
            0.0f,
            4,
            2.0f);
    }
}

void AHellRunVoxelNavVolume::BuildNavigationDataV2()
{
    UWorld* World = GetWorld();
    if (!World) return;

    Modify();
    LastBakeDisconnectedGroundCullCount = 0;
    const UHellRunTraversalNavigationSettings* S =
        GetDefault<UHellRunTraversalNavigationSettings>();
    GridOrigin = GetComponentsBoundingBox(true).Min;
    BakedBounds = GetComponentsBoundingBox(true);
    BakedVoxelSize = FMath::Max(25.0f, S->VoxelSize);
    const FVector GridSize = BakedBounds.GetSize();
    GridDimensions = FIntVector(
        FMath::Max(1, FMath::CeilToInt(GridSize.X / BakedVoxelSize)),
        FMath::Max(1, FMath::CeilToInt(GridSize.Y / BakedVoxelSize)),
        FMath::Max(1, FMath::CeilToInt(GridSize.Z / BakedVoxelSize)));
    const int64 CellCount64 = static_cast<int64>(GridDimensions.X)
        * GridDimensions.Y * GridDimensions.Z;
    if (CellCount64 <= 0 || CellCount64 > MAX_int32
        || CellCount64 > S->MaximumVoxelBakeCells)
    {
        UE_LOG(LogTemp, Error,
            TEXT("VOXEL21_BUILD_FAILED | cells=%lld limit=%d"),
            CellCount64, S->MaximumVoxelBakeCells);
        ClearNavigationData();
        return;
    }

#if WITH_EDITOR
    FNotificationInfo NotificationInfo(FText::Format(
        NSLOCTEXT(
            "HellRunTraversalNavigation",
            "Voxel24BakeStarted",
            "Building voxel navigation: {0} ({1} x {2} x {3}, {4} cells at {5} cm)"),
        FText::FromString(GetActorLabel()),
        FText::AsNumber(GridDimensions.X),
        FText::AsNumber(GridDimensions.Y),
        FText::AsNumber(GridDimensions.Z),
        FText::AsNumber(CellCount64),
        FText::AsNumber(BakedVoxelSize)));
    NotificationInfo.bFireAndForget = false;
    NotificationInfo.bUseThrobber = true;
    const TSharedPtr<SNotificationItem> BuildNotification =
        FSlateNotificationManager::Get().AddNotification(NotificationInfo);
    FScopedSlowTask BuildProgress(
        100.0f,
        FText::Format(
            NSLOCTEXT(
                "HellRunTraversalNavigation",
                "Voxel24BakeProgress",
                "Building {0}: {1} candidate cells ({2} x {3} x {4})"),
            FText::FromString(GetActorLabel()),
            FText::AsNumber(CellCount64),
            FText::AsNumber(GridDimensions.X),
            FText::AsNumber(GridDimensions.Y),
            FText::AsNumber(GridDimensions.Z)));
    BuildProgress.MakeDialogDelayed(0.5f, false);
    const float SamplingProgressPerCell =
        55.0f / static_cast<float>(CellCount64);
#endif

    Nodes.Reset();
    Edges.Reset();
    DebugTraversalFromNodeIndices.Reset();
    DebugTraversalEdgeIndices.Reset();
    bDebugTraversalEdgeCacheBuilt = false;
    CellToNode.Init(INDEX_NONE, static_cast<int32>(CellCount64));
    SharedFlowFields.Reset();
    DynamicObstacleNodes.Reset();
    DynamicBlockedNodeRefCounts.Reset();
    SpatialOctree.Reset();
    bSpatialOctreeBuilt = false;
    ++DynamicNavigationRevision;

    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunVoxel20Bake), false, this);
    const float Radius = S->VoxelBakeAgentRadius;
    const float HalfHeight = S->VoxelBakeAgentHalfHeight;
    const float CenterToFloor = HalfHeight + S->VoxelGroundClearance;
    const FVector WallDirections[] = {
        FVector::ForwardVector, FVector::BackwardVector,
        FVector::RightVector, FVector::LeftVector
    };

    // Modeling and semantic segmentation are separate passes conceptually, but
    // share the same collision sample here. Every node is a collision-free
    // regular-grid occupancy cell; Ground and Climb are semantic labels on it.
    for (int32 Z = 0; Z < GridDimensions.Z; ++Z)
    for (int32 Y = 0; Y < GridDimensions.Y; ++Y)
    for (int32 X = 0; X < GridDimensions.X; ++X)
    {
#if WITH_EDITOR
        BuildProgress.EnterProgressFrame(
            SamplingProgressPerCell,
            NSLOCTEXT(
                "HellRunTraversalNavigation",
                "Voxel24BakeSampling",
                "Sampling collision-free cells and supported surfaces"));
#endif
        const FIntVector Cell(X, Y, Z);
        const int32 Flat = FlattenCell(Cell);
        const FVector Center = GridOrigin
            + FVector(X + 0.5f, Y + 0.5f, Z + 0.5f) * BakedVoxelSize;
        if (!EncompassesPoint(Center))
        {
            continue;
        }

        FHellRunBakedVoxelNode Node;
        Node.Location = Center;
        Node.CellIndex = Flat;
        const bool bGridCenterClear = !CapsuleOverlapsTopology(
            *World, Center, Objects, Params, Radius, HalfHeight);

        const float FloorProbe = CenterToFloor + BakedVoxelSize * 0.75f;
        // A single fixed XY sample cannot represent interiors whose usable
        // clearance is smaller than a voxel. Doorways and narrow corridors
        // can lie completely between grid centers. Keep one canonical node
        // per cell, but choose its supported location from clearance-aware
        // subcell samples.
        const float SubcellOffset = BakedVoxelSize * 0.35f;
        const FVector2D GroundSampleOffsets[] = {
            FVector2D::ZeroVector,
            FVector2D(SubcellOffset, 0.0f),
            FVector2D(-SubcellOffset, 0.0f),
            FVector2D(0.0f, SubcellOffset),
            FVector2D(0.0f, -SubcellOffset),
            FVector2D(SubcellOffset, SubcellOffset),
            FVector2D(SubcellOffset, -SubcellOffset),
            FVector2D(-SubcellOffset, SubcellOffset),
            FVector2D(-SubcellOffset, -SubcellOffset)
        };
        for (const FVector2D& Offset : GroundSampleOffsets)
        {
            const FVector SampleCenter(
                Center.X + Offset.X,
                Center.Y + Offset.Y,
                Center.Z);
            if (!EncompassesPoint(SampleCenter))
            {
                continue;
            }
            FHitResult FloorHit;
            if (!TraceTopology(
                    *World,
                    FloorHit,
                    SampleCenter,
                    SampleCenter - FVector::UpVector * FloorProbe,
                    Objects,
                    Params)
                || FloorHit.ImpactNormal.Z < 0.55f)
            {
                continue;
            }
            const FVector SupportedCenter(
                SampleCenter.X,
                SampleCenter.Y,
                FloorHit.ImpactPoint.Z + CenterToFloor);
            // Exactly one regular Z layer should represent a given support
            // surface. This avoids aliases whose grid index and position refer
            // to different layers.
            if (FMath::Abs(SupportedCenter.Z - Center.Z)
                    <= BakedVoxelSize * 0.5f
                && !CapsuleOverlapsTopology(
                    *World, SupportedCenter, Objects, Params, Radius, HalfHeight))
            {
                Node.bGround = true;
                Node.Location = SupportedCenter;
                break;
            }
        }
        // A lattice center below the canonical supported capsule center can
        // overlap the floor even though its projection is valid. Reject the
        // raw center only after giving surface projection a chance to replace
        // it; otherwise entire floors disappear based on grid Z phase.
        if (!Node.bGround && !bGridCenterClear)
        {
            continue;
        }

        float BestWallDistance = BIG_NUMBER;
        for (const FVector& Direction : WallDirections)
        {
            FHitResult WallHit;
            if (TraceTopology(*World, WallHit, Node.Location,
                    Node.Location + Direction
                        * (Radius + S->ClimbSurfaceProbeDistance),
                    Objects, Params)
                && FMath::Abs(WallHit.ImpactNormal.Z) < 0.35f
                && (!WallHit.GetActor()
                    || !WallHit.GetActor()->ActorHasTag(S->NoClimbActorTag))
                && WallHit.Distance < BestWallDistance)
            {
                BestWallDistance = WallHit.Distance;
                Node.bClimb = true;
                Node.WallNormal = WallHit.ImpactNormal.GetSafeNormal2D();
            }
        }

        CellToNode[Flat] = Nodes.Add(Node);
    }

    TArray<TArray<FHellRunBakedVoxelEdge>> Adjacency;
    Adjacency.SetNum(Nodes.Num());
    auto AddEdge = [&Adjacency](
        int32 From, int32 To, EHellRunVoxelSegment Mode, float Cost)
    {
        if (From == To || !Adjacency.IsValidIndex(From)
            || !Adjacency.IsValidIndex(To) || Cost < 0.0f) return;
        TArray<FHellRunBakedVoxelEdge>& Outgoing = Adjacency[From];
        if (FHellRunBakedVoxelEdge* Existing = Outgoing.FindByPredicate(
            [To, Mode](const FHellRunBakedVoxelEdge& Edge)
            {
                return Edge.ToNode == To && Edge.Mode == Mode;
            }))
        {
            Existing->BaseCost = FMath::Min(Existing->BaseCost, Cost);
            return;
        }
        FHellRunBakedVoxelEdge& Edge = Outgoing.AddDefaulted_GetRef();
        Edge.ToNode = To;
        Edge.Mode = Mode;
        Edge.BaseCost = Cost;
    };
    const FCollisionShape AgentShape =
        FCollisionShape::MakeCapsule(Radius, HalfHeight);
    auto CapsuleSegmentClear = [
        World, &Objects, &Params, &AgentShape](const FVector& A, const FVector& B)
    {
        return SweepTopologyClear(
            *World, A, B, Objects, Params, AgentShape);
    };
    auto ArcClear = [&CapsuleSegmentClear](
        const FVector& From, const FVector& To, float ArcHeight)
    {
        FVector Previous = From;
        constexpr int32 Samples = 10;
        for (int32 Sample = 1; Sample <= Samples; ++Sample)
        {
            const float Alpha = static_cast<float>(Sample) / Samples;
            FVector Point = FMath::Lerp(From, To, Alpha);
            Point.Z += FMath::Sin(Alpha * UE_PI) * ArcHeight;
            if (!CapsuleSegmentClear(Previous, Point)) return false;
            Previous = Point;
        }
        return true;
    };
    auto SurfaceSupported = [
        World, &Objects, &Params, S, HalfHeight](
        const FVector& From, const FVector& To)
    {
        constexpr int32 Samples = 6;
        for (int32 Sample = 1; Sample < Samples; ++Sample)
        {
            const float Alpha = static_cast<float>(Sample) / Samples;
            const FVector ExpectedCenter = FMath::Lerp(From, To, Alpha);
            const FVector ExpectedFloor = ExpectedCenter
                - FVector::UpVector
                    * (HalfHeight + S->VoxelGroundClearance);
            FHitResult Hit;
            const float Probe = FMath::Max(
                S->GroundStepHeight, S->VoxelFloorProbeDepth);
            if (!TraceTopology(*World, Hit,
                    ExpectedFloor + FVector::UpVector * S->GroundStepHeight,
                    ExpectedFloor - FVector::UpVector * Probe,
                    Objects, Params)
                || Hit.ImpactNormal.Z < 0.55f
                || FMath::Abs(Hit.ImpactPoint.Z - ExpectedFloor.Z)
                    > S->GroundStepHeight)
            {
                return false;
            }
        }
        return true;
    };

    // Preserve the exact collision-tested 26-neighbor flight topology in one
    // compact bit mask per node. Destinations and costs are deterministic from
    // the regular grid, so explicit reflected flight edge structs are redundant.
    FlightNeighborMasks.Init(0u, Nodes.Num());
    int32 ImplicitFlightEdgeCount = 0;
#if WITH_EDITOR
    const float FreeTopologyProgressPerNode =
        15.0f / static_cast<float>(FMath::Max(1, Nodes.Num()));
#endif
    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
#if WITH_EDITOR
        BuildProgress.EnterProgressFrame(
            FreeTopologyProgressPerNode,
            FText::Format(
                NSLOCTEXT(
                    "HellRunTraversalNavigation",
                    "Voxel24BakeFreeTopology",
                    "Building free-space topology ({0} navigable nodes)"),
                FText::AsNumber(Nodes.Num())));
#endif
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        int32 FlightNeighborBit = 0;
        for (int32 DZ = -1; DZ <= 1; ++DZ)
        for (int32 DY = -1; DY <= 1; ++DY)
        for (int32 DX = -1; DX <= 1; ++DX)
        {
            if (DX == 0 && DY == 0 && DZ == 0) continue;
            const int32 NeighborBit = FlightNeighborBit++;
            const int32 Flat = FlattenCell(
                FromCell + FIntVector(DX, DY, DZ));
            const int32 ToIndex =
                CellToNode.IsValidIndex(Flat) ? CellToNode[Flat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(ToIndex)) continue;
            const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
            const float DistanceVoxels = FVector::Distance(
                From.Location, To.Location) / BakedVoxelSize;
            const bool bSegmentClear =
                CapsuleSegmentClear(From.Location, To.Location);
            if (bSegmentClear)
            {
                FlightNeighborMasks[FromIndex] |= (1u << NeighborBit);
                ++ImplicitFlightEdgeCount;
            }
            if (From.bClimb && To.bClimb
                && (!From.bGround || !To.bGround)
                && FVector::DotProduct(From.WallNormal, To.WallNormal) >= 0.25f
                && bSegmentClear)
            {
                AddEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Climb,
                    S->VoxelClimbCost * DistanceVoxels);
            }
            if (From.bGround && !To.bGround && To.bClimb
                && FMath::Max(
                    0.0f,
                    FVector::Dist2D(From.Location, To.Location)
                        - BakedVoxelSize)
                    <= S->Climb.HorizontalReach)
            {
                // Enter the wall field from a supported approach cell. Ground
                // nodes need not themselves be wall-labelled; requiring that
                // label disconnected otherwise valid climb columns. Do not
                // free-space sweep this edge: its destination intentionally
                // sits beside the supporting wall, which can be reported as
                // terminal contact even though both endpoint capsules are
                // independently baked clear.
                AddEdge(FromIndex, ToIndex, EHellRunVoxelSegment::Climb,
                    S->VoxelClimbCost * DistanceVoxels);
            }
            if (!From.bGround && From.bClimb && To.bGround
                && To.Location.Z - From.Location.Z
                    >= -S->GroundStepHeight
                && To.Location.Z - From.Location.Z
                    <= S->Mantle.MaximumDepth
                && FMath::Max(
                    0.0f,
                    FVector::Dist2D(From.Location, To.Location)
                        - BakedVoxelSize)
                    <= S->Mantle.HorizontalReach)
            {
                // Leave the wall field through the same backed-off lift and
                // pull-over corridor used by regular mantle edges.
                const FVector MantleForward =
                    (To.Location - From.Location).GetSafeNormal2D();
                const FVector LiftBase = From.Location - MantleForward
                    * FMath::Max(S->Mantle.DistanceFromEdge, 12.0f);
                const FVector LiftTop(
                    LiftBase.X,
                    LiftBase.Y,
                    To.Location.Z + S->PullOverHeight);
                if (CapsuleSegmentClear(From.Location, LiftBase)
                    && CapsuleSegmentClear(LiftBase, LiftTop)
                    && CapsuleSegmentClear(LiftTop, To.Location))
                {
                    AddEdge(FromIndex, ToIndex,
                        EHellRunVoxelSegment::Mantle,
                        S->VoxelMantleCost * DistanceVoxels);
                }
            }
        }
    }

    const FIntPoint TravelDirections[] = {
        FIntPoint(1,0), FIntPoint(-1,0),
        FIntPoint(0,1), FIntPoint(0,-1),
        FIntPoint(1,1), FIntPoint(1,-1),
        FIntPoint(-1,1), FIntPoint(-1,-1)
    };

    auto GroundNodeAtCell = [this](const FIntVector& Cell) -> int32
    {
        const int32 Flat = FlattenCell(Cell);
        if (!CellToNode.IsValidIndex(Flat)) return INDEX_NONE;
        const int32 NodeIndex = CellToNode[Flat];
        return Nodes.IsValidIndex(NodeIndex) && Nodes[NodeIndex].bGround
            ? NodeIndex : static_cast<int32>(INDEX_NONE);
    };
    auto FindGroundAt = [
        this, &GroundNodeAtCell](
        int32 X, int32 Y, int32 CenterZ, int32 VerticalCells,
        const FVector& Reference, TFunctionRef<bool(const FHellRunBakedVoxelNode&)> Accept)
    {
        int32 Best = INDEX_NONE;
        float BestDistance = BIG_NUMBER;
        for (int32 DZ = -VerticalCells; DZ <= VerticalCells; ++DZ)
        {
            const int32 Candidate = GroundNodeAtCell(
                FIntVector(X, Y, CenterZ + DZ));
            if (!Nodes.IsValidIndex(Candidate) || !Accept(Nodes[Candidate]))
            {
                continue;
            }
            const float Distance = FVector::DistSquared(
                Reference, Nodes[Candidate].Location);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Best = Candidate;
            }
        }
        return Best;
    };
    auto HasLowObstacle = [
        this, World, &Objects, &Params, HalfHeight, S](
        const FVector& From, const FVector& To)
    {
        const FVector Direction = (To - From).GetSafeNormal2D();
        if (Direction.IsNearlyZero()) return false;
        const float LowerProbeHeight = FMath::Clamp(
            S->VaultMinimumArcHeight * 0.45f, 25.0f, HalfHeight * 0.8f);
        const FVector FromFloor = From - FVector::UpVector
            * (HalfHeight + S->VoxelGroundClearance);
        const FVector ToFloor = To - FVector::UpVector
            * (HalfHeight + S->VoxelGroundClearance);
        FHitResult LowerHit;
        if (!TraceTopology(*World, LowerHit,
                FromFloor + FVector::UpVector * LowerProbeHeight,
                ToFloor + FVector::UpVector * LowerProbeHeight,
                Objects, Params)
            || FMath::Abs(LowerHit.ImpactNormal.Z) > 0.45f)
        {
            return false;
        }

        // A vault obstacle must end before the landing. A wall or a long
        // occupied run is not a vaultable semantic feature.
        FHitResult ReverseHit;
        if (!TraceTopology(*World, ReverseHit,
                ToFloor + FVector::UpVector * LowerProbeHeight,
                FromFloor + FVector::UpVector * LowerProbeHeight,
                Objects, Params)
            || ReverseHit.GetActor() != LowerHit.GetActor())
        {
            return false;
        }
        const float ObstacleWidth = FVector::Dist2D(
            LowerHit.ImpactPoint, ReverseHit.ImpactPoint);
        return ObstacleWidth <= FMath::Max(
            BakedVoxelSize * 1.5f, S->Vault.MaximumDepth);
    };
    int32 ReverseDropRoutesAttempted = 0;
    int32 ReverseDropRoutesNoWallNodes = 0;
    int32 ReverseDropRoutesNoEndpointPair = 0;
    int32 ReverseDropRoutesAdded = 0;
    int32 ReverseDropVaultsAdded = 0;
    auto AddReverseWallRouteForDrop = [
        this, S, &AddEdge, &ReverseDropRoutesAttempted,
        &ReverseDropRoutesNoWallNodes, &ReverseDropRoutesNoEndpointPair,
        &ReverseDropRoutesAdded, &ReverseDropVaultsAdded](
        int32 LowGroundIndex,
        int32 HighGroundIndex)
    {
        ++ReverseDropRoutesAttempted;
        if (!Nodes.IsValidIndex(LowGroundIndex)
            || !Nodes.IsValidIndex(HighGroundIndex))
        {
            return;
        }
        const FHellRunBakedVoxelNode& Low =
            Nodes[LowGroundIndex];
        const FHellRunBakedVoxelNode& High =
            Nodes[HighGroundIndex];
        const FVector WallDirection =
            (High.Location - Low.Location).GetSafeNormal2D();
        if (WallDirection.IsNearlyZero()) return;

        const FIntVector LowCell =
            UnflattenCell(Low.CellIndex);
        const FIntVector HighCell =
            UnflattenCell(High.CellIndex);
        const int32 MinimumX =
            FMath::Max(0, FMath::Min(LowCell.X, HighCell.X) - 1);
        const int32 MaximumX = FMath::Min(
            GridDimensions.X - 1,
            FMath::Max(LowCell.X, HighCell.X) + 1);
        const int32 MinimumY =
            FMath::Max(0, FMath::Min(LowCell.Y, HighCell.Y) - 1);
        const int32 MaximumY = FMath::Min(
            GridDimensions.Y - 1,
            FMath::Max(LowCell.Y, HighCell.Y) + 1);
        const int32 MinimumZ =
            FMath::Max(0, FMath::Min(LowCell.Z, HighCell.Z) - 1);
        const int32 MaximumZ = FMath::Min(
            GridDimensions.Z - 1,
            FMath::Max(LowCell.Z, HighCell.Z) + 1);

        TArray<int32> WallNodes;
        const FVector FlatLow(
            Low.Location.X, Low.Location.Y, 0.0f);
        const FVector FlatHigh(
            High.Location.X, High.Location.Y, 0.0f);
        for (int32 Z = MinimumZ; Z <= MaximumZ; ++Z)
        for (int32 Y = MinimumY; Y <= MaximumY; ++Y)
        for (int32 X = MinimumX; X <= MaximumX; ++X)
        {
            const int32 Flat = FlattenCell(
                FIntVector(X, Y, Z));
            const int32 NodeIndex =
                CellToNode.IsValidIndex(Flat)
                    ? CellToNode[Flat] : INDEX_NONE;
            if (!Nodes.IsValidIndex(NodeIndex)
                || Nodes[NodeIndex].bGround
                || !Nodes[NodeIndex].bClimb)
            {
                continue;
            }
            const FHellRunBakedVoxelNode& Candidate =
                Nodes[NodeIndex];
            const FVector FlatCandidate(
                Candidate.Location.X,
                Candidate.Location.Y,
                0.0f);
            if (FMath::PointDistToSegment(
                    FlatCandidate,
                    FlatLow,
                    FlatHigh)
                    > BakedVoxelSize * 0.8f
                || FMath::Abs(FVector::DotProduct(
                    Candidate.WallNormal,
                    WallDirection))
                    < 0.35f)
            {
                continue;
            }
            WallNodes.AddUnique(NodeIndex);
        }
        if (WallNodes.IsEmpty())
        {
            // A short clear ledge without a climbable wall is an authored
            // vault-style traversal, not a climb through open air. The
            // corresponding high-to-low drop already proved the corridor
            // clear. Keep this deliberately capped at three voxels; taller
            // drops remain one-way unless a real climb column was baked.
            const float Rise = High.Location.Z - Low.Location.Z;
            const float Horizontal = FVector::Dist2D(
                Low.Location, High.Location);
            if (Rise > S->GroundStepHeight
                && Rise <= BakedVoxelSize * 3.0f
                && Horizontal <= S->Vault.HorizontalReach)
            {
                AddEdge(
                    LowGroundIndex,
                    HighGroundIndex,
                    EHellRunVoxelSegment::Vault,
                    S->VaultAreaCost * FVector::Distance(
                        Low.Location, High.Location)
                        / BakedVoxelSize);
                ++ReverseDropVaultsAdded;
            }
            ++ReverseDropRoutesNoWallNodes;
            return;
        }

        // A corridor can contain a wall column extending below the lower
        // landing. Selecting the globally lowest node rejects an otherwise
        // valid climb. Anchor each end to the wall node nearest its landing,
        // then require both nodes to belong to the same local wall face.
        int32 BottomNode = INDEX_NONE;
        float BestBottomDistance = BIG_NUMBER;
        for (const int32 Candidate : WallNodes)
        {
            const float HeightDelta = FMath::Abs(
                Nodes[Candidate].Location.Z - Low.Location.Z);
            const float Distance = FVector::Distance(
                Nodes[Candidate].Location, Low.Location);
            if (HeightDelta <= BakedVoxelSize * 1.5f
                && Distance < BestBottomDistance)
            {
                BottomNode = Candidate;
                BestBottomDistance = Distance;
            }
        }
        int32 TopNode = INDEX_NONE;
        float BestTopDistance = BIG_NUMBER;
        if (Nodes.IsValidIndex(BottomNode))
        {
            for (const int32 Candidate : WallNodes)
            {
                const float HeightDelta = FMath::Abs(
                    Nodes[Candidate].Location.Z - High.Location.Z);
                const float Distance = FVector::Distance(
                    Nodes[Candidate].Location, High.Location);
                if (HeightDelta <= BakedVoxelSize * 1.5f
                    && FVector::Dist2D(
                        Nodes[Candidate].Location,
                        Nodes[BottomNode].Location)
                        <= BakedVoxelSize * 1.5f
                    && FVector::DotProduct(
                        Nodes[Candidate].WallNormal,
                        Nodes[BottomNode].WallNormal)
                        >= 0.25f
                    && Distance < BestTopDistance)
                {
                    TopNode = Candidate;
                    BestTopDistance = Distance;
                }
            }
        }
        if (!Nodes.IsValidIndex(BottomNode)
            || !Nodes.IsValidIndex(TopNode))
        {
            ++ReverseDropRoutesNoEndpointPair;
            return;
        }

        AddEdge(
            LowGroundIndex,
            BottomNode,
            EHellRunVoxelSegment::Climb,
            S->VoxelClimbCost * FVector::Distance(
                Low.Location,
                Nodes[BottomNode].Location)
                / BakedVoxelSize);
        AddEdge(
            TopNode,
            HighGroundIndex,
            EHellRunVoxelSegment::Mantle,
            S->VoxelMantleCost * FVector::Distance(
                Nodes[TopNode].Location,
                High.Location)
                / BakedVoxelSize);
        ++ReverseDropRoutesAdded;
    };

    // Ground topology is built from adjacent 3D cells. Traversal portals are
    // emitted only when that adjacency is absent in a specific travel
    // direction; no arbitrary XY endpoint pairing participates in semantics.
    const float MaximumReach = FMath::Max(
        FMath::Max(S->Jump.HorizontalReach, S->Vault.HorizontalReach),
        FMath::Max(S->Mantle.HorizontalReach, S->Drop.HorizontalReach));
    const int32 Span = FMath::Max(
        1, FMath::CeilToInt(MaximumReach / BakedVoxelSize));
#if WITH_EDITOR
    const float SurfaceTopologyProgressPerNode =
        25.0f / static_cast<float>(FMath::Max(1, Nodes.Num()));
#endif
    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
#if WITH_EDITOR
        BuildProgress.EnterProgressFrame(
            SurfaceTopologyProgressPerNode,
            FText::Format(
                NSLOCTEXT(
                    "HellRunTraversalNavigation",
                    "Voxel24BakeSurfaceTopology",
                    "Classifying walk and traversal edges ({0} navigable nodes)"),
                FText::AsNumber(Nodes.Num())));
#endif
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        if (!From.bGround) continue;
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        for (const FIntPoint& Direction : TravelDirections)
        {
            const bool bDiagonal = Direction.X != 0 && Direction.Y != 0;
            if (bDiagonal && !S->bAllowDiagonalVoxelWalk) continue;

            const int32 WalkVerticalCells = FMath::Max(
                1, FMath::CeilToInt(S->GroundStepHeight / BakedVoxelSize));
            const int32 WalkNode = FindGroundAt(
                FromCell.X + Direction.X,
                FromCell.Y + Direction.Y,
                FromCell.Z,
                WalkVerticalCells,
                From.Location,
                [&](const FHellRunBakedVoxelNode& Candidate)
                {
                    return FMath::Abs(
                            Candidate.Location.Z - From.Location.Z)
                            <= S->GroundStepHeight
                        && SurfaceSupported(
                            From.Location, Candidate.Location)
                        && CapsuleSegmentClear(
                            From.Location, Candidate.Location);
                });
            if (Nodes.IsValidIndex(WalkNode))
            {
                AddEdge(FromIndex, WalkNode, EHellRunVoxelSegment::Walk,
                    S->VoxelWalkCost * FVector::Distance(
                        From.Location, Nodes[WalkNode].Location)
                        / BakedVoxelSize);
                continue;
            }

            // This is a true surface-graph boundary in this direction. Search
            // outward along the same 3D lattice ray for the first physically
            // reachable supported landing and classify the intervening space.
            for (int32 DistanceCells = 1;
                DistanceCells <= Span; ++DistanceCells)
            {
                const int32 X = FromCell.X + Direction.X * DistanceCells;
                const int32 Y = FromCell.Y + Direction.Y * DistanceCells;
                if (X < 0 || Y < 0
                    || X >= GridDimensions.X || Y >= GridDimensions.Y) break;

                const float MaximumVerticalReach = FMath::Max(
                    FMath::Max(S->Mantle.MaximumDepth, S->Drop.MaximumDepth),
                    FMath::Max(
                        S->Jump.EndpointHeightTolerance,
                        S->Vault.EndpointHeightTolerance));
                const int32 VerticalCells = FMath::Max(1, FMath::CeilToInt(
                    MaximumVerticalReach
                    / BakedVoxelSize));
                const int32 ToIndex = FindGroundAt(
                    X, Y, FromCell.Z, VerticalCells, From.Location,
                    [](const FHellRunBakedVoxelNode&) { return true; });
                if (!Nodes.IsValidIndex(ToIndex)) continue;
                const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
                const float Horizontal =
                    FVector::Dist2D(From.Location, To.Location);
                if (Horizontal > MaximumReach) break;
                const float DeltaZ = To.Location.Z - From.Location.Z;
                const float DistanceVoxels = FVector::Distance(
                    From.Location, To.Location) / BakedVoxelSize;
                const bool bSupportContinuous =
                    SurfaceSupported(From.Location, To.Location);
                bool bAdded = false;

                if (DeltaZ > S->GroundStepHeight
                    && DeltaZ <= S->Mantle.MaximumDepth
                    && Horizontal <= S->Mantle.HorizontalReach)
                {
                    const FVector TravelDirection =
                        (To.Location - From.Location).GetSafeNormal2D();
                    const FVector LiftBase = From.Location - TravelDirection
                        * FMath::Max(S->Mantle.DistanceFromEdge, 12.0f);
                    const FVector LiftTop(
                        LiftBase.X, LiftBase.Y,
                        To.Location.Z + S->PullOverHeight);
                    if (CapsuleSegmentClear(From.Location, LiftBase)
                        && CapsuleSegmentClear(LiftBase, LiftTop)
                        && CapsuleSegmentClear(LiftTop, To.Location))
                    {
                        AddEdge(FromIndex, ToIndex,
                            EHellRunVoxelSegment::Mantle,
                            S->VoxelMantleCost * DistanceVoxels);
                        bAdded = true;

                        // A ledge boundary is directed, but its two directions
                        // are separate locomotion actions. Once the lower-to-
                        // upper mantle is established, independently validate
                        // the upper-to-lower drop so raised surface islands do
                        // not become one-way traps.
                        if (DeltaZ <= S->Drop.MaximumDepth
                            && Horizontal <= S->Drop.HorizontalReach)
                        {
                            const float DropArc = FMath::Max(
                                FMath::Abs(DeltaZ) * 0.6f
                                    + S->VoxelGroundClearance,
                                S->DropTakeoffForwardDistance * 0.35f);
                            if (ArcClear(
                                    To.Location,
                                    From.Location,
                                    DropArc))
                            {
                                AddEdge(
                                    ToIndex,
                                    FromIndex,
                                    EHellRunVoxelSegment::Drop,
                                    S->DropAreaCost * DistanceVoxels);
                            }
                        }
                    }
                }
                else if (DeltaZ < -S->GroundStepHeight
                    && -DeltaZ <= S->Drop.MaximumDepth
                    && Horizontal <= S->Drop.HorizontalReach)
                {
                    const float Arc = FMath::Max(
                        FMath::Abs(DeltaZ) * 0.6f
                            + S->VoxelGroundClearance,
                        S->DropTakeoffForwardDistance * 0.35f);
                    if (ArcClear(From.Location, To.Location, Arc))
                    {
                        AddEdge(FromIndex, ToIndex,
                            EHellRunVoxelSegment::Drop,
                            S->DropAreaCost * DistanceVoxels);
                        AddReverseWallRouteForDrop(
                            ToIndex,
                            FromIndex);
                        bAdded = true;
                    }
                }
                else if (!bSupportContinuous
                    && FMath::Abs(DeltaZ)
                        <= S->Vault.EndpointHeightTolerance
                    && Horizontal <= S->Vault.HorizontalReach
                    && HasLowObstacle(From.Location, To.Location))
                {
                    const float Arc = FMath::Max(
                        S->VaultMinimumArcHeight,
                        FMath::Abs(DeltaZ) + 70.0f);
                    if (ArcClear(From.Location, To.Location, Arc))
                    {
                        AddEdge(FromIndex, ToIndex,
                            EHellRunVoxelSegment::Vault,
                            S->VaultAreaCost * DistanceVoxels);
                        bAdded = true;
                    }
                }
                else if (!bSupportContinuous
                    && FMath::Abs(DeltaZ)
                        <= S->Jump.EndpointHeightTolerance
                    && Horizontal <= S->Jump.HorizontalReach)
                {
                    const float Arc = FMath::Max(
                        S->JumpMinimumArcHeight,
                        FMath::Abs(DeltaZ) + 70.0f);
                    if (ArcClear(From.Location, To.Location, Arc))
                    {
                        AddEdge(FromIndex, ToIndex,
                            EHellRunVoxelSegment::Jump,
                            S->JumpAreaCost * DistanceVoxels);
                        bAdded = true;
                    }
                }
                // Only the nearest landing in this lattice direction defines
                // the boundary transition. Looking through it would create
                // non-local shortcuts that no longer represent voxel topology.
                if (bAdded || bSupportContinuous) break;
            }
        }
    }

    auto HasShortWalkConnection = [
        this, &Adjacency, ShortLimit = FMath::Max(64, Span * 8)](
        int32 StartNode,
        int32 GoalNode)
    {
        if (StartNode == GoalNode) return true;
        struct FWalkVisit
        {
            int32 Node = INDEX_NONE;
            int32 Depth = 0;
        };
        TArray<FWalkVisit> Queue;
        TSet<int32> Visited;
        Queue.Add({StartNode, 0});
        Visited.Add(StartNode);
        for (int32 QueueIndex = 0;
            QueueIndex < Queue.Num();
            ++QueueIndex)
        {
            const FWalkVisit Visit = Queue[QueueIndex];
            if (Visit.Depth >= ShortLimit
                || !Adjacency.IsValidIndex(Visit.Node))
            {
                continue;
            }
            for (const FHellRunBakedVoxelEdge& Edge :
                Adjacency[Visit.Node])
            {
                if (Edge.Mode != EHellRunVoxelSegment::Walk
                    || Visited.Contains(Edge.ToNode))
                {
                    continue;
                }
                if (Edge.ToNode == GoalNode) return true;
                Visited.Add(Edge.ToNode);
                Queue.Add({Edge.ToNode, Visit.Depth + 1});
            }
        }
        return false;
    };

    // Short raised ledges are not guaranteed to lie on one of the eight
    // cardinal/diagonal lattice rays above. Search the complete local vault
    // footprint so off-axis landings such as (2,3) cells are represented.
    // This remains a local semantic portal: both endpoints must be supported,
    // the rise is capped at three voxels, and the baked-agent capsule arc must
    // be clear. It cannot introduce a long arbitrary surface shortcut.
    const int32 ShortLedgeVaultSpan = FMath::Max(
        1,
        FMath::CeilToInt(
            S->Vault.HorizontalReach
            / FMath::Max(BakedVoxelSize, 1.0f)));
    const int32 ShortLedgeVerticalCells = FMath::Max(
        1,
        FMath::CeilToInt(
            BakedVoxelSize * 3.0f
            / FMath::Max(BakedVoxelSize, 1.0f)));
    for (int32 FromIndex = 0; FromIndex < Nodes.Num(); ++FromIndex)
    {
        const FHellRunBakedVoxelNode& From = Nodes[FromIndex];
        if (!From.bGround) continue;
        const FIntVector FromCell = UnflattenCell(From.CellIndex);
        for (int32 OffsetY = -ShortLedgeVaultSpan;
            OffsetY <= ShortLedgeVaultSpan;
            ++OffsetY)
        for (int32 OffsetX = -ShortLedgeVaultSpan;
            OffsetX <= ShortLedgeVaultSpan;
            ++OffsetX)
        {
            if (OffsetX == 0 && OffsetY == 0) continue;
            const float LatticeHorizontal = FVector2D(
                static_cast<double>(OffsetX),
                static_cast<double>(OffsetY)).Size()
                * BakedVoxelSize;
            if (LatticeHorizontal > S->Vault.HorizontalReach
                + KINDA_SMALL_NUMBER)
            {
                continue;
            }
            const int32 ToIndex = FindGroundAt(
                FromCell.X + OffsetX,
                FromCell.Y + OffsetY,
                FromCell.Z,
                ShortLedgeVerticalCells,
                From.Location,
                [](const FHellRunBakedVoxelNode&) { return true; });
            if (!Nodes.IsValidIndex(ToIndex)
                || ToIndex == FromIndex)
            {
                continue;
            }
            const FHellRunBakedVoxelNode& To = Nodes[ToIndex];
            const float Rise = To.Location.Z - From.Location.Z;
            const float Horizontal =
                FVector::Dist2D(From.Location, To.Location);
            const bool bImmediateNeighbor =
                FMath::Max(
                    FMath::Abs(OffsetX),
                    FMath::Abs(OffsetY)) == 1;
            if (bImmediateNeighbor
                && Rise <= Horizontal * 0.8f
                && SurfaceSupported(From.Location, To.Location)
                && CapsuleSegmentClear(From.Location, To.Location))
            {
                AddEdge(
                    FromIndex,
                    ToIndex,
                    EHellRunVoxelSegment::Walk,
                    S->VoxelWalkCost * FVector::Distance(
                        From.Location, To.Location)
                        / BakedVoxelSize);
                continue;
            }
            if (Rise <= S->GroundStepHeight
                || Rise > BakedVoxelSize * 3.0f
                || Horizontal > S->Vault.HorizontalReach
                || HasShortWalkConnection(FromIndex, ToIndex))
            {
                continue;
            }
            const float Arc = FMath::Max(
                S->VaultMinimumArcHeight,
                Rise + 70.0f);
            const bool bVaultArcClear =
                ArcClear(From.Location, To.Location, Arc);
            if (!bVaultArcClear)
            {
                continue;
            }
            const float DistanceVoxels = FVector::Distance(
                From.Location, To.Location)
                / BakedVoxelSize;
            AddEdge(
                FromIndex,
                ToIndex,
                EHellRunVoxelSegment::Vault,
                S->VaultAreaCost * DistanceVoxels);

            // Preserve the opposite one-way action where its independent drop
            // constraints permit it.
            if (Rise <= S->Drop.MaximumDepth
                && Horizontal <= S->Drop.HorizontalReach)
            {
                const float DropArc = FMath::Max(
                    Rise * 0.6f + S->VoxelGroundClearance,
                    S->DropTakeoffForwardDistance * 0.35f);
                if (ArcClear(To.Location, From.Location, DropArc))
                {
                    AddEdge(
                        ToIndex,
                        FromIndex,
                        EHellRunVoxelSegment::Drop,
                        S->DropAreaCost * DistanceVoxels);
                }
            }
        }
    }

    // Traversal is a topological fallback, not a shortcut across a surface
    // that the character can already walk. The directional classifier above
    // cannot see an around-the-corner walk route while inspecting one ray, so
    // remove locally redundant jump/vault portals after the complete walk
    // graph exists. This also prevents voxel sampling noise on ramps and broad
    // platforms from turning an ordinary walk section into an animation.
    for (int32 FromIndex = 0; FromIndex < Adjacency.Num(); ++FromIndex)
    {
        TArray<int32> RedundantTraversalEdges;
        for (int32 EdgeIndex = 0;
            EdgeIndex < Adjacency[FromIndex].Num();
            ++EdgeIndex)
        {
            const FHellRunBakedVoxelEdge& Edge =
                Adjacency[FromIndex][EdgeIndex];
            if ((Edge.Mode == EHellRunVoxelSegment::Jump
                    || Edge.Mode == EHellRunVoxelSegment::Vault)
                && HasShortWalkConnection(FromIndex, Edge.ToNode))
            {
                RedundantTraversalEdges.Add(EdgeIndex);
            }
        }
        for (int32 Index = RedundantTraversalEdges.Num() - 1;
            Index >= 0;
            --Index)
        {
            Adjacency[FromIndex].RemoveAt(
                RedundantTraversalEdges[Index],
                1,
                EAllowShrinking::No);
        }
    }

    // Build weak ground components using exactly the traversal skills shared by
    // grounded agents. A floor island is valid only when walk/jump/vault/
    // mantle/drop edges connect it to the main ground surface. Climb and flight
    // cannot make an otherwise trapped grounded spawn island valid.
    TArray<int32> GroundParent;
    GroundParent.SetNumUninitialized(Nodes.Num());
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        GroundParent[Index] = Index;
    }
    auto FindGroundRoot = [&GroundParent](int32 Node)
    {
        int32 Root = Node;
        while (GroundParent[Root] != Root) Root = GroundParent[Root];
        while (GroundParent[Node] != Node)
        {
            const int32 Next = GroundParent[Node];
            GroundParent[Node] = Root;
            Node = Next;
        }
        return Root;
    };
    auto IsGroundedTraversal = [](EHellRunVoxelSegment Mode)
    {
        return Mode == EHellRunVoxelSegment::Walk
            || Mode == EHellRunVoxelSegment::Jump
            || Mode == EHellRunVoxelSegment::Vault
            || Mode == EHellRunVoxelSegment::Mantle
            || Mode == EHellRunVoxelSegment::Drop;
    };
    for (int32 FromIndex = 0; FromIndex < Adjacency.Num(); ++FromIndex)
    {
        if (!Nodes[FromIndex].bGround) continue;
        for (const FHellRunBakedVoxelEdge& Edge : Adjacency[FromIndex])
        {
            if (!Nodes.IsValidIndex(Edge.ToNode)
                || !Nodes[Edge.ToNode].bGround
                || !IsGroundedTraversal(Edge.Mode))
            {
                continue;
            }
            const int32 A = FindGroundRoot(FromIndex);
            const int32 B = FindGroundRoot(Edge.ToNode);
            if (A != B) GroundParent[B] = A;
        }
    }
    TMap<int32, int32> GroundComponentSizes;
    int32 PrimaryGroundRoot = INDEX_NONE;
    int32 PrimaryGroundSize = 0;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        if (!Nodes[Index].bGround) continue;
        const int32 Root = FindGroundRoot(Index);
        const int32 Size = ++GroundComponentSizes.FindOrAdd(Root);
        if (Size > PrimaryGroundSize)
        {
            PrimaryGroundRoot = Root;
            PrimaryGroundSize = Size;
        }
    }
    int32 DisconnectedGroundNodesCulled = 0;
    int32 DisconnectedGroundIslandsCulled = FMath::Max(
        0, GroundComponentSizes.Num() - (PrimaryGroundRoot != INDEX_NONE ? 1 : 0));
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        if (Nodes[Index].bGround
            && FindGroundRoot(Index) != PrimaryGroundRoot)
        {
            Nodes[Index].bGround = false;
            Nodes[Index].GroundExitMask = 0;
            Nodes[Index].ObstacleExitMask = 0;
            ++DisconnectedGroundNodesCulled;
        }
    }
    LastBakeDisconnectedGroundCullCount = DisconnectedGroundNodesCulled;
    TMap<int32, int32> GroundComponentIds;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        if (!Nodes[Index].bGround) continue;
        const int32 Root = FindGroundRoot(Index);
        Nodes[Index].ComponentId = GroundComponentIds.FindOrAdd(
            Root, GroundComponentIds.Num());
    }

    Edges.Reset();
    int32 ModeCounts[7] = {};
    ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)] =
        ImplicitFlightEdgeCount;
    int32 GroundCount = 0;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        FHellRunBakedVoxelNode& Node = Nodes[Index];
        if (Node.bGround) ++GroundCount;
        Node.FirstEdge = Edges.Num();
        Node.EdgeCount = Adjacency[Index].Num();
        for (const FHellRunBakedVoxelEdge& Edge : Adjacency[Index])
        {
            const int32 Mode = static_cast<int32>(Edge.Mode);
            if (Mode >= 0 && Mode < UE_ARRAY_COUNT(ModeCounts))
            {
                ++ModeCounts[Mode];
            }
            Edges.Add(Edge);
        }
    }
    // Version 33 removes ground islands that have no grounded-agent traversal
    // chain to the main navigation surface and forces Build Paths to replace
    // earlier experimental version-31 data. This connectivity is never rebuilt
    // at runtime.
    BakedGraphVersion = CurrentBakedGraphVersion;
    RebuildRuntimeSearchCostCache();
#if WITH_EDITOR
    BuildProgress.EnterProgressFrame(
        // The per-cell updates can accumulate several units of float error on
        // large campaign volumes. Finalization only updates the status text;
        // charging more work here can overrun FScopedSlowTask's total.
        0.0f,
        FText::Format(
            NSLOCTEXT(
                "HellRunTraversalNavigation",
                "Voxel24BakeFinalize",
                "Finalizing {0} nodes and {1} directed edges"),
            FText::AsNumber(Nodes.Num()),
            FText::AsNumber(Edges.Num())));
#endif
    MarkPackageDirty();
    UE_LOG(LogTemp, Display,
        TEXT("VOXEL24_BUILD | cells=%d free=%d ground=%d edges=%d ")
        TEXT("walk=%d climb=%d mantle=%d drop=%d fly=%d jump=%d vault=%d ")
        TEXT("disconnectedGroundCulled=%d disconnectedIslandsCulled=%d ")
        TEXT("reverseDrop=[climb:%d vault:%d attempted:%d noWall:%d noPair:%d]"),
        CellToNode.Num(), Nodes.Num(), GroundCount, Edges.Num(),
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Walk)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Climb)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Mantle)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Drop)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Jump)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Vault)],
        DisconnectedGroundNodesCulled, DisconnectedGroundIslandsCulled,
        ReverseDropRoutesAdded, ReverseDropVaultsAdded,
        ReverseDropRoutesAttempted,
        ReverseDropRoutesNoWallNodes, ReverseDropRoutesNoEndpointPair);
#if WITH_EDITOR
    if (BuildNotification.IsValid())
    {
        BuildNotification->SetText(FText::Format(
            NSLOCTEXT(
                "HellRunTraversalNavigation",
                "Voxel24BakeComplete",
                "Voxel navigation built: {0} nodes ({1} ground), {2} edges "
                "[walk {3}, climb {4}, mantle {5}, drop {6}, fly {7}, jump {8}, vault {9}]"),
            FText::AsNumber(Nodes.Num()),
            FText::AsNumber(GroundCount),
            FText::AsNumber(Edges.Num()),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Walk)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Climb)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Mantle)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Drop)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Jump)]),
            FText::AsNumber(ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Vault)])));
        BuildNotification->SetCompletionState(SNotificationItem::CS_Success);
        BuildNotification->ExpireAndFadeout();
    }
#endif
}

FNavPathSharedPtr AHellRunVoxelNavVolume::FindPathV2(
    const ACharacter& Character,
    const FVector& Start,
    const FVector& Goal) const
{
    QUICK_SCOPE_CYCLE_COUNTER(STAT_HellRunVoxelFindPathV2);
    LastPathDiagnostic.Reset();
    if (!HasAuthoritativeTypedEdgeGraph()) return nullptr;
    const UHellRunTraversalComponent* Traversal =
        Character.FindComponentByClass<UHellRunTraversalComponent>();
    const bool bWalk = !Traversal || Traversal->CanWalkNavigation();
    const bool bClimb = Traversal && Traversal->CanWallClimbNavigation();
    const bool bMantle = !Traversal || Traversal->CanMantleNavigation();
    const bool bDrop = !Traversal || Traversal->CanDropNavigation();
    const bool bJump = !Traversal || Traversal->CanJumpNavigation();
    const bool bVault = !Traversal || Traversal->CanVaultNavigation();
    const bool bFly = Traversal && Traversal->CanFlyNavigation();
    const UHellRunTraversalNavigationSettings* S =
        GetDefault<UHellRunTraversalNavigationSettings>();

    UWorld* QueryWorld = GetWorld();
    FCollisionObjectQueryParams EndpointObjects;
    EndpointObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    EndpointObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams EndpointParams(
        SCENE_QUERY_STAT(HellRunVoxelEndpointConnector),
        false,
        &Character);
    EndpointParams.AddIgnoredActor(this);
    if (QueryWorld)
    {
        for (TActorIterator<APawn> It(QueryWorld); It; ++It)
        {
            EndpointParams.AddIgnoredActor(*It);
        }
    }
    const UCapsuleComponent* QueryCapsule =
        Character.GetCapsuleComponent();
    const float QueryAgentRadius = QueryCapsule
        ? QueryCapsule->GetScaledCapsuleRadius()
        : S->VoxelBakeAgentRadius;
    const float QueryAgentHalfHeight = QueryCapsule
        ? QueryCapsule->GetScaledCapsuleHalfHeight()
        : S->VoxelBakeAgentHalfHeight;
    const FCollisionShape EndpointShape = FCollisionShape::MakeCapsule(
        FMath::Max(1.0f, QueryAgentRadius - 2.0f),
        FMath::Max(
            QueryAgentRadius,
            QueryAgentHalfHeight - FMath::Max(
                8.0f,
                S->VoxelGroundClearance + 4.0f)));
    auto EndpointConnectorClear = [
        QueryWorld,
        S,
        this,
        &EndpointObjects,
        &EndpointParams,
        &EndpointShape,
        QueryAgentHalfHeight,
        bFly](
            const FVector& Position,
            const FHellRunBakedVoxelNode& Node)
    {
        if (!QueryWorld)
        {
            return false;
        }
        if (bFly || !Node.bGround)
        {
            return !QueryWorld->SweepTestByChannel(
                Position,
                Node.Location,
                FQuat::Identity,
                ECC_Pawn,
                EndpointShape,
                EndpointParams);
        }

        const FVector RuntimeNodeCenter =
            Node.Location
            + FVector::UpVector
                * (QueryAgentHalfHeight
                    - S->VoxelBakeAgentHalfHeight);
        const FVector ConnectorStart = Position;
        const float HorizontalDistance =
            FVector::Dist2D(ConnectorStart, RuntimeNodeCenter);
        const int32 SampleCount = FMath::Max(
            2,
            FMath::CeilToInt(
                HorizontalDistance
                / FMath::Max(10.0f, BakedVoxelSize * 0.25f)));
        FVector PreviousCenter = ConnectorStart;
        for (int32 Sample = 1; Sample <= SampleCount; ++Sample)
        {
            const float Alpha =
                static_cast<float>(Sample) / SampleCount;
            const FVector ExpectedCenter =
                FMath::Lerp(
                    ConnectorStart,
                    RuntimeNodeCenter,
                    Alpha);
            const FVector ExpectedFloor = ExpectedCenter
                - FVector::UpVector
                    * (QueryAgentHalfHeight
                        + S->VoxelGroundClearance);
            FHitResult FloorHit;
            const float ProbeHeight = FMath::Max(
                S->GroundStepHeight,
                S->VoxelFloorProbeDepth);
            if (!TraceTopology(
                    *QueryWorld,
                    FloorHit,
                    ExpectedFloor
                        + FVector::UpVector * S->GroundStepHeight,
                    ExpectedFloor
                        - FVector::UpVector * ProbeHeight,
                    EndpointObjects,
                    EndpointParams)
                || FloorHit.ImpactNormal.Z < 0.55f
                || FMath::Abs(
                    FloorHit.ImpactPoint.Z - ExpectedFloor.Z)
                    > S->GroundStepHeight)
            {
                return false;
            }
            if (QueryWorld->SweepTestByChannel(
                    PreviousCenter,
                    ExpectedCenter,
                    FQuat::Identity,
                    ECC_Pawn,
                    EndpointShape,
                    EndpointParams))
            {
                return false;
            }
            PreviousCenter = ExpectedCenter;
        }
        return true;
    };

    auto CollectAnchors = [
        this,
        bWalk,
        bClimb,
        bFly,
        S,
        &EndpointConnectorClear](
        const FVector& Position)
    {
        TArray<int32> Result;
        const float EndpointRadius = FMath::Max(
            BakedVoxelSize * 1.75f,
            S->VoxelEndpointConnectionRadius);
        const FVector EndpointExtent(
            EndpointRadius,
            EndpointRadius,
            bFly ? EndpointRadius : S->VoxelSearchPadding);
        TArray<int32> NearbyNodes;
        EnsureSpatialOctree();
        QuerySpatialOctree(
            FBox(Position - EndpointExtent, Position + EndpointExtent),
            NearbyNodes);

        // The expanded endpoint radius can contain thousands of baked nodes.
        // Connector validation performs multiple capsule sweeps per node, so
        // validating the entire octree result made a single zombie path request
        // stall the game thread for 200+ ms. Apply cheap eligibility/distance
        // tests first, then validate only the closest useful candidates.
        NearbyNodes.RemoveAll([
            this, Position, EndpointRadius, bWalk, bClimb, bFly, S](int32 Index)
        {
            if (!Nodes.IsValidIndex(Index)
                || DynamicBlockedNodeRefCounts.Contains(Index))
            {
                return true;
            }
            const FHellRunBakedVoxelNode& Node = Nodes[Index];
            if (!(bFly || (bWalk && Node.bGround)
                    || (bClimb && Node.bClimb)))
            {
                return true;
            }
            const float Horizontal = FVector::Dist2D(
                Position, Node.Location);
            const float Vertical = FMath::Abs(
                Position.Z - Node.Location.Z);
            return bFly
                ? FVector::Distance(Position, Node.Location) > EndpointRadius
                : Horizontal > EndpointRadius
                    || Vertical > S->VoxelSearchPadding;
        });
        NearbyNodes.Sort([this, Position](int32 A, int32 B)
        {
            return FVector::DistSquared(Position, Nodes[A].Location)
                < FVector::DistSquared(Position, Nodes[B].Location);
        });
        constexpr int32 MaximumConnectorCandidates = 32;
        if (NearbyNodes.Num() > MaximumConnectorCandidates)
        {
            NearbyNodes.SetNum(
                MaximumConnectorCandidates, EAllowShrinking::No);
        }

        for (const int32 Index : NearbyNodes)
        {
            const FHellRunBakedVoxelNode& Node = Nodes[Index];
            const float Horizontal = FVector::Dist2D(
                Position, Node.Location);
            const float Vertical = FMath::Abs(
                Position.Z - Node.Location.Z);
            const bool bAlreadyAtBakedNode =
                Horizontal <= BakedVoxelSize * 0.75f
                && Vertical <= FMath::Max(
                    BakedVoxelSize * 0.75f,
                    S->GroundStepHeight);
            if (!bAlreadyAtBakedNode
                && !EndpointConnectorClear(Position, Node)) continue;
            Result.AddUnique(Index);
        }
        if (!bFly && !Result.IsEmpty())
        {
            float BestVertical = BIG_NUMBER;
            for (const int32 Index : Result)
            {
                BestVertical = FMath::Min(BestVertical,
                    FMath::Abs(Position.Z - Nodes[Index].Location.Z));
            }
            const float TierTolerance =
                FMath::Max(10.0f, BakedVoxelSize * 0.35f);
            Result.RemoveAll([
                this, Position, BestVertical, TierTolerance](int32 Index)
            {
                return FMath::Abs(Position.Z - Nodes[Index].Location.Z)
                    > BestVertical + TierTolerance;
            });
        }
        Result.Sort([this, Position](int32 A, int32 B)
        {
            return FVector::DistSquared(Position, Nodes[A].Location)
                < FVector::DistSquared(Position, Nodes[B].Location);
        });
        if (Result.Num() > 24)
        {
            Result.SetNum(24, EAllowShrinking::No);
        }
        return Result;
    };

    const TArray<int32> Starts = CollectAnchors(Start);
    const TArray<int32> Goals = CollectAnchors(Goal);
    if (Starts.IsEmpty() || Goals.IsEmpty())
    {
        LastPathDiagnostic = FString::Printf(
            TEXT("ENDPOINT FAILED | starts=%d goals=%d"),
            Starts.Num(), Goals.Num());
        return nullptr;
    }

    FHellRunVoxelTraversalCostProfile Profile =
        S->GetVoxelCostProfileForCharacter(Character);
    if (Traversal && Traversal->bOverrideVoxelCostProfile)
    {
        Profile = Traversal->VoxelCostProfileOverride;
    }
    const float PathVariationStrength = Traversal
        && Traversal->bUseVoxelPathVariation
        ? FMath::Clamp(
            Traversal->VoxelPathVariationStrength, 0.0f, 0.5f)
        : 0.0f;
    const uint32 PathVariationSeed = Traversal
        && Traversal->VoxelPathVariationSeed != 0
        ? static_cast<uint32>(Traversal->VoxelPathVariationSeed)
        : GetTypeHash(Character.GetPathName());
    auto ModeMultiplier = [=](EHellRunVoxelSegment Mode, bool& bAllowed)
    {
        bAllowed = false;
        switch (Mode)
        {
        case EHellRunVoxelSegment::Walk:
            bAllowed = bWalk; return Profile.WalkMultiplier;
        case EHellRunVoxelSegment::Climb:
            bAllowed = bClimb; return Profile.ClimbMultiplier;
        case EHellRunVoxelSegment::Mantle:
            bAllowed = bMantle; return Profile.MantleMultiplier;
        case EHellRunVoxelSegment::Drop:
            bAllowed = bDrop; return Profile.DropMultiplier;
        case EHellRunVoxelSegment::Jump:
            bAllowed = bJump; return Profile.JumpMultiplier;
        case EHellRunVoxelSegment::Vault:
            bAllowed = bVault; return Profile.VaultMultiplier;
        case EHellRunVoxelSegment::Fly:
            bAllowed = bFly; return Profile.FlightMultiplier;
        default:
            return 1.0f;
        }
    };
    if (!bRuntimeSearchCostCacheBuilt)
    {
        RebuildRuntimeSearchCostCache();
    }
    float MinimumCost = BIG_NUMBER;
    for (int32 ModeIndex = 0; ModeIndex < MinimumBaseEdgeCostByMode.Num(); ++ModeIndex)
    {
        const float BaseCost = MinimumBaseEdgeCostByMode[ModeIndex];
        if (!FMath::IsFinite(BaseCost)) continue;
        bool bAllowed = false;
        const float Multiplier = ModeMultiplier(
            static_cast<EHellRunVoxelSegment>(ModeIndex), bAllowed);
        if (bAllowed)
        {
            MinimumCost = FMath::Min(
                MinimumCost, BaseCost * Multiplier);
        }
    }
    if (!FMath::IsFinite(MinimumCost)) MinimumCost = 0.0f;
    const float HeuristicPerCm =
        MinimumCost / FMath::Max(BakedVoxelSize * 2.0f, 1.0f);

    struct FSearchRecord
    {
        float G = BIG_NUMBER;
        float F = BIG_NUMBER;
        int32 Parent = INDEX_NONE;
        EHellRunVoxelSegment Mode = EHellRunVoxelSegment::Walk;
        bool bClosed = false;
    };
    TMap<int32, FSearchRecord> Records;
    Records.Reserve(FMath::Min(Nodes.Num(), 4096));
    TArray<int32> Open;
    auto Push = [&Open, &Records](int32 Index)
    {
        int32 Position = Open.Add(Index);
        while (Position > 0)
        {
            const int32 Parent = (Position - 1) / 2;
            if (Records.FindChecked(Open[Parent]).F
                <= Records.FindChecked(Open[Position]).F) break;
            Open.Swap(Parent, Position);
            Position = Parent;
        }
    };
    auto Pop = [&Open, &Records]()
    {
        const int32 Result = Open[0];
        Open[0] = Open.Last();
        Open.Pop(EAllowShrinking::No);
        int32 Position = 0;
        while (Open.IsValidIndex(Position))
        {
            const int32 Left = Position * 2 + 1;
            const int32 Right = Left + 1;
            if (!Open.IsValidIndex(Left)) break;
            int32 Best = Left;
            if (Open.IsValidIndex(Right)
                && Records.FindChecked(Open[Right]).F
                    < Records.FindChecked(Open[Left]).F)
            {
                Best = Right;
            }
            if (Records.FindChecked(Open[Position]).F
                <= Records.FindChecked(Open[Best]).F) break;
            Open.Swap(Position, Best);
            Position = Best;
        }
        return Result;
    };
    auto Heuristic = [this, Goal, HeuristicPerCm](int32 Index)
    {
        return FVector::Distance(Nodes[Index].Location, Goal)
            * HeuristicPerCm;
    };
    for (const int32 StartIndex : Starts)
    {
        FSearchRecord& Record = Records.FindOrAdd(StartIndex);
        Record.G = FVector::Distance(Start, Nodes[StartIndex].Location)
            / BakedVoxelSize;
        Record.F = Record.G + Heuristic(StartIndex);
        Push(StartIndex);
    }
    TSet<int32> GoalSet(Goals);
    int32 Reached = INDEX_NONE;
    int32 Expanded = 0;
    while (!Open.IsEmpty())
    {
        const int32 Current = Pop();
        FSearchRecord& MutableCurrentRecord = Records.FindChecked(Current);
        if (MutableCurrentRecord.bClosed) continue;
        MutableCurrentRecord.bClosed = true;
        // FindOrAdd below may grow the sparse map. Keep the expanded record by
        // value so neighbor insertion can never invalidate our current state.
        const FSearchRecord CurrentRecord = MutableCurrentRecord;
        ++Expanded;
        if (GoalSet.Contains(Current))
        {
            Reached = Current;
            break;
        }
        const FHellRunBakedVoxelNode& Node = Nodes[Current];
        auto RelaxNeighbor = [&](int32 ToNode,
            EHellRunVoxelSegment Mode, float BaseCost)
        {
            if (!Nodes.IsValidIndex(ToNode)
                || DynamicBlockedNodeRefCounts.Contains(ToNode)) return;
            bool bAllowed = false;
            const float Multiplier = ModeMultiplier(Mode, bAllowed);
            if (!bAllowed) return;
            FSearchRecord& Next = Records.FindOrAdd(ToNode);
            if (Next.bClosed) return;
            float EdgeCost = BaseCost * Multiplier;
            if (Mode == EHellRunVoxelSegment::Walk
                && S->GroundBoundaryAvoidancePenalty > 0.0f)
            {
                // Four cardinal walk exits identify an interior surface node.
                // Missing exits indicate a wall, ledge, or obstacle boundary.
                // Biasing the destination cost moves paths toward the middle
                // without making a narrow but valid passage unreachable.
                int32 CardinalWalkExits = 0;
                const FHellRunBakedVoxelNode& DestinationNode = Nodes[ToNode];
                for (int32 DestinationEdgeIndex = DestinationNode.FirstEdge;
                    DestinationEdgeIndex < DestinationNode.FirstEdge
                        + DestinationNode.EdgeCount;
                    ++DestinationEdgeIndex)
                {
                    if (!Edges.IsValidIndex(DestinationEdgeIndex)
                        || Edges[DestinationEdgeIndex].Mode
                            != EHellRunVoxelSegment::Walk)
                    {
                        continue;
                    }
                    const FVector ExitDelta =
                        Nodes[Edges[DestinationEdgeIndex].ToNode].Location
                        - DestinationNode.Location;
                    if ((FMath::IsNearlyZero(ExitDelta.X)
                            != FMath::IsNearlyZero(ExitDelta.Y))
                        && FMath::Abs(ExitDelta.Z) <= S->GroundStepHeight)
                    {
                        ++CardinalWalkExits;
                    }
                }
                const float BoundaryFraction = static_cast<float>(
                    FMath::Max(0, 4 - CardinalWalkExits)) / 4.0f;
                EdgeCost += S->GroundBoundaryAvoidancePenalty
                    * BoundaryFraction;
            }
            if (PathVariationStrength > 0.0f)
            {
                // Hash a coarse spatial patch rather than each individual
                // edge. This gives one enemy a stable preference for whole
                // corridors and avoids high-frequency zig-zag noise. Because
                // the multiplier is non-negative, the base heuristic remains
                // admissible.
                const FVector EdgeMidpoint = (
                    Nodes[Current].Location + Nodes[ToNode].Location) * 0.5f;
                const float PatchSize =
                    FMath::Max(BakedVoxelSize * 5.0f, 1.0f);
                const FIntVector PreferencePatch(
                    FMath::FloorToInt(EdgeMidpoint.X / PatchSize),
                    FMath::FloorToInt(EdgeMidpoint.Y / PatchSize),
                    FMath::FloorToInt(EdgeMidpoint.Z / PatchSize));
                uint32 VariationHash = HashCombineFast(
                    PathVariationSeed, GetTypeHash(PreferencePatch));
                VariationHash = HashCombineFast(
                    VariationHash, static_cast<uint32>(Mode));
                const float UnitVariation =
                    static_cast<float>(VariationHash & 0xffffu) / 65535.0f;
                EdgeCost *= 1.0f + PathVariationStrength * UnitVariation;
            }
            if (CurrentRecord.Parent != INDEX_NONE)
            {
                if (CurrentRecord.Mode != Mode)
                {
                    EdgeCost += Profile.LocomotionStateChangePenalty;
                }
                const FVector PreviousDirection =
                    (Nodes[Current].Location
                        - Nodes[CurrentRecord.Parent].Location).GetSafeNormal();
                const FVector NextDirection =
                    (Nodes[ToNode].Location
                        - Nodes[Current].Location).GetSafeNormal();
                EdgeCost += Profile.TurnPenalty
                    * (1.0f - FVector::DotProduct(
                        PreviousDirection, NextDirection));
            }
            const float NewG = CurrentRecord.G + EdgeCost;
            if (NewG < Next.G)
            {
                Next.G = NewG;
                Next.F = NewG + Heuristic(ToNode);
                Next.Parent = Current;
                Next.Mode = Mode;
                Push(ToNode);
            }
        };
        for (int32 EdgeIndex = Node.FirstEdge;
            EdgeIndex < Node.FirstEdge + Node.EdgeCount; ++EdgeIndex)
        {
            if (!Edges.IsValidIndex(EdgeIndex)) continue;
            const FHellRunBakedVoxelEdge& Edge = Edges[EdgeIndex];
            RelaxNeighbor(Edge.ToNode, Edge.Mode, Edge.BaseCost);
        }

        // Flight topology uses one collision-tested 26-neighbor bit mask per
        // node instead of millions of reflected edge structs. Enumerating it
        // here produces the same destinations and geometric costs as the
        // previous explicit representation.
        if (bFly && FlightNeighborMasks.IsValidIndex(Current))
        {
            const uint32 FlightMask = FlightNeighborMasks[Current];
            const FIntVector FromCell = UnflattenCell(Node.CellIndex);
            int32 NeighborBit = 0;
            for (int32 DZ = -1; DZ <= 1; ++DZ)
            for (int32 DY = -1; DY <= 1; ++DY)
            for (int32 DX = -1; DX <= 1; ++DX)
            {
                if (DX == 0 && DY == 0 && DZ == 0) continue;
                const int32 Bit = NeighborBit++;
                if ((FlightMask & (1u << Bit)) == 0) continue;
                const int32 FlatIndex = FlattenCell(
                    FromCell + FIntVector(DX, DY, DZ));
                const int32 ToNode = CellToNode.IsValidIndex(FlatIndex)
                    ? CellToNode[FlatIndex] : INDEX_NONE;
                if (!Nodes.IsValidIndex(ToNode)) continue;
                const float DistanceVoxels = FVector::Distance(
                    Node.Location, Nodes[ToNode].Location)
                    / FMath::Max(BakedVoxelSize, 1.0f);
                RelaxNeighbor(ToNode, EHellRunVoxelSegment::Fly,
                    S->VoxelFlightCost * DistanceVoxels);
            }
        }
    }
    if (Reached == INDEX_NONE)
    {
        LastPathDiagnostic = FString::Printf(
            TEXT("SEARCH FAILED | starts=%d goals=%d expanded=%d"),
            Starts.Num(), Goals.Num(), Expanded);
        return nullptr;
    }

    TArray<int32> Route;
    for (int32 Node = Reached; Node != INDEX_NONE;
        Node = Records.FindChecked(Node).Parent)
    {
        Route.Add(Node);
    }
    Algo::Reverse(Route);

    // A cardinal ground graph deliberately prevents corner cutting, but its
    // equally optimal diagonal routes reconstruct as a visible staircase.
    // String-pull only inside uninterrupted Walk runs. Every proposed chord is
    // validated against the current supported surface and a baked-agent
    // capsule, so discrete traversal boundaries and obstacle clearance remain
    // authoritative.
    UWorld* World = GetWorld();
    FCollisionObjectQueryParams ShortcutObjects;
    ShortcutObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    ShortcutObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams ShortcutParams(
        SCENE_QUERY_STAT(HellRunVoxelWalkShortcut),
        false,
        &Character);
    ShortcutParams.AddIgnoredActor(this);
    const FCollisionShape ShortcutShape = FCollisionShape::MakeCapsule(
        S->VoxelBakeAgentRadius,
        S->VoxelBakeAgentHalfHeight);
    auto WalkShortcutClear =
        [World, S, this, &ShortcutObjects, &ShortcutParams, &ShortcutShape](
            const FVector& From,
            const FVector& To)
    {
        if (!World || BakedVoxelSize <= 0.0f) return false;
        const float HorizontalDistance = FVector::Dist2D(From, To);
        if (HorizontalDistance <= BakedVoxelSize * 1.05f) return false;

        const float SampleSpacing =
            FMath::Max(10.0f, BakedVoxelSize * 0.25f);
        const int32 SampleCount = FMath::Max(
            2,
            FMath::CeilToInt(HorizontalDistance / SampleSpacing));
        FVector PreviousCenter = From;
        for (int32 Sample = 1; Sample <= SampleCount; ++Sample)
        {
            const float Alpha =
                static_cast<float>(Sample) / SampleCount;
            const FVector ExpectedCenter = FMath::Lerp(From, To, Alpha);
            const FVector ExpectedFloor = ExpectedCenter
                - FVector::UpVector
                    * (S->VoxelBakeAgentHalfHeight
                        + S->VoxelGroundClearance);
            FHitResult FloorHit;
            const float ProbeHeight = FMath::Max(
                S->GroundStepHeight,
                S->VoxelFloorProbeDepth);
            if (!TraceTopology(
                    *World,
                    FloorHit,
                    ExpectedFloor
                        + FVector::UpVector * S->GroundStepHeight,
                    ExpectedFloor
                        - FVector::UpVector * ProbeHeight,
                    ShortcutObjects,
                    ShortcutParams)
                || FloorHit.ImpactNormal.Z < 0.55f
                || FMath::Abs(
                    FloorHit.ImpactPoint.Z - ExpectedFloor.Z)
                    > S->GroundStepHeight)
            {
                return false;
            }

            const FVector SupportedCenter(
                ExpectedCenter.X,
                ExpectedCenter.Y,
                FloorHit.ImpactPoint.Z
                    + S->VoxelBakeAgentHalfHeight
                    + S->VoxelGroundClearance);
            if (!SweepTopologyClear(
                    *World,
                    PreviousCenter,
                    SupportedCenter,
                    ShortcutObjects,
                    ShortcutParams,
                    ShortcutShape))
            {
                return false;
            }
            PreviousCenter = SupportedCenter;
        }
        return true;
    };

    TArray<int32> CorridorRoute;
    CorridorRoute.Reserve(Route.Num());
    if (!Route.IsEmpty())
    {
        CorridorRoute.Add(Route[0]);
        int32 RouteIndex = 0;
        while (RouteIndex < Route.Num() - 1)
        {
            int32 SelectedIndex = RouteIndex + 1;
            if (Records.FindChecked(Route[SelectedIndex]).Mode
                == EHellRunVoxelSegment::Walk)
            {
                int32 WalkRunEnd = SelectedIndex;
                while (WalkRunEnd + 1 < Route.Num()
                    && Records.FindChecked(Route[WalkRunEnd + 1]).Mode
                        == EHellRunVoxelSegment::Walk)
                {
                    ++WalkRunEnd;
                }
                for (int32 Candidate = WalkRunEnd;
                    Candidate > SelectedIndex;
                    --Candidate)
                {
                    // Collision sweeps alone are not sufficient corridor
                    // evidence: complex/brush collision can report a clear
                    // chord across a wall even though A* correctly routed
                    // around it. Only compact points that already lie on the
                    // same graph-space line. This removes redundant points on
                    // straight runs without changing the chosen topology.
                    bool bRouteFollowsChord = true;
                    const FVector ChordStart =
                        Nodes[Route[RouteIndex]].Location;
                    const FVector ChordEnd =
                        Nodes[Route[Candidate]].Location;
                    const float CorridorTolerance =
                        FMath::Max(2.0f, BakedVoxelSize * 0.1f);
                    for (int32 Intermediate = RouteIndex + 1;
                        Intermediate < Candidate;
                        ++Intermediate)
                    {
                        if (FMath::PointDistToSegment(
                                Nodes[Route[Intermediate]].Location,
                                ChordStart,
                                ChordEnd)
                            > CorridorTolerance)
                        {
                            bRouteFollowsChord = false;
                            break;
                        }
                    }
                    if (bRouteFollowsChord
                        && WalkShortcutClear(
                            Nodes[Route[RouteIndex]].Location,
                            Nodes[Route[Candidate]].Location))
                    {
                        SelectedIndex = Candidate;
                        break;
                    }
                }
            }
            CorridorRoute.Add(Route[SelectedIndex]);
            RouteIndex = SelectedIndex;
        }
    }

    TSharedRef<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe> Path =
        MakeShared<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe>();
    Path->GetPathPoints().Reserve(CorridorRoute.Num() + 2);
    Path->SegmentCosts.Reserve(CorridorRoute.Num() + 2);

    // Graph anchors prove connectivity, but path following moves the pawn
    // between world-space endpoints. Omitting those endpoints produced a
    // one-point "successful" route whenever start and goal shared an anchor.
    // The controller rejected that route and immediately queried it again,
    // causing the safe-room frame collapse.
    FNavPathPoint StartPoint(Start);
    StartPoint.Flags = HellRunVoxelPath::MakeFlags(
        EHellRunVoxelSegment::Walk, 0.0f);
    Path->GetPathPoints().Add(StartPoint);
    Path->SegmentCosts.Add(0.0f);

    for (int32 Index = 0; Index < CorridorRoute.Num(); ++Index)
    {
        const int32 Node = CorridorRoute[Index];
        if (FVector::DistSquared(
                Path->GetPathPoints().Last().Location,
                Nodes[Node].Location)
            <= FMath::Square(2.0f))
        {
            continue;
        }
        const EHellRunVoxelSegment Mode = Index == 0
            ? EHellRunVoxelSegment::Walk : Records.FindChecked(Node).Mode;
        const float SegmentCost = Index == 0 ? 0.0f
            : Records.FindChecked(Node).G
                - Records.FindChecked(CorridorRoute[Index - 1]).G;
        FNavPathPoint Point(Nodes[Node].Location);
        Point.Flags = HellRunVoxelPath::MakeFlags(Mode, SegmentCost);
        Path->GetPathPoints().Add(Point);
        Path->SegmentCosts.Add(SegmentCost);
    }

    if (FVector::DistSquared(
            Path->GetPathPoints().Last().Location, Goal)
        > FMath::Square(2.0f))
    {
        const float GoalConnectorCost =
            FVector::Distance(
                Path->GetPathPoints().Last().Location, Goal)
            / FMath::Max(BakedVoxelSize, 1.0f)
            * S->VoxelWalkCost * Profile.WalkMultiplier;
        FNavPathPoint GoalPoint(Goal);
        GoalPoint.Flags = HellRunVoxelPath::MakeFlags(
            EHellRunVoxelSegment::Walk, GoalConnectorCost);
        Path->GetPathPoints().Add(GoalPoint);
        Path->SegmentCosts.Add(GoalConnectorCost);
    }
    Path->TotalCost = Records.FindChecked(Reached).G;
    Path->MarkReady();
    LastPathDiagnostic = FString::Printf(
        TEXT("COMPLETE | points=%d rawPoints=%d expanded=%d cost=%.2f variation=[seed:%u strength:%.2f]"),
        Path->GetPathPoints().Num(), Route.Num(), Expanded, Path->TotalCost,
        PathVariationSeed, PathVariationStrength);
    return Path;
}
