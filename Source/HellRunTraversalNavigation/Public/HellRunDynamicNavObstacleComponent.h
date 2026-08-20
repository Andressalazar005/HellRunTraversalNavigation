#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HellRunDynamicNavObstacleComponent.generated.h"

/** Registers a moving actor's bounds with every overlapping baked voxel volume. */
UCLASS(ClassGroup=(AI), meta=(BlueprintSpawnableComponent))
class HELLRUNTRAVERSALNAVIGATION_API UHellRunDynamicNavObstacleComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UHellRunDynamicNavObstacleComponent();

    UFUNCTION(BlueprintCallable, Category="HellRun|Dynamic Navigation")
    void RefreshNavigationObstacle();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic Navigation")
    bool bNavigationObstacleEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic Navigation", meta=(ClampMin="0.02", Units="s"))
    float UpdateInterval = 0.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic Navigation", meta=(ClampMin="0.0", Units="cm"))
    float MovementThreshold = 10.0f;

    /** Used instead of actor bounds when non-zero. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic Navigation", meta=(Units="cm"))
    FVector BoundsExtentOverride = FVector::ZeroVector;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    FBox GetObstacleBounds() const;
    void RemoveFromNavigation();

    FBox LastRegisteredBounds = FBox(ForceInit);
    bool bWasRegistered = false;
};
