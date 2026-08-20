#include "HellRunTraversalNavigationSettings.h"

#include "HellRunTraversalNavigation.h"
#include "GameFramework/Character.h"
#include "UObject/UObjectIterator.h"

UHellRunTraversalNavigationSettings::UHellRunTraversalNavigationSettings()
{
    Jump.HorizontalReach = 450.0f;
    Jump.DistanceFromEdge = 12.0f;
    Jump.MaximumDepth = 110.0f;
    Jump.ArcHeight = 180.0f;
    Jump.EndpointHeightTolerance = 90.0f;
    Jump.SimilarLinkFilterDistance = 60.0f;

    Vault.HorizontalReach = 320.0f;
    Vault.DistanceFromEdge = 35.0f;
    Vault.MaximumDepth = 80.0f;
    Vault.ArcHeight = 145.0f;
    Vault.EndpointHeightTolerance = 65.0f;
    Vault.SimilarLinkFilterDistance = 0.0f;
    Vault.bCreateExtremityLinks = true;

    Mantle.HorizontalReach = 160.0f;
    Mantle.DistanceFromEdge = 8.0f;
    // Must match ABaseCharacter::MaxMantleHeight. Navigation must never emit a
    // mantle edge that the character locomotion implementation cannot perform.
    Mantle.MaximumDepth = 300.0f;
    Mantle.ArcHeight = 170.0f;
    Mantle.EndpointHeightTolerance = 70.0f;
    Mantle.SimilarLinkFilterDistance = 40.0f;

    Climb.HorizontalReach = 135.0f;
    Climb.DistanceFromEdge = 8.0f;
    Climb.MaximumDepth = 900.0f;
    Climb.ArcHeight = 120.0f;
    Climb.EndpointHeightTolerance = 140.0f;
    Climb.SimilarLinkFilterDistance = 50.0f;

    Drop.HorizontalReach = 260.0f;
    Drop.DistanceFromEdge = 10.0f;
    Drop.MaximumDepth = 700.0f;
    Drop.ArcHeight = 55.0f;
    Drop.EndpointHeightTolerance = 100.0f;
    Drop.SimilarLinkFilterDistance = 65.0f;
}

FHellRunVoxelTraversalCostProfile UHellRunTraversalNavigationSettings::GetVoxelCostProfileForCharacter(const ACharacter& Character) const
{
    const UClass* CharacterClass = Character.GetClass();
    const FHellRunVoxelTraversalCostProfile* BestProfile = nullptr;
    int32 BestDistance = MAX_int32;

    for (const TPair<TSoftClassPtr<ACharacter>, FHellRunVoxelTraversalCostProfile>& Pair : VoxelCostProfilesByCharacterClass)
    {
        const UClass* CandidateClass = Pair.Key.Get();
        if (!CandidateClass || !CharacterClass->IsChildOf(CandidateClass))
        {
            continue;
        }

        int32 Distance = 0;
        for (const UClass* Class = CharacterClass; Class && Class != CandidateClass; Class = Class->GetSuperClass())
        {
            ++Distance;
        }
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            BestProfile = &Pair.Value;
        }
    }

    return BestProfile ? *BestProfile : DefaultVoxelCostProfile;
}

#if WITH_EDITOR
void UHellRunTraversalNavigationSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    UNavArea_HellRunJump::StaticClass()->GetDefaultObject<UNavArea_HellRunJump>()->DefaultCost = JumpAreaCost;
    UNavArea_HellRunJump::StaticClass()->GetDefaultObject<UNavArea_HellRunJump>()->DrawColor = JumpAreaColor;
    UNavArea_HellRunVault::StaticClass()->GetDefaultObject<UNavArea_HellRunVault>()->DefaultCost = VaultAreaCost;
    UNavArea_HellRunVault::StaticClass()->GetDefaultObject<UNavArea_HellRunVault>()->DrawColor = VaultAreaColor;
    UNavArea_HellRunMantle::StaticClass()->GetDefaultObject<UNavArea_HellRunMantle>()->DefaultCost = MantleAreaCost;
    UNavArea_HellRunMantle::StaticClass()->GetDefaultObject<UNavArea_HellRunMantle>()->DrawColor = MantleAreaColor;
    UNavArea_HellRunClimb::StaticClass()->GetDefaultObject<UNavArea_HellRunClimb>()->DefaultCost = ClimbAreaCost;
    UNavArea_HellRunClimb::StaticClass()->GetDefaultObject<UNavArea_HellRunClimb>()->DrawColor = ClimbAreaColor;
    UNavArea_HellRunDrop::StaticClass()->GetDefaultObject<UNavArea_HellRunDrop>()->DefaultCost = DropAreaCost;
    UNavArea_HellRunDrop::StaticClass()->GetDefaultObject<UNavArea_HellRunDrop>()->DrawColor = DropAreaColor;
    for (TObjectIterator<AHellRunRecastNavMesh> It; It; ++It)
    {
        if (!It->HasAnyFlags(RF_ClassDefaultObject))
        {
            It->RefreshTraversalSettings(true);
        }
    }
}
#endif
