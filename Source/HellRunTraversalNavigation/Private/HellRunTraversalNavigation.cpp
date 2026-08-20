#include "HellRunTraversalNavigation.h"
#include "HellRunVoxelNavVolume.h"

#include "HellRunTraversalComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavMesh/NavMeshRenderingComponent.h"
#include "NavMesh/RecastNavMeshGenerator.h"
#include "NavAreas/NavArea_Null.h"
#include "EngineUtils.h"

#if WITH_EDITOR
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

namespace HellRunTraversalNavigationPrivate
{
    static ACharacter* ResolveCharacter(UObject* PathObject)
    {
        const UPathFollowingComponent* PathFollowing = Cast<UPathFollowingComponent>(PathObject);
        const AController* Controller = PathFollowing ? Cast<AController>(PathFollowing->GetOwner()) : nullptr;
        return Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
    }

    static bool StartDrop(UObject* PathObject, const FVector& Destination)
    {
        ACharacter* Character = ResolveCharacter(PathObject);
        const UHellRunTraversalComponent* Traversal = Character ? Character->FindComponentByClass<UHellRunTraversalComponent>() : nullptr;
        UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
        if (!Character || !Movement || !Traversal || !Traversal->CanUseGeneratedTraversalLinks())
        {
            return false;
        }
        const FVector Delta = Destination - Character->GetActorLocation();
        const FVector Horizontal(Delta.X, Delta.Y, 0.0f);
        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
        const float FlightTime = FMath::Clamp(Horizontal.Size() /
            FMath::Max(Movement->MaxWalkSpeed, Settings->DropMinimumHorizontalSpeed), 0.22f, 0.75f);
        FVector Velocity = Horizontal / FlightTime;
        Velocity.Z = FMath::Min((Delta.Z - 0.5f * Movement->GetGravityZ() * FlightTime * FlightTime) / FlightTime,
            Settings->DropMaximumUpwardSpeed);
        Character->LaunchCharacter(Velocity, true, true);
        return false;
    }

    static bool StartTraversal(UObject* PathObject, const FVector& Destination, UObject* Link,
        EHellRunGeneratedTraversalType Type)
    {
        ACharacter* Character = ResolveCharacter(PathObject);
        UHellRunTraversalComponent* Traversal = Character ? Character->FindComponentByClass<UHellRunTraversalComponent>() : nullptr;
        return Traversal && Traversal->CanUseGeneratedTraversalLinks()
            && Traversal->StartGeneratedTraversal(Destination, Cast<UPathFollowingComponent>(PathObject), Link, Type);
    }

    static FNavLinkGenerationJumpConfig BuildConfig(FName Name, const FHellRunTraversalLinkSettings& Source,
        TSubclassOf<UNavAreaBase> DownArea, TSubclassOf<UNavAreaBase> UpArea,
        TSubclassOf<UBaseGeneratedNavLinksProxy> Proxy)
    {
        FNavLinkGenerationJumpConfig Result;
        Result.bEnabled = Source.bEnabled;
        Result.Name = Name;
        Result.JumpLength = Source.HorizontalReach;
        Result.JumpDistanceFromEdge = Source.DistanceFromEdge;
        Result.JumpMaxDepth = Source.MaximumDepth;
        Result.JumpHeight = Source.ArcHeight;
        Result.JumpEndsHeightTolerance = Source.EndpointHeightTolerance;
        Result.SamplingSeparationFactor = Source.SamplingSeparationFactor;
        Result.FilterDistanceThreshold = Source.SimilarLinkFilterDistance;
        Result.LinkBuilderFlags = 0;
        if (Source.bCreateCenterLink)
        {
            Result.LinkBuilderFlags |= static_cast<uint16>(ENavLinkBuilderFlags::CreateCenterPointLink);
        }
        if (Source.bCreateExtremityLinks)
        {
            Result.LinkBuilderFlags |= static_cast<uint16>(ENavLinkBuilderFlags::CreateExtremityLink);
        }
        Result.DownDirectionAreaClass = DownArea;
        Result.UpDirectionAreaClass = UpArea;
        Result.LinkProxyClass = Proxy;
        return Result;
    }
}

void UHellRunNavigationSystemV1::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    AHellRunVoxelNavVolume* SerializedVoxelVolume = nullptr;
    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        if (It->HasBakedNavigationData())
        {
            SerializedVoxelVolume = *It;
            break;
        }
    }
    if (!SerializedVoxelVolume)
    {
        HandledSerializedVoxelVolume.Reset();
        SerializedVoxelStartupTime = 0.0f;
        return;
    }
    if (HandledSerializedVoxelVolume.Get() == SerializedVoxelVolume)
    {
        return;
    }

    SerializedVoxelStartupTime += DeltaSeconds;
    if (SerializedVoxelStartupTime < 0.5f)
    {
        return;
    }

    HandledSerializedVoxelVolume = SerializedVoxelVolume;
    SerializedVoxelStartupTime = 0.0f;
    const int32 DirtyAreaCount = GetNumDirtyAreas();
    ResetDefaultDirtyAreasController();
    UE_LOG(LogTemp, Display,
        TEXT("HellRun navigation startup: loaded serialized voxel data; discarded %d load-time dirty area(s)."),
        DirtyAreaCount);
}

UHellRunNavigationSystemConfig::UHellRunNavigationSystemConfig(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    NavigationSystemClass = UHellRunNavigationSystemV1::StaticClass();
    DefaultAgentName = TEXT("HellRunZombie");
}

UNavArea_HellRunJump::UNavArea_HellRunJump() { const auto* S = GetDefault<UHellRunTraversalNavigationSettings>(); DefaultCost = S->JumpAreaCost; DrawColor = S->JumpAreaColor; }
UNavArea_HellRunVault::UNavArea_HellRunVault() { const auto* S = GetDefault<UHellRunTraversalNavigationSettings>(); DefaultCost = S->VaultAreaCost; DrawColor = S->VaultAreaColor; }
UNavArea_HellRunMantle::UNavArea_HellRunMantle() { const auto* S = GetDefault<UHellRunTraversalNavigationSettings>(); DefaultCost = S->MantleAreaCost; DrawColor = S->MantleAreaColor; }
UNavArea_HellRunClimb::UNavArea_HellRunClimb() { const auto* S = GetDefault<UHellRunTraversalNavigationSettings>(); DefaultCost = S->ClimbAreaCost; DrawColor = S->ClimbAreaColor; }
UNavArea_HellRunDrop::UNavArea_HellRunDrop() { const auto* S = GetDefault<UHellRunTraversalNavigationSettings>(); DefaultCost = S->DropAreaCost; DrawColor = S->DropAreaColor; }

bool UHellRunJumpNavLinkProxy::OnLinkMoveStarted(UObject* P, const FVector& D) { return HellRunTraversalNavigationPrivate::StartTraversal(P, D, this, EHellRunGeneratedTraversalType::Jump); }
bool UHellRunVaultNavLinkProxy::OnLinkMoveStarted(UObject* P, const FVector& D) { return HellRunTraversalNavigationPrivate::StartTraversal(P, D, this, EHellRunGeneratedTraversalType::Vault); }
bool UHellRunMantleNavLinkProxy::OnLinkMoveStarted(UObject* P, const FVector& D) { return HellRunTraversalNavigationPrivate::StartTraversal(P, D, this, EHellRunGeneratedTraversalType::Mantle); }
bool UHellRunClimbNavLinkProxy::OnLinkMoveStarted(UObject* P, const FVector& D) { return HellRunTraversalNavigationPrivate::StartTraversal(P, D, this, EHellRunGeneratedTraversalType::Climb); }
bool UHellRunDropNavLinkProxy::OnLinkMoveStarted(UObject* P, const FVector& D) { return HellRunTraversalNavigationPrivate::StartDrop(P, D); }

class FHellRunRecastNavMeshGenerator final : public FRecastNavMeshGenerator
{
public:
    explicit FHellRunRecastNavMeshGenerator(ARecastNavMesh& NavMesh) : FRecastNavMeshGenerator(NavMesh) {}
};

AHellRunRecastNavMesh::AHellRunRecastNavMesh(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    // Static navigation is serialized with the map. Keep this aligned with the
    // authored map value. With the project's 19 cm cell size, 1000 cm
    // quantizes to the serialized 988 cm tile payload; forcing 1024 cm instead
    // makes UE expect 1007 cm and discard every saved tile on map load.
    bForceRebuildOnLoad = false;
    RuntimeGeneration = ERuntimeGenerationType::Static;
    TileSizeUU = 1000.0f;
    // Campaign traversal is authored by the serialized voxel graph. Enabling
    // UE's experimental Recast link generator as well creates a second,
    // unsaved navigation product and makes an otherwise valid voxel-backed map
    // report that paths need rebuilding every time it is opened.
    bGenerateNavLinks = false;
    bDrawNavLinks = true;
    bDrawFailedNavLinks = true;
    SyncTraversalGenerationConfigs();
}

void AHellRunRecastNavMesh::SyncTraversalGenerationConfigs()
{
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    TArray<FNavLinkGenerationJumpConfig> NewConfigs;
    NewConfigs.Add(HellRunTraversalNavigationPrivate::BuildConfig(TEXT("HellRunGapJump"), S->Jump, UNavArea_HellRunJump::StaticClass(), UNavArea_HellRunJump::StaticClass(), UHellRunJumpNavLinkProxy::StaticClass()));
    NewConfigs.Add(HellRunTraversalNavigationPrivate::BuildConfig(TEXT("HellRunVault"), S->Vault, UNavArea_HellRunVault::StaticClass(), UNavArea_HellRunVault::StaticClass(), UHellRunVaultNavLinkProxy::StaticClass()));
    NewConfigs.Add(HellRunTraversalNavigationPrivate::BuildConfig(TEXT("HellRunMantle"), S->Mantle, nullptr, UNavArea_HellRunMantle::StaticClass(), UHellRunMantleNavLinkProxy::StaticClass()));
    NewConfigs.Add(HellRunTraversalNavigationPrivate::BuildConfig(TEXT("HellRunClimb"), S->Climb, nullptr, UNavArea_HellRunClimb::StaticClass(), UHellRunClimbNavLinkProxy::StaticClass()));
    NewConfigs.Add(HellRunTraversalNavigationPrivate::BuildConfig(TEXT("HellRunDrop"), S->Drop, UNavArea_HellRunDrop::StaticClass(), nullptr, UHellRunDropNavLinkProxy::StaticClass()));

    for (FNavLinkGenerationJumpConfig& NewConfig : NewConfigs)
    {
        if (const FNavLinkGenerationJumpConfig* Existing = NavLinkJumpConfigs.FindByPredicate(
            [&NewConfig](const FNavLinkGenerationJumpConfig& Candidate) { return Candidate.Name == NewConfig.Name; }))
        {
            NewConfig.LinkProxyId = Existing->LinkProxyId;
            NewConfig.LinkProxy = Existing->LinkProxy;
            NewConfig.bLinkProxyRegistered = Existing->bLinkProxyRegistered;
        }
    }
    NavLinkJumpConfigs = MoveTemp(NewConfigs);
}

void AHellRunRecastNavMesh::RefreshTraversalSettings(bool bRebuildNavigation)
{
    SyncTraversalGenerationConfigs();
    ClimbPathDebugRefreshTime = 0.0f;
    if (bRebuildNavigation)
    {
        RebuildAll();
    }
}

void AHellRunRecastNavMesh::ConditionalConstructGenerator() { SyncTraversalGenerationConfigs(); Super::ConditionalConstructGenerator(); }
void AHellRunRecastNavMesh::PostLoadPreRebuild()
{
    bForceRebuildOnLoad = false;
    RuntimeGeneration = ERuntimeGenerationType::Static;
    // Normalize legacy instances before the next Build Paths/save so their
    // serialized owner settings continue to match the generated tile payload.
    TileSizeUU = 1000.0f;
    bGenerateNavLinks = false;
    SyncTraversalGenerationConfigs();
    Super::PostLoadPreRebuild();
}
FRecastNavMeshGenerator* AHellRunRecastNavMesh::CreateGeneratorInstance() { return new FHellRunRecastNavMeshGenerator(*this); }

void AHellRunRecastNavMesh::PruneDisconnectedGroundPolys()
{
    TArray<FNavTileRef> TileRefs;
    GetAllNavMeshTiles(TileRefs);
    TArray<FNavPoly> Polys;
    for (const FNavTileRef TileRef : TileRefs)
    {
        TArray<FNavPoly> TilePolys;
        if (GetPolysInTile(TileRef, TilePolys))
        {
            Polys.Append(TilePolys);
        }
    }
    if (Polys.IsEmpty()) return;

    TMap<NavNodeRef, int32> PolyIndices;
    PolyIndices.Reserve(Polys.Num());
    TArray<int32> Parent;
    Parent.SetNumUninitialized(Polys.Num());
    for (int32 Index = 0; Index < Polys.Num(); ++Index)
    {
        Parent[Index] = Index;
        PolyIndices.Add(Polys[Index].Ref, Index);
    }
    auto FindRoot = [&Parent](int32 Index)
    {
        int32 Root = Index;
        while (Parent[Root] != Root) Root = Parent[Root];
        while (Parent[Index] != Index)
        {
            const int32 Next = Parent[Index];
            Parent[Index] = Root;
            Index = Next;
        }
        return Root;
    };
    for (int32 Index = 0; Index < Polys.Num(); ++Index)
    {
        TArray<NavNodeRef> Neighbors;
        if (!GetPolyNeighbors(Polys[Index].Ref, Neighbors)) continue;
        for (const NavNodeRef Neighbor : Neighbors)
        {
            const int32* NeighborIndex = PolyIndices.Find(Neighbor);
            if (!NeighborIndex) continue;
            const int32 A = FindRoot(Index);
            const int32 B = FindRoot(*NeighborIndex);
            if (A != B) Parent[B] = A;
        }
    }

    TMap<int32, double> ComponentAreas;
    int32 PrimaryRoot = INDEX_NONE;
    double PrimaryArea = 0.0;
    for (int32 Index = 0; Index < Polys.Num(); ++Index)
    {
        const int32 Root = FindRoot(Index);
        const double Area = ComponentAreas.FindOrAdd(Root)
            += FMath::Max(0.0, static_cast<double>(
                GetPolySurfaceArea(Polys[Index].Ref)));
        if (Area > PrimaryArea)
        {
            PrimaryArea = Area;
            PrimaryRoot = Root;
        }
    }

    // A Recast component is valid when it contains any ground surface retained
    // by the capability-aware voxel graph. This preserves separate Recast
    // layers connected through jump/vault/mantle/drop traversal while rejecting
    // building interiors whose voxel ground island was culled.
    TSet<int32> RetainedComponentRoots;
    if (const UWorld* World = GetWorld())
    {
        const FVector ProjectionExtent(75.0f, 75.0f, 300.0f);
        for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
        {
            TArray<FVector> RetainedGroundLocations;
            It->GetRetainedGroundNodeLocations(RetainedGroundLocations);
            for (const FVector& Location : RetainedGroundLocations)
            {
                const NavNodeRef PolyRef = FindNearestPoly(
                    Location, ProjectionExtent);
                if (const int32* PolyIndex = PolyIndices.Find(PolyRef))
                {
                    RetainedComponentRoots.Add(FindRoot(*PolyIndex));
                }
            }
        }
    }
    if (RetainedComponentRoots.IsEmpty() && PrimaryRoot != INDEX_NONE)
    {
        RetainedComponentRoots.Add(PrimaryRoot);
    }

    TArray<FNavPoly> DisconnectedPolys;
    double DisconnectedArea = 0.0;
    for (int32 Index = 0; Index < Polys.Num(); ++Index)
    {
        if (RetainedComponentRoots.Contains(FindRoot(Index))) continue;
        DisconnectedArea += FMath::Max(0.0, static_cast<double>(
            GetPolySurfaceArea(Polys[Index].Ref)));
        DisconnectedPolys.Add(Polys[Index]);
    }
    if (DisconnectedPolys.IsEmpty()) return;

    SetPolyArrayArea(DisconnectedPolys, UNavArea_Null::StaticClass());
    RequestDrawingUpdate(true);
    MarkPackageDirty();
    UE_LOG(LogTemp, Display,
        TEXT("HELLRUN_RECAST_ISLAND_PRUNE | components=%d retainedComponents=%d keptPolys=%d culledPolys=%d largestArea=%.1f culledArea=%.1f"),
        ComponentAreas.Num(), RetainedComponentRoots.Num(),
        Polys.Num() - DisconnectedPolys.Num(), DisconnectedPolys.Num(),
        PrimaryArea, DisconnectedArea);
}

bool AHellRunRecastNavMesh::NeedsRebuild() const
{
    // The voxel dataset is the authoritative campaign navigation payload. A
    // deliberately empty Recast fallback must not invalidate a map whose voxel
    // graph was baked and serialized successfully.
    if (const UWorld* World = GetWorld())
    {
        bool bFoundVoxelVolume = false;
        for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
        {
            bFoundVoxelVolume = true;
            // Serialized data is authoritative across editor sessions. Only
            // an absent or outdated graph should dirty navigation on load;
            // ordinary geometry dirty areas still cause Build Paths to run.
            if (!It->HasCurrentBakedNavigationData())
            {
                return true;
            }
        }
        if (bFoundVoxelVolume) return false;
    }
    return Super::NeedsRebuild();
}

void AHellRunRecastNavMesh::OnNavMeshGenerationFinished()
{
    Super::OnNavMeshGenerationFinished();
#if WITH_EDITOR
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    UWorld* World = GetWorld();
    const bool bEditorWorld = World && !World->IsGameWorld();
    UE_LOG(LogTemp, Display, TEXT("HellRun navigation generation finished (editor world: %s, voxel Build Paths integration: %s)."),
        bEditorWorld ? TEXT("yes") : TEXT("no"), S->bBakeVoxelVolumesWithBuildPaths ? TEXT("enabled") : TEXT("disabled"));
    if (bEditorWorld && S->bBakeVoxelVolumesWithBuildPaths)
    {
        UE_LOG(LogTemp, Display, TEXT("Starting HellRun voxel navigation bake from navigation generation completion."));
        int32 VolumeCount = 0;
        for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
        {
            ++VolumeCount;
            It->BuildNavigationData();
        }
        if (VolumeCount == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("Build Paths found no HellRunVoxelNavVolume actors. Place and size a HellRun Voxel Nav Volume, then run Build Paths again."));
            FNotificationInfo MissingVolumeInfo(NSLOCTEXT("HellRunTraversalNavigation", "VoxelBakeMissingVolume",
                "Voxel navigation not built: no HellRun Voxel Nav Volume exists in this level."));
            MissingVolumeInfo.ExpireDuration = 8.0f;
            MissingVolumeInfo.bUseSuccessFailIcons = true;
            if (const TSharedPtr<SNotificationItem> MissingVolumeNotification = FSlateNotificationManager::Get().AddNotification(MissingVolumeInfo))
            {
                MissingVolumeNotification->SetCompletionState(SNotificationItem::CS_Fail);
            }
        }
        else
        {
            // Recast components are classified only after the voxel graph has
            // removed ground islands using the agent's traversal capabilities.
            PruneDisconnectedGroundPolys();
            UE_LOG(LogTemp, Display, TEXT("Build Paths baked %d HellRun voxel navigation volume(s)."), VolumeCount);
        }
    }
#endif
}

void AHellRunRecastNavMesh::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    if (!S->bDrawActualClimbPaths || !UNavMeshRenderingComponent::IsNavigationShowFlagSet(GetWorld()))
    {
        return;
    }
    ClimbPathDebugRefreshTime -= DeltaSeconds;
    if (ClimbPathDebugRefreshTime <= 0.0f)
    {
        RefreshClimbPathDebugCache();
        ClimbPathDebugRefreshTime = S->DebugCacheRefreshInterval;
    }
    DrawActualClimbPaths();
}

void AHellRunRecastNavMesh::RefreshClimbPathDebugCache()
{
    CachedClimbPathEndpoints.Reset();
    const int32 AreaId = GetAreaID(UNavArea_HellRunClimb::StaticClass());
    if (AreaId < 0) return;
    TArray<FNavTileRef> Tiles;
    GetAllNavMeshTiles(Tiles);
    for (const FNavTileRef Tile : Tiles)
    {
        TArray<FNavPoly> Polys;
        if (!GetPolysInTile(Tile, Polys)) continue;
        for (const FNavPoly& Poly : Polys)
        {
            if (!IsCustomLink(Poly.Ref) || static_cast<int32>(GetPolyAreaID(Poly.Ref)) != AreaId) continue;
            FVector A, B;
            if (GetLinkEndPoints(Poly.Ref, A, B))
            {
                CachedClimbPathEndpoints.Emplace(A.Z <= B.Z ? A : B, A.Z <= B.Z ? B : A);
            }
        }
    }
}

void AHellRunRecastNavMesh::DrawActualClimbPaths() const
{
    UWorld* World = GetWorld();
    if (!World) return;
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    for (const TPair<FVector, FVector>& Endpoints : CachedClimbPathEndpoints)
    {
        const FVector Top(Endpoints.Key.X, Endpoints.Key.Y, Endpoints.Value.Z + S->ClimbPathPullOverHeight);
        DrawDebugDirectionalArrow(World, Endpoints.Key, Top, 28.0f, S->ClimbPathColor, false, 0.0f, 5, S->ClimbPathThickness);
        DrawDebugDirectionalArrow(World, Top, Endpoints.Value, 28.0f, S->ClimbPathColor, false, 0.0f, 5, S->ClimbPathThickness);
        DrawDebugSphere(World, Endpoints.Key, 13.0f, 8, S->ClimbPathColor, false, 0.0f, 5, 2.5f);
        DrawDebugSphere(World, Endpoints.Value, 13.0f, 8, S->ClimbPathColor, false, 0.0f, 5, 2.5f);
        DrawDebugString(World, (Top + Endpoints.Value) * 0.5f + FVector(0, 0, 16), TEXT("CLIMB"), nullptr, S->ClimbPathColor, 0.0f, false, 0.9f);
    }
}
