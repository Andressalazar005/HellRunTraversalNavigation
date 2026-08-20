#include "HellRunDynamicNavObstacleComponent.h"

#include "EngineUtils.h"
#include "HellRunVoxelNavVolume.h"

UHellRunDynamicNavObstacleComponent::UHellRunDynamicNavObstacleComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UHellRunDynamicNavObstacleComponent::BeginPlay()
{
    Super::BeginPlay();
    PrimaryComponentTick.TickInterval = FMath::Max(0.02f, UpdateInterval);
    RefreshNavigationObstacle();
}

void UHellRunDynamicNavObstacleComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RemoveFromNavigation();
    Super::EndPlay(EndPlayReason);
}

FBox UHellRunDynamicNavObstacleComponent::GetObstacleBounds() const
{
    const AActor* Owner = GetOwner();
    if (!Owner) return FBox(ForceInit);
    if (!BoundsExtentOverride.IsNearlyZero())
    {
        return FBox::BuildAABB(Owner->GetActorLocation(), BoundsExtentOverride.GetAbs());
    }
    return Owner->GetComponentsBoundingBox(true);
}

void UHellRunDynamicNavObstacleComponent::RefreshNavigationObstacle()
{
    UWorld* World = GetWorld();
    if (!World) return;
    if (!bNavigationObstacleEnabled)
    {
        RemoveFromNavigation();
        return;
    }

    const FBox NewBounds = GetObstacleBounds();
    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        It->UpdateDynamicObstacle(this, NewBounds, false);
    }
    LastRegisteredBounds = NewBounds;
    bWasRegistered = true;
}

void UHellRunDynamicNavObstacleComponent::RemoveFromNavigation()
{
    UWorld* World = GetWorld();
    if (!World || !bWasRegistered) return;
    for (TActorIterator<AHellRunVoxelNavVolume> It(World); It; ++It)
    {
        It->UpdateDynamicObstacle(this, FBox(ForceInit), true);
    }
    LastRegisteredBounds = FBox(ForceInit);
    bWasRegistered = false;
}

void UHellRunDynamicNavObstacleComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    PrimaryComponentTick.TickInterval = FMath::Max(0.02f, UpdateInterval);
    if (!bNavigationObstacleEnabled)
    {
        RemoveFromNavigation();
        return;
    }

    const FBox CurrentBounds = GetObstacleBounds();
    const bool bMoved = !bWasRegistered
        || FVector::DistSquared(CurrentBounds.GetCenter(), LastRegisteredBounds.GetCenter())
            >= FMath::Square(FMath::Max(0.0f, MovementThreshold))
        || !CurrentBounds.GetExtent().Equals(LastRegisteredBounds.GetExtent(), 1.0f);
    if (bMoved)
    {
        RefreshNavigationObstacle();
    }
}
