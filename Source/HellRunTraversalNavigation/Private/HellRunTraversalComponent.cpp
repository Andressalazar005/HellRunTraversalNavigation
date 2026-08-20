#include "HellRunTraversalComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"

#include "HellRunTraversalNavigationSettings.h"
#include "HellRunNavigationDebugLog.h"
#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavLinkCustomInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
    const TCHAR* LocomotionStateName(EHellRunLocomotionState State)
    {
        switch (State)
        {
        case EHellRunLocomotionState::Grounded: return TEXT("Grounded");
        case EHellRunLocomotionState::VoxelApproach: return TEXT("VoxelApproach");
        case EHellRunLocomotionState::Ballistic: return TEXT("Ballistic");
        case EHellRunLocomotionState::VoxelClimb: return TEXT("VoxelClimb");
        case EHellRunLocomotionState::VoxelMantle: return TEXT("VoxelMantle");
        case EHellRunLocomotionState::VoxelDrop: return TEXT("VoxelDrop");
        case EHellRunLocomotionState::VoxelJump: return TEXT("VoxelJump");
        case EHellRunLocomotionState::VoxelVault: return TEXT("VoxelVault");
        case EHellRunLocomotionState::Flying: return TEXT("Flying");
        case EHellRunLocomotionState::GeneratedJump: return TEXT("GeneratedJump");
        case EHellRunLocomotionState::GeneratedVault: return TEXT("GeneratedVault");
        case EHellRunLocomotionState::GeneratedMantle: return TEXT("GeneratedMantle");
        case EHellRunLocomotionState::GeneratedClimb: return TEXT("GeneratedClimb");
        case EHellRunLocomotionState::Recovering: return TEXT("Recovering");
        default: return TEXT("Unknown");
        }
    }
}

UHellRunTraversalComponent::UHellRunTraversalComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    SetIsReplicatedByDefault(true);
}

void UHellRunTraversalComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UHellRunTraversalComponent, bTraversalActive);
    DOREPLIFETIME(UHellRunTraversalComponent, TraversalType);
    DOREPLIFETIME(UHellRunTraversalComponent, LocomotionState);
    DOREPLIFETIME(UHellRunTraversalComponent, AnimationTraversalAction);
    DOREPLIFETIME(UHellRunTraversalComponent, bAnimationTraversalActive);
    DOREPLIFETIME(UHellRunTraversalComponent, AnimationTraversalStartTime);
    DOREPLIFETIME(UHellRunTraversalComponent, AnimationTraversalFinishTime);
}

bool UHellRunTraversalComponent::HasTraversalAuthority() const
{
    const AActor* Owner = GetOwner();
    return Owner && Owner->HasAuthority();
}

void UHellRunTraversalComponent::ForceTraversalNetUpdate() const
{
    if (AActor* Owner = GetOwner())
    {
        Owner->ForceNetUpdate();
    }
}

EHellRunTraversalAnimationAction UHellRunTraversalComponent::GetAnimationActionForState(
    EHellRunLocomotionState State)
{
    switch (State)
    {
    case EHellRunLocomotionState::Ballistic:
    case EHellRunLocomotionState::VoxelJump:
    case EHellRunLocomotionState::GeneratedJump:
        return EHellRunTraversalAnimationAction::Jump;
    case EHellRunLocomotionState::VoxelVault:
    case EHellRunLocomotionState::GeneratedVault:
        return EHellRunTraversalAnimationAction::Vault;
    case EHellRunLocomotionState::VoxelMantle:
    case EHellRunLocomotionState::GeneratedMantle:
        return EHellRunTraversalAnimationAction::Mantle;
    case EHellRunLocomotionState::VoxelClimb:
    case EHellRunLocomotionState::GeneratedClimb:
        return EHellRunTraversalAnimationAction::Climb;
    case EHellRunLocomotionState::VoxelDrop:
        return EHellRunTraversalAnimationAction::Drop;
    case EHellRunLocomotionState::Flying:
        return EHellRunTraversalAnimationAction::Flying;
    default:
        return EHellRunTraversalAnimationAction::None;
    }
}

float UHellRunTraversalComponent::GetSynchronizedWorldTimeSeconds() const
{
    const UWorld* World = GetWorld();
    if (!World)
    {
        return 0.0f;
    }
    if (const AGameStateBase* GameState = World->GetGameState())
    {
        return GameState->GetServerWorldTimeSeconds();
    }
    return World->GetTimeSeconds();
}

void UHellRunTraversalComponent::UpdateTraversalAnimationAction(
    EHellRunLocomotionState OldState,
    EHellRunLocomotionState NewState)
{
    const EHellRunTraversalAnimationAction OldAction = GetAnimationActionForState(OldState);
    const EHellRunTraversalAnimationAction NewAction = GetAnimationActionForState(NewState);
    if (OldAction == NewAction)
    {
        return;
    }

    const float Now = GetSynchronizedWorldTimeSeconds();
    if (NewAction != EHellRunTraversalAnimationAction::None)
    {
        AnimationTraversalAction = NewAction;
        bAnimationTraversalActive = true;
        AnimationTraversalStartTime = Now;
        AnimationTraversalFinishTime = -BIG_NUMBER;
        PlayTraversalAnimation(NewAction);
    }
    else if (OldAction != EHellRunTraversalAnimationAction::None)
    {
        AnimationTraversalAction = OldAction;
        bAnimationTraversalActive = false;
        AnimationTraversalFinishTime = Now;
        StopTraversalAnimation();
    }
}

void UHellRunTraversalComponent::OnRep_LocomotionState(
    EHellRunLocomotionState OldState)
{
    UpdateTraversalAnimationAction(OldState, LocomotionState);
}

void UHellRunTraversalComponent::PlayTraversalAnimation(
    EHellRunTraversalAnimationAction Action)
{
    UAnimSequenceBase* Sequence = nullptr;
    switch (Action)
    {
    case EHellRunTraversalAnimationAction::Vault:
        Sequence = VaultTraversalAnimation;
        break;
    case EHellRunTraversalAnimationAction::Mantle:
        Sequence = MantleTraversalAnimation;
        break;
    case EHellRunTraversalAnimationAction::Climb:
        Sequence = ClimbTraversalAnimation;
        break;
    default:
        break;
    }

    ACharacter* Character = Cast<ACharacter>(GetOwner());
    UAnimInstance* AnimInstance = Character && Character->GetMesh()
        ? Character->GetMesh()->GetAnimInstance() : nullptr;
    if (!Sequence || !AnimInstance)
    {
        return;
    }

    StopTraversalAnimation();
    const int32 LoopCount =
        Action == EHellRunTraversalAnimationAction::Climb ? 12 : 1;
    const FName SlotName = TraversalAnimationSlotName.IsNone()
        ? FName(TEXT("DefaultSlot")) : TraversalAnimationSlotName;
    ActiveTraversalMontage = AnimInstance->PlaySlotAnimationAsDynamicMontage(
        Sequence, SlotName, 0.08f, 0.12f, 1.0f,
        LoopCount, 0.0f, 0.0f);
    if (!ActiveTraversalMontage)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("%s could not play traversal animation %s in slot %s"),
            *GetNameSafe(GetOwner()), *GetNameSafe(Sequence), *SlotName.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("%s playing traversal animation %s in slot %s"),
            *GetNameSafe(GetOwner()), *GetNameSafe(Sequence), *SlotName.ToString());
    }
}

void UHellRunTraversalComponent::StopTraversalAnimation()
{
    ACharacter* Character = Cast<ACharacter>(GetOwner());
    UAnimInstance* AnimInstance = Character && Character->GetMesh()
        ? Character->GetMesh()->GetAnimInstance() : nullptr;
    if (AnimInstance && ActiveTraversalMontage)
    {
        AnimInstance->Montage_Stop(0.12f, ActiveTraversalMontage);
    }
    ActiveTraversalMontage = nullptr;
}

FHellRunTraversalAnimationState UHellRunTraversalComponent::GetTraversalAnimationState() const
{
    FHellRunTraversalAnimationState State;
    State.Action = AnimationTraversalAction;
    const float Now = GetSynchronizedWorldTimeSeconds();
    State.bStarting = bAnimationTraversalActive
        && Now - AnimationTraversalStartTime <= AnimationTraversalStartWindow;
    State.bDoing = bAnimationTraversalActive;
    State.bFinishing = !bAnimationTraversalActive
        && AnimationTraversalAction != EHellRunTraversalAnimationAction::None
        && Now - AnimationTraversalFinishTime <= AnimationTraversalFinishWindow;

    const bool bExposeAction = State.bStarting || State.bDoing || State.bFinishing;
    State.bJump = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Jump;
    State.bVault = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Vault;
    State.bMantle = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Mantle;
    State.bClimb = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Climb;
    State.bDrop = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Drop;
    State.bFlying = bExposeAction && State.Action == EHellRunTraversalAnimationAction::Flying;
    return State;
}

void UHellRunTraversalComponent::BeginPlay()
{
    Super::BeginPlay();
    if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            Movement->MaxStepHeight = FMath::Max(Movement->MaxStepHeight,
                GetDefault<UHellRunTraversalNavigationSettings>()->GroundStepHeight);
        }
    }
}

bool UHellRunTraversalComponent::StartGeneratedTraversal(const FVector& Destination,
    UPathFollowingComponent* PathFollowing, UObject* NavLinkObject, EHellRunGeneratedTraversalType NewType)
{
    ACharacter* Character = Cast<ACharacter>(GetOwner());
    UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
    const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
    if (!HasTraversalAuthority()
        || !Character || !Movement || !PathFollowing || !NavLinkObject || bTraversalActive)
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("GENERATED_TRAVERSAL_REJECTED"), FString::Printf(
            TEXT("destination=%s type=%d character=%d movement=%d pathFollowing=%d link=%d alreadyActive=%d"),
            *Destination.ToCompactString(), static_cast<int32>(NewType), Character != nullptr,
            Movement != nullptr, PathFollowing != nullptr, NavLinkObject != nullptr, bTraversalActive));
        return false;
    }

    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    const float CapsuleHalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.0f;
    StartLocation = Character->GetActorLocation();
    DestinationLocation = Destination + FVector(0.0f, 0.0f, CapsuleHalfHeight + Settings->CapsuleLandingClearance);
    const FVector Facing = (DestinationLocation - StartLocation).GetSafeNormal2D();
    if (!Facing.IsNearlyZero())
    {
        Character->SetActorRotation(Facing.Rotation());
    }

    VerticalTarget = FVector(StartLocation.X, StartLocation.Y, DestinationLocation.Z + Settings->PullOverHeight);
    PathFollowingComponent = PathFollowing;
    NavLink = NavLinkObject;
    TraversalType = NewType;
    const EHellRunLocomotionState OldState = LocomotionState;
    switch (NewType)
    {
    case EHellRunGeneratedTraversalType::Jump: LocomotionState = EHellRunLocomotionState::GeneratedJump; break;
    case EHellRunGeneratedTraversalType::Vault: LocomotionState = EHellRunLocomotionState::GeneratedVault; break;
    case EHellRunGeneratedTraversalType::Mantle: LocomotionState = EHellRunLocomotionState::GeneratedMantle; break;
    default: LocomotionState = EHellRunLocomotionState::GeneratedClimb; break;
    }
    UpdateTraversalAnimationAction(OldState, LocomotionState);
    Progress = 0.0f;
    bPullingOver = false;
    bTraversalActive = true;
    ForceTraversalNetUpdate();

    if (NewType == EHellRunGeneratedTraversalType::Jump || NewType == EHellRunGeneratedTraversalType::Vault)
    {
        const bool bJump = NewType == EHellRunGeneratedTraversalType::Jump;
        const float Speed = bJump ? Settings->JumpSpeed : Settings->VaultSpeed;
        const float MinimumArc = bJump ? Settings->JumpMinimumArcHeight : Settings->VaultMinimumArcHeight;
        Duration = FMath::Clamp(FVector::Distance(StartLocation, DestinationLocation) / FMath::Max(Speed, 1.0f),
            0.22f, bJump ? 0.8f : 0.65f);
        ArcHeight = FMath::Max(MinimumArc, FMath::Abs(DestinationLocation.Z - StartLocation.Z) + 70.0f);
    }

    // Stop residual ground velocity without notifying path following that the
    // agent is unable to move. StopMovementImmediately() fires OnUnableToMove,
    // which aborts the very path whose traversal segment we are starting.
    Movement->Velocity = FVector::ZeroVector;
    Movement->UpdateComponentVelocity();
    Movement->SetMovementMode(MOVE_Flying);
    SetComponentTickEnabled(true);
    FHellRunNavigationDebugLog::Write(Character, TEXT("GENERATED_TRAVERSAL_STARTED"), FString::Printf(
        TEXT("state=%s start=%s destination=%s verticalTarget=%s duration=%.3f arc=%.1f"),
        LocomotionStateName(LocomotionState), *StartLocation.ToCompactString(),
        *DestinationLocation.ToCompactString(), *VerticalTarget.ToCompactString(), Duration, ArcHeight));
    return true;
}

void UHellRunTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!HasTraversalAuthority())
    {
        SetComponentTickEnabled(false);
        return;
    }
    if (LocomotionState == EHellRunLocomotionState::Recovering)
    {
        RecoveryTimeRemaining -= DeltaTime;
        if (RecoveryTimeRemaining <= 0.0f)
        {
            SetGroundedLocomotion();
        }
        return;
    }
    if (LocomotionState == EHellRunLocomotionState::Ballistic)
    {
        const ACharacter* BallisticCharacter = Cast<ACharacter>(GetOwner());
        const UCharacterMovementComponent* BallisticMovement = BallisticCharacter ? BallisticCharacter->GetCharacterMovement() : nullptr;
        if (BallisticMovement && BallisticMovement->IsFalling())
        {
            bObservedBallisticAirborne = true;
        }
        else if (bObservedBallisticAirborne)
        {
            BeginLocomotionRecovery();
        }
        return;
    }
    if (bVoxelLocomotionActive)
    {
        ACharacter* VoxelCharacter = Cast<ACharacter>(GetOwner());
        UCharacterMovementComponent* VoxelMovement = VoxelCharacter ? VoxelCharacter->GetCharacterMovement() : nullptr;
        if (!VoxelCharacter || !VoxelMovement)
        {
            SetGroundedLocomotion();
            return;
        }

        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();

        if (LocomotionState == EHellRunLocomotionState::VoxelDrop)
        {
            VoxelActionElapsed += DeltaTime;
            if (!bDropLaunched)
            {
                VoxelMovement->Velocity = FVector::ZeroVector;
                VoxelMovement->UpdateComponentVelocity();
                VoxelMovement->SetMovementMode(MOVE_Flying);
                const FVector NewLocation = FMath::VInterpConstantTo(VoxelCharacter->GetActorLocation(),
                    DropTakeoffLocation, DeltaTime, Settings->DropTakeoffSpeed);
                if (!MoveVoxelWithSweep(*VoxelCharacter, NewLocation))
                {
                    return;
                }
                if (FVector::DistSquared(NewLocation, DropTakeoffLocation) <= FMath::Square(5.0f))
                {
                    const FVector Delta = DestinationLocation - NewLocation;
                    const float HorizontalDistance = Delta.Size2D();
                    const float HorizontalSpeed = FMath::Max(1.0f, Settings->DropMinimumHorizontalSpeed);
                    const float Gravity = FMath::Abs(VoxelMovement->GetGravityZ());
                    const float DropHeight = FMath::Max(0.0f, -Delta.Z);
                    const float NaturalFallTime = FMath::Sqrt(2.0f * DropHeight / FMath::Max(Gravity, 1.0f));
                    const float TravelTime = FMath::Max3(
                        0.18f, HorizontalDistance / HorizontalSpeed, NaturalFallTime);
                    const float RequiredVerticalSpeed = (Delta.Z + 0.5f * Gravity * FMath::Square(TravelTime)) / TravelTime;
                    VoxelMovement->SetMovementMode(MOVE_Falling);
                    VoxelMovement->Velocity = Delta.GetSafeNormal2D() * HorizontalSpeed
                        + FVector::UpVector * FMath::Min(RequiredVerticalSpeed, Settings->DropMaximumUpwardSpeed);
                    VoxelMovement->UpdateComponentVelocity();
                    bDropLaunched = true;
                    VoxelActionElapsed = 0.0f;
                    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_DROP_LAUNCHED"), FString::Printf(
                        TEXT("takeoff=%s landing=%s velocity=%s travelTime=%.3f gravity=%.1f"),
                        *NewLocation.ToCompactString(), *DestinationLocation.ToCompactString(),
                        *VoxelMovement->Velocity.ToCompactString(), TravelTime, Gravity));
                }
            }
            else
            {
                bDropObservedAirborne |= VoxelMovement->IsFalling();
                const float DistanceToLanding = FVector::Distance(VoxelCharacter->GetActorLocation(), DestinationLocation);
                const bool bStuckAfterLaunch = VoxelActionElapsed >= Settings->DropStuckDetectionDelay
                    && VoxelMovement->Velocity.SizeSquared() <= FMath::Square(25.0f)
                    && DistanceToLanding > Settings->VoxelTraversalPointReachDistance;
                if (bStuckAfterLaunch && !bDropRecoveryActive)
                {
                    bDropRecoveryActive = true;
                    VoxelMovement->SetMovementMode(MOVE_Flying);
                    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_DROP_RECOVERY"), FString::Printf(
                        TEXT("location=%s landing=%s distance=%.1f elapsed=%.3f"),
                        *VoxelCharacter->GetActorLocation().ToCompactString(),
                        *DestinationLocation.ToCompactString(), DistanceToLanding, VoxelActionElapsed));
                }
                if (bDropRecoveryActive)
                {
                    const FVector NewLocation = FMath::VInterpConstantTo(VoxelCharacter->GetActorLocation(),
                        DestinationLocation, DeltaTime, Settings->DropRecoverySpeed);
                    if (!MoveVoxelWithSweep(*VoxelCharacter, NewLocation))
                    {
                        return;
                    }
                    if (FVector::DistSquared(NewLocation, DestinationLocation)
                        <= FMath::Square(Settings->VoxelTraversalPointReachDistance))
                    {
                        SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
                    }
                }
                else if (bDropObservedAirborne && !VoxelMovement->IsFalling())
                {
                    if (DistanceToLanding <= Settings->VoxelTraversalPointReachDistance)
                    {
                        SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
                    }
                    else
                    {
                        // A ballistic drop can land short after colliding with the
                        // ledge or nearby geometry. Recover to the authored endpoint
                        // instead of returning to the takeoff and repeating the drop.
                        bDropRecoveryActive = true;
                        VoxelMovement->SetMovementMode(MOVE_Flying);
                        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_DROP_LANDING_RECOVERY"),
                            FString::Printf(TEXT("location=%s landing=%s distance=%.1f"),
                                *VoxelCharacter->GetActorLocation().ToCompactString(),
                                *DestinationLocation.ToCompactString(), DistanceToLanding));
                    }
                }
            }
            return;
        }

        if (LocomotionState == EHellRunLocomotionState::VoxelJump
            || LocomotionState == EHellRunLocomotionState::VoxelVault)
        {
            VoxelMovement->Velocity = FVector::ZeroVector;
            VoxelMovement->UpdateComponentVelocity();
            VoxelMovement->SetMovementMode(MOVE_Flying);
            Progress = FMath::Clamp(Progress + DeltaTime / FMath::Max(Duration, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
            const float Alpha = FMath::InterpEaseInOut(0.0f, 1.0f, Progress, 2.0f);
            FVector Location = FMath::Lerp(StartLocation, DestinationLocation, Alpha);
            Location.Z += FMath::Sin(Progress * UE_PI) * ArcHeight;
            if (!MoveVoxelWithSweep(*VoxelCharacter, Location)) return;
            if (Progress >= 1.0f)
            {
                SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
            }
            return;
        }

        if (LocomotionState == EHellRunLocomotionState::VoxelMantle)
        {
            const FVector Target = bVoxelMantlePullingOver ? DestinationLocation : VerticalTarget;
            const float Speed = bVoxelMantlePullingOver
                ? Settings->MantlePullOverSpeed : Settings->MantleVerticalSpeed;
            const FVector NewLocation = FMath::VInterpConstantTo(
                VoxelCharacter->GetActorLocation(), Target, DeltaTime, Speed);
            if (!MoveVoxelWithSweep(*VoxelCharacter, NewLocation)) return;
            if (FVector::DistSquared(VoxelCharacter->GetActorLocation(), Target) <= FMath::Square(8.0f))
            {
                if (bVoxelMantlePullingOver)
                {
                    SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
                }
                else
                {
                    bVoxelMantlePullingOver = true;
                }
            }
            return;
        }

        // The locomotion FSM owns translation while this typed segment is
        // active. Clear velocity directly; StopMovementImmediately() reports a
        // MovementStop to path following and destroys the active voxel route.
        VoxelMovement->Velocity = FVector::ZeroVector;
        VoxelMovement->UpdateComponentVelocity();
        VoxelMovement->SetMovementMode(MOVE_Flying);
        const float Speed = LocomotionState == EHellRunLocomotionState::VoxelClimb
            ? Settings->ClimbVerticalSpeed
            : (LocomotionState == EHellRunLocomotionState::Flying
                ? Settings->FlightSpeed
                : Settings->MantlePullOverSpeed);
        const FVector NewLocation = FMath::VInterpConstantTo(
            VoxelCharacter->GetActorLocation(), DestinationLocation, DeltaTime, Speed);
        if (!MoveVoxelWithSweep(*VoxelCharacter, NewLocation))
        {
            return;
        }
        if (FVector::DistSquared(NewLocation, DestinationLocation)
            <= FMath::Square(Settings->VoxelTraversalPointReachDistance))
        {
            SetVoxelLocomotionState(EHellRunLocomotionState::VoxelApproach);
        }
        return;
    }
    if (!bTraversalActive)
    {
        return;
    }

    ACharacter* Character = Cast<ACharacter>(GetOwner());
    if (!Character)
    {
        FinishTraversal(false);
        return;
    }

    // A pooled agent can be relocated without EndPlay or UnPossess. In that
    // case the old smart-link state must not keep translating the recycled
    // character toward a destination from its previous life.
    if (!PathFollowingComponent.IsValid() || !NavLink.IsValid())
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("GENERATED_TRAVERSAL_ABORTED"),
            TEXT("reason=link_context_lost"));
        FinishTraversal(false);
        return;
    }
    const float PlannedDistance = FVector::Distance(StartLocation, DestinationLocation);
    const float DistanceFromPlan = FMath::Min(
        FVector::Distance(Character->GetActorLocation(), StartLocation),
        FVector::Distance(Character->GetActorLocation(), DestinationLocation));
    if (DistanceFromPlan > FMath::Max(PlannedDistance + 300.0f, 600.0f))
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("GENERATED_TRAVERSAL_ABORTED"),
            FString::Printf(TEXT("reason=external_relocation distanceFromPlan=%.1f plannedDistance=%.1f"),
                DistanceFromPlan, PlannedDistance));
        FinishTraversal(false);
        return;
    }

    // Generated traversal has exclusive ownership of translation. CharacterMovement
    // can reconstruct velocity from SetActorLocation and apply it again on its next
    // tick; clearing it only at traversal start allowed mantles to drift for seconds
    // and carry agents through open air instead of reaching their fixed endpoint.
    if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
    {
        Movement->Velocity = FVector::ZeroVector;
        Movement->UpdateComponentVelocity();
        Movement->SetMovementMode(MOVE_Flying);
    }

    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    if (TraversalType == EHellRunGeneratedTraversalType::Jump || TraversalType == EHellRunGeneratedTraversalType::Vault)
    {
        Progress = FMath::Clamp(Progress + DeltaTime / FMath::Max(Duration, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
        const float Alpha = FMath::InterpEaseInOut(0.0f, 1.0f, Progress, 2.0f);
        FVector Location = FMath::Lerp(StartLocation, DestinationLocation, Alpha);
        Location.Z += FMath::Sin(Progress * UE_PI) * ArcHeight;
        if (!MoveVoxelWithSweep(*Character, Location))
        {
            FinishTraversal(false);
            return;
        }
        if (Progress >= 1.0f)
        {
            FinishTraversal(true);
        }
        return;
    }

    const FVector Target = bPullingOver ? DestinationLocation : VerticalTarget;
    const bool bMantle = TraversalType == EHellRunGeneratedTraversalType::Mantle;
    const float VerticalSpeed = bMantle ? Settings->MantleVerticalSpeed : Settings->ClimbVerticalSpeed;
    const float PullSpeed = bMantle ? Settings->MantlePullOverSpeed : Settings->ClimbPullOverSpeed;
    const FVector NewLocation = FMath::VInterpConstantTo(
        Character->GetActorLocation(), Target, DeltaTime,
        bPullingOver ? PullSpeed : VerticalSpeed);
    if (!MoveVoxelWithSweep(*Character, NewLocation))
    {
        FinishTraversal(false);
        return;
    }

    if (FVector::DistSquared(Character->GetActorLocation(), Target) <= FMath::Square(8.0f))
    {
        if (!bPullingOver)
        {
            bPullingOver = true;
        }
        else
        {
            FinishTraversal(true);
        }
    }
}

void UHellRunTraversalComponent::FinishTraversal(bool bCompleteLink)
{
    UPathFollowingComponent* PathFollowing = PathFollowingComponent.Get();
    UObject* LinkObject = NavLink.Get();
    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("GENERATED_TRAVERSAL_FINISHED"), FString::Printf(
        TEXT("state=%s complete=%d location=%s destination=%s pathFollowing=%d link=%d"),
        LocomotionStateName(LocomotionState), bCompleteLink,
        GetOwner() ? *GetOwner()->GetActorLocation().ToCompactString() : TEXT("none"),
        *DestinationLocation.ToCompactString(), PathFollowing != nullptr, LinkObject != nullptr));
    bTraversalActive = false;
    bPullingOver = false;
    PathFollowingComponent.Reset();
    NavLink.Reset();
    BeginLocomotionRecovery();

    if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            Movement->SetMovementMode(MOVE_Walking);
        }
    }
    if (bCompleteLink && PathFollowing && LinkObject)
    {
        if (INavLinkCustomInterface* CustomLink = Cast<INavLinkCustomInterface>(LinkObject))
        {
            PathFollowing->FinishUsingCustomLink(CustomLink);
        }
    }
}

void UHellRunTraversalComponent::SetVoxelLocomotionState(EHellRunLocomotionState NewState)
{
    if (!HasTraversalAuthority())
    {
        return;
    }
    if (bTraversalActive)
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_STATE_REJECTED"), FString::Printf(
            TEXT("requested=%s reason=generated_traversal_active current=%s"),
            LocomotionStateName(NewState), LocomotionStateName(LocomotionState)));
        return;
    }
    const EHellRunLocomotionState OldState = LocomotionState;
    LocomotionState = NewState;
    UpdateTraversalAnimationAction(OldState, NewState);
    bVoxelLocomotionActive = NewState == EHellRunLocomotionState::VoxelClimb
        || NewState == EHellRunLocomotionState::VoxelMantle
        || NewState == EHellRunLocomotionState::VoxelDrop
        || NewState == EHellRunLocomotionState::VoxelJump
        || NewState == EHellRunLocomotionState::VoxelVault
        || NewState == EHellRunLocomotionState::Flying;
    RecoveryTimeRemaining = 0.0f;
    bObservedBallisticAirborne = false;
    SetComponentTickEnabled(bVoxelLocomotionActive);
    if (OldState != NewState)
    {
        ForceTraversalNetUpdate();
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("LOCOMOTION_STATE"), FString::Printf(
            TEXT("old=%s new=%s destination=%s voxelActive=%d"),
            LocomotionStateName(OldState), LocomotionStateName(NewState),
            *DestinationLocation.ToCompactString(), bVoxelLocomotionActive));
    }
}

void UHellRunTraversalComponent::StartVoxelLocomotion(EHellRunLocomotionState NewState, const FVector& TargetLocation)
{
    if (!HasTraversalAuthority())
    {
        return;
    }
    if (bTraversalActive)
    {
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_START_REJECTED"), FString::Printf(
            TEXT("requested=%s target=%s reason=generated_traversal_active"),
            LocomotionStateName(NewState), *TargetLocation.ToCompactString()));
        return;
    }
    if (bVoxelLocomotionActive)
    {
        // SetMoveSegment can be refreshed by crowd/path-following while the
        // same typed edge is already executing. Restarting here resets the
        // vault arc or drop takeoff every frame, which makes the character
        // creep through open air and can replace a committed drop before
        // gravity takes over. The active FSM owns translation until it changes
        // itself back to VoxelApproach.
        if (NewState != LocomotionState
            || !TargetLocation.Equals(DestinationLocation, 1.0f))
        {
            FHellRunNavigationDebugLog::Write(
                GetOwner(),
                TEXT("VOXEL_START_IGNORED"),
                FString::Printf(
                    TEXT("requested=%s target=%s reason=typed_segment_active current=%s destination=%s"),
                    LocomotionStateName(NewState),
                    *TargetLocation.ToCompactString(),
                    LocomotionStateName(LocomotionState),
                    *DestinationLocation.ToCompactString()));
        }
        return;
    }
    DestinationLocation = TargetLocation;
    StartLocation = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
    Progress = 0.0f;
    VoxelActionElapsed = 0.0f;
    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_SEGMENT_STARTED"), FString::Printf(
        TEXT("state=%s from=%s target=%s distance=%.1f"), LocomotionStateName(NewState),
        GetOwner() ? *GetOwner()->GetActorLocation().ToCompactString() : TEXT("none"),
        *TargetLocation.ToCompactString(), GetOwner() ? FVector::Distance(GetOwner()->GetActorLocation(), TargetLocation) : -1.0f));
    SetVoxelLocomotionState(NewState);
    if (NewState == EHellRunLocomotionState::VoxelMantle)
    {
        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
        const FVector MantleForward = (DestinationLocation - StartLocation).GetSafeNormal2D();
        // Begin the lift slightly away from the supporting face. A capsule may
        // already be touching that wall at the route point, and a strictly
        // vertical sweep then reports time-zero blocking even though moving up
        // beside the wall is the intended mantle motion.
        const FVector LiftBase = StartLocation
            - MantleForward * FMath::Max(Settings->Mantle.DistanceFromEdge, 12.0f);
        VerticalTarget = FVector(LiftBase.X, LiftBase.Y,
            DestinationLocation.Z + Settings->PullOverHeight);
        bVoxelMantlePullingOver = false;
    }

    if (NewState == EHellRunLocomotionState::VoxelDrop)
    {
        ACharacter* Character = Cast<ACharacter>(GetOwner());
        UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
        if (Character && Movement)
        {
            const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
            const FVector HorizontalDirection = (DestinationLocation - StartLocation).GetSafeNormal2D();
            const float HorizontalDistance = FVector::Dist2D(StartLocation, DestinationLocation);
            const float ForwardDistance = FMath::Clamp(
                HorizontalDistance - Settings->DropTakeoffLandingInset,
                FMath::Min(Settings->DropTakeoffForwardDistance, HorizontalDistance), HorizontalDistance);
            DropTakeoffLocation = StartLocation + HorizontalDirection * ForwardDistance;
            bDropLaunched = false;
            bDropObservedAirborne = false;
            bDropRecoveryActive = false;
            FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("VOXEL_DROP_TAKEOFF"), FString::Printf(
                TEXT("start=%s takeoff=%s landing=%s forward=%.1f"), *StartLocation.ToCompactString(),
                *DropTakeoffLocation.ToCompactString(), *DestinationLocation.ToCompactString(), ForwardDistance));
        }
    }
    else if (NewState == EHellRunLocomotionState::VoxelJump || NewState == EHellRunLocomotionState::VoxelVault)
    {
        const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
        const bool bJump = NewState == EHellRunLocomotionState::VoxelJump;
        const float Speed = bJump ? Settings->JumpSpeed : Settings->VaultSpeed;
        Duration = FMath::Clamp(FVector::Distance(StartLocation, DestinationLocation) / FMath::Max(Speed, 1.0f),
            0.22f, bJump ? 0.8f : 0.65f);
        ArcHeight = FMath::Max(bJump ? Settings->JumpMinimumArcHeight : Settings->VaultMinimumArcHeight,
            FMath::Abs(DestinationLocation.Z - StartLocation.Z) + 70.0f);
    }
}

FVector UHellRunTraversalComponent::GetSafeTraversalLaneDestination(
    const FVector& Start,
    const FVector& Destination) const
{
    const ACharacter* Character = Cast<ACharacter>(GetOwner());
    const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
    const UWorld* World = GetWorld();
    if (!Character || !Capsule || !World) return Destination;

    const UHellRunTraversalNavigationSettings* Settings = GetDefault<UHellRunTraversalNavigationSettings>();
    const int32 BucketCount = FMath::Clamp(Settings->AlternativeRouteBuckets, 1, 5);
    if (BucketCount <= 1 || Settings->TraversalCorridorHalfWidth <= 0.0f) return Destination;
    const int32 Bucket = static_cast<int32>(
        GetTypeHash(Character->GetFName()) % static_cast<uint32>(BucketCount));
    const float LaneAlpha = FMath::GetMappedRangeValueClamped(
        FVector2D(0.0f, static_cast<float>(BucketCount - 1)),
        FVector2D(-1.0f, 1.0f),
        static_cast<float>(Bucket));
    const FVector Forward = (Destination - Start).GetSafeNormal2D();
    const FVector Candidate = Destination
        + FVector::CrossProduct(FVector::UpVector, Forward)
            * LaneAlpha * Settings->TraversalCorridorHalfWidth;

    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(HellRunTraversalLane), false, Character);
    TArray<FOverlapResult> Overlaps;
    World->OverlapMultiByObjectType(
        Overlaps,
        Candidate,
        FQuat::Identity,
        Objects,
        FCollisionShape::MakeCapsule(
            Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight()),
        Params);
    const bool bBlocked = Overlaps.ContainsByPredicate([](const FOverlapResult& Overlap)
    {
        return !Cast<APawn>(Overlap.GetActor());
    });
    return bBlocked ? Destination : Candidate;
}

bool UHellRunTraversalComponent::MoveVoxelWithSweep(
    ACharacter& Character,
    const FVector& Destination)
{
    FHitResult Hit;
    if (Character.SetActorLocation(Destination, true, &Hit, ETeleportType::None))
    {
        return true;
    }

    FHellRunNavigationDebugLog::Write(&Character, TEXT("VOXEL_TRAVERSAL_BLOCKED"), FString::Printf(
        TEXT("state=%s destination=%s hit=%s"),
        LocomotionStateName(LocomotionState), *Destination.ToCompactString(), *GetNameSafe(Hit.GetActor())));
    AAIController* Controller = Cast<AAIController>(Character.GetController());
    FVector RouteGoal = Destination;
    if (const UPathFollowingComponent* PathFollowing =
            Controller ? Controller->GetPathFollowingComponent() : nullptr)
    {
        const FNavPathSharedPtr ActivePath = PathFollowing->GetPath();
        if (ActivePath.IsValid() && !ActivePath->GetPathPoints().IsEmpty())
        {
            RouteGoal = ActivePath->GetPathPoints().Last().Location;
        }
    }
    SetVoxelLocomotionState(EHellRunLocomotionState::Grounded);
    if (Controller)
    {
        Controller->StopMovement();
        const TWeakObjectPtr<AAIController> WeakController = Controller;
        GetWorld()->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateWeakLambda(this,
                [WeakController, RouteGoal]()
                {
                    AAIController* RepathController = WeakController.Get();
                    if (!RepathController || !RepathController->GetPawn()
                        || RouteGoal.ContainsNaN())
                    {
                        return;
                    }
                    const EPathFollowingRequestResult::Type Result =
                        RepathController->MoveToLocation(
                            RouteGoal, -1.0f, true, true, true, false,
                            nullptr, true);
                    FHellRunNavigationDebugLog::Write(
                        RepathController->GetPawn(),
                        TEXT("VOXEL_TRAVERSAL_REPATH"),
                        FString::Printf(
                            TEXT("goal=%s result=%d"),
                            *RouteGoal.ToCompactString(),
                            static_cast<int32>(Result)));
                }));
    }
    return false;
}

void UHellRunTraversalComponent::BeginBallisticLocomotion()
{
    if (!HasTraversalAuthority() || bTraversalActive) return;
    const EHellRunLocomotionState OldState = LocomotionState;
    LocomotionState = EHellRunLocomotionState::Ballistic;
    UpdateTraversalAnimationAction(OldState, LocomotionState);
    bVoxelLocomotionActive = false;
    bObservedBallisticAirborne = false;
    SetComponentTickEnabled(true);
    ForceTraversalNetUpdate();
    FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("LOCOMOTION_STATE"), FString::Printf(
        TEXT("old=%s new=Ballistic"), LocomotionStateName(OldState)));
}

void UHellRunTraversalComponent::BeginLocomotionRecovery()
{
    if (!HasTraversalAuthority())
    {
        return;
    }
    const EHellRunLocomotionState OldState = LocomotionState;
    LocomotionState = EHellRunLocomotionState::Recovering;
    UpdateTraversalAnimationAction(OldState, LocomotionState);
    bVoxelLocomotionActive = false;
    RecoveryTimeRemaining = 0.15f;
    bObservedBallisticAirborne = false;
    SetComponentTickEnabled(true);
    if (OldState != LocomotionState)
    {
        ForceTraversalNetUpdate();
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("LOCOMOTION_STATE"), FString::Printf(
            TEXT("old=%s new=Recovering"), LocomotionStateName(OldState)));
    }
}

void UHellRunTraversalComponent::SetGroundedLocomotion()
{
    if (!HasTraversalAuthority() || bTraversalActive) return;
    const EHellRunLocomotionState OldState = LocomotionState;
    LocomotionState = EHellRunLocomotionState::Grounded;
    UpdateTraversalAnimationAction(OldState, LocomotionState);
    bVoxelLocomotionActive = false;
    RecoveryTimeRemaining = 0.0f;
    bObservedBallisticAirborne = false;
    SetComponentTickEnabled(false);
    if (OldState != LocomotionState)
    {
        ForceTraversalNetUpdate();
        FHellRunNavigationDebugLog::Write(GetOwner(), TEXT("LOCOMOTION_STATE"), FString::Printf(
            TEXT("old=%s new=Grounded"), LocomotionStateName(OldState)));
    }
}

void UHellRunTraversalComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (HasTraversalAuthority() && bTraversalActive)
    {
        FinishTraversal(false);
    }
    Super::EndPlay(EndPlayReason);
}
