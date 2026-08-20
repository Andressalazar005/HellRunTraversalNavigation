#pragma once

#include "CoreMinimal.h"
#include "HellRunTraversalNavigationSettings.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Navigation/PathFollowingComponent.h"
#include "HellRunVoxelNavigation.generated.h"

class AAIController;
class ACharacter;
class UWorld;
struct FAIMoveRequest;

UENUM(BlueprintType)
enum class EHellRunVoxelSegment : uint8
{
    Walk = 0,
    Climb = 1,
    Mantle = 2,
    Drop = 3,
    Fly = 4,
    Jump = 5,
    Vault = 6
};

namespace HellRunVoxelPath
{
    constexpr uint32 Magic = 0x48520000u;
    constexpr uint32 MagicMask = 0xFFFF0000u;
    constexpr uint32 ModeMask = 0x0000FF00u;
    constexpr uint32 ModeShift = 8u;
    constexpr uint32 CostMask = 0x000000FFu;
    constexpr float CostPrecision = 0.1f;
    inline uint32 MakeFlags(EHellRunVoxelSegment Mode, float SegmentCost = 0.0f)
    {
        const uint32 EncodedCost = static_cast<uint32>(FMath::Clamp(
            FMath::RoundToInt(SegmentCost / CostPrecision), 0, 255));
        return Magic | (static_cast<uint32>(Mode) << ModeShift) | EncodedCost;
    }
    inline bool IsVoxelFlags(uint32 Flags) { return (Flags & MagicMask) == Magic; }
    inline EHellRunVoxelSegment GetMode(uint32 Flags) { return static_cast<EHellRunVoxelSegment>((Flags & ModeMask) >> ModeShift); }
    inline float GetSegmentCost(uint32 Flags)
    {
        return static_cast<float>(Flags & CostMask) * CostPrecision;
    }
}

/**
 * Authoritative voxel path plus the exact accumulated A* costs used to select it.
 * SegmentCosts is destination-indexed and parallels GetPathPoints().
 */
class HELLRUNTRAVERSALNAVIGATION_API FHellRunVoxelNavigationPath : public FNavigationPath
{
public:
    using FNavigationPath::FNavigationPath;

    TArray<float> SegmentCosts;
    float TotalCost = 0.0f;
};

enum class EHellRunNavigationPathOutcome : uint8
{
    Complete,
    Partial,
    Unreachable
};

enum class EHellRunNavigationPathProvider : uint8
{
    None,
    Recast,
    Voxel
};

/**
 * Provider-neutral result returned to movement policy. Navigation reports what it
 * found; the controller decides whether to retain, follow, or retry that result.
 */
struct HELLRUNTRAVERSALNAVIGATION_API FHellRunNavigationPathResult
{
    FNavPathSharedPtr Path;
    EHellRunNavigationPathOutcome Outcome = EHellRunNavigationPathOutcome::Unreachable;
    EHellRunNavigationPathProvider Provider = EHellRunNavigationPathProvider::None;

    bool HasUsablePath() const { return Path.IsValid() && Path->IsValid() && Path->GetPathPoints().Num() > 1; }
    bool IsComplete() const { return Outcome == EHellRunNavigationPathOutcome::Complete; }
};

/** Game-thread path query service used by Volumetric Hybrid mode. */
class HELLRUNTRAVERSALNAVIGATION_API FHellRunVoxelNavigation
{
public:
    static FNavPathSharedPtr FindPath(ACharacter& Character, const FVector& Goal);
    static FNavPathSharedPtr FindPathFrom(
        ACharacter& Character,
        const FVector& Start,
        const FVector& Goal,
        bool bRequireEscapeRoute = true);
    static const FString& GetLastQueryDiagnostic();
    static bool HasAuthoritativeTypedEdgeGraph(const UWorld* World);
    /** Uses the same typed-route cost model used by provider selection. */
    static float EstimateTraversalSeconds(const ACharacter& Character, const FNavPathSharedPtr& Path);
    static FHellRunNavigationPathResult QueryBestPath(
        ACharacter& Character,
        const FVector& Goal,
        const FNavPathSharedPtr& GroundPath,
        bool bRequireEscapeRoute = true);
};

/** Directly follows authoritative typed voxel segments. */
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunTraversalPathFollowingComponent : public UPathFollowingComponent
{
    GENERATED_BODY()

public:
    virtual void AbortMove(const UObject& Instigator, FPathFollowingResultFlags::Type AbortFlags,
        FAIRequestID RequestID = FAIRequestID::CurrentRequest,
        EPathFollowingVelocityMode VelocityMode = EPathFollowingVelocityMode::Reset) override;
    virtual void SetMoveSegment(int32 SegmentStartIndex) override;
    virtual void UpdatePathSegment() override;
    virtual void FollowPathSegment(float DeltaTime) override;
    virtual void OnPathFinished(const FPathFollowingResult& Result) override;

    bool IsFollowingVoxelPath() const;
    bool IsExecutingVoxelTraversal() const;
    EHellRunVoxelSegment GetCurrentVoxelMode() const;

protected:
    virtual bool IsOnPath() const override;

private:
    const FNavigationPath* CommittedVoxelPath = nullptr;
};

/** Thin routing entry used by AI controllers; all selection and generation stays in the plugin. */
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunNavigationModeLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="HellRun|Navigation")
    static void SetNavigationMode(EHellRunNavigationMode NewMode);

    UFUNCTION(BlueprintPure, Category="HellRun|Navigation")
    static EHellRunNavigationMode GetNavigationMode();
};
