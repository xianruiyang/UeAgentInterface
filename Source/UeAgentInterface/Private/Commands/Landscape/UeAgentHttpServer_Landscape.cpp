// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Landscape.h"

#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

static ALandscape* UeAgentFindFirstLandscape(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (ALandscape* L = *It)
		{
			return L;
		}
	}
	return nullptr;
}

bool FUeAgentHttpServer::CmdLandscapeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	if (UeAgentFindFirstLandscape(World))
	{
		OutError = TEXT("landscape_already_exists");
		return false;
	}

	int32 QuadsPerSection = 63;
	int32 SectionsPerComponent = 1;
	int32 ComponentCountX = 8;
	int32 ComponentCountY = 8;

	double NumVal = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quads_per_section"), NumVal)) { QuadsPerSection = FMath::Clamp((int32)NumVal, 7, 255); }
	if (JsonTryGetNumber(Ctx.Params, TEXT("sections_per_component"), NumVal)) { SectionsPerComponent = FMath::Clamp((int32)NumVal, 1, 2); }
	if (JsonTryGetNumber(Ctx.Params, TEXT("component_count_x"), NumVal)) { ComponentCountX = FMath::Clamp((int32)NumVal, 1, 64); }
	if (JsonTryGetNumber(Ctx.Params, TEXT("component_count_y"), NumVal)) { ComponentCountY = FMath::Clamp((int32)NumVal, 1, 64); }

	FVector Location(0, 0, 0);
	FRotator Rotation(0, 0, 0);
	FVector Scale(100, 100, 100);
	JsonTryGetVector(Ctx.Params, TEXT("location"), Location);
	JsonTryGetRotator(Ctx.Params, TEXT("rotation"), Rotation);
	JsonTryGetVector(Ctx.Params, TEXT("scale"), Scale);

	FString Label = TEXT("UA_Landscape_01");
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);

	const int32 QuadsPerComponent = SectionsPerComponent * QuadsPerSection;
	const int32 SizeX = ComponentCountX * QuadsPerComponent + 1;
	const int32 SizeY = ComponentCountY * QuadsPerComponent + 1;

	const FVector CenteringOffset = FTransform(Rotation, FVector::ZeroVector, Scale).TransformVector(
		FVector(-ComponentCountX * QuadsPerComponent / 2.0, -ComponentCountY * QuadsPerComponent / 2.0, 0.0));

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "LandscapeCreateTx", "UeAgentInterface Create Landscape"));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transactional;

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location + CenteringOffset, Rotation, SpawnParams);
	if (!Landscape)
	{
		OutError = TEXT("spawn_landscape_failed");
		return false;
	}

	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->SetActorLabel(Label);

	TArray<uint16> HeightData;
	HeightData.Init(LandscapeDataAccess::GetTexHeight(0.0f), SizeX * SizeY);

	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));

	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerInfosPerLayers;
	MaterialLayerInfosPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	const TArrayView<const FLandscapeLayer> EmptyLayers;
	Landscape->Import(
		FGuid::NewGuid(),
		0, 0, SizeX - 1, SizeY - 1,
		SectionsPerComponent,
		QuadsPerSection,
		HeightDataPerLayers,
		TEXT(""),
		MaterialLayerInfosPerLayers,
		ELandscapeImportAlphamapType::Additive,
		EmptyLayers);

	Landscape->CreateLandscapeInfo();
	Landscape->PostEditChange();
	Landscape->MarkPackageDirty();

	OutData->SetStringField(TEXT("name"), Landscape->GetName());
	OutData->SetStringField(TEXT("label"), Landscape->GetActorLabel());
	OutData->SetNumberField(TEXT("size_x"), SizeX);
	OutData->SetNumberField(TEXT("size_y"), SizeY);
	OutData->SetNumberField(TEXT("quads_per_section"), QuadsPerSection);
	OutData->SetNumberField(TEXT("sections_per_component"), SectionsPerComponent);
	OutData->SetNumberField(TEXT("component_count_x"), ComponentCountX);
	OutData->SetNumberField(TEXT("component_count_y"), ComponentCountY);
	OutData->SetObjectField(TEXT("location"), VecToJson(Landscape->GetActorLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(Landscape->GetActorRotation()));
	OutData->SetObjectField(TEXT("scale"), VecToJson(Landscape->GetActorScale3D()));
	return true;
}

bool FUeAgentHttpServer::CmdLandscapeRaiseCircle(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	ALandscape* Landscape = UeAgentFindFirstLandscape(World);
	if (!Landscape)
	{
		OutError = TEXT("no_landscape_in_level");
		return false;
	}

	FVector CenterWorld(0, 0, 0);
	double RadiusCm = 2000.0;
	double StrengthCm = 300.0;
	double Falloff = 1.0;

	JsonTryGetVector(Ctx.Params, TEXT("center"), CenterWorld);
	JsonTryGetNumber(Ctx.Params, TEXT("radius_cm"), RadiusCm);
	JsonTryGetNumber(Ctx.Params, TEXT("strength_cm"), StrengthCm);
	JsonTryGetNumber(Ctx.Params, TEXT("falloff"), Falloff);
	Falloff = FMath::Clamp(Falloff, 0.0, 1.0);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		OutError = TEXT("missing_landscape_info");
		return false;
	}

	int32 ExtMinX = 0, ExtMinY = 0, ExtMaxX = 0, ExtMaxY = 0;
	if (!Info->GetLandscapeExtent(ExtMinX, ExtMinY, ExtMaxX, ExtMaxY))
	{
		OutError = TEXT("failed_get_landscape_extent");
		return false;
	}

	const FVector Scale = Landscape->GetActorScale3D();
	const FVector CenterLocal = Landscape->GetTransform().InverseTransformPosition(CenterWorld);
	const double CenterVX = CenterLocal.X;
	const double CenterVY = CenterLocal.Y;
	const double RadiusV = FMath::Max(1.0, RadiusCm / FMath::Max(1.0, (double)Scale.X));

	const int32 X1 = FMath::Clamp((int32)FMath::FloorToInt(CenterVX - RadiusV), ExtMinX, ExtMaxX);
	const int32 Y1 = FMath::Clamp((int32)FMath::FloorToInt(CenterVY - RadiusV), ExtMinY, ExtMaxY);
	const int32 X2 = FMath::Clamp((int32)FMath::CeilToInt(CenterVX + RadiusV), ExtMinX, ExtMaxX);
	const int32 Y2 = FMath::Clamp((int32)FMath::CeilToInt(CenterVY + RadiusV), ExtMinY, ExtMaxY);

	const int32 W = X2 - X1 + 1;
	const int32 H = Y2 - Y1 + 1;
	if (W <= 0 || H <= 0)
	{
		OutError = TEXT("invalid_region");
		return false;
	}

	const bool bUseAutoTx = [&]()
	{
		FScopeLock Lock(&TransactionMutex);
		return ActiveTransactionIndex == INDEX_NONE;
	}();
	TOptional<FScopedTransaction> AutoTx;
	if (bUseAutoTx)
	{
		AutoTx.Emplace(NSLOCTEXT("UeAgentInterface", "LandscapeSculptTx", "UeAgentInterface Sculpt Landscape"));
	}

	FLandscapeEditDataInterface Edit(Info);
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(W * H);
	Edit.GetHeightDataFast(X1, Y1, X2, Y2, Heights.GetData(), W, nullptr);

	const double DeltaLocalMax = StrengthCm / FMath::Max(1.0, (double)Scale.Z);

	for (int32 yi = 0; yi < H; yi++)
	{
		const int32 Y = Y1 + yi;
		for (int32 xi = 0; xi < W; xi++)
		{
			const int32 X = X1 + xi;
			const double dx = (double)X - CenterVX;
			const double dy = (double)Y - CenterVY;
			const double d = FMath::Sqrt(dx * dx + dy * dy);
			if (d > RadiusV)
			{
				continue;
			}

			double t = 1.0 - (d / RadiusV);
			if (Falloff > 0.0)
			{
				const double s = t * t * (3.0 - 2.0 * t);
				t = FMath::Lerp(t, s, Falloff);
			}

			const int32 Idx = yi * W + xi;
			const float OldLocal = LandscapeDataAccess::GetLocalHeight(Heights[Idx]);
			const float NewLocal = OldLocal + (float)(DeltaLocalMax * t);
			Heights[Idx] = LandscapeDataAccess::GetTexHeight(NewLocal);
		}
	}

	Edit.SetHeightData(X1, Y1, X2, Y2, Heights.GetData(), W, /*InCalcNormals*/ true);
	Landscape->PostEditChange();
	Landscape->MarkPackageDirty();

	OutData->SetStringField(TEXT("name"), Landscape->GetName());
	OutData->SetStringField(TEXT("label"), Landscape->GetActorLabel());
	OutData->SetObjectField(TEXT("center_world"), VecToJson(CenterWorld));
	OutData->SetNumberField(TEXT("radius_cm"), RadiusCm);
	OutData->SetNumberField(TEXT("strength_cm"), StrengthCm);
	OutData->SetNumberField(TEXT("region_x1"), X1);
	OutData->SetNumberField(TEXT("region_y1"), Y1);
	OutData->SetNumberField(TEXT("region_x2"), X2);
	OutData->SetNumberField(TEXT("region_y2"), Y2);
	return true;
}
