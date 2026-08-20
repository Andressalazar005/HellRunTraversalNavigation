#include "HellRunVoxelPathDebugPawn.h"

#include "Components/BillboardComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/TextRenderComponent.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "HellRunNavigationDebugLog.h"
#include "HellRunTraversalComponent.h"
#include "HellRunVoxelNavigation.h"
#include "HellRunTraversalNavigationSettings.h"

AHellRunVoxelPathDebugPawn::AHellRunVoxelPathDebugPawn(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    SetActorTickEnabled(true);
    const UHellRunTraversalNavigationSettings* NavigationSettings =
        GetDefault<UHellRunTraversalNavigationSettings>();
    GetCapsuleComponent()->InitCapsuleSize(
        NavigationSettings->VoxelBakeAgentRadius,
        NavigationSettings->VoxelBakeAgentHalfHeight);

    TraversalCapabilities = CreateDefaultSubobject<UHellRunTraversalComponent>(
        TEXT("TraversalCapabilities"));
    TraversalCapabilities->bEditableWhenInherited = true;
    TraversalCapabilities->bCanWallClimbNavigation = true;
    EditorIcon = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("PathDebuggerIcon"));
    if (EditorIcon)
    {
        EditorIcon->SetupAttachment(GetRootComponent());
        EditorIcon->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
        EditorIcon->SetHiddenInGame(true);
        EditorIcon->bIsScreenSizeScaled = true;
    }
    PathLineBatcher = CreateDefaultSubobject<ULineBatchComponent>(TEXT("PersistentPathDrawing"));
    PathLineBatcher->SetupAttachment(GetRootComponent());
    PathLineBatcher->SetHiddenInGame(true);

    RoleLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("RoleLabel"));
    RoleLabel->SetupAttachment(GetRootComponent());
    RoleLabel->SetRelativeLocation(FVector(0.0f, 0.0f, 135.0f));
    // TextRender faces along local +X. Pitching +90 points its front face
    // upward; -90 exposes the back face and makes the text appear mirrored.
    RoleLabel->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
    RoleLabel->SetHorizontalAlignment(EHTA_Center);
    RoleLabel->SetWorldSize(24.0f);
    RoleLabel->SetHiddenInGame(true);

    ResultLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ResultLabel"));
    ResultLabel->SetupAttachment(GetRootComponent());
    ResultLabel->SetRelativeLocation(FVector(0.0f, 0.0f, 165.0f));
    ResultLabel->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
    ResultLabel->SetHorizontalAlignment(EHTA_Center);
    ResultLabel->SetWorldSize(20.0f);
    ResultLabel->SetHiddenInGame(true);
}

void AHellRunVoxelPathDebugPawn::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RebuildPersistentLabels();
}

void AHellRunVoxelPathDebugPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!GetWorld())
    {
        return;
    }
    if (GetWorld()->IsGameWorld())
    {
        return;
    }
    if (EndpointRole != EHellRunPathDebugPawnRole::EnemySource)
    {
        RebuildPersistentLabels();
        return;
    }
    if (!bLiveUpdateInEditor)
    {
        return;
    }

    TimeUntilNextUpdate -= DeltaSeconds;
    AActor* Target = ResolveTarget();
    if (!Target)
    {
        CachedPath.Reset();
        CachedPathVariations.Reset();
        UniquePathVariationCount = 0;
        Result = TEXT("NO PLAYER TARGET");
        RebuildPersistentPathDrawing();
        DrawCurrentResult(FMath::Max(UpdateInterval * 1.5f, 0.1f));
        return;
    }

    const FVector QueryStart = ResolveSupportedQueryLocation(this);
    const FVector Goal = ResolveSupportedQueryLocation(Target);
    const bool bEndpointsChanged = !QueryStart.Equals(LastQueryStart, 1.0f)
        || !Goal.Equals(LastQueryGoal, 1.0f);
    const bool bNeedsRefresh = bDrawPathVariations
        ? (bEndpointsChanged || CachedPathVariations.IsEmpty())
        : true;
    if (TimeUntilNextUpdate <= 0.0f)
    {
        if (bNeedsRefresh)
        {
            RefreshDebugPath();
        }
        else
        {
            // PIE teardown and editor render-state recreation can clear a
            // line batcher's primitives while the cached route remains valid.
            // Re-submit the inexpensive cached drawing at the normal refresh
            // cadence without rerunning pathfinding.
            RebuildPersistentPathDrawing();
        }
        TimeUntilNextUpdate = bDrawPathVariations
            ? FMath::Max(0.25f, PathVariationUpdateInterval)
            : FMath::Max(0.05f, UpdateInterval);
    }
    DrawCurrentResult(FMath::Max(UpdateInterval * 1.5f, 0.1f));
}

AActor* AHellRunVoxelPathDebugPawn::ResolveTarget() const
{
    if (IsValid(ExplicitTarget)) return ExplicitTarget;
    UWorld* World = GetWorld();
    if (!World) return nullptr;

    AActor* BestTarget = nullptr;
    float BestDistanceSq = BIG_NUMBER;
    for (TActorIterator<AHellRunVoxelPathDebugPawn> It(World); It; ++It)
    {
        if (*It == this || It->EndpointRole != EHellRunPathDebugPawnRole::PlayerTarget) continue;
        const float DistanceSq = FVector::DistSquared(GetActorLocation(), It->GetActorLocation());
        if (DistanceSq < BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestTarget = *It;
        }
    }
    return BestTarget;
}

FVector AHellRunVoxelPathDebugPawn::ResolveSupportedQueryLocation(
    const AActor* Endpoint) const
{
    if (!IsValid(Endpoint))
    {
        return FVector(BIG_NUMBER);
    }

    const AHellRunVoxelPathDebugPawn* DebugEndpoint =
        Cast<AHellRunVoxelPathDebugPawn>(Endpoint);
    UWorld* World = GetWorld();
    if (!DebugEndpoint || !World)
    {
        return Endpoint->GetActorLocation();
    }

    const UCapsuleComponent* Capsule =
        DebugEndpoint->GetCapsuleComponent();
    const UHellRunTraversalNavigationSettings* Settings =
        GetDefault<UHellRunTraversalNavigationSettings>();
    const float HalfHeight = Capsule
        ? Capsule->GetScaledCapsuleHalfHeight()
        : Settings->VoxelBakeAgentHalfHeight;
    const float ProbeAbove = FMath::Max(
        Settings->GroundStepHeight,
        HalfHeight * 0.5f);
    FCollisionObjectQueryParams FloorObjects;
    FloorObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    FloorObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams FloorParams(
        SCENE_QUERY_STAT(HellRunDebugPawnSupportedEndpoint),
        false,
        DebugEndpoint);
    FHitResult FloorHit;
    const FVector EndpointLocation = Endpoint->GetActorLocation();
    if (World->LineTraceSingleByObjectType(
            FloorHit,
            EndpointLocation + FVector::UpVector * ProbeAbove,
            EndpointLocation
                - FVector::UpVector * (HalfHeight + ProbeAbove),
            FloorObjects,
            FloorParams)
        && FloorHit.ImpactNormal.Z >= 0.55f)
    {
        return FloorHit.ImpactPoint
            + FVector::UpVector
                * (HalfHeight + Settings->SpawnNavSurfaceClearance);
    }
    return EndpointLocation;
}

void AHellRunVoxelPathDebugPawn::RefreshDebugPath()
{
    AActor* Target = ResolveTarget();
    LastQueryStart = ResolveSupportedQueryLocation(this);
    LastQueryGoal = Target
        ? ResolveSupportedQueryLocation(Target)
        : FVector(BIG_NUMBER);
    CachedPath.Reset();
    CachedPathVariations.Reset();
    UniquePathVariationCount = 0;
    PathPointCount = 0;
    EstimatedTravelSeconds = 0.0f;
    PathLength = 0.0f;
    bAuthoritativeTypedGraphFound = FHellRunVoxelNavigation::HasAuthoritativeTypedEdgeGraph(GetWorld());
    auto LogQueryResult = [this, Target]()
    {
        FString SegmentSummary;
        FString PointSummary;
        if (CachedPath.IsValid())
        {
            const TArray<FNavPathPoint>& Points = CachedPath->GetPathPoints();
            for (int32 Index = 0; Index < Points.Num(); ++Index)
            {
                if (!PointSummary.IsEmpty()) PointSummary += TEXT("|");
                PointSummary += FString::Printf(
                    TEXT("%.0f,%.0f,%.0f"),
                    Points[Index].Location.X,
                    Points[Index].Location.Y,
                    Points[Index].Location.Z);
            }
            for (int32 Index = 1; Index < Points.Num(); ++Index)
            {
                if (!SegmentSummary.IsEmpty()) SegmentSummary += TEXT(",");
                const uint8 Mode = HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                    ? static_cast<uint8>(HellRunVoxelPath::GetMode(Points[Index].Flags))
                    : static_cast<uint8>(EHellRunVoxelSegment::Walk);
                SegmentSummary += LabelForMode(Mode);
            }
        }
        if (SegmentSummary.IsEmpty()) SegmentSummary = TEXT("none");
        if (PointSummary.IsEmpty()) PointSummary = TEXT("none");
        const FString Details = FString::Printf(
            TEXT("source=%s target=%s targetActor=%s graph=%s capabilities=[walk:%d jump:%d vault:%d mantle:%d drop:%d wallClimb:%d fly:%d] result=\"%s\" diagnostic=\"%s\" points=%d lengthCm=%.1f seconds=%.2f segments=[%s] path=[%s]"),
            *LastQueryStart.ToCompactString(),
            *LastQueryGoal.ToCompactString(),
            *GetNameSafe(Target),
            bAuthoritativeTypedGraphFound ? TEXT("baked-typed-edges") : TEXT("query-time-typed-adjacency"),
            TraversalCapabilities && TraversalCapabilities->CanWalkNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanJumpNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanVaultNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanMantleNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanDropNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanWallClimbNavigation(),
            TraversalCapabilities && TraversalCapabilities->CanFlyNavigation(),
            *Result, *QueryDiagnostic, PathPointCount, PathLength,
            EstimatedTravelSeconds, *SegmentSummary, *PointSummary);
        const FString Signature = FString::Printf(TEXT("%s|%s|%s"),
            *LastQueryStart.ToCompactString(), *LastQueryGoal.ToCompactString(), *Details);
        if (Signature == LastLoggedQuerySignature) return;
        LastLoggedQuerySignature = Signature;
        UE_LOG(LogTemp, Display, TEXT("VOXEL_DEBUG_QUERY | actor=%s | %s"), *GetName(), *Details);
        FHellRunNavigationDebugLog::Write(this, TEXT("VOXEL_DEBUG_QUERY"), Details);
    };

    if (!Target)
    {
        Result = TEXT("NO PLAYER TARGET");
        RebuildPersistentPathDrawing();
        LogQueryResult();
        return;
    }

    const bool bOriginalVariationEnabled =
        TraversalCapabilities->bUseVoxelPathVariation;
    const float OriginalVariationStrength =
        TraversalCapabilities->VoxelPathVariationStrength;
    const int32 OriginalVariationSeed =
        TraversalCapabilities->VoxelPathVariationSeed;
    TraversalCapabilities->bUseVoxelPathVariation = bDrawPathVariations;
    TraversalCapabilities->VoxelPathVariationStrength =
        PathVariationStrength;

    TSet<FString> UniqueSignatures;
    const int32 DesiredPathCount = bDrawPathVariations
        ? FMath::Clamp(PathVariationCount, 1, 12) : 1;
    const int32 MaximumAttempts = bDrawPathVariations
        ? DesiredPathCount * 3 : 1;
    for (int32 Attempt = 0;
        Attempt < MaximumAttempts
            && CachedPathVariations.Num() < DesiredPathCount;
        ++Attempt)
    {
        TraversalCapabilities->VoxelPathVariationSeed =
            PathVariationSeed + Attempt * 104729;
        FNavPathSharedPtr Candidate =
            FHellRunVoxelNavigation::FindPathFrom(
                *this, LastQueryStart, LastQueryGoal);
        const FString CandidateDiagnostic =
            FHellRunVoxelNavigation::GetLastQueryDiagnostic();
        if (Attempt == 0)
        {
            QueryDiagnostic = CandidateDiagnostic;
        }
        if (!Candidate.IsValid() || !Candidate->IsValid()
            || Candidate->GetPathPoints().Num() < 2)
        {
            continue;
        }
        TArray<FString> SignatureParts;
        for (const FNavPathPoint& Point : Candidate->GetPathPoints())
        {
            const int32 Mode = HellRunVoxelPath::IsVoxelFlags(Point.Flags)
                ? static_cast<int32>(
                    HellRunVoxelPath::GetMode(Point.Flags))
                : static_cast<int32>(EHellRunVoxelSegment::Walk);
            SignatureParts.Add(FString::Printf(
                TEXT("%.0f,%.0f,%.0f:%d"),
                Point.Location.X, Point.Location.Y, Point.Location.Z,
                Mode));
        }
        const FString Signature =
            FString::Join(SignatureParts, TEXT("|"));
        if (!UniqueSignatures.Contains(Signature))
        {
            UniqueSignatures.Add(Signature);
            CachedPathVariations.Add(Candidate);
        }
    }
    TraversalCapabilities->bUseVoxelPathVariation =
        bOriginalVariationEnabled;
    TraversalCapabilities->VoxelPathVariationStrength =
        OriginalVariationStrength;
    TraversalCapabilities->VoxelPathVariationSeed =
        OriginalVariationSeed;
    UniquePathVariationCount = CachedPathVariations.Num();
    CachedPath = CachedPathVariations.IsEmpty()
        ? nullptr : CachedPathVariations[0];
    if (!CachedPath.IsValid() || !CachedPath->IsValid()
        || CachedPath->GetPathPoints().Num() < 2)
    {
        CachedPath.Reset();
        Result = QueryDiagnostic.IsEmpty()
            ? TEXT("NO COMPLETE ROUTE")
            : QueryDiagnostic;
        RebuildPersistentPathDrawing();
        LogQueryResult();
        return;
    }

    const TArray<FNavPathPoint>& Points = CachedPath->GetPathPoints();
    PathPointCount = Points.Num();
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        PathLength += FVector::Distance(Points[Index - 1].Location, Points[Index].Location);
    }
    EstimatedTravelSeconds = FHellRunVoxelNavigation::EstimateTraversalSeconds(*this, CachedPath);
    float TotalPathCost = 0.0f;
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        if (HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags))
        {
            TotalPathCost += HellRunVoxelPath::GetSegmentCost(Points[Index].Flags);
        }
    }
    Result = FString::Printf(TEXT("COMPLETE ROUTE: %d points | %.1fm | %.2fs | cost %.2f | variants %d/%d"),
        PathPointCount, PathLength / 100.0f, EstimatedTravelSeconds,
        TotalPathCost, UniquePathVariationCount, DesiredPathCount);
    RebuildPersistentPathDrawing();
    LogQueryResult();
}

void AHellRunVoxelPathDebugPawn::ClearDebugPath()
{
    CachedPath.Reset();
    CachedPathVariations.Reset();
    UniquePathVariationCount = 0;
    PathPointCount = 0;
    EstimatedTravelSeconds = 0.0f;
    PathLength = 0.0f;
    Result = TEXT("CLEARED");
    RebuildPersistentPathDrawing();
}

void AHellRunVoxelPathDebugPawn::RebuildPersistentPathDrawing()
{
    if (!PathLineBatcher) return;
    PathLineBatcher->Flush();

    AActor* Target = ResolveTarget();
    if (!CachedPath.IsValid())
    {
        if (Target)
        {
            PathLineBatcher->DrawLine(GetActorLocation(), Target->GetActorLocation(),
                FLinearColor::Red, 10, 2.0f, -1.0f);
        }
        RebuildPersistentLabels();
        return;
    }

    // Draw alternatives first in route-specific colors so overlapping
    // corridors remain legible, then draw the primary route with its
    // traversal-mode colors and labels.
    for (int32 VariationIndex = CachedPathVariations.Num() - 1;
        VariationIndex >= 1;
        --VariationIndex)
    {
        const FNavPathSharedPtr& Variation =
            CachedPathVariations[VariationIndex];
        if (!Variation.IsValid()) continue;
        const FLinearColor VariationColor =
            FLinearColor::MakeFromHSV8(
                static_cast<uint8>(
                    (VariationIndex * 47 + 18) % 255),
                190,
                255);
        const TArray<FNavPathPoint>& VariationPoints =
            Variation->GetPathPoints();
        const FVector HeightOffset(
            0.0f, 0.0f,
            static_cast<float>(VariationIndex) * 3.0f);
        for (int32 Index = 0;
            Index < VariationPoints.Num();
            ++Index)
        {
            const FVector PointLocation =
                VariationPoints[Index].Location + HeightOffset;
            PathLineBatcher->DrawPoint(
                PointLocation,
                VariationColor,
                PointRadius * 1.35f,
                9,
                -1.0f);
            if (Index > 0)
            {
                PathLineBatcher->DrawLine(
                    VariationPoints[Index - 1].Location + HeightOffset,
                    PointLocation,
                    VariationColor,
                    9,
                    FMath::Max(1.0f, PathThickness * 0.65f),
                    -1.0f);
            }
        }
    }

    const TArray<FNavPathPoint>& Points = CachedPath->GetPathPoints();
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        const uint8 Mode = HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
            ? static_cast<uint8>(HellRunVoxelPath::GetMode(Points[Index].Flags))
            : static_cast<uint8>(EHellRunVoxelSegment::Walk);
        const FLinearColor Color(ColorForMode(Mode));
        PathLineBatcher->DrawPoint(Points[Index].Location, Color,
            PointRadius * 2.0f, 10, -1.0f);
        if (Index > 0)
        {
            PathLineBatcher->DrawLine(Points[Index - 1].Location, Points[Index].Location,
                Color, 10, PathThickness, -1.0f);
        }
    }
    RebuildPersistentLabels();
}

void AHellRunVoxelPathDebugPawn::RebuildPersistentLabels()
{
    for (UTextRenderComponent* Label : SegmentLabels)
    {
        if (IsValid(Label))
        {
            Label->SetVisibility(false);
        }
    }

    const FColor EndpointColor = EndpointRole == EHellRunPathDebugPawnRole::EnemySource
        ? FColor::Red : FColor::Green;
    if (RoleLabel)
    {
        RoleLabel->SetText(FText::FromString(
            EndpointRole == EHellRunPathDebugPawnRole::EnemySource
                ? TEXT("ENEMY SOURCE") : TEXT("PLAYER TARGET")));
        RoleLabel->SetTextRenderColor(EndpointColor);
    }
    if (ResultLabel)
    {
        ResultLabel->SetVisibility(EndpointRole == EHellRunPathDebugPawnRole::EnemySource);
        ResultLabel->SetText(FText::FromString(Result));
        ResultLabel->SetTextRenderColor(CachedPath.IsValid() ? FColor::Green : FColor::Red);
    }

    if (!bDrawPointLabels || !CachedPath.IsValid() || !GetWorld()) return;
    const TArray<FNavPathPoint>& Points = CachedPath->GetPathPoints();
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        const uint8 Mode = HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
            ? static_cast<uint8>(HellRunVoxelPath::GetMode(Points[Index].Flags))
            : static_cast<uint8>(EHellRunVoxelSegment::Walk);
        const int32 LabelIndex = Index - 1;
        UTextRenderComponent* Label = SegmentLabels.IsValidIndex(LabelIndex)
            ? SegmentLabels[LabelIndex] : nullptr;
        if (!IsValid(Label))
        {
            Label = NewObject<UTextRenderComponent>(
                this, *FString::Printf(TEXT("PathSegmentLabel_%d"), LabelIndex), RF_Transient);
            AddInstanceComponent(Label);
            Label->SetupAttachment(GetRootComponent());
            Label->RegisterComponent();
            SegmentLabels.SetNum(LabelIndex + 1);
            SegmentLabels[LabelIndex] = Label;
        }
        Label->SetVisibility(true);
        Label->SetText(FText::FromString(FString::Printf(TEXT("%d %s | cost %.2f"),
            Index, LabelForMode(Mode),
            HellRunVoxelPath::IsVoxelFlags(Points[Index].Flags)
                ? HellRunVoxelPath::GetSegmentCost(Points[Index].Flags) : 0.0f)));
        Label->SetTextRenderColor(ColorForMode(Mode));
        Label->SetHorizontalAlignment(EHTA_Center);
        Label->SetWorldSize(20.0f);
        Label->SetHiddenInGame(true);
        const FVector Segment = Points[Index].Location - Points[Index - 1].Location;
        FVector LabelOffset = FVector::CrossProduct(
            FVector::UpVector, Segment.GetSafeNormal2D()) * 34.0f;
        if (LabelOffset.IsNearlyZero())
        {
            const float Angle = static_cast<float>(Index) * 2.39996323f;
            LabelOffset = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f) * 48.0f;
        }
        Label->SetWorldLocation(
            FMath::Lerp(Points[Index - 1].Location, Points[Index].Location, 0.5f)
            + LabelOffset + FVector(0.0f, 0.0f, 24.0f));
        Label->SetWorldRotation(FRotator(90.0f, 0.0f, 0.0f));
    }
}

FColor AHellRunVoxelPathDebugPawn::ColorForMode(uint8 Mode)
{
    switch (static_cast<EHellRunVoxelSegment>(Mode))
    {
    case EHellRunVoxelSegment::Walk: return FColor::Cyan;
    case EHellRunVoxelSegment::Mantle: return FColor(220, 0, 255);
    case EHellRunVoxelSegment::Vault: return FColor::Green;
    case EHellRunVoxelSegment::Jump: return FColor(40, 140, 255);
    case EHellRunVoxelSegment::Drop: return FColor::Orange;
    case EHellRunVoxelSegment::Climb: return FColor(145, 70, 255);
    case EHellRunVoxelSegment::Fly: return FColor::White;
    default: return FColor::Silver;
    }
}

const TCHAR* AHellRunVoxelPathDebugPawn::LabelForMode(uint8 Mode)
{
    switch (static_cast<EHellRunVoxelSegment>(Mode))
    {
    case EHellRunVoxelSegment::Walk: return TEXT("WALK");
    case EHellRunVoxelSegment::Mantle: return TEXT("MANTLE");
    case EHellRunVoxelSegment::Vault: return TEXT("VAULT");
    case EHellRunVoxelSegment::Jump: return TEXT("JUMP");
    case EHellRunVoxelSegment::Drop: return TEXT("DROP");
    case EHellRunVoxelSegment::Climb: return TEXT("CLIMB");
    case EHellRunVoxelSegment::Fly: return TEXT("FLY");
    default: return TEXT("UNKNOWN");
    }
}

void AHellRunVoxelPathDebugPawn::DrawCurrentResult(float Lifetime) const
{
    UWorld* World = GetWorld();
    if (!World || World->IsGameWorld()) return;

    const FColor EndpointColor = EndpointRole == EHellRunPathDebugPawnRole::EnemySource
        ? FColor::Red : FColor::Green;
    DrawDebugCapsule(World, GetActorLocation(), GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
        GetCapsuleComponent()->GetScaledCapsuleRadius(), FQuat::Identity,
        EndpointColor, false, Lifetime, 10, 2.0f);
    DrawDebugString(World, GetActorLocation() + FVector(0.0f, 0.0f, 125.0f),
        EndpointRole == EHellRunPathDebugPawnRole::EnemySource ? TEXT("ENEMY SOURCE") : TEXT("PLAYER TARGET"),
        nullptr, EndpointColor, Lifetime, false, 1.1f);

    if (EndpointRole != EHellRunPathDebugPawnRole::EnemySource) return;
    const FColor ResultColor = CachedPath.IsValid() ? FColor::Green : FColor::Red;
    DrawDebugString(World, GetActorLocation() + FVector(0.0f, 0.0f, 155.0f),
        Result, nullptr, ResultColor, Lifetime, false, 1.0f);

}
