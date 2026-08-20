#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "HellRunTraversalComponent.h"
#include "HellRunTraversalNavigation.h"
#include "HellRunTraversalNavigationSettings.h"
#include "HellRunVoxelNavigation.h"
#include "HellRunVoxelNavVolume.h"
#include "HellRunVoxelPathDebugPawn.h"
#include "NavigationSystem.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FHellRunHellHole02SubterraneanTest,
    "HellRun.Navigation.Voxel.HellHole02SubterraneanRoutes",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::EngineFilter
        | EAutomationTestFlags::HighPriority)

namespace
{
    bool IsCompleteRoute(const FNavPathSharedPtr& Path)
    {
        return Path.IsValid()
            && Path->IsValid()
            && !Path->IsPartial()
            && Path->GetPathPoints().Num() > 1;
    }

    struct FStackedSurfacePair
    {
        FVector Low = FVector::ZeroVector;
        FVector High = FVector::ZeroVector;
        float HorizontalDistance = BIG_NUMBER;
    };
}

bool FHellRunHellHole02SubterraneanTest::RunTest(
    const FString& Parameters)
{
    constexpr TCHAR MapPath[] =
        TEXT("/Game/Levels/Campaigns/HellHole/HellHole_02_NightmareContinues");
    FAutomationEditorCommonUtils::LoadMap(MapPath);
    UWorld* World = GEditor
        ? GEditor->GetEditorWorldContext().World()
        : nullptr;
    if (!TestNotNull(TEXT("Hell Run 02 editor world"), World))
    {
        return false;
    }

    AHellRunRecastNavMesh* SerializedNavMesh = nullptr;
    for (TActorIterator<AHellRunRecastNavMesh> It(World); It; ++It)
    {
        SerializedNavMesh = *It;
        break;
    }
    TestNotNull(TEXT("Hell Run 02 serialized Recast navigation"),
        SerializedNavMesh);
    if (SerializedNavMesh)
    {
        TestFalse(
            TEXT("Current serialized voxel navigation is clean after map load"),
            SerializedNavMesh->NeedsRebuild());
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transient;
    SpawnParameters.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AHellRunVoxelPathDebugPawn* QueryPawn =
        World->SpawnActor<AHellRunVoxelPathDebugPawn>(
            AHellRunVoxelPathDebugPawn::StaticClass(),
            FVector::ZeroVector,
            FRotator::ZeroRotator,
            SpawnParameters);
    if (!TestNotNull(TEXT("subterranean route query pawn"), QueryPawn))
    {
        return false;
    }
    if (UHellRunTraversalComponent* Traversal =
            QueryPawn->FindComponentByClass<UHellRunTraversalComponent>())
    {
        Traversal->bCanWalkNavigation = true;
        Traversal->bCanClimbNavigation = true;
        Traversal->bCanWallClimbNavigation = false;
        Traversal->bCanMantleNavigation = true;
        Traversal->bCanDropNavigation = true;
        Traversal->bCanJumpNavigation = true;
        Traversal->bCanVaultNavigation = true;
        Traversal->bCanFlyNavigation = false;
    }

    const UHellRunTraversalNavigationSettings* Settings =
        GetDefault<UHellRunTraversalNavigationSettings>();
    const float MinimumLayerSeparation = FMath::Max(
        150.0f,
        Settings->VoxelSize * 2.0f);
    TArray<FStackedSurfacePair> Pairs;
    int32 BakedVolumeCount = 0;
    int32 TotalSampleCount = 0;
    int32 TotalDisconnectedGroundNodesCulled = 0;

    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        // This is a bake regression test, not a compatibility check against
        // whatever graph version happens to be serialized in the map. Rebuild
        // so spawn-island classification and stair topology both exercise the
        // current schema.
        It->BuildNavigationData();
        if (!It->HasAuthoritativeTypedEdgeGraph())
        {
            continue;
        }
        ++BakedVolumeCount;
        TotalDisconnectedGroundNodesCulled +=
            It->GetAutomationLastDisconnectedGroundCullCount();
        TArray<FVector> Samples;
        It->GetAutomationGroundNodeLocations(Samples, 2048);
        TotalSampleCount += Samples.Num();

        // For every sampled surface retain its horizontally nearest surface on
        // another elevation. Stairs and underground entrances consequently
        // sort ahead of unrelated high/low points elsewhere in the level.
        for (int32 A = 0; A < Samples.Num(); ++A)
        {
            int32 BestB = INDEX_NONE;
            float BestHorizontal = BIG_NUMBER;
            for (int32 B = 0; B < Samples.Num(); ++B)
            {
                const float HeightDelta = Samples[B].Z - Samples[A].Z;
                if (HeightDelta < MinimumLayerSeparation)
                {
                    continue;
                }
                const float Horizontal = FVector::Dist2D(
                    Samples[A], Samples[B]);
                if (Horizontal < BestHorizontal)
                {
                    BestHorizontal = Horizontal;
                    BestB = B;
                }
            }
            if (BestB != INDEX_NONE)
            {
                Pairs.Add({Samples[A], Samples[BestB], BestHorizontal});
            }
        }
    }

    TestTrue(TEXT("Hell Run 02 contains baked voxel navigation"),
        BakedVolumeCount > 0);
    TestTrue(
        TEXT("Hell Run 02 bake removes disconnected ground islands"),
        TotalDisconnectedGroundNodesCulled > 0);
    TestTrue(TEXT("Hell Run 02 exposes sampled navigation surfaces"),
        TotalSampleCount > 1);
    Pairs.Sort([](
        const FStackedSurfacePair& A,
        const FStackedSurfacePair& B)
    {
        return A.HorizontalDistance < B.HorizontalDistance;
    });

    bool bFoundDescendingRoute = false;
    bool bFoundAscendingRoute = false;
    AHellRunVoxelNavVolume* ValidatedPairVolume = nullptr;
    FVector ValidatedLowerSurface = FVector::ZeroVector;
    FVector ValidatedUpperSurface = FVector::ZeroVector;
    FNavPathSharedPtr ValidatedDescendingRoute;
    constexpr int32 MaximumPairsToQuery = 96;
    const int32 PairLimit = FMath::Min(MaximumPairsToQuery, Pairs.Num());
    for (int32 PairIndex = 0; PairIndex < PairLimit; ++PairIndex)
    {
        const FStackedSurfacePair& Pair = Pairs[PairIndex];
        AHellRunVoxelNavVolume* PairVolume = nullptr;
        for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
        {
            if (It->HasAuthoritativeTypedEdgeGraph()
                && It->ContainsRouteEndpoints(Pair.Low, Pair.High))
            {
                PairVolume = *It;
                break;
            }
        }
        if (!PairVolume)
        {
            continue;
        }

        const FNavPathSharedPtr Descending = PairVolume->FindPath(
            *QueryPawn, Pair.High, Pair.Low);
        const FNavPathSharedPtr Ascending = PairVolume->FindPath(
            *QueryPawn, Pair.Low, Pair.High);
        bFoundDescendingRoute |= IsCompleteRoute(Descending);
        bFoundAscendingRoute |= IsCompleteRoute(Ascending);
        if (bFoundDescendingRoute && bFoundAscendingRoute)
        {
            ValidatedPairVolume = PairVolume;
            ValidatedLowerSurface = Pair.Low;
            ValidatedUpperSurface = Pair.High;
            ValidatedDescendingRoute = Descending;
            AddInfo(FString::Printf(
                TEXT("Hell Run 02 stacked route validated low=%s high=%s horizontal=%.1f vertical=%.1f"),
                *Pair.Low.ToCompactString(),
                *Pair.High.ToCompactString(),
                Pair.HorizontalDistance,
                Pair.High.Z - Pair.Low.Z));
            break;
        }
    }

    TestTrue(TEXT("Hell Run 02 has a complete route down into a lower area"),
        bFoundDescendingRoute);
    TestTrue(TEXT("Hell Run 02 has a complete route back up from a lower area"),
        bFoundAscendingRoute);

    if (ValidatedDescendingRoute.IsValid())
    {
        int32 JumpSegments = 0;
        float MaximumRouteZ = -BIG_NUMBER;
        TArray<FString> RouteModes;
        for (const FNavPathPoint& Point :
            ValidatedDescendingRoute->GetPathPoints())
        {
            const EHellRunVoxelSegment Mode =
                HellRunVoxelPath::GetMode(Point.Flags);
            JumpSegments += Mode == EHellRunVoxelSegment::Jump ? 1 : 0;
            MaximumRouteZ = FMath::Max(MaximumRouteZ, Point.Location.Z);
            RouteModes.Add(FString::FromInt(static_cast<int32>(Mode)));
        }
        AddInfo(FString::Printf(
            TEXT("Hell Run 02 descending route points=%d jumps=%d maxZ=%.1f modes=%s"),
            ValidatedDescendingRoute->GetPathPoints().Num(),
            JumpSegments,
            MaximumRouteZ,
            *FString::Join(RouteModes, TEXT(","))));
    }

    if (ValidatedPairVolume)
    {
        QueryPawn->SetActorLocation(
            ValidatedUpperSurface,
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        FNavPathSharedPtr WrongLayerGroundPath =
            MakeShared<FNavigationPath>();
        WrongLayerGroundPath->GetPathPoints().Add(
            FNavPathPoint(ValidatedUpperSurface));
        WrongLayerGroundPath->GetPathPoints().Add(FNavPathPoint(FVector(
            ValidatedLowerSurface.X,
            ValidatedLowerSurface.Y,
            ValidatedUpperSurface.Z)));
        WrongLayerGroundPath->MarkReady();
        const FHellRunNavigationPathResult SelectedRoute =
            FHellRunVoxelNavigation::QueryBestPath(
                *QueryPawn,
                ValidatedLowerSurface,
                WrongLayerGroundPath,
                false);
        TestEqual(
            TEXT("Hell Run 02 rejects NavMesh above a subterranean goal"),
            SelectedRoute.Provider,
            EHellRunNavigationPathProvider::Voxel);
        TestTrue(
            TEXT("Hell Run 02 selects a complete lower-layer voxel route"),
            IsCompleteRoute(SelectedRoute.Path));
    }

    // Coordinates captured from the reported Hell Run 02 stair failure. The
    // upper-floor bots must accept a route to the player's capsule center in
    // the subterranean room instead of retaining an old roof route.
    const FVector ReportedStairStart(31885.25f, 56620.16f, 98.15f);
    const FVector ReportedStairGoal(33030.07f, 56438.78f, -287.77f);
    QueryPawn->SetActorLocation(
        ReportedStairStart,
        false,
        nullptr,
        ETeleportType::TeleportPhysics);
    FNavPathSharedPtr ReportedGroundPath;
    if (UNavigationSystemV1* NavSystem =
            FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
    {
        if (const ANavigationData* NavData =
                NavSystem->GetDefaultNavDataInstance(
                    FNavigationSystem::DontCreate))
        {
            FPathFindingQuery GroundQuery(
                QueryPawn,
                *NavData,
                ReportedStairStart,
                ReportedStairGoal);
            ReportedGroundPath =
                NavSystem->FindPathSync(GroundQuery).Path;
        }
    }
    TestTrue(
        TEXT("Hell Run 02 reported stairs have a Recast candidate route"),
        ReportedGroundPath.IsValid()
            && ReportedGroundPath->IsValid());
    if (ReportedGroundPath.IsValid())
    {
        for (int32 PointIndex = 0;
            PointIndex < ReportedGroundPath->GetPathPoints().Num();
            ++PointIndex)
        {
            const FNavPathPoint& Point =
                ReportedGroundPath->GetPathPoints()[PointIndex];
            AddInfo(FString::Printf(
                TEXT("Reported stair ground point=%d location=%s customLink=%d flags=%u"),
                PointIndex,
                *Point.Location.ToCompactString(),
                Point.CustomNavLinkId.IsValid() ? 1 : 0,
                Point.Flags));
        }
    }
    const FHellRunNavigationPathResult ReportedStairRoute =
        FHellRunVoxelNavigation::QueryBestPath(
            *QueryPawn,
            ReportedStairGoal,
            ReportedGroundPath,
            false);
    AddInfo(FString::Printf(
        TEXT("Reported stair ground valid=%d partial=%d points=%d endpoint=%s selectedProvider=%d selectedOutcome=%d diagnostic=%s"),
        ReportedGroundPath.IsValid() && ReportedGroundPath->IsValid() ? 1 : 0,
        ReportedGroundPath.IsValid() && ReportedGroundPath->IsPartial() ? 1 : 0,
        ReportedGroundPath.IsValid()
            ? ReportedGroundPath->GetPathPoints().Num() : 0,
        ReportedGroundPath.IsValid()
            && !ReportedGroundPath->GetPathPoints().IsEmpty()
            ? *ReportedGroundPath->GetPathPoints().Last().Location.ToCompactString()
            : TEXT("none"),
        static_cast<int32>(ReportedStairRoute.Provider),
        static_cast<int32>(ReportedStairRoute.Outcome),
        *FHellRunVoxelNavigation::GetLastQueryDiagnostic()));
    TestTrue(
        TEXT("Hell Run 02 bot can enter the reported subterranean area"),
        IsCompleteRoute(ReportedStairRoute.Path));
    if (IsCompleteRoute(ReportedStairRoute.Path))
    {
        const FVector RouteEndpoint =
            ReportedStairRoute.Path->GetPathPoints().Last().Location;
        const float EndpointCenterZ =
            ReportedStairRoute.Provider
                == EHellRunNavigationPathProvider::Recast
            ? RouteEndpoint.Z
                + Settings->VoxelBakeAgentHalfHeight
                + Settings->VoxelGroundClearance
            : RouteEndpoint.Z;
        TestTrue(
            TEXT("Hell Run 02 stair route ends on the subterranean layer"),
            FVector::Dist2D(RouteEndpoint, ReportedStairGoal)
                    <= Settings->VoxelRouteCacheGoalTolerance
                && FMath::Abs(EndpointCenterZ - ReportedStairGoal.Z)
                    <= Settings->GroundStepHeight);
        AddInfo(FString::Printf(
            TEXT("Reported stair route provider=%d points=%d endpoint=%s endpointCenterZ=%.1f goal=%s"),
            static_cast<int32>(ReportedStairRoute.Provider),
            ReportedStairRoute.Path->GetPathPoints().Num(),
            *RouteEndpoint.ToCompactString(),
            EndpointCenterZ,
            *ReportedStairGoal.ToCompactString()));
    }

    // Exercise the production spawn sampler from the lower campaign layer.
    // Disconnected ground islands are removed from the baked graph itself, so
    // this query only needs to prove that the retained subterranean network remains a
    // usable spawn surface. Directed one-way traversal is valid level topology
    // and must not be rejected relative to this one arbitrary test anchor.
    if (ValidatedPairVolume)
    {
        TArray<FVector> SpawnCandidates;
        ValidatedPairVolume->GetSpawnSurfaceLocationsInRange(
            ValidatedLowerSurface,
            250.0f,
            5000.0f,
            SpawnCandidates,
            64);
        TestTrue(
            TEXT("Hell Run 02 lower area produces connected spawn candidates"),
            !SpawnCandidates.IsEmpty());
        const float LayerTolerance = FMath::Max(
            Settings->VoxelSize * 2.0f,
            Settings->GroundStepHeight * 2.0f);
        TestTrue(
            TEXT("Hell Run 02 lower-area spawn candidates stay on the subterranean layer"),
            !SpawnCandidates.ContainsByPredicate(
                [ValidatedLowerSurface, LayerTolerance](const FVector& Candidate)
                {
                    return FMath::Abs(Candidate.Z - ValidatedLowerSurface.Z)
                        > LayerTolerance;
                }));
    }
    QueryPawn->Destroy();
    return !HasAnyErrors();
}

#endif
