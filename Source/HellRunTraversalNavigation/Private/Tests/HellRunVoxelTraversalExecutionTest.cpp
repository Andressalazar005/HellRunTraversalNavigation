#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "AIController.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HellRunTraversalComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "HellRunVoxelNavVolume.h"
#include "HellRunVoxelNavigation.h"
#include "HellRunVoxelPathDebugPawn.h"
#include "Navigation/PathFollowingComponent.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FHellRunVoxelTraversalExecutionTest,
    "HellRun.Navigation.Voxel.ExecuteRealTraversalSegments",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::EngineFilter
        | EAutomationTestFlags::HighPriority)

namespace
{
    struct FTraversalExecutionScenario
    {
        FString Name;
        EHellRunVoxelSegment ExpectedMode = EHellRunVoxelSegment::Walk;
        FVector Start = FVector::ZeroVector;
        FVector Goal = FVector::ZeroVector;
        FNavPathSharedPtr Path;
        float TimeoutSeconds = 10.0f;
    };

    EHellRunLocomotionState ExpectedLocomotionState(EHellRunVoxelSegment Mode)
    {
        switch (Mode)
        {
        case EHellRunVoxelSegment::Climb: return EHellRunLocomotionState::VoxelClimb;
        case EHellRunVoxelSegment::Mantle: return EHellRunLocomotionState::VoxelMantle;
        case EHellRunVoxelSegment::Drop: return EHellRunLocomotionState::VoxelDrop;
        case EHellRunVoxelSegment::Fly: return EHellRunLocomotionState::Flying;
        case EHellRunVoxelSegment::Jump: return EHellRunLocomotionState::VoxelJump;
        case EHellRunVoxelSegment::Vault: return EHellRunLocomotionState::VoxelVault;
        default: return EHellRunLocomotionState::Grounded;
        }
    }

    FNavPathSharedPtr MakeSingleSegmentPath(
        const FVector& Start,
        const FVector& Goal,
        EHellRunVoxelSegment Mode)
    {
        TSharedPtr<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe> Path =
            MakeShared<FHellRunVoxelNavigationPath, ESPMode::ThreadSafe>();
        Path->GetPathPoints().Add(FNavPathPoint(Start));
        Path->GetPathPoints().Add(FNavPathPoint(Goal));
        Path->GetPathPoints()[0].Flags =
            HellRunVoxelPath::MakeFlags(EHellRunVoxelSegment::Walk);
        Path->GetPathPoints()[1].Flags = HellRunVoxelPath::MakeFlags(Mode);
        Path->SetIsPartial(false);
        Path->MarkReady();
        return Path;
    }

    int32 CountTypedSegments(const FNavPathSharedPtr& Path)
    {
        int32 Count = 0;
        if (!Path.IsValid()) return Count;
        const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            if (HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                && HellRunVoxelPath::GetMode(Points[Index].Flags)
                    != EHellRunVoxelSegment::Walk)
            {
                ++Count;
            }
        }
        return Count;
    }

    class FHellRunExecuteRealTraversalCommand final
        : public IAutomationLatentCommand
    {
    public:
        explicit FHellRunExecuteRealTraversalCommand(
            FAutomationTestBase* InTest)
            : Test(InTest)
        {
        }

        virtual bool Update() override
        {
            if (!Test) return true;
            if (!bInitialized)
            {
                return Initialize();
            }
            if (!PathFollowing.IsValid()
                || !Character.IsValid()
                || !Traversal.IsValid())
            {
                Test->AddError(
                    TEXT("Traversal execution fixture was destroyed during PIE."));
                return true;
            }

            if (!bScenarioActive)
            {
                if (ScenarioIndex >= Scenarios.Num())
                {
                    Test->TestTrue(
                        TEXT("real baked traversal executes multiple typed modes"),
                        CompletedTypedModes >= 3);
                    Test->TestTrue(
                        TEXT("real baked traversal executes a complete multi-segment route"),
                        bCompletedFullRoute);
                    Test->TestTrue(
                        TEXT("vault completed while duplicate segment starts were issued"),
                        bExercisedDuplicateVaultStart);
                    Test->TestTrue(
                        TEXT("drop completed while duplicate segment starts were issued"),
                        bExercisedDuplicateDropStart);
                    return true;
                }
                StartScenario();
                return false;
            }

            const EHellRunLocomotionState State =
                Traversal->GetLocomotionState();
            if (State == ExpectedLocomotionState(
                    Scenarios[ScenarioIndex].ExpectedMode))
            {
                bObservedExpectedLocomotion = true;
                if (Scenarios[ScenarioIndex].ExpectedMode
                        == EHellRunVoxelSegment::Vault
                    || Scenarios[ScenarioIndex].ExpectedMode
                        == EHellRunVoxelSegment::Drop)
                {
                    // Crowd/path refreshes can call SetMoveSegment repeatedly.
                    // Reproduce that pressure explicitly: the committed action
                    // must keep progressing instead of restarting its arc or
                    // takeoff on every update.
                    Traversal->StartVoxelLocomotion(
                        State,
                        Scenarios[ScenarioIndex].Goal);
                    PathFollowing->SetMoveSegment(0);
                    bExercisedDuplicateVaultStart |=
                        Scenarios[ScenarioIndex].ExpectedMode
                        == EHellRunVoxelSegment::Vault;
                    bExercisedDuplicateDropStart |=
                        Scenarios[ScenarioIndex].ExpectedMode
                        == EHellRunVoxelSegment::Drop;
                }
            }

            const double Elapsed =
                FPlatformTime::Seconds() - ScenarioStartTime;
            const FTraversalExecutionScenario& Scenario =
                Scenarios[ScenarioIndex];
            const float DistanceToGoal = FVector::Distance(
                Character->GetActorLocation(), Scenario.Goal);
            const float ReachDistance = FMath::Max(
                75.0f,
                GetDefault<UHellRunTraversalNavigationSettings>()
                    ->VoxelTraversalPointReachDistance * 2.0f);
            if (PathFollowing->GetStatus()
                    == EPathFollowingStatus::Idle
                && DistanceToGoal <= ReachDistance)
            {
                if (Scenario.ExpectedMode != EHellRunVoxelSegment::Walk)
                {
                    Test->TestTrue(
                        Scenario.Name
                            + TEXT(" entered its typed locomotion state"),
                        bObservedExpectedLocomotion);
                    ++CompletedTypedModes;
                }
                if (Scenario.Name == TEXT("Full real-data route"))
                {
                    bCompletedFullRoute = true;
                }
                Test->AddInfo(FString::Printf(
                    TEXT("%s completed in %.2fs at %s (goal distance %.1f)."),
                    *Scenario.Name,
                    Elapsed,
                    *Character->GetActorLocation().ToCompactString(),
                    DistanceToGoal));
                ++ScenarioIndex;
                bScenarioActive = false;
                return false;
            }

            if (Elapsed >= Scenario.TimeoutSeconds)
            {
                Test->AddError(FString::Printf(
                    TEXT("%s timed out after %.2fs. location=%s goal=%s distance=%.1f pathStatus=%d locomotion=%d expectedObserved=%d"),
                    *Scenario.Name,
                    Elapsed,
                    *Character->GetActorLocation().ToCompactString(),
                    *Scenario.Goal.ToCompactString(),
                    DistanceToGoal,
                    static_cast<int32>(PathFollowing->GetStatus()),
                    static_cast<int32>(State),
                    bObservedExpectedLocomotion));
                PathFollowing->AbortMove(
                    *Controller.Get(),
                    FPathFollowingResultFlags::ForcedScript);
                ++ScenarioIndex;
                bScenarioActive = false;
            }
            return false;
        }

    private:
        bool Initialize()
        {
            FWorldContext* PIEContext =
                GEditor ? GEditor->GetPIEWorldContext() : nullptr;
            UWorld* World = PIEContext ? PIEContext->World() : nullptr;
            if (!World || !World->HasBegunPlay())
            {
                return false;
            }

            AHellRunVoxelNavVolume* Volume = nullptr;
            for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
            {
                if (It->HasAuthoritativeTypedEdgeGraph())
                {
                    Volume = *It;
                    break;
                }
            }
            if (!Volume)
            {
                Test->AddError(
                    TEXT("PIE TestZombie_Map has no authoritative typed voxel graph."));
                return true;
            }

            FActorSpawnParameters SpawnParameters;
            SpawnParameters.ObjectFlags |= RF_Transient;
            SpawnParameters.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AHellRunVoxelPathDebugPawn* NewCharacter =
                World->SpawnActor<AHellRunVoxelPathDebugPawn>(
                    AHellRunVoxelPathDebugPawn::StaticClass(),
                    FTransform::Identity,
                    SpawnParameters);
            AAIController* NewController =
                World->SpawnActor<AAIController>(
                    AAIController::StaticClass(),
                    FTransform::Identity,
                    SpawnParameters);
            if (!NewCharacter || !NewController)
            {
                Test->AddError(
                    TEXT("Could not spawn the traversal execution fixture."));
                return true;
            }

            NewController->Possess(NewCharacter);
            if (UPathFollowingComponent* OldPathFollowing =
                    NewController->GetPathFollowingComponent())
            {
                NewController->SetPathFollowingComponent(nullptr);
                OldPathFollowing->DestroyComponent();
            }
            UHellRunTraversalPathFollowingComponent* NewPathFollowing =
                NewObject<UHellRunTraversalPathFollowingComponent>(
                    NewController,
                    TEXT("TraversalExecutionPathFollowing"));
            NewPathFollowing->RegisterComponent();
            NewController->SetPathFollowingComponent(NewPathFollowing);
            NewPathFollowing->Initialize();

            UHellRunTraversalComponent* NewTraversal =
                NewCharacter->FindComponentByClass<
                    UHellRunTraversalComponent>();
            if (!NewTraversal)
            {
                Test->AddError(
                    TEXT("Traversal execution fixture has no traversal component."));
                return true;
            }
            NewTraversal->bCanWalkNavigation = true;
            NewTraversal->bCanClimbNavigation = true;
            NewTraversal->bCanWallClimbNavigation = true;
            NewTraversal->bCanMantleNavigation = true;
            NewTraversal->bCanDropNavigation = true;
            NewTraversal->bCanJumpNavigation = true;
            NewTraversal->bCanVaultNavigation = true;
            NewTraversal->bCanFlyNavigation = true;

            Character = NewCharacter;
            Controller = NewController;
            PathFollowing = NewPathFollowing;
            Traversal = NewTraversal;

            const EHellRunVoxelSegment ProbeModes[] = {
                EHellRunVoxelSegment::Jump,
                EHellRunVoxelSegment::Vault,
                EHellRunVoxelSegment::Mantle,
                EHellRunVoxelSegment::Drop,
                EHellRunVoxelSegment::Climb,
                EHellRunVoxelSegment::Fly
            };
            for (const EHellRunVoxelSegment Mode : ProbeModes)
            {
                FVector Start;
                FVector Goal;
                if (!Volume->GetAutomationTraversalProbe(
                        *NewCharacter, Mode, Start, Goal))
                {
                    Test->AddInfo(FString::Printf(
                        TEXT("Real baked graph has no execution probe for mode %d."),
                        static_cast<int32>(Mode)));
                    continue;
                }
                FTraversalExecutionScenario& Scenario =
                    Scenarios.AddDefaulted_GetRef();
                Scenario.Name = FString::Printf(
                    TEXT("Real-data %s segment"),
                    *UEnum::GetValueAsString(Mode));
                Scenario.ExpectedMode = Mode;
                Scenario.Start = Start;
                Scenario.Goal = Goal;
                Scenario.Path = MakeSingleSegmentPath(Start, Goal, Mode);
                Scenario.TimeoutSeconds =
                    Mode == EHellRunVoxelSegment::Drop ? 12.0f : 8.0f;
            }

            AddFullRouteScenario(*Volume, *NewCharacter);
            if (Scenarios.IsEmpty())
            {
                Test->AddError(
                    TEXT("Real baked graph supplied no executable traversal scenarios."));
                return true;
            }
            bInitialized = true;
            return false;
        }

        void AddFullRouteScenario(
            AHellRunVoxelNavVolume& Volume,
            ACharacter& QueryCharacter)
        {
            TArray<FVector> Samples;
            Volume.GetAutomationGroundNodeLocations(Samples, 384);
            for (int32 Offset = 1; Offset < Samples.Num(); ++Offset)
            {
                const int32 StartIndex = (Offset * 37) % Samples.Num();
                const int32 GoalIndex =
                    (Samples.Num() - 1 - Offset * 53 + Samples.Num() * 2)
                    % Samples.Num();
                if (StartIndex == GoalIndex) continue;
                const FNavPathSharedPtr Candidate =
                    FHellRunVoxelNavigation::FindPathFrom(
                        QueryCharacter,
                        Samples[StartIndex],
                        Samples[GoalIndex]);
                const int32 TypedSegments = CountTypedSegments(Candidate);
                if (!Candidate.IsValid()
                    || !Candidate->IsValid()
                    || Candidate->IsPartial()
                    || Candidate->GetPathPoints().Num() < 4
                    || Candidate->GetPathPoints().Num() > 40
                    || TypedSegments < 2)
                {
                    continue;
                }

                FTraversalExecutionScenario& Scenario =
                    Scenarios.AddDefaulted_GetRef();
                Scenario.Name = TEXT("Full real-data route");
                Scenario.ExpectedMode = EHellRunVoxelSegment::Walk;
                Scenario.Start =
                    Candidate->GetPathPoints()[0].Location;
                Scenario.Goal =
                    Candidate->GetPathPoints().Last().Location;
                Scenario.Path = Candidate;
                Scenario.TimeoutSeconds = FMath::Clamp(
                    FHellRunVoxelNavigation::EstimateTraversalSeconds(
                        QueryCharacter, Candidate) * 4.0f + 5.0f,
                    12.0f,
                    35.0f);
                Test->AddInfo(FString::Printf(
                    TEXT("Selected full route with %d points, %d typed segments, timeout %.1fs."),
                    Candidate->GetPathPoints().Num(),
                    TypedSegments,
                    Scenario.TimeoutSeconds));
                return;
            }
            Test->AddError(
                TEXT("Could not find a complete multi-segment real-data route for execution."));
        }

        void StartScenario()
        {
            const FTraversalExecutionScenario& Scenario =
                Scenarios[ScenarioIndex];
            PathFollowing->AbortMove(
                *Controller.Get(),
                FPathFollowingResultFlags::ForcedScript);
            Traversal->SetGroundedLocomotion();
            if (UCharacterMovementComponent* Movement =
                    Character->GetCharacterMovement())
            {
                Movement->StopMovementImmediately();
                Movement->SetMovementMode(MOVE_Walking);
            }
            Character->SetActorLocation(
                Scenario.Start,
                false,
                nullptr,
                ETeleportType::TeleportPhysics);

            FAIMoveRequest MoveRequest(Scenario.Goal);
            MoveRequest.SetAcceptanceRadius(5.0f);
            MoveRequest.SetAllowPartialPath(false);
            MoveRequest.SetUsePathfinding(false);
            const FAIRequestID RequestId =
                PathFollowing->RequestMove(
                    MoveRequest,
                    Scenario.Path);
            if (!RequestId.IsValid())
            {
                Test->AddError(
                    Scenario.Name + TEXT(" failed to start."));
                ++ScenarioIndex;
                return;
            }

            ScenarioStartTime = FPlatformTime::Seconds();
            bObservedExpectedLocomotion = false;
            bScenarioActive = true;
            Test->AddInfo(FString::Printf(
                TEXT("Starting %s from %s to %s (%d points)."),
                *Scenario.Name,
                *Scenario.Start.ToCompactString(),
                *Scenario.Goal.ToCompactString(),
                Scenario.Path->GetPathPoints().Num()));
        }

        FAutomationTestBase* Test = nullptr;
        TWeakObjectPtr<AHellRunVoxelPathDebugPawn> Character;
        TWeakObjectPtr<AAIController> Controller;
        TWeakObjectPtr<UHellRunTraversalPathFollowingComponent>
            PathFollowing;
        TWeakObjectPtr<UHellRunTraversalComponent> Traversal;
        TArray<FTraversalExecutionScenario> Scenarios;
        int32 ScenarioIndex = 0;
        int32 CompletedTypedModes = 0;
        double ScenarioStartTime = 0.0;
        bool bInitialized = false;
        bool bScenarioActive = false;
        bool bObservedExpectedLocomotion = false;
        bool bCompletedFullRoute = false;
        bool bExercisedDuplicateVaultStart = false;
        bool bExercisedDuplicateDropStart = false;
    };
}

bool FHellRunVoxelTraversalExecutionTest::RunTest(
    const FString& Parameters)
{
    FAutomationEditorCommonUtils::LoadMap(
        TEXT("/Game/Levels/Testing/TestZombie_Map"));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(
        FHellRunExecuteRealTraversalCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

#endif
