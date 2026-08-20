#pragma once

#include "BaseGeneratedNavLinksProxy.h"
#include "NavigationSystem.h"
#include "NavAreas/NavArea.h"
#include "NavMesh/RecastNavMesh.h"
#include "HellRunTraversalNavigation.generated.h"

class AHellRunVoxelNavVolume;

/** Navigation system for serialized HellRun voxel navigation maps. */
UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UHellRunNavigationSystemV1
    : public UNavigationSystemV1
{
    GENERATED_BODY()

public:
    virtual void Tick(float DeltaSeconds) override;

private:
    float SerializedVoxelStartupTime = 0.0f;
    TWeakObjectPtr<AHellRunVoxelNavVolume> HandledSerializedVoxelVolume;
};

UCLASS(Config=Engine, DefaultConfig, EditInlineNew)
class HELLRUNTRAVERSALNAVIGATION_API UHellRunNavigationSystemConfig
    : public UNavigationSystemModuleConfig
{
    GENERATED_BODY()

public:
    UHellRunNavigationSystemConfig(
        const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UNavArea_HellRunJump : public UNavArea
{ GENERATED_BODY() public: UNavArea_HellRunJump(); };
UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UNavArea_HellRunVault : public UNavArea
{ GENERATED_BODY() public: UNavArea_HellRunVault(); };
UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UNavArea_HellRunMantle : public UNavArea
{ GENERATED_BODY() public: UNavArea_HellRunMantle(); };
UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UNavArea_HellRunClimb : public UNavArea
{ GENERATED_BODY() public: UNavArea_HellRunClimb(); };
UCLASS(Config=Engine)
class HELLRUNTRAVERSALNAVIGATION_API UNavArea_HellRunDrop : public UNavArea
{ GENERATED_BODY() public: UNavArea_HellRunDrop(); };

UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunJumpNavLinkProxy : public UBaseGeneratedNavLinksProxy
{ GENERATED_BODY() public: virtual bool OnLinkMoveStarted(UObject* PathComp, const FVector& DestPoint) override; };
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunVaultNavLinkProxy : public UBaseGeneratedNavLinksProxy
{ GENERATED_BODY() public: virtual bool OnLinkMoveStarted(UObject* PathComp, const FVector& DestPoint) override; };
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunMantleNavLinkProxy : public UBaseGeneratedNavLinksProxy
{ GENERATED_BODY() public: virtual bool OnLinkMoveStarted(UObject* PathComp, const FVector& DestPoint) override; };
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunClimbNavLinkProxy : public UBaseGeneratedNavLinksProxy
{ GENERATED_BODY() public: virtual bool OnLinkMoveStarted(UObject* PathComp, const FVector& DestPoint) override; };
UCLASS()
class HELLRUNTRAVERSALNAVIGATION_API UHellRunDropNavLinkProxy : public UBaseGeneratedNavLinksProxy
{ GENERATED_BODY() public: virtual bool OnLinkMoveStarted(UObject* PathComp, const FVector& DestPoint) override; };

/** Recast nav data that generates typed traversal edges during tile generation. */
UCLASS(Config=Engine, DefaultConfig)
class HELLRUNTRAVERSALNAVIGATION_API AHellRunRecastNavMesh : public ARecastNavMesh
{
    GENERATED_BODY()

public:
    AHellRunRecastNavMesh(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void ConditionalConstructGenerator() override;
    virtual void PostLoadPreRebuild() override;
    virtual void OnNavMeshGenerationFinished() override;
    virtual bool NeedsRebuild() const override;
    void RefreshTraversalSettings(bool bRebuildNavigation);

protected:
    virtual FRecastNavMeshGenerator* CreateGeneratorInstance() override;

private:
    void SyncTraversalGenerationConfigs();
    void PruneDisconnectedGroundPolys();
    void RefreshClimbPathDebugCache();
    void DrawActualClimbPaths() const;
    TArray<TPair<FVector, FVector>> CachedClimbPathEndpoints;
    float ClimbPathDebugRefreshTime = 0.0f;
};
