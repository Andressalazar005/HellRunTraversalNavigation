#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "HellRunVoxelNavigation.h"
#include "HellRunVoxelNavVolume.h"
#include "HellRunVoxelPathDebugPawn.h"
#include "HellRunTraversalComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "Math/RandomStream.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FHellRunVoxelNavigationDatasetTest,
    "HellRun.Navigation.Voxel.TestZombieDataset",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::EngineFilter
        | EAutomationTestFlags::HighPriority)

namespace
{
    FString PathSignature(const FNavPathSharedPtr& Path)
    {
        if (!Path.IsValid() || !Path->IsValid()) return TEXT("INVALID");
        TArray<FString> Parts;
        for (const FNavPathPoint& Point : Path->GetPathPoints())
        {
            const EHellRunVoxelSegment Mode =
                HellRunVoxelPath::IsVoxelFlags(Point.Flags)
                ? HellRunVoxelPath::GetMode(Point.Flags)
                : EHellRunVoxelSegment::Walk;
            Parts.Add(FString::Printf(
                TEXT("%.0f,%.0f,%.0f:%d"),
                Point.Location.X, Point.Location.Y, Point.Location.Z,
                static_cast<int32>(Mode)));
        }
        return FString::Join(Parts, TEXT("|"));
    }

    bool ValidateTypedSegments(
        FAutomationTestBase& Test,
        const FString& QueryName,
        const FNavPathSharedPtr& Path,
        int32 (&ModeCounts)[7])
    {
        if (!Path.IsValid() || !Path->IsValid()) return false;
        const UHellRunTraversalNavigationSettings* Settings =
            GetDefault<UHellRunTraversalNavigationSettings>();
        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
        bool bValid = true;
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            const EHellRunVoxelSegment Mode =
                HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                ? HellRunVoxelPath::GetMode(Points[Index].Flags)
                : EHellRunVoxelSegment::Walk;
            const int32 ModeIndex = static_cast<int32>(Mode);
            if (ModeIndex >= 0 && ModeIndex < UE_ARRAY_COUNT(ModeCounts))
            {
                ++ModeCounts[ModeIndex];
            }
            const FVector Delta =
                Points[Index].Location - Points[Index - 1].Location;
            const float Horizontal = Delta.Size2D();
            const float Cost = HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                ? HellRunVoxelPath::GetSegmentCost(Points[Index].Flags) : 0.0f;
            if (Delta.ContainsNaN() || !FMath::IsFinite(Cost))
            {
                Test.AddError(FString::Printf(
                    TEXT("%s segment %d contains non-finite route data."),
                    *QueryName, Index));
                bValid = false;
                continue;
            }

            auto Require = [&](bool bCondition, const TCHAR* Rule)
            {
                if (bCondition) return;
                Test.AddError(FString::Printf(
                    TEXT("%s segment %d mode=%d violates %s; delta=%s horizontal=%.1f"),
                    *QueryName, Index, ModeIndex, Rule,
                    *Delta.ToCompactString(), Horizontal));
                bValid = false;
            };
            switch (Mode)
            {
            case EHellRunVoxelSegment::Jump:
                Require(Horizontal <= Settings->Jump.HorizontalReach + 1.0f,
                    TEXT("Jump.HorizontalReach"));
                Require(FMath::Abs(Delta.Z)
                        <= Settings->Jump.EndpointHeightTolerance + 1.0f,
                    TEXT("Jump.EndpointHeightTolerance"));
                break;
            case EHellRunVoxelSegment::Vault:
                Require(Horizontal <= Settings->Vault.HorizontalReach + 1.0f,
                    TEXT("Vault.HorizontalReach"));
                Require(FMath::Abs(Delta.Z)
                        <= FMath::Max(
                            Settings->Vault.EndpointHeightTolerance,
                            Settings->VoxelSize * 3.0f)
                            + 1.0f,
                    TEXT("Vault short-ledge rise"));
                break;
            case EHellRunVoxelSegment::Mantle:
                // A ground-to-ground mantle gains a full ledge height. A
                // climb-to-ground pull-over starts at lip height, so its
                // explicit action segment may be nearly level.
                Require(Delta.Z >= -Settings->GroundStepHeight,
                    TEXT("Mantle cannot descend below pull-over tolerance"));
                Require(Delta.Z <= Settings->Mantle.MaximumDepth + 1.0f,
                    TEXT("Mantle.MaximumDepth"));
                Require(FMath::Max(
                        0.0f,
                        Horizontal - Settings->VoxelSize)
                        <= Settings->Mantle.HorizontalReach + 1.0f,
                    TEXT("Mantle.HorizontalReach"));
                break;
            case EHellRunVoxelSegment::Drop:
                Require(Delta.Z < -Settings->GroundStepHeight,
                    TEXT("Drop must lose elevation"));
                Require(-Delta.Z <= Settings->Drop.MaximumDepth + 1.0f,
                    TEXT("Drop.MaximumDepth"));
                Require(Horizontal <= Settings->Drop.HorizontalReach + 1.0f,
                    TEXT("Drop.HorizontalReach"));
                break;
            default:
                break;
            }
        }
        return bValid;
    }

    bool PathContainsMode(
        const FNavPathSharedPtr& Path,
        EHellRunVoxelSegment ExpectedMode)
    {
        if (!Path.IsValid()) return false;
        for (const FNavPathPoint& Point : Path->GetPathPoints())
        {
            if (HellRunVoxelPath::IsVoxelFlags(Point.Flags)
                && HellRunVoxelPath::GetMode(Point.Flags)
                    == ExpectedMode)
            {
                return true;
            }
        }
        return false;
    }

    bool ValidateWalkCorridors(
        FAutomationTestBase& Test,
        const FString& QueryName,
        const ACharacter& Character,
        const FNavPathSharedPtr& Path)
    {
        if (!Path.IsValid() || !Character.GetWorld()
            || !Character.GetCapsuleComponent())
        {
            return false;
        }
        const UHellRunTraversalNavigationSettings* Settings =
            GetDefault<UHellRunTraversalNavigationSettings>();
        const FCollisionShape Shape = FCollisionShape::MakeCapsule(
            Settings->VoxelBakeAgentRadius,
            Settings->VoxelBakeAgentHalfHeight);
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        FCollisionQueryParams Params(
            SCENE_QUERY_STAT(HellRunVoxelAutomationWalkCorridor),
            false,
            &Character);
        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
        bool bValid = true;
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            const EHellRunVoxelSegment Mode =
                HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                ? HellRunVoxelPath::GetMode(Points[Index].Flags)
                : EHellRunVoxelSegment::Walk;
            if (Mode != EHellRunVoxelSegment::Walk) continue;

            TArray<FHitResult> Hits;
            Character.GetWorld()->SweepMultiByObjectType(
                Hits,
                Points[Index - 1].Location
                    + FVector(0.0f, 0.0f, 2.0f),
                Points[Index].Location
                    + FVector(0.0f, 0.0f, 2.0f),
                FQuat::Identity,
                Objects,
                Shape,
                Params);
            for (const FHitResult& Hit : Hits)
            {
                if (Cast<APawn>(Hit.GetActor())
                    || Cast<AHellRunVoxelNavVolume>(Hit.GetActor())
                    || (Hit.GetComponent()
                        && !Hit.GetComponent()
                            ->CanEverAffectNavigation())
                    || Hit.ImpactNormal.Z > 0.7f
                    || Hit.Normal.Z > 0.7f)
                {
                    continue;
                }
                Test.AddError(FString::Printf(
                    TEXT("%s walk segment %d intersects %s/%s at time %.3f"),
                    *QueryName,
                    Index,
                    *GetNameSafe(Hit.GetActor()),
                    *GetNameSafe(Hit.GetComponent()),
                    Hit.Time));
                bValid = false;
                break;
            }
        }
        return bValid;
    }
}

bool FHellRunVoxelNavigationDatasetTest::RunTest(const FString& Parameters)
{
    constexpr TCHAR MapPath[] =
        TEXT("/Game/Levels/Testing/TestZombie_Map");
    FAutomationEditorCommonUtils::LoadMap(MapPath);
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("TestZombie_Map has no usable world."));
        return false;
    }

    AHellRunVoxelNavVolume* Volume = nullptr;
    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        if (It->HasBakedNavigationData())
        {
            Volume = *It;
            break;
        }
    }
    if (!Volume)
    {
        AddError(TEXT("No baked HellRun voxel volume exists in TestZombie_Map."));
        return false;
    }
    // This is a generation test, not merely a serialized-data query test.
    // Always exercise the current baker so collision/navigation-relevance
    // regressions cannot hide behind a previously saved graph.
    AddInfo(TEXT("Rebuilding voxel dataset with the current graph schema."));
    Volume->BuildNavigationData();
    if (!Volume->HasAuthoritativeTypedEdgeGraph())
    {
        AddError(TEXT("Current typed voxel graph could not be built."));
        return false;
    }

    AHellRunVoxelPathDebugPawn* Target = nullptr;
    TArray<AHellRunVoxelPathDebugPawn*> Sources;
    for (TActorIterator<AHellRunVoxelPathDebugPawn> It(World); It; ++It)
    {
        if (It->EndpointRole == EHellRunPathDebugPawnRole::PlayerTarget)
        {
            Target = *It;
        }
        else
        {
            Sources.Add(*It);
        }
    }
    if (!Target || Sources.IsEmpty())
    {
        AddError(TEXT("Dataset requires a PlayerTarget and at least one EnemySource debug pawn."));
        return false;
    }

    int32 CompleteDebugQueries = 0;
    int32 CompleteEditorNormalizedQueries = 0;
    int32 ModeCounts[7] = {};
    bool bObservedSmoothedWalkCorridor = false;
    for (AHellRunVoxelPathDebugPawn* Source : Sources)
    {
        // Apply the debugger's exposed flags before exercising the same
        // authoritative navigation request used by gameplay.
        Source->RefreshDebugPath();
        CompleteEditorNormalizedQueries +=
            Source->PathPointCount > 1 ? 1 : 0;
        const FVector Start = Source->GetActorLocation();
        const FVector Goal = Target->GetActorLocation();
        FHitResult SourceFloor;
        FCollisionObjectQueryParams FloorObjects;
        FloorObjects.AddObjectTypesToQuery(ECC_WorldStatic);
        FloorObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
        FCollisionQueryParams FloorParams(
            SCENE_QUERY_STAT(HellRunPlacedSourceFloor), false, Source);
        FloorParams.AddIgnoredActor(Volume);
        World->LineTraceSingleByObjectType(
            SourceFloor,
            Start,
            Start - FVector::UpVector * 1000.0f,
            FloorObjects,
            FloorParams);
        AddInfo(FString::Printf(
            TEXT("%s placement=%s floor=%s floorActor=%s distance=%.1f"),
            *Source->GetName(),
            *Start.ToCompactString(),
            SourceFloor.bBlockingHit
                ? *SourceFloor.ImpactPoint.ToCompactString() : TEXT("none"),
            *GetNameSafe(SourceFloor.GetActor()),
            SourceFloor.bBlockingHit ? SourceFloor.Distance : -1.0f));
        const FNavPathSharedPtr First =
            FHellRunVoxelNavigation::FindPathFrom(*Source, Start, Goal);
        const FString FirstDiagnostic =
            FHellRunVoxelNavigation::GetLastQueryDiagnostic();
        const FNavPathSharedPtr Second =
            FHellRunVoxelNavigation::FindPathFrom(*Source, Start, Goal);
        const FString SecondDiagnostic =
            FHellRunVoxelNavigation::GetLastQueryDiagnostic();

        const bool bFirstComplete = First.IsValid() && First->IsValid()
            && First->GetPathPoints().Num() > 1 && !First->IsPartial();
        const bool bSecondComplete = Second.IsValid() && Second->IsValid()
            && Second->GetPathPoints().Num() > 1 && !Second->IsPartial();
        TestEqual(
            FString::Printf(TEXT("%s repeated query completion"), *Source->GetName()),
            bFirstComplete, bSecondComplete);
        TestEqual(
            FString::Printf(TEXT("%s repeated query path"), *Source->GetName()),
            PathSignature(First), PathSignature(Second));
        if (!bFirstComplete)
        {
            if (FirstDiagnostic.StartsWith(TEXT("ENDPOINT FAILED")))
            {
                AddInfo(FString::Printf(
                    TEXT("%s is outside a capsule-clear baked endpoint connector and was correctly rejected: [%s]"),
                    *Source->GetName(),
                    *FirstDiagnostic));
                continue;
            }
            const FNavPathSharedPtr Reverse =
                FHellRunVoxelNavigation::FindPathFrom(
                    *Source, Goal, Start);
            const FString ReverseDiagnostic =
                FHellRunVoxelNavigation::GetLastQueryDiagnostic();
            UHellRunTraversalComponent* Capabilities =
                Source->FindComponentByClass<UHellRunTraversalComponent>();
            const bool bOriginalWallClimb = Capabilities
                && Capabilities->bCanWallClimbNavigation;
            FNavPathSharedPtr FullCapabilityPath;
            FString FullCapabilityDiagnostic = TEXT("NO TRAVERSAL COMPONENT");
            if (Capabilities)
            {
                Capabilities->bCanWallClimbNavigation = true;
                Capabilities->bCanClimbNavigation = true;
                FullCapabilityPath = FHellRunVoxelNavigation::FindPathFrom(
                    *Source, Start, Goal);
                FullCapabilityDiagnostic =
                    FHellRunVoxelNavigation::GetLastQueryDiagnostic();
                Capabilities->bCanWallClimbNavigation = bOriginalWallClimb;
            }
            AddError(FString::Printf(
                TEXT("%s cannot reach %s. first=[%s] second=[%s] reverseValid=%d reverse=[%s] reversePath=[%s] wallClimbOriginally=%d fullCapabilityValid=%d fullCapability=[%s] fullCapabilityPath=[%s]"),
                *Source->GetName(), *Target->GetName(),
                *FirstDiagnostic, *SecondDiagnostic,
                Reverse.IsValid() && Reverse->IsValid()
                    && !Reverse->IsPartial() ? 1 : 0,
                *ReverseDiagnostic,
                *PathSignature(Reverse),
                bOriginalWallClimb ? 1 : 0,
                FullCapabilityPath.IsValid() && FullCapabilityPath->IsValid()
                    && !FullCapabilityPath->IsPartial() ? 1 : 0,
                *FullCapabilityDiagnostic,
                *PathSignature(FullCapabilityPath)));
        }
        else
        {
            ++CompleteDebugQueries;
            ValidateTypedSegments(
                *this, Source->GetName(), First, ModeCounts);
            ValidateWalkCorridors(
                *this, Source->GetName(), *Source, First);
            const TArray<FNavPathPoint>& Points =
                First->GetPathPoints();
            const float VoxelSize =
                GetDefault<UHellRunTraversalNavigationSettings>()
                    ->VoxelSize;
            for (int32 PointIndex = 1;
                PointIndex < Points.Num();
                ++PointIndex)
            {
                const EHellRunVoxelSegment Mode =
                    HellRunVoxelPath::GetMode(
                        Points[PointIndex].Flags);
                if (Mode == EHellRunVoxelSegment::Walk
                    && FVector::Dist2D(
                        Points[PointIndex - 1].Location,
                        Points[PointIndex].Location)
                        > VoxelSize * 1.05f)
                {
                    bObservedSmoothedWalkCorridor = true;
                    break;
                }
            }
        }
    }
    // Exercise deterministic pairs drawn from the actual baked dataset. These
    // pairs are diagnostic coverage; disconnected surface islands are allowed,
    // but identical inputs must never alternate between success and failure.
    TArray<FVector> Samples;
    Volume->GetAutomationGroundNodeLocations(Samples, 256);
    int32 StableSampleQueries = 0;
    for (int32 Index = 0; Index + 1 < Samples.Num(); Index += 2)
    {
        AHellRunVoxelPathDebugPawn* QueryPawn =
            Sources[Index % Sources.Num()];
        const FNavPathSharedPtr First = FHellRunVoxelNavigation::FindPathFrom(
            *QueryPawn, Samples[Index], Samples[Index + 1]);
        const FNavPathSharedPtr Second = FHellRunVoxelNavigation::FindPathFrom(
            *QueryPawn, Samples[Index], Samples[Index + 1]);
        const bool bStable = PathSignature(First) == PathSignature(Second);
        TestTrue(
            FString::Printf(TEXT("sample pair %d deterministic"), Index / 2),
            bStable);
        StableSampleQueries += bStable ? 1 : 0;
        if (First.IsValid())
        {
            ValidateTypedSegments(
                *this,
                FString::Printf(TEXT("sample pair %d"), Index / 2),
                First,
                ModeCounts);
            ValidateWalkCorridors(
                *this,
                FString::Printf(TEXT("sample pair %d"), Index / 2),
                *QueryPawn,
                First);
        }

        const FNavPathSharedPtr Reverse =
            FHellRunVoxelNavigation::FindPathFrom(
                *QueryPawn, Samples[Index + 1], Samples[Index]);
        const FNavPathSharedPtr ReverseRepeat =
            FHellRunVoxelNavigation::FindPathFrom(
                *QueryPawn, Samples[Index + 1], Samples[Index]);
        TestEqual(
            FString::Printf(
                TEXT("sample pair %d reverse deterministic"),
                Index / 2),
            PathSignature(Reverse),
            PathSignature(ReverseRepeat));
        if (Reverse.IsValid())
        {
            ValidateTypedSegments(
                *this,
                FString::Printf(
                    TEXT("sample pair %d reverse"),
                    Index / 2),
                Reverse,
                ModeCounts);
            ValidateWalkCorridors(
                *this,
                FString::Printf(
                    TEXT("sample pair %d reverse"),
                    Index / 2),
                *QueryPawn,
                Reverse);
        }
    }

    UHellRunTraversalComponent* ProbeCapabilities =
        Sources[0]->FindComponentByClass<UHellRunTraversalComponent>();
    if (!ProbeCapabilities)
    {
        AddError(TEXT("Debug source requires a traversal component for isolated mode probes."));
        return false;
    }
    const bool SavedCanClimb = ProbeCapabilities->bCanClimbNavigation;
    const bool SavedCanMantle = ProbeCapabilities->bCanMantleNavigation;
    const bool SavedCanDrop = ProbeCapabilities->bCanDropNavigation;
    const bool SavedCanJump = ProbeCapabilities->bCanJumpNavigation;
    const bool SavedCanVault = ProbeCapabilities->bCanVaultNavigation;
    const bool SavedOverride = ProbeCapabilities->bOverrideVoxelCostProfile;
    const FHellRunVoxelTraversalCostProfile SavedProfile =
        ProbeCapabilities->VoxelCostProfileOverride;

    const EHellRunVoxelSegment ProbeModes[] = {
        EHellRunVoxelSegment::Walk,
        EHellRunVoxelSegment::Jump,
        EHellRunVoxelSegment::Vault,
        EHellRunVoxelSegment::Mantle,
        EHellRunVoxelSegment::Drop
    };
    bool AvailableProbeModes[7] = {};
    bool RejectedTrapProbeModes[7] = {};
    for (const EHellRunVoxelSegment ProbeMode : ProbeModes)
    {
        FVector ProbeStart;
        FVector ProbeGoal;
        const bool bFoundProbe = Volume->GetAutomationTraversalProbe(
            *Sources[0], ProbeMode, ProbeStart, ProbeGoal);
        const int32 ProbeModeIndex =
            static_cast<int32>(ProbeMode);
        AvailableProbeModes[ProbeModeIndex] = bFoundProbe;
        if (!bFoundProbe)
        {
            AddInfo(FString::Printf(
                TEXT("fresh bake contains no topology for optional traversal mode %d"),
                ProbeModeIndex));
            continue;
        }

        ProbeCapabilities->bCanClimbNavigation =
            ProbeMode != EHellRunVoxelSegment::Walk;
        ProbeCapabilities->bCanMantleNavigation =
            ProbeMode == EHellRunVoxelSegment::Mantle;
        ProbeCapabilities->bCanDropNavigation =
            ProbeMode == EHellRunVoxelSegment::Drop;
        ProbeCapabilities->bCanJumpNavigation =
            ProbeMode == EHellRunVoxelSegment::Jump;
        ProbeCapabilities->bCanVaultNavigation =
            ProbeMode == EHellRunVoxelSegment::Vault;
        ProbeCapabilities->bOverrideVoxelCostProfile = true;
        ProbeCapabilities->VoxelCostProfileOverride =
            FHellRunVoxelTraversalCostProfile();
        ProbeCapabilities->VoxelCostProfileOverride.WalkMultiplier = 100.0f;
        switch (ProbeMode)
        {
        case EHellRunVoxelSegment::Jump:
            ProbeCapabilities->VoxelCostProfileOverride.JumpMultiplier = 0.01f;
            break;
        case EHellRunVoxelSegment::Vault:
            ProbeCapabilities->VoxelCostProfileOverride.VaultMultiplier = 0.01f;
            break;
        case EHellRunVoxelSegment::Mantle:
            ProbeCapabilities->VoxelCostProfileOverride.MantleMultiplier = 0.01f;
            break;
        case EHellRunVoxelSegment::Drop:
            ProbeCapabilities->VoxelCostProfileOverride.DropMultiplier = 0.01f;
            break;
        default:
            ProbeCapabilities->VoxelCostProfileOverride.WalkMultiplier = 1.0f;
            break;
        }

        const FNavPathSharedPtr ProbePath =
            FHellRunVoxelNavigation::FindPathFrom(
                *Sources[0], ProbeStart, ProbeGoal);
        const FString ProbeDiagnostic =
            FHellRunVoxelNavigation::GetLastQueryDiagnostic();
        if (!ProbePath.IsValid()
            && ProbeDiagnostic.StartsWith(
                TEXT("TRAP DESTINATION REJECTED")))
        {
            RejectedTrapProbeModes[ProbeModeIndex] = true;
            AddInfo(FString::Printf(
                TEXT("targeted mode %d correctly rejected one-way trap start=%s goal=%s diagnostic=[%s]"),
                ProbeModeIndex,
                *ProbeStart.ToCompactString(),
                *ProbeGoal.ToCompactString(),
                *ProbeDiagnostic));
            continue;
        }
        ValidateTypedSegments(
            *this,
            FString::Printf(
                TEXT("targeted mode %d"),
                static_cast<int32>(ProbeMode)),
            ProbePath,
            ModeCounts);
        ValidateWalkCorridors(
            *this,
            FString::Printf(
                TEXT("targeted mode %d"),
                static_cast<int32>(ProbeMode)),
            *Sources[0],
            ProbePath);
        if (!PathContainsMode(ProbePath, ProbeMode))
        {
            AddError(FString::Printf(
                TEXT("targeted path missing mode %d start=%s goal=%s diagnostic=[%s] path=[%s]"),
                static_cast<int32>(ProbeMode),
                *ProbeStart.ToCompactString(),
                *ProbeGoal.ToCompactString(),
                *ProbeDiagnostic,
                *PathSignature(ProbePath)));
        }
    }

    ProbeCapabilities->bCanClimbNavigation = SavedCanClimb;
    ProbeCapabilities->bCanMantleNavigation = SavedCanMantle;
    ProbeCapabilities->bCanDropNavigation = SavedCanDrop;
    ProbeCapabilities->bCanJumpNavigation = SavedCanJump;
    ProbeCapabilities->bCanVaultNavigation = SavedCanVault;
    ProbeCapabilities->bOverrideVoxelCostProfile = SavedOverride;
    ProbeCapabilities->VoxelCostProfileOverride = SavedProfile;

    // Deterministic randomized surface stress. The fixed seed makes every
    // failure reproducible while still exercising endpoint projection and A*
    // across unrelated areas of the real baked map. Each archetype uses the
    // same base character with a distinct capability/cost profile so the test
    // isolates voxel routing policy from unrelated pawn Blueprint behavior.
    struct FRandomAgentProfile
    {
        const TCHAR* Name = TEXT("");
        bool bCanWalk = true;
        bool bCanWallClimb = false;
        bool bCanFly = false;
        bool bPreferFly = false;
        bool bCanJump = true;
        bool bCanVault = true;
        bool bCanMantle = true;
        bool bCanDrop = true;
        FHellRunVoxelTraversalCostProfile Costs;
    };

    FRandomAgentProfile ZombieProfile;
    ZombieProfile.Name = TEXT("Zombie");
    ZombieProfile.Costs.WalkMultiplier = 1.0f;
    ZombieProfile.Costs.JumpMultiplier = 1.25f;
    ZombieProfile.Costs.VaultMultiplier = 0.85f;
    ZombieProfile.Costs.MantleMultiplier = 1.1f;
    ZombieProfile.Costs.DropMultiplier = 0.9f;
    ZombieProfile.Costs.ClimbMultiplier = 4.0f;
    ZombieProfile.Costs.FlightMultiplier = 10.0f;

    FRandomAgentProfile FlyingProfile;
    FlyingProfile.Name = TEXT("FlyingEnemy");
    FlyingProfile.bCanWalk = false;
    FlyingProfile.bCanFly = true;
    FlyingProfile.bPreferFly = true;
    FlyingProfile.bCanJump = false;
    FlyingProfile.bCanVault = false;
    FlyingProfile.bCanMantle = false;
    FlyingProfile.bCanDrop = false;
    FlyingProfile.Costs.WalkMultiplier = 5.0f;
    FlyingProfile.Costs.ClimbMultiplier = 5.0f;
    FlyingProfile.Costs.MantleMultiplier = 5.0f;
    FlyingProfile.Costs.DropMultiplier = 5.0f;
    FlyingProfile.Costs.JumpMultiplier = 5.0f;
    FlyingProfile.Costs.VaultMultiplier = 5.0f;
    FlyingProfile.Costs.FlightMultiplier = 0.35f;
    FlyingProfile.Costs.LocomotionStateChangePenalty = 0.0f;

    FRandomAgentProfile ClimbingProfile;
    ClimbingProfile.Name = TEXT("ClimbingEnemy");
    ClimbingProfile.bCanWallClimb = true;
    ClimbingProfile.Costs.WalkMultiplier = 1.4f;
    ClimbingProfile.Costs.ClimbMultiplier = 0.35f;
    ClimbingProfile.Costs.MantleMultiplier = 0.4f;
    ClimbingProfile.Costs.DropMultiplier = 0.75f;
    ClimbingProfile.Costs.JumpMultiplier = 1.4f;
    ClimbingProfile.Costs.VaultMultiplier = 1.1f;
    ClimbingProfile.Costs.FlightMultiplier = 10.0f;
    ClimbingProfile.Costs.ClimbLateralMultiplier = 1.05f;
    ClimbingProfile.Costs.ClimbDownMultiplier = 1.05f;

    const FRandomAgentProfile RandomProfiles[] = {
        ZombieProfile,
        FlyingProfile,
        ClimbingProfile
    };
    const bool SavedCanWalk = ProbeCapabilities->bCanWalkNavigation;
    const bool SavedCanWallClimb = ProbeCapabilities->bCanWallClimbNavigation;
    const bool SavedCanFly = ProbeCapabilities->bCanFlyNavigation;
    const bool SavedPreferFly = ProbeCapabilities->bPreferFlyingNavigation;
    constexpr int32 RandomSeed = 0x48E11A;
    constexpr int32 RandomQueriesPerProfile = 24;
    TArray<FVector> RandomSurfacePoints;
    Volume->GetAutomationGroundNodeLocations(RandomSurfacePoints, 2048);
    TestTrue(
        TEXT("randomized profile stress has enough surface samples"),
        RandomSurfacePoints.Num() >= 2);
    float MinimumSurfaceZ = BIG_NUMBER;
    float MaximumSurfaceZ = -BIG_NUMBER;
    for (const FVector& Point : RandomSurfacePoints)
    {
        MinimumSurfaceZ = FMath::Min(MinimumSurfaceZ, Point.Z);
        MaximumSurfaceZ = FMath::Max(MaximumSurfaceZ, Point.Z);
    }
    const float SurfaceHeightRange =
        MaximumSurfaceZ - MinimumSurfaceZ;
    const float MinimumHeightSeparation = FMath::Max(
        GetDefault<UHellRunTraversalNavigationSettings>()->VoxelSize
            * 2.0f,
        150.0f);
    const float LowBandMaximum =
        MinimumSurfaceZ + SurfaceHeightRange * 0.25f;
    const float HighBandMinimum =
        MaximumSurfaceZ - SurfaceHeightRange * 0.25f;
    TArray<FVector> LowSurfacePoints;
    TArray<FVector> HighSurfacePoints;
    for (const FVector& Point : RandomSurfacePoints)
    {
        if (Point.Z <= LowBandMaximum)
        {
            LowSurfacePoints.Add(Point);
        }
        if (Point.Z >= HighBandMinimum)
        {
            HighSurfacePoints.Add(Point);
        }
    }
    TestTrue(
        TEXT("dataset spans at least two voxel heights"),
        SurfaceHeightRange >= MinimumHeightSeparation);
    TestTrue(
        TEXT("dataset has enough low-elevation surface samples"),
        LowSurfacePoints.Num() >= 8);
    TestTrue(
        TEXT("dataset has enough high-elevation surface samples"),
        HighSurfacePoints.Num() >= 8);

    FRandomStream Random(RandomSeed);
    for (const FRandomAgentProfile& Profile : RandomProfiles)
    {
        ProbeCapabilities->bCanWalkNavigation = Profile.bCanWalk;
        ProbeCapabilities->bCanClimbNavigation =
            Profile.bCanWallClimb
            || Profile.bCanJump
            || Profile.bCanVault
            || Profile.bCanMantle
            || Profile.bCanDrop;
        ProbeCapabilities->bCanWallClimbNavigation = Profile.bCanWallClimb;
        ProbeCapabilities->bCanFlyNavigation = Profile.bCanFly;
        ProbeCapabilities->bPreferFlyingNavigation = Profile.bPreferFly;
        ProbeCapabilities->bCanJumpNavigation = Profile.bCanJump;
        ProbeCapabilities->bCanVaultNavigation = Profile.bCanVault;
        ProbeCapabilities->bCanMantleNavigation = Profile.bCanMantle;
        ProbeCapabilities->bCanDropNavigation = Profile.bCanDrop;
        ProbeCapabilities->bOverrideVoxelCostProfile = true;
        ProbeCapabilities->VoxelCostProfileOverride = Profile.Costs;

        int32 CompleteQueries = 0;
        int32 EscapableCompleteQueries = 0;
        int32 LowToHighCompleteQueries = 0;
        int32 HighToLowCompleteQueries = 0;
        int32 ProfileModeCounts[7] = {};
        for (int32 QueryIndex = 0;
            QueryIndex < RandomQueriesPerProfile
                && RandomSurfacePoints.Num() >= 2;
            ++QueryIndex)
        {
            constexpr int32 HeightQueriesPerDirection = 8;
            const bool bLowToHigh =
                QueryIndex < HeightQueriesPerDirection;
            const bool bHighToLow =
                QueryIndex >= HeightQueriesPerDirection
                && QueryIndex < HeightQueriesPerDirection * 2;
            const TArray<FVector>& StartPool =
                RandomSurfacePoints;
            const TArray<FVector>& GoalPool =
                RandomSurfacePoints;
            if (StartPool.IsEmpty() || GoalPool.IsEmpty())
            {
                continue;
            }
            int32 StartIndex =
                Random.RandRange(0, StartPool.Num() - 1);
            int32 GoalIndex =
                Random.RandRange(0, GoalPool.Num() - 1);
            if (bLowToHigh || bHighToLow)
            {
                for (int32 Retry = 0; Retry < 256; ++Retry)
                {
                    const float HeightDelta =
                        GoalPool[GoalIndex].Z
                        - StartPool[StartIndex].Z;
                    const bool bHeightPairFound = bLowToHigh
                        ? HeightDelta >= MinimumHeightSeparation
                        : HeightDelta <= -MinimumHeightSeparation;
                    if (bHeightPairFound)
                    {
                        break;
                    }
                    StartIndex = Random.RandRange(
                        0, StartPool.Num() - 1);
                    GoalIndex = Random.RandRange(
                        0, GoalPool.Num() - 1);
                }
            }
            for (int32 Retry = 0;
                Retry < 8
                    && &StartPool == &GoalPool
                    && GoalIndex == StartIndex;
                ++Retry)
            {
                GoalIndex =
                    Random.RandRange(0, GoalPool.Num() - 1);
            }
            if (&StartPool == &GoalPool
                && GoalIndex == StartIndex)
            {
                GoalIndex = (StartIndex + 1) % GoalPool.Num();
            }

            const FVector RandomStart = StartPool[StartIndex];
            const FVector RandomGoal = GoalPool[GoalIndex];
            const TCHAR* HeightDirection = bLowToHigh
                ? TEXT("low-to-high")
                : (bHighToLow
                    ? TEXT("high-to-low")
                    : TEXT("unrestricted"));
            const FString QueryName = FString::Printf(
                TEXT("random seed=%d profile=%s height=%s query=%d startIndex=%d goalIndex=%d"),
                RandomSeed,
                Profile.Name,
                HeightDirection,
                QueryIndex,
                StartIndex,
                GoalIndex);
            if (bLowToHigh || bHighToLow)
            {
                TestTrue(
                    QueryName
                        + TEXT(" has explicit height separation"),
                    FMath::Abs(RandomGoal.Z - RandomStart.Z)
                        >= MinimumHeightSeparation);
            }
            const FNavPathSharedPtr First =
                FHellRunVoxelNavigation::FindPathFrom(
                    *Sources[0], RandomStart, RandomGoal);
            const FString FirstDiagnostic =
                FHellRunVoxelNavigation::GetLastQueryDiagnostic();
            const FNavPathSharedPtr Repeat =
                FHellRunVoxelNavigation::FindPathFrom(
                    *Sources[0], RandomStart, RandomGoal);
            const FString RepeatDiagnostic =
                FHellRunVoxelNavigation::GetLastQueryDiagnostic();

            TestEqual(
                QueryName + TEXT(" deterministic signature"),
                PathSignature(First),
                PathSignature(Repeat));
            const bool bComplete = First.IsValid()
                && First->IsValid()
                && !First->IsPartial()
                && First->GetPathPoints().Num() > 1;
            const bool bRepeatComplete = Repeat.IsValid()
                && Repeat->IsValid()
                && !Repeat->IsPartial()
                && Repeat->GetPathPoints().Num() > 1;
            TestEqual(
                QueryName + TEXT(" deterministic completion"),
                bComplete,
                bRepeatComplete);
            if (!bComplete)
            {
                AddInfo(FString::Printf(
                    TEXT("%s unreachable first=[%s] repeat=[%s] start=%s goal=%s"),
                    *QueryName,
                    *FirstDiagnostic,
                    *RepeatDiagnostic,
                    *RandomStart.ToCompactString(),
                    *RandomGoal.ToCompactString()));
                continue;
            }

            ++CompleteQueries;
            const FNavPathSharedPtr EscapePath =
                FHellRunVoxelNavigation::FindPathFrom(
                    *Sources[0], RandomGoal, RandomStart);
            const bool bEscapeComplete =
                EscapePath.IsValid()
                && EscapePath->IsValid()
                && !EscapePath->IsPartial()
                && EscapePath->GetPathPoints().Num() > 1;
            TestTrue(
                QueryName
                    + TEXT(" completed destination has an outbound escape route"),
                bEscapeComplete);
            EscapableCompleteQueries += bEscapeComplete ? 1 : 0;
            LowToHighCompleteQueries += bLowToHigh ? 1 : 0;
            HighToLowCompleteQueries += bHighToLow ? 1 : 0;
            ValidateTypedSegments(
                *this, QueryName, First, ProfileModeCounts);
            ValidateWalkCorridors(
                *this, QueryName, *Sources[0], First);
            const TArray<FNavPathPoint>& RandomPathPoints =
                First->GetPathPoints();
            const float RandomVoxelSize =
                GetDefault<UHellRunTraversalNavigationSettings>()
                    ->VoxelSize;
            for (int32 PointIndex = 1;
                PointIndex < RandomPathPoints.Num();
                ++PointIndex)
            {
                if (HellRunVoxelPath::GetMode(
                        RandomPathPoints[PointIndex].Flags)
                        == EHellRunVoxelSegment::Walk
                    && FVector::Dist2D(
                        RandomPathPoints[PointIndex - 1].Location,
                        RandomPathPoints[PointIndex].Location)
                        > RandomVoxelSize * 1.05f)
                {
                    bObservedSmoothedWalkCorridor = true;
                    break;
                }
            }
        }

        const int32 MinimumCompleteQueries =
            RandomQueriesPerProfile / 2;
        TestTrue(
            FString::Printf(
                TEXT("random profile %s completes at least %d/%d surface queries"),
                Profile.Name,
                MinimumCompleteQueries,
                RandomQueriesPerProfile),
            CompleteQueries >= MinimumCompleteQueries);
        TestEqual(
            FString::Printf(
                TEXT("random profile %s has no completed routes into trap regions"),
                Profile.Name),
            EscapableCompleteQueries,
            CompleteQueries);
        TestTrue(
            FString::Printf(
                TEXT("random profile %s completes a low-to-high route"),
                Profile.Name),
            LowToHighCompleteQueries > 0);
        TestTrue(
            FString::Printf(
                TEXT("random profile %s completes a high-to-low route"),
                Profile.Name),
            HighToLowCompleteQueries > 0);
        TestTrue(
            FString::Printf(
                TEXT("random profile %s produces its primary locomotion mode"),
                Profile.Name),
            Profile.bCanFly
                ? ProfileModeCounts[
                    static_cast<int32>(EHellRunVoxelSegment::Fly)] > 0
                : Profile.bCanWallClimb
                    ? (ProfileModeCounts[
                        static_cast<int32>(EHellRunVoxelSegment::Climb)] > 0
                        || ProfileModeCounts[
                            static_cast<int32>(EHellRunVoxelSegment::Mantle)] > 0)
                    : ProfileModeCounts[
                        static_cast<int32>(EHellRunVoxelSegment::Walk)] > 0);
        AddInfo(FString::Printf(
            TEXT("Random profile summary: seed=%d profile=%s complete=%d/%d ")
            TEXT("height=[up:%d/8 down:%d/8 range:%.1fcm] ")
            TEXT("modes=[walk:%d climb:%d mantle:%d drop:%d fly:%d jump:%d vault:%d]"),
            RandomSeed,
            Profile.Name,
            CompleteQueries,
            RandomQueriesPerProfile,
            LowToHighCompleteQueries,
            HighToLowCompleteQueries,
            SurfaceHeightRange,
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Walk)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Climb)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Mantle)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Drop)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Jump)],
            ProfileModeCounts[static_cast<int32>(EHellRunVoxelSegment::Vault)]));
    }
    ProbeCapabilities->bCanWalkNavigation = SavedCanWalk;
    ProbeCapabilities->bCanClimbNavigation = SavedCanClimb;
    ProbeCapabilities->bCanWallClimbNavigation = SavedCanWallClimb;
    ProbeCapabilities->bCanFlyNavigation = SavedCanFly;
    ProbeCapabilities->bPreferFlyingNavigation = SavedPreferFly;
    ProbeCapabilities->bCanMantleNavigation = SavedCanMantle;
    ProbeCapabilities->bCanDropNavigation = SavedCanDrop;
    ProbeCapabilities->bCanJumpNavigation = SavedCanJump;
    ProbeCapabilities->bCanVaultNavigation = SavedCanVault;
    ProbeCapabilities->bOverrideVoxelCostProfile = SavedOverride;
    ProbeCapabilities->VoxelCostProfileOverride = SavedProfile;

    TestTrue(
        TEXT("open walk corridor is string-pulled beyond one voxel"),
        bObservedSmoothedWalkCorridor);
    TestTrue(
        TEXT("legacy placed debug pawns draw at least one normalized editor route"),
        CompleteEditorNormalizedQueries > 0);

    AddInfo(FString::Printf(
        TEXT("Dataset summary: debugComplete=%d/%d editorNormalized=%d/%d sampledStable=%d/%d nodes=%d ")
        TEXT("modes=[walk:%d climb:%d mantle:%d drop:%d fly:%d jump:%d vault:%d]"),
        CompleteDebugQueries, Sources.Num(),
        CompleteEditorNormalizedQueries, Sources.Num(), StableSampleQueries,
        Samples.Num() / 2, Volume->GetBakedNodeCount(),
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Walk)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Climb)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Mantle)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Drop)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Fly)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Jump)],
        ModeCounts[static_cast<int32>(EHellRunVoxelSegment::Vault)]));

    const EHellRunVoxelSegment RequiredModes[] = {
        EHellRunVoxelSegment::Walk,
        EHellRunVoxelSegment::Jump,
        EHellRunVoxelSegment::Vault,
        EHellRunVoxelSegment::Mantle,
        EHellRunVoxelSegment::Drop
    };
    for (const EHellRunVoxelSegment RequiredMode : RequiredModes)
    {
        const int32 ModeIndex = static_cast<int32>(RequiredMode);
        if (!AvailableProbeModes[ModeIndex])
        {
            continue;
        }
        TestTrue(
            FString::Printf(
                TEXT("dataset safely exercises or rejects available traversal mode %d"),
                ModeIndex),
            ModeCounts[ModeIndex] > 0
                || RejectedTrapProbeModes[ModeIndex]);
    }
    return !HasAnyErrors();
}

#endif
