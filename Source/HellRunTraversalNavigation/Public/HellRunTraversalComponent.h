#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HellRunTraversalNavigationSettings.h"
#include "HellRunTraversalComponent.generated.h"

class UPathFollowingComponent;
class UAnimMontage;
class UAnimSequenceBase;

UENUM(BlueprintType)
enum class EHellRunGeneratedTraversalType : uint8
{
    Jump,
    Vault,
    Mantle,
    Climb
};

UENUM(BlueprintType)
enum class EHellRunLocomotionState : uint8
{
    Grounded,
    VoxelApproach,
    Ballistic,
    VoxelClimb,
    VoxelMantle,
    VoxelDrop,
    VoxelJump,
    VoxelVault,
    Flying,
    GeneratedJump,
    GeneratedVault,
    GeneratedMantle,
    GeneratedClimb,
    Recovering
};

/** Animation-facing identity for both generated-link and voxel traversal actions. */
UENUM(BlueprintType)
enum class EHellRunTraversalAnimationAction : uint8
{
    None,
    Jump,
    Vault,
    Mantle,
    Climb,
    Drop,
    Flying
};

/**
 * Replication-safe traversal lifecycle snapshot. Starting and finishing are
 * intentionally held for a short window so an AnimBP cannot miss the edge
 * when an enemy is using adaptive network update frequency.
 */
USTRUCT(BlueprintType)
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunTraversalAnimationState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    EHellRunTraversalAnimationAction Action = EHellRunTraversalAnimationAction::None;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bStarting = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bDoing = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bFinishing = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bJump = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bVault = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bMantle = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bClimb = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bDrop = false;

    UPROPERTY(BlueprintReadOnly, Category="AI|Traversal|Animation")
    bool bFlying = false;
};

/** Executes generated off-mesh links without depending on a project character class. */
UCLASS(ClassGroup=(AI),
    meta=(BlueprintSpawnableComponent, PrioritizeCategories="AI"))
class HELLRUNTRAVERSALNAVIGATION_API UHellRunTraversalComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UHellRunTraversalComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    bool StartGeneratedTraversal(const FVector& Destination, UPathFollowingComponent* PathFollowing,
        UObject* NavLinkObject, EHellRunGeneratedTraversalType TraversalType);

    UFUNCTION(BlueprintPure, Category="AI|Traversal")
    bool IsTraversalActive() const { return bTraversalActive; }

    UFUNCTION(BlueprintPure, Category="AI|Traversal")
    bool IsClimbing() const { return bTraversalActive && TraversalType == EHellRunGeneratedTraversalType::Climb; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanWalkNavigation() const { return bCanWalkNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanClimbNavigation() const { return bCanClimbNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanMantleNavigation() const { return bCanMantleNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanDropNavigation() const { return bCanDropNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanJumpNavigation() const { return bCanJumpNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanVaultNavigation() const { return bCanVaultNavigation; }

    /** True wall-surface locomotion. Keep this disabled for common zombies; they still use vault, mantle and drop edges. */
    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanWallClimbNavigation() const { return bCanClimbNavigation && bCanWallClimbNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool CanFlyNavigation() const { return bCanFlyNavigation; }

    UFUNCTION(BlueprintPure, Category="AI|Navigation Capabilities")
    bool PrefersFlyingNavigation() const { return bPreferFlyingNavigation; }

    bool CanUseGeneratedTraversalLinks() const { return bCanUseGeneratedTraversalLinks; }

    UFUNCTION(BlueprintPure, Category="AI|Locomotion")
    EHellRunLocomotionState GetLocomotionState() const { return LocomotionState; }

    UFUNCTION(BlueprintPure, Category="AI|Traversal|Animation")
    FHellRunTraversalAnimationState GetTraversalAnimationState() const;

    /** Optional sequences played automatically when a replicated traversal action begins. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Traversal|Animation")
    TObjectPtr<UAnimSequenceBase> VaultTraversalAnimation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Traversal|Animation")
    TObjectPtr<UAnimSequenceBase> MantleTraversalAnimation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Traversal|Animation")
    TObjectPtr<UAnimSequenceBase> ClimbTraversalAnimation;

    /** AnimGraph slot that receives the generated traversal montage. Override this for archetypes with a custom full-body layer. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Traversal|Animation")
    FName TraversalAnimationSlotName = TEXT("DefaultSlot");

    UFUNCTION(BlueprintPure, Category="AI|Locomotion")
    bool BlocksExternalMovementControl() const { return LocomotionState != EHellRunLocomotionState::Grounded; }

    UFUNCTION(BlueprintPure, Category="AI|Locomotion")
    bool AllowsBoidSteering() const
    {
        // The approach to a typed edge is still ordinary grounded movement.
        // Keeping separation active here prevents a crowd from permanently
        // pinning the last pawn in a traversal-entry conga line. Steering is
        // disabled only after the vertical/ballistic action owns movement.
        return LocomotionState == EHellRunLocomotionState::Grounded
            || LocomotionState == EHellRunLocomotionState::VoxelApproach;
    }

    void SetVoxelLocomotionState(EHellRunLocomotionState NewState);
    void StartVoxelLocomotion(EHellRunLocomotionState NewState, const FVector& TargetLocation);
    void BeginBallisticLocomotion();
    void BeginLocomotionRecovery();
    void SetGroundedLocomotion();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanWalkNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanUseGeneratedTraversalLinks = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanClimbNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanMantleNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanDropNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanJumpNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanVaultNavigation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities", meta=(EditCondition="bCanClimbNavigation"))
    bool bCanWallClimbNavigation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities")
    bool bCanFlyNavigation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Capabilities", meta=(EditCondition="bCanFlyNavigation"))
    bool bPreferFlyingNavigation = false;

    /** Lets an individual enemy or enemy Blueprint override its class profile from Project Settings. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Costs")
    bool bOverrideVoxelCostProfile = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Costs", meta=(EditCondition="bOverrideVoxelCostProfile"))
    FHellRunVoxelTraversalCostProfile VoxelCostProfileOverride;

    /**
     * Gives each enemy a stable, slightly different preference among
     * near-equal voxel corridors. This changes route selection, not collision
     * avoidance; crowd steering still handles local separation.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Variation")
    bool bUseVoxelPathVariation = true;

    /** Maximum proportional edge-cost variation. Keep modest so detours remain sensible. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Variation",
        meta=(EditCondition="bUseVoxelPathVariation", ClampMin="0.0", ClampMax="0.5"))
    float VoxelPathVariationStrength = 0.12f;

    /**
     * Stable route personality. Zero derives a seed from the owning actor;
     * assign explicit seeds when spawning pooled enemies that reuse actors.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Navigation Variation",
        meta=(EditCondition="bUseVoxelPathVariation"))
    int32 VoxelPathVariationSeed = 0;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    UFUNCTION()
    void OnRep_LocomotionState(EHellRunLocomotionState OldState);
    void PlayTraversalAnimation(EHellRunTraversalAnimationAction Action);
    void StopTraversalAnimation();
    bool HasTraversalAuthority() const;
    void ForceTraversalNetUpdate() const;
    void UpdateTraversalAnimationAction(EHellRunLocomotionState OldState, EHellRunLocomotionState NewState);
    static EHellRunTraversalAnimationAction GetAnimationActionForState(EHellRunLocomotionState State);
    float GetSynchronizedWorldTimeSeconds() const;
    void FinishTraversal(bool bCompleteLink);
    FVector GetSafeTraversalLaneDestination(const FVector& Start, const FVector& Destination) const;
    bool MoveVoxelWithSweep(ACharacter& Character, const FVector& Destination);

    UPROPERTY(Replicated)
    bool bTraversalActive = false;
    bool bPullingOver = false;
    FVector StartLocation = FVector::ZeroVector;
    FVector VerticalTarget = FVector::ZeroVector;
    FVector DestinationLocation = FVector::ZeroVector;
    UPROPERTY(Replicated)
    EHellRunGeneratedTraversalType TraversalType = EHellRunGeneratedTraversalType::Climb;

    UPROPERTY(ReplicatedUsing=OnRep_LocomotionState)
    EHellRunLocomotionState LocomotionState = EHellRunLocomotionState::Grounded;

    UPROPERTY(Replicated)
    EHellRunTraversalAnimationAction AnimationTraversalAction = EHellRunTraversalAnimationAction::None;

    UPROPERTY(Replicated)
    bool bAnimationTraversalActive = false;

    UPROPERTY(Replicated)
    float AnimationTraversalStartTime = -BIG_NUMBER;

    UPROPERTY(Replicated)
    float AnimationTraversalFinishTime = -BIG_NUMBER;

    UPROPERTY(Transient)
    TObjectPtr<UAnimMontage> ActiveTraversalMontage;

    /** Minimum AnimBP-visible start pulse, independent of rendering/network frame rate. */
    UPROPERTY(EditAnywhere, Category="AI|Traversal|Animation", meta=(ClampMin="0.05", Units="s"))
    float AnimationTraversalStartWindow = 0.18f;

    /** Minimum AnimBP-visible finish pulse, independent of rendering/network frame rate. */
    UPROPERTY(EditAnywhere, Category="AI|Traversal|Animation", meta=(ClampMin="0.05", Units="s"))
    float AnimationTraversalFinishWindow = 0.22f;

    float RecoveryTimeRemaining = 0.0f;
    bool bObservedBallisticAirborne = false;
    bool bVoxelLocomotionActive = false;
    bool bVoxelMantlePullingOver = false;
    float Progress = 0.0f;
    float Duration = 0.35f;
    float ArcHeight = 145.0f;
    FVector DropTakeoffLocation = FVector::ZeroVector;
    bool bDropLaunched = false;
    bool bDropObservedAirborne = false;
    bool bDropRecoveryActive = false;
    float VoxelActionElapsed = 0.0f;
    TWeakObjectPtr<UPathFollowingComponent> PathFollowingComponent;
    TWeakObjectPtr<UObject> NavLink;
};
