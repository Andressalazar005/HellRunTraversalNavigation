#include "HellRunVoxelNavigation.h"

#include "AIController.h"
#include "AITypes.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "HellRunNavigationDebugLog.h"
#include "HellRunTraversalComponent.h"
#include "HellRunVoxelNavVolume.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"

namespace HellRunVoxelNavigationPrivate
{
    static FString LastQueryDiagnostic;

    static float EstimateTraversalSeconds(const ACharacter& Character, const FNavPathSharedPtr& Path)
    {
        if (!Path.IsValid() || !Path->IsValid() || Path->GetPathPoints().Num() < 2) return BIG_NUMBER;
        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
        const UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
        const float WalkSpeed = FMath::Max(1.0f, Movement ? Movement->MaxWalkSpeed : 700.0f);
        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
        float Seconds = 0.0f;
        EHellRunVoxelSegment PreviousMode = EHellRunVoxelSegment::Walk;
        for (int32 PointIndex = 1; PointIndex < Points.Num(); ++PointIndex)
        {
            const FVector Delta = Points[PointIndex].Location - Points[PointIndex - 1].Location;
            const EHellRunVoxelSegment Mode = HellRunVoxelPath::IsVoxelFlags(Points[PointIndex].Flags)
                ? HellRunVoxelPath::GetMode(Points[PointIndex].Flags) : EHellRunVoxelSegment::Walk;
            switch (Mode)
            {
            case EHellRunVoxelSegment::Drop:
            {
                const float HorizontalTime = Delta.Size2D() / FMath::Max(1.0f, Settings->DropMinimumHorizontalSpeed);
                const float Gravity = FMath::Abs(Movement ? Movement->GetGravityZ() : -980.0f);
                const float FallTime = Delta.Z < 0.0f
                    ? FMath::Sqrt(2.0f * FMath::Abs(Delta.Z) / FMath::Max(1.0f, Gravity)) : 0.0f;
                Seconds += FMath::Max(HorizontalTime, FallTime);
                break;
            }
            case EHellRunVoxelSegment::Jump:
                Seconds += Delta.Size() / FMath::Max(1.0f, Settings->JumpSpeed);
                break;
            case EHellRunVoxelSegment::Vault:
                Seconds += Delta.Size() / FMath::Max(1.0f, Settings->VaultSpeed);
                break;
            case EHellRunVoxelSegment::Mantle:
                Seconds += FMath::Abs(Delta.Z) / FMath::Max(1.0f, Settings->MantleVerticalSpeed)
                    + Delta.Size2D() / FMath::Max(1.0f, Settings->MantlePullOverSpeed);
                break;
            case EHellRunVoxelSegment::Climb:
                Seconds += FMath::Abs(Delta.Z) / FMath::Max(1.0f, Settings->ClimbVerticalSpeed)
                    + Delta.Size2D() / FMath::Max(1.0f, Settings->ClimbPullOverSpeed);
                break;
            case EHellRunVoxelSegment::Fly:
                Seconds += Delta.Size() / FMath::Max(1.0f, Settings->FlightSpeed);
                break;
            default:
                Seconds += Delta.Size() / WalkSpeed;
                break;
            }
            if (Mode != PreviousMode) Seconds += 0.08f;
            PreviousMode = Mode;
        }
        if (Path->ContainsAnyCustomLink()) Seconds *= 1.15f;
        return Seconds;
    }

    static FNavPathSharedPtr FindGroundSection(ACharacter& Character, const FVector& Start, const FVector& End)
    {
        UNavigationSystemV1* NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Character.GetWorld());
        AAIController* Controller = Cast<AAIController>(Character.GetController());
        if (!NavSystem || !Controller) return nullptr;

        // Baked ground nodes are stored at capsule-center height, while Recast
        // path points lie on the navmesh surface. Passing the baked coordinates
        // directly into FPathFindingQuery can therefore fail even though the
        // entire walk run is over valid navmesh. Project only the endpoints of
        // the walk run; typed traversal endpoints remain untouched.
        const FVector ProjectionExtent(120.0f, 120.0f, 160.0f);
        FNavLocation ProjectedStart;
        FNavLocation ProjectedEnd;
        if (!NavSystem->ProjectPointToNavigation(
                Start, ProjectedStart, ProjectionExtent, &Controller->GetNavAgentPropertiesRef())
            || !NavSystem->ProjectPointToNavigation(
                End, ProjectedEnd, ProjectionExtent, &Controller->GetNavAgentPropertiesRef()))
        {
            return nullptr;
        }

        const UHellRunTraversalNavigationSettings* Settings =
            GetDefault<UHellRunTraversalNavigationSettings>();
        const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
        const float HalfHeight = Capsule
            ? Capsule->GetScaledCapsuleHalfHeight() : Settings->VoxelBakeAgentHalfHeight;
        const float MaximumLayerError = Settings->GroundStepHeight + Settings->VoxelGroundClearance;
        const float StartLayerError = FMath::Abs(
            ProjectedStart.Location.Z + HalfHeight + Settings->VoxelGroundClearance - Start.Z);
        const float EndLayerError = FMath::Abs(
            ProjectedEnd.Location.Z + HalfHeight + Settings->VoxelGroundClearance - End.Z);
        if (StartLayerError > MaximumLayerError || EndLayerError > MaximumLayerError)
        {
            FHellRunNavigationDebugLog::Write(&Character, TEXT("HYBRID_PROJECTION_REJECTED"),
                FString::Printf(TEXT("startError=%.1f endError=%.1f maxError=%.1f start=%s projectedStart=%s end=%s projectedEnd=%s"),
                    StartLayerError, EndLayerError, MaximumLayerError,
                    *Start.ToCompactString(), *ProjectedStart.Location.ToCompactString(),
                    *End.ToCompactString(), *ProjectedEnd.Location.ToCompactString()));
            return nullptr;
        }

        const ANavigationData* NavData = NavSystem->GetNavDataForProps(
            Controller->GetNavAgentPropertiesRef(), ProjectedStart.Location);
        if (!NavData) return nullptr;

        FPathFindingQuery Query(Controller, *NavData, ProjectedStart.Location, ProjectedEnd.Location);
        const FPathFindingResult Result = NavSystem->FindPathSync(Query, EPathFindingMode::Regular);
        if (!Result.IsSuccessful() || !Result.Path.IsValid() || Result.Path->IsPartial()
            || Result.Path->ContainsAnyCustomLink())
        {
            return nullptr;
        }

        UWorld* World = Character.GetWorld();
        if (!Capsule || !World) return nullptr;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunHybridGroundCorridor), false, &Character);
        const FCollisionShape Shape = FCollisionShape::MakeCapsule(
            Capsule->GetScaledCapsuleRadius(), HalfHeight);
        const TArray<FNavPathPoint>& GroundPoints = Result.Path->GetPathPoints();
        for (int32 PointIndex = 1; PointIndex < GroundPoints.Num(); ++PointIndex)
        {
            const float CenterHeight = HalfHeight + Settings->VoxelGroundClearance;
            const FVector From = GroundPoints[PointIndex - 1].Location + FVector::UpVector * CenterHeight;
            const FVector To = GroundPoints[PointIndex].Location + FVector::UpVector * CenterHeight;
            if (World->SweepTestByObjectType(From, To, FQuat::Identity, Objects, Shape, Params))
            {
                FHellRunNavigationDebugLog::Write(&Character, TEXT("HYBRID_CORRIDOR_REJECTED"),
                    FString::Printf(TEXT("segment=%d from=%s to=%s"),
                        PointIndex - 1, *From.ToCompactString(), *To.ToCompactString()));
                return nullptr;
            }
        }
        return Result.Path;
    }

    /**
     * The voxel graph chooses topology and typed traversal edges. It must not
     * drive ordinary walking cell-by-cell: cardinal A* creates Manhattan
     * zigzags and bypasses Recast corridor/string-pulling. Replace every
     * contiguous Walk run with a real ground-nav path while retaining the
     * voxel flags needed by the typed traversal follower.
     */
    static bool ReplaceVoxelWalkRunsWithGroundPaths(ACharacter& Character, const FNavPathSharedPtr& Path)
    {
        if (!Path.IsValid() || Path->GetPathPoints().Num() < 2) return false;
        const TArray<FNavPathPoint> VoxelPoints = Path->GetPathPoints();
        TArray<FNavPathPoint> HybridPoints;
        HybridPoints.Reserve(VoxelPoints.Num());
        HybridPoints.Add(VoxelPoints[0]);
        HybridPoints[0].Flags = HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);

        int32 PointIndex = 1;
        while (PointIndex < VoxelPoints.Num())
        {
            const EHellRunVoxelSegment Mode = HellRunVoxelPath::GetMode(VoxelPoints[PointIndex].Flags);
            if (Mode != EHellRunVoxelSegment::Walk)
            {
                HybridPoints.Add(VoxelPoints[PointIndex++]);
                continue;
            }

            int32 RunEnd = PointIndex;
            while (RunEnd + 1 < VoxelPoints.Num()
                && HellRunVoxelPath::GetMode(VoxelPoints[RunEnd + 1].Flags) == EHellRunVoxelSegment::Walk)
            {
                ++RunEnd;
            }

            const FVector GroundStart = HybridPoints.Last().Location;
            const FVector GroundEnd = VoxelPoints[RunEnd].Location;
            const FNavPathSharedPtr GroundSection = FindGroundSection(Character, GroundStart, GroundEnd);
            if (!GroundSection.IsValid() || GroundSection->GetPathPoints().Num() < 2)
            {
                FHellRunNavigationDebugLog::Write(&Character, TEXT("HYBRID_GROUND_FAILED"), FString::Printf(
                    TEXT("start=%s end=%s voxelRunPoints=%d fallback=VOXEL_WALK"), *GroundStart.ToCompactString(),
                    *GroundEnd.ToCompactString(), RunEnd - PointIndex + 1));
                // Recast refinement is an optimization, not a validity gate.
                // Preserve the graph's walk run so a failed ground subsection
                // cannot erase valid mantle/jump edges elsewhere in the route.
                for (int32 VoxelIndex = PointIndex; VoxelIndex <= RunEnd; ++VoxelIndex)
                {
                    FNavPathPoint WalkPoint = VoxelPoints[VoxelIndex];
                    WalkPoint.Flags = HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);
                    if (!HybridPoints.Last().Location.Equals(WalkPoint.Location, 1.0f))
                    {
                        HybridPoints.Add(WalkPoint);
                    }
                }
                PointIndex = RunEnd + 1;
                continue;
            }

            const TArray<FNavPathPoint>& GroundPoints = GroundSection->GetPathPoints();
            for (int32 GroundIndex = 1; GroundIndex < GroundPoints.Num(); ++GroundIndex)
            {
                FNavPathPoint GroundPoint = GroundPoints[GroundIndex];
                GroundPoint.Flags = HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);
                if (!HybridPoints.Last().Location.Equals(GroundPoint.Location, 1.0f))
                {
                    HybridPoints.Add(GroundPoint);
                }
            }
            PointIndex = RunEnd + 1;
        }

        FHellRunNavigationDebugLog::Write(&Character, TEXT("HYBRID_PATH_BUILT"), FString::Printf(
            TEXT("voxelPoints=%d hybridPoints=%d"), VoxelPoints.Num(), HybridPoints.Num()));
        Path->GetPathPoints() = MoveTemp(HybridPoints);
        Path->MarkReady();
        return Path->GetPathPoints().Num() > 1;
    }

    FString PathFollowingFlagsToString(FPathFollowingResultFlags::Type Flags)
    {
        TArray<FString> Names;
        if (Flags & FPathFollowingResultFlags::Success) Names.Add(TEXT("Success"));
        if (Flags & FPathFollowingResultFlags::Blocked) Names.Add(TEXT("Blocked"));
        if (Flags & FPathFollowingResultFlags::OffPath) Names.Add(TEXT("OffPath"));
        if (Flags & FPathFollowingResultFlags::UserAbort) Names.Add(TEXT("UserAbort"));
        if (Flags & FPathFollowingResultFlags::OwnerFinished) Names.Add(TEXT("OwnerFinished"));
        if (Flags & FPathFollowingResultFlags::InvalidPath) Names.Add(TEXT("InvalidPath"));
        if (Flags & FPathFollowingResultFlags::MovementStop) Names.Add(TEXT("MovementStop"));
        if (Flags & FPathFollowingResultFlags::NewRequest) Names.Add(TEXT("NewRequest"));
        if (Flags & FPathFollowingResultFlags::ForcedScript) Names.Add(TEXT("ForcedScript"));
        if (Flags & FPathFollowingResultFlags::AlreadyAtGoal) Names.Add(TEXT("AlreadyAtGoal"));
        return Names.IsEmpty() ? TEXT("None") : FString::Join(Names, TEXT("|"));
    }

    static void SetNavigationMode(const TArray<FString>& Args)
    {
        if (Args.IsEmpty())
        {
            UE_LOG(LogTemp, Display, TEXT("HellRun navigation mode: %s"),
            GetDefault<UHellRunTraversalNavigationSettings>()->NavigationMode == EHellRunNavigationMode::VolumetricHybrid
                    ? TEXT("VolumetricHybrid") : (GetDefault<UHellRunTraversalNavigationSettings>()->NavigationMode == EHellRunNavigationMode::AutomaticHybrid
                    ? TEXT("AutomaticHybrid") : TEXT("GeneratedLinks")));
            return;
        }
        UHellRunTraversalNavigationSettings* Settings = GetMutableDefault<UHellRunTraversalNavigationSettings>();
        Settings->NavigationMode = Args[0].Equals(TEXT("auto"), ESearchCase::IgnoreCase)
            || Args[0].Equals(TEXT("automatic"), ESearchCase::IgnoreCase) || Args[0] == TEXT("2")
            ? EHellRunNavigationMode::AutomaticHybrid
            : (Args[0].Equals(TEXT("voxel"), ESearchCase::IgnoreCase)
            || Args[0].Equals(TEXT("volumetric"), ESearchCase::IgnoreCase) || Args[0] == TEXT("1")
            ? EHellRunNavigationMode::VolumetricHybrid : EHellRunNavigationMode::GeneratedLinks);
        UE_LOG(LogTemp, Display, TEXT("HellRun navigation mode set to %s"),
            Settings->NavigationMode == EHellRunNavigationMode::VolumetricHybrid ? TEXT("VolumetricHybrid")
            : (Settings->NavigationMode == EHellRunNavigationMode::AutomaticHybrid ? TEXT("AutomaticHybrid") : TEXT("GeneratedLinks")));
    }

    static void ToggleVoxelDebug()
    {
        UHellRunTraversalNavigationSettings* Settings = GetMutableDefault<UHellRunTraversalNavigationSettings>();
        Settings->bDrawVoxelSearch = !Settings->bDrawVoxelSearch;
        UE_LOG(LogTemp, Display, TEXT("HellRun voxel navigation debug: %s"), Settings->bDrawVoxelSearch ? TEXT("ON") : TEXT("OFF"));
    }

    static FAutoConsoleCommand SetModeCommand(TEXT("HellRun.NavigationMode"),
        TEXT("HellRun.NavigationMode [links|voxel|auto]"), FConsoleCommandWithArgsDelegate::CreateStatic(&SetNavigationMode));
    static FAutoConsoleCommand ToggleDebugCommand(TEXT("HellRun.ToggleVoxelNavigationDebug"),
        TEXT("Toggle volumetric voxel search and selected path drawing."), FConsoleCommandDelegate::CreateStatic(&ToggleVoxelDebug));

    struct FCellInfo
    {
        bool bEvaluated = false;
        bool bFree = false;
        bool bGround = false;
        bool bClimb = false;
        FVector WallNormal = FVector::ZeroVector;
    };

    struct FSearchNode
    {
        FIntVector Cell = FIntVector::ZeroValue;
        int32 Parent = INDEX_NONE;
        float G = BIG_NUMBER;
        float F = BIG_NUMBER;
        EHellRunVoxelSegment ArrivalMode = EHellRunVoxelSegment::Walk;
        bool bClosed = false;
    };

    class FSearch
    {
    public:
        FSearch(ACharacter& InCharacter, const FVector& InGoal)
            : Character(InCharacter), World(*InCharacter.GetWorld()), Goal(InGoal), Settings(*GetDefault<UHellRunTraversalNavigationSettings>())
        {
            const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
            Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 34.0f;
            HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.0f;
            const UHellRunTraversalComponent* Capabilities = Character.FindComponentByClass<UHellRunTraversalComponent>();
            bCanWalk = !Capabilities || Capabilities->CanWalkNavigation();
            bCanClimb = !Capabilities || Capabilities->CanClimbNavigation();
            bCanFly = Capabilities && Capabilities->CanFlyNavigation();
            CellSize = FMath::Max(Settings.VoxelSize, Radius * 1.25f);
            Start = Character.GetActorLocation();
            const FVector Min = Start.ComponentMin(Goal) - FVector(Settings.VoxelSearchPadding);
            const FVector Max = Start.ComponentMax(Goal) + FVector(Settings.VoxelSearchPadding);
            MinCell = ToCell(Min);
            MaxCell = ToCell(Max);
        }

        FNavPathSharedPtr Run()
        {
            const FIntVector StartCell = FindNearestUsableCell(ToCell(Start));
            const FIntVector GoalCell = FindNearestUsableCell(ToCell(Goal));
            if (StartCell == InvalidCell || GoalCell == InvalidCell)
            {
                return nullptr;
            }

            AddOrImprove(StartCell, INDEX_NONE, 0.0f, Heuristic(StartCell, GoalCell), EHellRunVoxelSegment::Walk);
            int32 GoalNode = INDEX_NONE;
            int32 Expanded = 0;
            while (Open.Num() > 0 && Expanded++ < Settings.MaximumVoxelSearchNodes)
            {
                const int32 CurrentIndex = PopOpen();
                FSearchNode& Current = Nodes[CurrentIndex];
                if (Current.bClosed) continue;
                Current.bClosed = true;
                if (Current.Cell == GoalCell)
                {
                    GoalNode = CurrentIndex;
                    break;
                }
                Expand(CurrentIndex, GoalCell);
            }

            if (GoalNode == INDEX_NONE) return nullptr;
            return BuildPath(GoalNode);
        }

    private:
        static inline const FIntVector InvalidCell = FIntVector(MAX_int32);

        FIntVector ToCell(const FVector& P) const
        {
            return FIntVector(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize), FMath::FloorToInt(P.Z / CellSize));
        }

        FVector ToWorld(const FIntVector& C) const
        {
            return FVector((C.X + 0.5f) * CellSize, (C.Y + 0.5f) * CellSize, (C.Z + 0.5f) * CellSize);
        }

        bool InBounds(const FIntVector& C) const
        {
            return C.X >= MinCell.X && C.Y >= MinCell.Y && C.Z >= MinCell.Z
                && C.X <= MaxCell.X && C.Y <= MaxCell.Y && C.Z <= MaxCell.Z;
        }

        const FCellInfo& Evaluate(const FIntVector& C)
        {
            FCellInfo& Info = Cells.FindOrAdd(C);
            if (Info.bEvaluated) return Info;
            Info.bEvaluated = true;
            const FVector Center = ToWorld(C);
            FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunVoxelOccupancy), false, &Character);
            FCollisionObjectQueryParams Objects;
            Objects.AddObjectTypesToQuery(ECC_WorldStatic);
            Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
            Info.bFree = !World.OverlapAnyTestByObjectType(Center, FQuat::Identity, Objects,
                FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);
            if (!Info.bFree) return Info;

            FHitResult FloorHit;
            Info.bGround = World.LineTraceSingleByObjectType(FloorHit, Center,
                Center - FVector(0, 0, HalfHeight + Settings.VoxelFloorProbeDepth), Objects, Params)
                && FloorHit.ImpactNormal.Z >= 0.55f;

            const FVector Directions[] = { FVector::ForwardVector, FVector::BackwardVector, FVector::RightVector, FVector::LeftVector };
            float BestDistance = BIG_NUMBER;
            for (const FVector& Direction : Directions)
            {
                FHitResult WallHit;
                if (World.LineTraceSingleByObjectType(WallHit, Center, Center + Direction * (Radius + Settings.ClimbSurfaceProbeDistance), Objects, Params)
                    && FMath::Abs(WallHit.ImpactNormal.Z) < 0.35f
                    && (!WallHit.GetActor() || !WallHit.GetActor()->ActorHasTag(Settings.NoClimbActorTag))
                    && WallHit.Distance < BestDistance)
                {
                    BestDistance = WallHit.Distance;
                    Info.bClimb = true;
                    Info.WallNormal = WallHit.ImpactNormal.GetSafeNormal2D();
                }
            }

            if (Settings.bDrawVoxelSearch)
            {
                const FColor Color = Info.bGround ? FColor::Cyan : (Info.bClimb ? FColor(145, 70, 255) : FColor(70, 70, 70));
                DrawDebugBox(&World, Center, FVector(CellSize * 0.18f), Color, false, Settings.VoxelDebugLifetime, 2, 1.0f);
            }
            return Info;
        }

        FIntVector FindNearestUsableCell(const FIntVector& Center)
        {
            for (int32 R = 0; R <= 3; ++R)
            {
                for (int32 Z = -R; Z <= R; ++Z)
                for (int32 Y = -R; Y <= R; ++Y)
                for (int32 X = -R; X <= R; ++X)
                {
                    const FIntVector Candidate = Center + FIntVector(X, Y, Z);
                    if (InBounds(Candidate))
                    {
                        const FCellInfo& Info = Evaluate(Candidate);
                        if (Info.bFree && ((bCanWalk && Info.bGround) || (bCanClimb && Info.bClimb) || bCanFly)) return Candidate;
                    }
                }
            }
            return InvalidCell;
        }

        float Heuristic(const FIntVector& A, const FIntVector& B) const
        {
            return FVector::Distance(ToWorld(A), ToWorld(B)) / CellSize;
        }

        void AddOrImprove(const FIntVector& Cell, int32 Parent, float G, float H, EHellRunVoxelSegment Mode)
        {
            int32* ExistingIndex = NodeIndices.Find(Cell);
            if (!ExistingIndex)
            {
                const int32 Index = Nodes.Add({Cell, Parent, G, G + H, Mode, false});
                NodeIndices.Add(Cell, Index);
                PushOpen(Index);
            }
            else if (!Nodes[*ExistingIndex].bClosed && G < Nodes[*ExistingIndex].G)
            {
                FSearchNode& Node = Nodes[*ExistingIndex];
                Node.Parent = Parent;
                Node.G = G;
                Node.F = G + H;
                Node.ArrivalMode = Mode;
                PushOpen(*ExistingIndex);
            }
        }

        void PushOpen(int32 NodeIndex)
        {
            int32 Position = Open.Add(NodeIndex);
            while (Position > 0)
            {
                const int32 ParentPosition = (Position - 1) / 2;
                if (Nodes[Open[ParentPosition]].F <= Nodes[Open[Position]].F) break;
                Open.Swap(ParentPosition, Position);
                Position = ParentPosition;
            }
        }

        int32 PopOpen()
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
                if (Open.IsValidIndex(Right) && Nodes[Open[Right]].F < Nodes[Open[Left]].F) Best = Right;
                if (Nodes[Open[Position]].F <= Nodes[Open[Best]].F) break;
                Open.Swap(Position, Best);
                Position = Best;
            }
            return Result;
        }

        void Expand(int32 CurrentIndex, const FIntVector& GoalCell)
        {
            const FSearchNode Current = Nodes[CurrentIndex];
            const FCellInfo& CurrentInfo = Evaluate(Current.Cell);
            static const FIntVector Directions[] = {
                FIntVector(1,0,0), FIntVector(-1,0,0), FIntVector(0,1,0), FIntVector(0,-1,0),
                FIntVector(1,1,0), FIntVector(1,-1,0), FIntVector(-1,1,0), FIntVector(-1,-1,0),
                FIntVector(0,0,1), FIntVector(0,0,-1)
            };
            for (const FIntVector& Delta : Directions)
            {
                const FIntVector NextCell = Current.Cell + Delta;
                if (!InBounds(NextCell)) continue;
                const FCellInfo& NextInfo = Evaluate(NextCell);
                if (!NextInfo.bFree || (!(bCanWalk && NextInfo.bGround) && !(bCanClimb && NextInfo.bClimb))) continue;

                EHellRunVoxelSegment Mode;
                float Cost;
                if (Delta.Z != 0)
                {
                    if (!bCanClimb || !CurrentInfo.bClimb || !NextInfo.bClimb || FVector::DotProduct(CurrentInfo.WallNormal, NextInfo.WallNormal) < 0.25f) continue;
                    Mode = EHellRunVoxelSegment::Climb;
                    Cost = Settings.VoxelClimbCost;
                }
                else if (bCanWalk && CurrentInfo.bGround && NextInfo.bGround)
                {
                    Mode = EHellRunVoxelSegment::Walk;
                    Cost = Settings.VoxelWalkCost * (Delta.X != 0 && Delta.Y != 0 ? 1.4142f : 1.0f);
                }
                else if (bCanClimb && CurrentInfo.bClimb && NextInfo.bClimb)
                {
                    Mode = EHellRunVoxelSegment::Climb;
                    Cost = Settings.VoxelClimbCost;
                }
                else
                {
                    Mode = EHellRunVoxelSegment::Mantle;
                    Cost = Settings.VoxelMantleCost;
                }
                AddOrImprove(NextCell, CurrentIndex, Current.G + Cost, Heuristic(NextCell, GoalCell), Mode);
            }

            // Pull over the top of a wall: step up, then inward across the wall plane.
            if (bCanClimb && CurrentInfo.bClimb)
            {
                const FIntVector Across(FMath::RoundToInt(-CurrentInfo.WallNormal.X), FMath::RoundToInt(-CurrentInfo.WallNormal.Y), 1);
                const FIntVector MantleCell = Current.Cell + Across;
                if (InBounds(MantleCell))
                {
                    const FCellInfo& MantleInfo = Evaluate(MantleCell);
                    if (MantleInfo.bFree && MantleInfo.bGround)
                    {
                        AddOrImprove(MantleCell, CurrentIndex, Current.G + Settings.VoxelMantleCost,
                            Heuristic(MantleCell, GoalCell), EHellRunVoxelSegment::Mantle);
                    }
                }
            }

            if (bCanFly)
            {
                for (int32 Z = -1; Z <= 1; ++Z)
                for (int32 Y = -1; Y <= 1; ++Y)
                for (int32 X = -1; X <= 1; ++X)
                {
                    if (X == 0 && Y == 0 && Z == 0) continue;
                    const FIntVector NextCell = Current.Cell + FIntVector(X, Y, Z);
                    if (!InBounds(NextCell) || !Evaluate(NextCell).bFree) continue;
                    const float Distance = FVector(X, Y, Z).Size();
                    AddOrImprove(NextCell, CurrentIndex, Current.G + Settings.VoxelFlightCost * Distance,
                        Heuristic(NextCell, GoalCell), EHellRunVoxelSegment::Fly);
                }
            }
        }

        FNavPathSharedPtr BuildPath(int32 GoalNode)
        {
            TArray<int32> Reverse;
            for (int32 Index = GoalNode; Index != INDEX_NONE; Index = Nodes[Index].Parent) Reverse.Add(Index);
            Algo::Reverse(Reverse);
            TArray<FVector> Locations;
            Locations.Add(Start);
            for (int32 Index : Reverse) Locations.Add(ToWorld(Nodes[Index].Cell));
            Locations.Add(Goal);
            FNavPathSharedPtr Path = MakeShared<FNavigationPath>(Locations, nullptr);
            TArray<FNavPathPoint>& Points = Path->GetPathPoints();
            Points[0].Flags = HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);
            for (int32 I = 0; I < Reverse.Num(); ++I)
            {
                Points[I + 1].Flags = HellRunVoxelPath::MakeFlags(Nodes[Reverse[I]].ArrivalMode);
            }
            const FCellInfo& GoalInfo = Evaluate(Nodes[GoalNode].Cell);
            Points.Last().Flags = HellRunVoxelPath::MakeFlags(bCanWalk && GoalInfo.bGround
                ? EHellRunVoxelSegment::Walk : Nodes[GoalNode].ArrivalMode);
            Path->MarkReady();
            if (Settings.bDrawVoxelSearch)
            {
                for (int32 I = 1; I < Points.Num(); ++I)
                {
                    const EHellRunVoxelSegment Mode = HellRunVoxelPath::GetMode(Points[I].Flags);
                    const FColor Color = Mode == EHellRunVoxelSegment::Climb ? FColor(145,70,255)
                        : (Mode == EHellRunVoxelSegment::Mantle ? FColor::Green
                        : (Mode == EHellRunVoxelSegment::Fly ? FColor::Blue : FColor::Cyan));
                    DrawDebugDirectionalArrow(&World, Points[I-1].Location, Points[I].Location, 20.0f, Color,
                        false, Settings.VoxelDebugLifetime, 5, 4.0f);
                }
            }
            return Path;
        }

        ACharacter& Character;
        UWorld& World;
        FVector Start;
        FVector Goal;
        const UHellRunTraversalNavigationSettings& Settings;
        float Radius = 34.0f;
        float HalfHeight = 88.0f;
        float CellSize = 75.0f;
        bool bCanWalk = true;
        bool bCanClimb = true;
        bool bCanFly = false;
        FIntVector MinCell;
        FIntVector MaxCell;
        TMap<FIntVector, FCellInfo> Cells;
        TMap<FIntVector, int32> NodeIndices;
        TArray<FSearchNode> Nodes;
        TArray<int32> Open;
    };

    static FNavPathSharedPtr BuildDirectWalkPath(
        ACharacter& Character,
        const FVector& Start,
        const FVector& Goal,
        const UHellRunTraversalNavigationSettings& Settings,
        FString& OutDiagnostic)
    {
        UWorld* World = Character.GetWorld();
        const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
        const float Distance = FVector::Distance(Start, Goal);
        if (!World || !Capsule || Distance < 1.0f
            || Distance > Settings.VoxelEndpointConnectionRadius)
        {
            OutDiagnostic = TEXT("DIRECT_REJECTED | invalid_context_or_distance");
            return nullptr;
        }

        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        FCollisionQueryParams Params(
            SCENE_QUERY_STAT(HellRunVoxelDirectWalk), false, &Character);
        for (TActorIterator<APawn> It(World); It; ++It)
        {
            Params.AddIgnoredActor(*It);
        }
        const float Radius = Capsule->GetScaledCapsuleRadius();
        const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
        const int32 Samples = FMath::Max(
            2,
            FMath::CeilToInt(
                FVector::Dist2D(Start, Goal)
                / FMath::Max(50.0f, Radius)));
        TArray<FVector> SupportedCenters;
        SupportedCenters.Reserve(Samples + 1);
        for (int32 Sample = 0; Sample <= Samples; ++Sample)
        {
            const FVector RawCenter = FMath::Lerp(
                Start, Goal, static_cast<float>(Sample) / Samples);
            FHitResult Hit;
            if (!World->LineTraceSingleByObjectType(
                    Hit,
                    // Start inside the character's known free space. Starting
                    // a full step-height above the actor can put the probe
                    // above a low safe-room ceiling and misidentify that
                    // ceiling as the supporting floor.
                    RawCenter + FVector::UpVector
                        * FMath::Max(2.0f, Settings.VoxelGroundClearance),
                    RawCenter - FVector::UpVector
                        * (HalfHeight + Settings.VoxelFloorProbeDepth),
                    Objects,
                    Params)
                || Hit.ImpactNormal.Z < 0.55f)
            {
                OutDiagnostic = FString::Printf(
                    TEXT("DIRECT_REJECTED | unsupported_floor sample=%d/%d"),
                    Sample, Samples);
                return nullptr;
            }
            // The floor trace is only proof of support. Some authored room
            // shells use broad collision primitives whose trace starts inside
            // the primitive, so their reported impact point is not a usable
            // floor elevation. The actor and baked-node Z values are the
            // authoritative capsule centers.
            SupportedCenters.Add(RawCenter);
        }

        // Actor origins are not guaranteed to share the same height above the
        // floor (animations, spawn correction, and differing capsule sizes all
        // affect them). Sweep between floor-supported capsule centers so a
        // horizontal route cannot falsely descend into the floor.
        const FCollisionShape Shape = FCollisionShape::MakeCapsule(
            FMath::Max(1.0f, Radius - 2.0f),
            FMath::Max(
                Radius,
                HalfHeight - FMath::Max(
                    8.0f,
                    Settings.VoxelGroundClearance + 4.0f)));
        for (int32 Segment = 1; Segment < SupportedCenters.Num(); ++Segment)
        {
            FHitResult BlockingHit;
            if (World->SweepSingleByChannel(
                    BlockingHit,
                    SupportedCenters[Segment - 1],
                    SupportedCenters[Segment],
                    FQuat::Identity,
                    ECC_Pawn,
                    Shape,
                    Params))
            {
                OutDiagnostic = FString::Printf(
                    TEXT("DIRECT_REJECTED | capsule_sweep_blocked segment=%d/%d from=%s to=%s hit=%s actor=%s component=%s startPenetrating=%d depth=%.2f"),
                    Segment, Samples,
                    *SupportedCenters[Segment - 1].ToCompactString(),
                    *SupportedCenters[Segment].ToCompactString(),
                    *BlockingHit.ImpactPoint.ToCompactString(),
                    *GetNameSafe(BlockingHit.GetActor()),
                    *GetNameSafe(BlockingHit.GetComponent()),
                    BlockingHit.bStartPenetrating,
                    BlockingHit.PenetrationDepth);
                return nullptr;
            }
        }

        TSharedRef<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe> Path =
            MakeShared<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe>();
        float TotalCost = 0.0f;
        for (int32 PointIndex = 0; PointIndex < SupportedCenters.Num(); ++PointIndex)
        {
            FNavPathPoint Point(SupportedCenters[PointIndex]);
            Point.Flags =
                HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);
            Path->GetPathPoints().Add(Point);
            if (PointIndex == 0)
            {
                Path->SegmentCosts.Add(0.0f);
            }
            else
            {
                const float SegmentCost = FVector::Distance(
                    SupportedCenters[PointIndex - 1],
                    SupportedCenters[PointIndex]);
                Path->SegmentCosts.Add(SegmentCost);
                TotalCost += SegmentCost;
            }
        }
        Path->TotalCost = TotalCost;
        Path->MarkReady();
        OutDiagnostic = TEXT("COMPLETE | provider=VOXEL_DIRECT");
        return Path;
    }
}

FNavPathSharedPtr FHellRunVoxelNavigation::FindPath(ACharacter& Character, const FVector& Goal)
{
    return FindPathFrom(Character, Character.GetActorLocation(), Goal);
}

FNavPathSharedPtr FHellRunVoxelNavigation::FindPathFrom(
    ACharacter& Character,
    const FVector& Start,
    const FVector& Goal,
    bool bRequireEscapeRoute)
{
    using namespace HellRunVoxelNavigationPrivate;
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();

    FString DirectDiagnostic;
    FNavPathSharedPtr Result =
        BuildDirectWalkPath(Character, Start, Goal, *S, DirectDiagnostic);
    LastQueryDiagnostic.Reset();
    if (Result.IsValid())
    {
        LastQueryDiagnostic = DirectDiagnostic;
        return Result;
    }
    int32 VolumeCount = 0;
    int32 BakedVolumeCount = 0;
    int32 EndpointVolumeCount = 0;
    if (UWorld* World = Character.GetWorld())
    {
        for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
        {
            ++VolumeCount;
            if (It->HasBakedNavigationData())
            {
                ++BakedVolumeCount;
            }
            if (It->ContainsRouteEndpoints(Start, Goal))
            {
                ++EndpointVolumeCount;
                Result = S->bUseSharedTargetFlowFields
                    ? It->FindSharedPath(Character, Start, Goal)
                    : It->FindPath(Character, Start, Goal);
                if (!Result.IsValid() && S->bUseSharedTargetFlowFields)
                {
                    Result = It->FindPath(Character, Start, Goal);
                }
                LastQueryDiagnostic = It->GetLastPathDiagnostic();
                if (Result.IsValid()
                    && bRequireEscapeRoute
                    && S->bRequireTraversalDestinationEscapeRoute)
                {
                    bool bContainsTypedTraversal = false;
                    for (int32 PointIndex = 1;
                        PointIndex < Result->GetPathPoints().Num();
                        ++PointIndex)
                    {
                        const EHellRunVoxelSegment Mode =
                            HellRunVoxelPath::GetMode(
                                Result->GetPathPoints()[PointIndex].Flags);
                        if (Mode != EHellRunVoxelSegment::Walk
                            && Mode != EHellRunVoxelSegment::Fly)
                        {
                            bContainsTypedTraversal = true;
                            break;
                        }
                    }
                    if (bContainsTypedTraversal)
                    {
                        const FNavPathSharedPtr EscapePath =
                            It->FindPath(Character, Goal, Start);
                        const bool bHasEscape =
                            EscapePath.IsValid()
                            && EscapePath->IsValid()
                            && !EscapePath->IsPartial()
                            && EscapePath->GetPathPoints().Num() > 1;
                        if (!bHasEscape)
                        {
                            LastQueryDiagnostic = FString::Printf(
                                TEXT("TRAP DESTINATION REJECTED | forward=[%s] escape=[%s]"),
                                *LastQueryDiagnostic,
                                *It->GetLastPathDiagnostic());
                            FHellRunNavigationDebugLog::Write(
                                &Character,
                                TEXT("VOXEL_TRAP_ROUTE_REJECTED"),
                                FString::Printf(
                                    TEXT("start=%s goal=%s"),
                                    *Start.ToCompactString(),
                                    *Goal.ToCompactString()));
                            Result.Reset();
                        }
                    }
                }
                if (Result.IsValid()) break;
            }
        }
    }
    if (!Result.IsValid())
    {
        if (!DirectDiagnostic.IsEmpty())
        {
            LastQueryDiagnostic += FString::Printf(
                TEXT(" | direct=[%s]"), *DirectDiagnostic);
        }
        if (LastQueryDiagnostic.IsEmpty())
        {
            LastQueryDiagnostic = FString::Printf(
                TEXT("NO ENDPOINT VOLUME | volumes %d | baked %d | endpoints %d"),
                VolumeCount, BakedVolumeCount, EndpointVolumeCount);
        }
        FHellRunNavigationDebugLog::Write(&Character, TEXT("VOXEL_ROUTE_SEARCH_FAILED"), FString::Printf(
            TEXT("start=%s goal=%s volumes=%d bakedVolumes=%d endpointVolumes=%d sharedFlow=%d"),
            *Start.ToCompactString(), *Goal.ToCompactString(),
            VolumeCount, BakedVolumeCount, EndpointVolumeCount, S->bUseSharedTargetFlowFields));
    }
    return Result;
}

const FString& FHellRunVoxelNavigation::GetLastQueryDiagnostic()
{
    return HellRunVoxelNavigationPrivate::LastQueryDiagnostic;
}

float FHellRunVoxelNavigation::EstimateTraversalSeconds(
    const ACharacter& Character,
    const FNavPathSharedPtr& Path)
{
    return HellRunVoxelNavigationPrivate::EstimateTraversalSeconds(Character, Path);
}

bool FHellRunVoxelNavigation::HasAuthoritativeTypedEdgeGraph(const UWorld* World)
{
    if (!World) return false;
    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        if (It->HasAuthoritativeTypedEdgeGraph())
        {
            return true;
        }
    }
    return false;
}

FHellRunNavigationPathResult FHellRunVoxelNavigation::QueryBestPath(
    ACharacter& Character,
    const FVector& Goal,
    const FNavPathSharedPtr& GroundPath,
    bool bRequireEscapeRoute)
{
    QUICK_SCOPE_CYCLE_COUNTER(STAT_HellRunQueryBestPath);
    const UHellRunTraversalNavigationSettings* S = GetDefault<UHellRunTraversalNavigationSettings>();
    const bool bRawGroundUsable = GroundPath.IsValid() && GroundPath->IsValid()
        && GroundPath->GetPathPoints().Num() > 1;
    const bool bRequiresVerticalRoute =
        FMath::Abs(Goal.Z - Character.GetActorLocation().Z) > S->VolumetricActivationHeight;
    // Recast corners on stairs are intentionally sparse. Sweeping a capsule
    // along the straight chord between two such floor-surface corners cuts
    // through the intermediate stair treads and falsely rejects a valid navmesh
    // corridor. Recast already owns collision-valid ground topology; the
    // endpoint layer check below prevents an upper polygon directly above a
    // subterranean goal from being mistaken for the destination.
    const bool bGroundGeometryValid = true;
    const bool bGroundPathPartial = bRawGroundUsable
        && GroundPath->IsPartial();
    const FVector GroundEndpoint = bRawGroundUsable
        ? GroundPath->GetPathPoints().Last().Location
        : FVector::ZeroVector;
    const UCapsuleComponent* EndpointCapsule =
        Character.GetCapsuleComponent();
    const float EndpointHalfHeight = EndpointCapsule
        ? EndpointCapsule->GetScaledCapsuleHalfHeight()
        : S->VoxelBakeAgentHalfHeight;
    const float GroundEndpointCenterZ = GroundEndpoint.Z
        + EndpointHalfHeight
        + S->VoxelGroundClearance;
    const bool bGroundEndpointMatchesGoal = bRawGroundUsable
        && FVector::Dist2D(GroundEndpoint, Goal)
            <= S->VoxelRouteCacheGoalTolerance
        && FMath::Abs(GroundEndpointCenterZ - Goal.Z)
            <= S->GroundStepHeight;

    // Recast can project a subterranean goal onto a NavMesh polygon directly
    // above it and report a complete path. That route is only executable when
    // its endpoint belongs to the goal's vertical layer. Preserve genuinely
    // partial paths as useful boundary routes, but never execute a complete
    // route that terminates on the wrong floor.
    const bool bGroundUsable = bRawGroundUsable
        && bGroundGeometryValid
        && (bGroundPathPartial
            || !bRequiresVerticalRoute
            || bGroundEndpointMatchesGoal);
    const bool bGroundComplete = bGroundUsable && !GroundPath->IsPartial();
    auto MakeGroundResult = [&]()
    {
        FHellRunNavigationPathResult Result;
        // An invalid ground route is diagnostic input, not executable output.
        // Returning its still-valid shared pointer caused the controller to
        // follow a route that this selector had explicitly rejected.
        Result.Path = bGroundUsable ? GroundPath : nullptr;
        Result.Provider = bGroundUsable
            ? EHellRunNavigationPathProvider::Recast : EHellRunNavigationPathProvider::None;
        Result.Outcome = !bGroundUsable
            ? EHellRunNavigationPathOutcome::Unreachable
            : (bGroundComplete ? EHellRunNavigationPathOutcome::Complete
                               : EHellRunNavigationPathOutcome::Partial);
        return Result;
    };
    if (S->NavigationMode == EHellRunNavigationMode::GeneratedLinks) return MakeGroundResult();

    const UHellRunTraversalComponent* Traversal = Character.FindComponentByClass<UHellRunTraversalComponent>();
    const bool bCanUseVerticalNavigation = Traversal
        && (Traversal->CanClimbNavigation() || Traversal->CanFlyNavigation());
    const bool bCanRequireVerticalRoute = bCanUseVerticalNavigation && bRequiresVerticalRoute;
    const bool bGroundReachesGoal = bGroundComplete
        && bGroundEndpointMatchesGoal;

    if (S->NavigationMode == EHellRunNavigationMode::AutomaticHybrid
        && S->bSkipVoxelSearchForDirectGroundPaths
        && bGroundReachesGoal)
    {
        FHellRunNavigationDebugLog::Write(&Character, TEXT("ROUTE_SELECTED"),
            TEXT("provider=RECAST reason=complete_ground_route"));
        return MakeGroundResult();
    }

    FNavPathSharedPtr VoxelPath = FindPathFrom(
        Character,
        Character.GetActorLocation(),
        Goal,
        bRequireEscapeRoute);
    if (VoxelPath.IsValid())
    {
        // Voxel search chooses topology and typed transitions. Recast owns
        // ordinary surface corridors and string-pulling inside each contiguous
        // Walk run.
        if (!HellRunVoxelNavigationPrivate::ReplaceVoxelWalkRunsWithGroundPaths(
                Character, VoxelPath))
        {
            VoxelPath.Reset();
        }
        else if (S->NavigationMode == EHellRunNavigationMode::VolumetricHybrid
            || !bGroundReachesGoal)
        {
            FHellRunNavigationDebugLog::Write(&Character, TEXT("ROUTE_SELECTED"),
                TEXT("provider=VOXEL reason=preferred_or_no_ground"));
            return {VoxelPath, EHellRunNavigationPathOutcome::Complete,
                EHellRunNavigationPathProvider::Voxel};
        }
        else
        {
            const float GroundSeconds = HellRunVoxelNavigationPrivate::EstimateTraversalSeconds(Character, GroundPath);
            const float VoxelSeconds = HellRunVoxelNavigationPrivate::EstimateTraversalSeconds(Character, VoxelPath);
            const bool bChooseVoxel = VoxelSeconds + 0.05f < GroundSeconds;
            FHellRunNavigationDebugLog::Write(&Character, TEXT("ROUTE_COMPARED"), FString::Printf(
                TEXT("groundSeconds=%.3f voxelSeconds=%.3f selected=%s groundNodes=%d voxelNodes=%d"),
                GroundSeconds, VoxelSeconds, bChooseVoxel ? TEXT("VOXEL") : TEXT("RECAST"),
                GroundPath->GetPathPoints().Num(), VoxelPath->GetPathPoints().Num()));
            return bChooseVoxel
                ? FHellRunNavigationPathResult{VoxelPath, EHellRunNavigationPathOutcome::Complete,
                    EHellRunNavigationPathProvider::Voxel}
                : MakeGroundResult();
        }
    }

    FHellRunNavigationDebugLog::Write(&Character, TEXT("VOXEL_ROUTE_UNAVAILABLE"), FString::Printf(
        TEXT("goal=%s groundValid=%d groundReaches=%d verticalRequired=%d endpoint=%s endpointCenterZ=%.1f endpointLayerMatch=%d"),
        *Goal.ToCompactString(), bGroundComplete, bGroundReachesGoal,
        bCanRequireVerticalRoute, *GroundEndpoint.ToCompactString(),
        GroundEndpointCenterZ,
        bGroundEndpointMatchesGoal ? 1 : 0));

    // A partial ground route is still useful navigation data. Movement policy can
    // follow it toward the reachable boundary while deciding when to query again.
    return MakeGroundResult();
}

bool UHellRunTraversalPathFollowingComponent::IsFollowingVoxelPath() const
{
    return Path.IsValid() && Path->GetPathPoints().Num() > 1
        && HellRunVoxelPath::IsVoxelFlags(Path->GetPathPoints()[0].Flags);
}

EHellRunVoxelSegment UHellRunTraversalPathFollowingComponent::GetCurrentVoxelMode() const
{
    const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
    return Points.IsValidIndex(MoveSegmentEndIndex) ? HellRunVoxelPath::GetMode(Points[MoveSegmentEndIndex].Flags) : EHellRunVoxelSegment::Walk;
}

bool UHellRunTraversalPathFollowingComponent::IsExecutingVoxelTraversal() const
{
    return IsFollowingVoxelPath() && GetCurrentVoxelMode() != EHellRunVoxelSegment::Walk;
}

void UHellRunTraversalPathFollowingComponent::AbortMove(const UObject& Instigator,
    FPathFollowingResultFlags::Type AbortFlags, FAIRequestID RequestID,
    EPathFollowingVelocityMode VelocityMode)
{
    const AController* Controller = Cast<AController>(GetOwner());
    const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
    const UHellRunTraversalComponent* Traversal = Pawn
        ? Pawn->FindComponentByClass<UHellRunTraversalComponent>() : nullptr;
    FHellRunNavigationDebugLog::Write(Pawn ? static_cast<const UObject*>(Pawn) : GetOwner(),
        TEXT("PATH_ABORT_REQUESTED"), FString::Printf(
            TEXT("instigator=%s class=%s flags=%u flagsText=%s status=%d provider=%s current=%d next=%d mode=%d locomotion=%d"),
            *GetNameSafe(&Instigator), *GetNameSafe(Instigator.GetClass()), static_cast<uint32>(AbortFlags),
            *HellRunVoxelNavigationPrivate::PathFollowingFlagsToString(AbortFlags), static_cast<int32>(GetStatus()),
            IsFollowingVoxelPath() ? TEXT("VOXEL") : TEXT("RECAST"),
            static_cast<int32>(GetCurrentPathIndex()), static_cast<int32>(GetNextPathIndex()),
            IsFollowingVoxelPath() ? static_cast<int32>(GetCurrentVoxelMode()) : INDEX_NONE,
            Traversal ? static_cast<int32>(Traversal->GetLocomotionState()) : INDEX_NONE));
    Super::AbortMove(Instigator, AbortFlags, RequestID, VelocityMode);
}

void UHellRunTraversalPathFollowingComponent::SetMoveSegment(int32 SegmentStartIndex)
{
    if (!IsFollowingVoxelPath())
    {
        CommittedVoxelPath = nullptr;
        Super::SetMoveSegment(SegmentStartIndex);
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("PATH_SEGMENT"), FString::Printf(
            TEXT("provider=RECAST requestedStart=%d current=%d next=%d nodes=%d nextLocation=%s"),
            SegmentStartIndex, static_cast<int32>(GetCurrentPathIndex()), static_cast<int32>(GetNextPathIndex()),
            Path.IsValid() ? Path->GetPathPoints().Num() : 0, *GetCurrentTargetLocation().ToCompactString()));
        return;
    }

    // Crowd/path resume notifications can recompute a starting point against
    // the coarse voxel polyline and select an earlier cell. Never rewind an
    // already committed route; a genuine replacement arrives as a new Path
    // through RequestMove and starts with fresh segment state.
    const bool bSameCommittedPath = CommittedVoxelPath == Path.Get();
    if (bSameCommittedPath && GetStatus() == EPathFollowingStatus::Moving
        && SegmentStartIndex < MoveSegmentStartIndex)
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("PATH_REWIND_IGNORED"), FString::Printf(
            TEXT("provider=VOXEL requestedStart=%d retainedStart=%d next=%d nodes=%d"),
            SegmentStartIndex, MoveSegmentStartIndex, MoveSegmentEndIndex,
            Path.IsValid() ? Path->GetPathPoints().Num() : 0));
        return;
    }
    if (bSameCommittedPath
        && GetStatus() == EPathFollowingStatus::Moving)
    {
        const AController* ActiveController =
            Cast<AController>(GetOwner());
        const ACharacter* ActiveCharacter = ActiveController
            ? Cast<ACharacter>(ActiveController->GetPawn())
            : nullptr;
        const UHellRunTraversalComponent* ActiveTraversal =
            ActiveCharacter
            ? ActiveCharacter->FindComponentByClass<
                UHellRunTraversalComponent>()
            : nullptr;
        if (ActiveTraversal
            && ActiveTraversal->BlocksExternalMovementControl()
            && ActiveTraversal->GetLocomotionState()
                != EHellRunLocomotionState::VoxelApproach)
        {
            // Crowd following can refresh the current segment while a typed
            // action owns movement. Re-entering the setup below changes the
            // active vault/drop back to VoxelApproach; the next refresh then
            // starts the same edge from midair and adds another arc forever.
            FHellRunNavigationDebugLog::Write(
                ActiveCharacter,
                TEXT("PATH_SEGMENT_REFRESH_IGNORED"),
                FString::Printf(
                    TEXT("requestedStart=%d retainedStart=%d next=%d locomotion=%d"),
                    SegmentStartIndex,
                    MoveSegmentStartIndex,
                    MoveSegmentEndIndex,
                    static_cast<int32>(
                        ActiveTraversal->GetLocomotionState())));
            return;
        }
    }
    CommittedVoxelPath = Path.Get();
    UPathFollowingComponent::SetMoveSegment(SegmentStartIndex);
    const TArray<FNavPathPoint>& ActivePoints = Path->GetPathPoints();
    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("PATH_SEGMENT"), FString::Printf(
        TEXT("provider=VOXEL requestedStart=%d current=%d next=%d nodes=%d mode=%d pawn=%s nextLocation=%s"),
        SegmentStartIndex, static_cast<int32>(GetCurrentPathIndex()), static_cast<int32>(GetNextPathIndex()),
        ActivePoints.Num(), static_cast<int32>(GetCurrentVoxelMode()),
        *GetNameSafe(Cast<AController>(GetOwner()) ? Cast<AController>(GetOwner())->GetPawn() : nullptr),
        *GetCurrentTargetLocation().ToCompactString()));
    if (AController* Controller = Cast<AController>(GetOwner()))
    {
        if (ACharacter* Character = Cast<ACharacter>(Controller->GetPawn()))
        {
            if (UHellRunTraversalComponent* Traversal = Character->FindComponentByClass<UHellRunTraversalComponent>())
            {
                const EHellRunVoxelSegment SegmentMode =
                    GetCurrentVoxelMode();
                const bool bTraversalSegment =
                    SegmentMode != EHellRunVoxelSegment::Walk
                    && SegmentMode != EHellRunVoxelSegment::Fly;
                const FVector TakeoffLocation =
                    ActivePoints.IsValidIndex(MoveSegmentStartIndex)
                    ? ActivePoints[MoveSegmentStartIndex].Location
                    : Character->GetActorLocation();
                const float EntryReach =
                    GetDefault<UHellRunTraversalNavigationSettings>()
                    ->VoxelTraversalEntryReachDistance;
                const bool bAtTakeoff =
                    FVector::Dist2D(
                        Character->GetActorLocation(),
                        TakeoffLocation)
                    <= EntryReach;
                if (bTraversalSegment && !bAtTakeoff)
                {
                    Traversal->SetVoxelLocomotionState(
                        EHellRunLocomotionState::VoxelApproach);
                    if (UCharacterMovementComponent* Movement =
                        Character->GetCharacterMovement())
                    {
                        Movement->SetMovementMode(MOVE_Walking);
                    }
                    return;
                }
                switch (GetCurrentVoxelMode())
                {
                case EHellRunVoxelSegment::Climb: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelClimb, GetCurrentTargetLocation()); break;
                case EHellRunVoxelSegment::Mantle: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelMantle, GetCurrentTargetLocation()); break;
                case EHellRunVoxelSegment::Drop: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelDrop, GetCurrentTargetLocation()); break;
                case EHellRunVoxelSegment::Jump: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelJump, GetCurrentTargetLocation()); break;
                case EHellRunVoxelSegment::Vault: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelVault, GetCurrentTargetLocation()); break;
                case EHellRunVoxelSegment::Fly: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::Flying, GetCurrentTargetLocation()); break;
                default:
                {
                    // Lock pursuit to this route before reaching the wall. Without an
                    // approach state, the director can replace the path every refresh
                    // and the pawn almost never commits to its climb/mantle segment.
                    bool bHasPendingTraversal = false;
                    if (Path.IsValid())
                    {
                        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
                        const int32 NextPointIndex = MoveSegmentEndIndex + 1;
                        bHasPendingTraversal =
                            Points.IsValidIndex(NextPointIndex)
                            && HellRunVoxelPath::IsVoxelFlags(
                                Points[NextPointIndex].Flags)
                            && HellRunVoxelPath::GetMode(
                                Points[NextPointIndex].Flags)
                                != EHellRunVoxelSegment::Walk;
                    }
                    if (bHasPendingTraversal)
                    {
                        Traversal->SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
                    }
                    else
                    {
                        Traversal->SetGroundedLocomotion();
                    }
                    break;
                }
                }
            }
            if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
            {
                const EHellRunVoxelSegment Mode = GetCurrentVoxelMode();
                Movement->SetMovementMode(Mode == EHellRunVoxelSegment::Walk ? MOVE_Walking : MOVE_Flying);
            }
        }
    }
}

void UHellRunTraversalPathFollowingComponent::UpdatePathSegment()
{
    if (!IsFollowingVoxelPath())
    {
        Super::UpdatePathSegment();
        return;
    }

    const AController* Controller = Cast<AController>(GetOwner());
    const ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
    if (!Character || !Path.IsValid())
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("PATH_INVALID"), FString::Printf(
            TEXT("provider=VOXEL character=%d path=%d current=%d next=%d"),
            Character != nullptr, Path.IsValid(), static_cast<int32>(GetCurrentPathIndex()),
            static_cast<int32>(GetNextPathIndex())));
        OnPathFinished(FPathFollowingResult(EPathFollowingResult::Invalid, FPathFollowingResultFlags::InvalidPath));
        return;
    }

    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    const EHellRunVoxelSegment CurrentMode = GetCurrentVoxelMode();
    float ReachDistance = FMath::Max(1.0f, Settings->VoxelTraversalPointReachDistance);
    if (CurrentMode == EHellRunVoxelSegment::Walk)
    {
        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
        const int32 NextPointIndex = MoveSegmentEndIndex + 1;
        if (Points.IsValidIndex(NextPointIndex)
            && HellRunVoxelPath::IsVoxelFlags(Points[NextPointIndex].Flags)
            && HellRunVoxelPath::GetMode(Points[NextPointIndex].Flags) != EHellRunVoxelSegment::Walk)
        {
            ReachDistance = FMath::Max(ReachDistance, Settings->VoxelTraversalEntryReachDistance);
        }
    }

    const FVector Delta = GetCurrentTargetLocation() - Character->GetActorLocation();
    const bool bReached = CurrentMode == EHellRunVoxelSegment::Walk
        ? FVector2D(Delta.X, Delta.Y).SizeSquared() <= FMath::Square(ReachDistance)
        : CurrentMode == EHellRunVoxelSegment::Drop
            ? FVector2D(Delta.X, Delta.Y).SizeSquared() <= FMath::Square(Settings->VoxelTraversalEntryReachDistance)
                && FMath::Abs(Delta.Z) <= Settings->GroundStepHeight + Settings->VoxelGroundClearance
        : Delta.SizeSquared() <= FMath::Square(ReachDistance);
    if (bReached)
    {
        if (MoveSegmentEndIndex < Path->GetPathPoints().Num() - 1)
        {
            SetMoveSegment(MoveSegmentEndIndex);
        }
        else
        {
            OnPathFinished(FPathFollowingResult(EPathFollowingResult::Success, FPathFollowingResultFlags::None));
        }
    }
}

void UHellRunTraversalPathFollowingComponent::FollowPathSegment(float DeltaTime)
{
    if (!IsFollowingVoxelPath()) { Super::FollowPathSegment(DeltaTime); return; }
    const EHellRunVoxelSegment Mode = GetCurrentVoxelMode();
    if (Mode == EHellRunVoxelSegment::Walk)
    {
        UPathFollowingComponent::FollowPathSegment(DeltaTime);
        return;
    }
    AController* Controller = Cast<AController>(GetOwner());
    ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
    UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
    if (!Character || !Movement) return;
    UHellRunTraversalComponent* Traversal =
        Character->FindComponentByClass<UHellRunTraversalComponent>();
    if (Traversal
        && Traversal->BlocksExternalMovementControl()
        && Traversal->GetLocomotionState() != EHellRunLocomotionState::VoxelApproach)
    {
        // Once a typed action has started, its locomotion FSM exclusively owns
        // movement. Reissuing the takeoff approach here changes movement mode
        // back to walking and can drag a dropping or climbing pawn backward.
        return;
    }
    const TArray<FNavPathPoint>& ActivePoints =
        Path->GetPathPoints();
    const FVector TakeoffLocation =
        ActivePoints.IsValidIndex(MoveSegmentStartIndex)
        ? ActivePoints[MoveSegmentStartIndex].Location
        : Character->GetActorLocation();
    const float EntryReach =
        GetDefault<UHellRunTraversalNavigationSettings>()
        ->VoxelTraversalEntryReachDistance;
    if (FVector::Dist2D(
            Character->GetActorLocation(),
            TakeoffLocation)
        > EntryReach)
    {
        Movement->SetMovementMode(MOVE_Walking);
        const FVector ApproachVelocity =
            (TakeoffLocation - Character->GetActorLocation())
            .GetSafeNormal2D()
            * Movement->GetMaxSpeed();
        Movement->RequestDirectMove(
            ApproachVelocity,
            false);
        return;
    }
    // The locomotion FSM owns all non-ground movement. Path following only
    // observes arrival and advances to the next typed segment.
    if (Traversal)
    {
        if (Traversal->GetLocomotionState()
                == EHellRunLocomotionState::VoxelApproach
            || !Traversal->BlocksExternalMovementControl())
        {
            switch (Mode)
            {
            case EHellRunVoxelSegment::Climb: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelClimb, GetCurrentTargetLocation()); break;
            case EHellRunVoxelSegment::Mantle: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelMantle, GetCurrentTargetLocation()); break;
            case EHellRunVoxelSegment::Drop: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelDrop, GetCurrentTargetLocation()); break;
            case EHellRunVoxelSegment::Jump: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelJump, GetCurrentTargetLocation()); break;
            case EHellRunVoxelSegment::Vault: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::VoxelVault, GetCurrentTargetLocation()); break;
            default: Traversal->StartVoxelLocomotion(EHellRunLocomotionState::Flying, GetCurrentTargetLocation()); break;
            }
        }
    }
}

void UHellRunTraversalPathFollowingComponent::OnPathFinished(const FPathFollowingResult& Result)
{
    const bool bWasVoxelPath = IsFollowingVoxelPath();
    const int32 PointCount = Path.IsValid() ? Path->GetPathPoints().Num() : 0;
    const AController* TraceController = Cast<AController>(GetOwner());
    const APawn* TracePawn = TraceController ? TraceController->GetPawn() : nullptr;
    CommittedVoxelPath = nullptr;
    FHellRunNavigationDebugLog::Write(TracePawn ? static_cast<const UObject*>(TracePawn) : GetOwner(),
        TEXT("PATH_FINISHED"), FString::Printf(
            TEXT("provider=%s result=%d flags=%u flagsText=%s nodes=%d current=%d next=%d location=%s"),
            bWasVoxelPath ? TEXT("VOXEL") : TEXT("RECAST"), static_cast<int32>(Result.Code),
            static_cast<uint32>(Result.Flags), *HellRunVoxelNavigationPrivate::PathFollowingFlagsToString(Result.Flags), PointCount,
            static_cast<int32>(GetCurrentPathIndex()), static_cast<int32>(GetNextPathIndex()),
            TracePawn ? *TracePawn->GetActorLocation().ToCompactString() : TEXT("none")));
    if (bWasVoxelPath)
    {
        if (AController* Controller = Cast<AController>(GetOwner()))
        {
            if (ACharacter* Character = Cast<ACharacter>(Controller->GetPawn()))
            {
                if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement()) Movement->SetMovementMode(MOVE_Walking);
                if (UHellRunTraversalComponent* Traversal = Character->FindComponentByClass<UHellRunTraversalComponent>()) Traversal->BeginLocomotionRecovery();
            }
        }
    }
    Super::OnPathFinished(Result);
}

bool UHellRunTraversalPathFollowingComponent::IsOnPath() const
{
    return IsFollowingVoxelPath() || Super::IsOnPath();
}

void UHellRunNavigationModeLibrary::SetNavigationMode(EHellRunNavigationMode NewMode)
{
    GetMutableDefault<UHellRunTraversalNavigationSettings>()->NavigationMode = NewMode;
}

EHellRunNavigationMode UHellRunNavigationModeLibrary::GetNavigationMode()
{
    return GetDefault<UHellRunTraversalNavigationSettings>()->NavigationMode;
}
