#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "NavigationPath.h"
#include "HellRunVoxelPathDebugPawn.generated.h"

class UBillboardComponent;
class UHellRunTraversalComponent;
class ULineBatchComponent;
class UTextRenderComponent;

UENUM(BlueprintType)
enum class EHellRunPathDebugPawnRole : uint8
{
    EnemySource UMETA(DisplayName="Enemy Source"),
    PlayerTarget UMETA(DisplayName="Player Target")
};

/**
 * Editor-time endpoint for testing the authoritative voxel navigation API
 * without a controller, director, StateTree, EQS, recycling, or crowd steering.
 */
UCLASS(Blueprintable, hidecategories=(AI, Input, Replication),
    meta=(PrioritizeCategories="Tester"))
class HELLRUNTRAVERSALNAVIGATION_API AHellRunVoxelPathDebugPawn : public ACharacter
{
    GENERATED_BODY()

public:
    AHellRunVoxelPathDebugPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }

    UFUNCTION(CallInEditor, BlueprintCallable, Category="Tester")
    void RefreshDebugPath();

    UFUNCTION(CallInEditor, BlueprintCallable, Category="Tester")
    void ClearDebugPath();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester",
        meta=(DisplayPriority="1"))
    EHellRunPathDebugPawnRole EndpointRole = EHellRunPathDebugPawnRole::EnemySource;

    /** Optional explicit endpoint. If unset, the nearest Player Target debugger is used. */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Tester",
        meta=(DisplayPriority="2"))
    TObjectPtr<AActor> ExplicitTarget = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester",
        meta=(DisplayPriority="3"))
    bool bLiveUpdateInEditor = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester",
        meta=(DisplayPriority="4", ClampMin="0.05", Units="s"))
    float UpdateInterval = 0.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Drawing")
    bool bDrawPointLabels = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Path Variations")
    bool bDrawPathVariations = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Path Variations",
        meta=(EditCondition="bDrawPathVariations", ClampMin="1", ClampMax="12"))
    int32 PathVariationCount = 5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Path Variations",
        meta=(EditCondition="bDrawPathVariations", ClampMin="0.0", ClampMax="0.5"))
    float PathVariationStrength = 0.18f;

    /** Multiple full searches are expensive; live variation refreshes use this slower cadence. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Path Variations",
        meta=(EditCondition="bDrawPathVariations", ClampMin="0.25", Units="s"))
    float PathVariationUpdateInterval = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Path Variations",
        meta=(EditCondition="bDrawPathVariations"))
    int32 PathVariationSeed = 1337;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Drawing",
        meta=(ClampMin="0.5"))
    float PathThickness = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tester|Drawing",
        meta=(ClampMin="4.0", Units="cm"))
    float PointRadius = 14.0f;

    /** Traversal capabilities and cost profile used directly by debug path queries. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Instanced, Category="Tester",
        meta=(DisplayPriority="5"))
    TObjectPtr<UHellRunTraversalComponent> TraversalCapabilities;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    FString Result = TEXT("NOT QUERIED");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    FString QueryDiagnostic;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    int32 PathPointCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    float EstimatedTravelSeconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    float PathLength = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    bool bAuthoritativeTypedGraphFound = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Tester|Result")
    int32 UniquePathVariationCount = 0;

private:
    AActor* ResolveTarget() const;
    FVector ResolveSupportedQueryLocation(const AActor* Endpoint) const;
    void DrawCurrentResult(float Lifetime) const;
    void RebuildPersistentPathDrawing();
    void RebuildPersistentLabels();
    static FColor ColorForMode(uint8 Mode);
    static const TCHAR* LabelForMode(uint8 Mode);

    UPROPERTY()
    TObjectPtr<UBillboardComponent> EditorIcon;

    UPROPERTY(Transient)
    TObjectPtr<ULineBatchComponent> PathLineBatcher;

    UPROPERTY()
    TObjectPtr<UTextRenderComponent> RoleLabel;

    UPROPERTY()
    TObjectPtr<UTextRenderComponent> ResultLabel;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UTextRenderComponent>> SegmentLabels;

    FNavPathSharedPtr CachedPath;
    TArray<FNavPathSharedPtr> CachedPathVariations;
    FVector LastQueryStart = FVector(BIG_NUMBER);
    FVector LastQueryGoal = FVector(BIG_NUMBER);
    FString LastLoggedQuerySignature;
    float TimeUntilNextUpdate = 0.0f;
};
