#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Data/PCGPointData.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "PCGDataAsset.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Contract;

	class FActiveCancellation final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	FUeAgentCallerContext Caller()
	{
		return FUeAgentCallerContext(
			TEXT("test.agent"),
			{TEXT("pcg_ai_node_graph.read"),
				TEXT("pcg_ai_node_graph.write")},
			TEXT("correlation:w01:pcg-domain-closure"));
	}

	bool TryGetOperationError(
		const FUeAgentOperationResult& Result,
		FString& OutCode,
		FString& OutDetail)
	{
		OutCode.Reset();
		OutDetail.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (!Root->TryGetObjectField(TEXT("operation_error"), Error)
			|| !Error || !Error->IsValid())
		{
			return false;
		}
		(*Error)->TryGetStringField(TEXT("code"), OutCode);
		(*Error)->TryGetStringField(TEXT("detail"), OutDetail);
		return !OutCode.IsEmpty();
	}

	bool TestSucceeded(
		FAutomationTestBase& Test,
		const FString& Label,
		const FUeAgentOperationResult& Result)
	{
		if (!Test.TestTrue(*(Label + TEXT(" returns a valid result")),
			Result.IsValid()))
		{
			return false;
		}
		FString Code;
		FString Detail;
		if (TryGetOperationError(Result, Code, Detail))
		{
			Test.AddError(Label + TEXT(": ") + Code + TEXT(":") + Detail);
			return false;
		}
		return true;
	}

	FUeAgentOperationResult Submit(
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const FString& RequestSuffix,
		const FString& Payload)
	{
		const FPcgAnimationGraphRouteProfile* const Profile =
			FindPcgAnimationGraphRouteProfile(CapabilityId);
		check(Profile);
		return Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w01:") + RequestSuffix,
				TEXT("idempotency:w01:") + RequestSuffix,
				FUeAgentContractValue(Payload)),
			Caller(), MakeShared<FActiveCancellation>());
	}

	class FScopedPcgDataAsset final
	{
	public:
		explicit FScopedPcgDataAsset(const FString& Stem)
			: AssetName(Stem + TEXT("_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, PackageName(
				TEXT("/Game/UAI/Tests/GraphCompound/") + AssetName)
		{
		}

		~FScopedPcgDataAsset()
		{
			Unload();
			IFileManager::Get().Delete(*Filename(), false, true);
		}

		UPCGDataAsset* CreateAndSave(
			FAutomationTestBase& Test,
			const FString& SourceRef,
			const int32 PointCount) const
		{
			UPackage* const Package = CreatePackage(*PackageName);
			UPCGDataAsset* const Asset = Package
				? NewObject<UPCGDataAsset>(
					Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional)
				: nullptr;
			if (!Test.TestNotNull(TEXT("PCG data asset fixture exists"), Asset))
			{
				return nullptr;
			}
			UPCGPointData* const PointData = NewObject<UPCGPointData>(
				Asset, TEXT("W01InitialPoints"), RF_Transactional);
			if (!Test.TestNotNull(TEXT("initial PCG point data exists"), PointData))
			{
				return nullptr;
			}
			TArray<FPCGPoint> Points;
			for (int32 Index = 0; Index < PointCount; ++Index)
			{
				Points.Emplace(
					FTransform(FVector(Index * 10.0, 0.0, 0.0)),
					1.0f, Index + 1);
			}
			PointData->SetPoints(Points);
			FPCGTaggedData Tagged;
			Tagged.Data = PointData;
			Tagged.Pin = TEXT("Out");
			Tagged.Tags = {TEXT("w01")};
			Asset->Data.TaggedData = {MoveTemp(Tagged)};
			Asset->ObjectPath = FSoftObjectPath(SourceRef);
			Asset->SetFlags(RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!Test.TestTrue(TEXT("PCG data asset fixture is durable"),
				UPackage::SavePackage(Package, Asset, *Filename(), Args)))
			{
				return nullptr;
			}
			Package->SetDirtyFlag(false);
			return Asset;
		}

		bool Unload() const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded =
				UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && !FindPackage(nullptr, *PackageName);
		}

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}

		FString AssetName;
		FString PackageName;
	};

	class FScopedSourceWorld final
	{
	public:
		explicit FScopedSourceWorld(FAutomationTestBase& Test)
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Package = CreatePackage(*(
				TEXT("/Game/UAI/Tests/GraphCompound/W01SourceWorld_")
				+ Suffix));
			World = Package ? UWorld::CreateWorld(
				EWorldType::EditorPreview, false,
				FName(*(TEXT("W01SourceWorld_") + Suffix)), Package) : nullptr;
			if (!Test.TestNotNull(TEXT("owned source world exists"), World))
			{
				return;
			}
			if (GEngine && !GEngine->GetWorldContextFromWorld(World))
			{
				FWorldContext& Context =
					GEngine->CreateNewWorldContext(EWorldType::EditorPreview);
				Context.SetCurrentWorld(World);
				bOwnsWorldContext = true;
			}
			FActorSpawnParameters Params;
			Params.Name = FName(*(TEXT("W01SourceActor_") + Suffix));
			Actor = World->SpawnActor<AActor>(
				FVector(125.0, 250.0, 375.0),
				FRotator::ZeroRotator, Params);
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			Test.TestNotNull(TEXT("source world actor exists"), Actor);
		}

		~FScopedSourceWorld()
		{
			if (World)
			{
				if (bOwnsWorldContext && GEngine)
				{
					GEngine->DestroyWorldContext(World);
				}
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			CollectGarbage(RF_NoFlags, true);
		}

		bool IsValid() const
		{
			return Package && World && Actor && GEngine
				&& GEngine->GetWorldContextFromWorld(World);
		}

		FString ObjectPath() const
		{
			return World ? World->GetPathName() : FString();
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		AActor* Actor = nullptr;
		bool bOwnsWorldContext = false;
	};

	bool TestPointDataReadback(
		FAutomationTestBase& Test,
		const FScopedPcgDataAsset& Fixture,
		const FString& ExpectedSource,
		const int32 ExpectedPointCount)
	{
		if (!Test.TestTrue(TEXT("PCG data asset unloads for independent readback"),
			Fixture.Unload()))
		{
			return false;
		}
		UPCGDataAsset* const Reloaded =
			LoadObject<UPCGDataAsset>(nullptr, *Fixture.ObjectPath());
		if (!Test.TestNotNull(TEXT("PCG data asset reloads from disk"), Reloaded))
		{
			return false;
		}
		if (!Test.TestEqual(TEXT("readback has one tagged data payload"),
			Reloaded->Data.TaggedData.Num(), 1))
		{
			return false;
		}
		const UPCGPointData* const PointData =
			Cast<UPCGPointData>(Reloaded->Data.TaggedData[0].Data);
		return Test.TestNotNull(TEXT("readback preserves exact point data type"),
			PointData)
			&& Test.TestEqual(TEXT("readback preserves point count"),
				PointData->GetPoints().Num(), ExpectedPointCount)
			&& Test.TestEqual(TEXT("readback preserves source provenance"),
				Reloaded->ObjectPath.ToString(), ExpectedSource)
			&& Test.TestFalse(TEXT("readback package is clean"),
				Reloaded->GetOutermost()->IsDirty());
	}

	bool TestLevelExportReadback(
		FAutomationTestBase& Test,
		const FScopedPcgDataAsset& Fixture,
		const FString& ExpectedSource)
	{
		if (!Test.TestTrue(TEXT("level export unloads for independent readback"),
			Fixture.Unload()))
		{
			return false;
		}
		UPCGDataAsset* const Reloaded =
			LoadObject<UPCGDataAsset>(nullptr, *Fixture.ObjectPath());
		if (!Test.TestNotNull(TEXT("level export reloads from disk"), Reloaded))
		{
			return false;
		}
		bool bAllPointData = Reloaded->Data.TaggedData.Num() >= 2;
		for (const FPCGTaggedData& Tagged : Reloaded->Data.TaggedData)
		{
			bAllPointData = bAllPointData
				&& Tagged.Data
				&& Tagged.Data->IsA<UPCGPointData>();
		}
		return Test.TestTrue(TEXT("level export contains native point datasets"),
			bAllPointData)
			&& Test.TestEqual(TEXT("level export preserves exact source world"),
				Reloaded->ObjectPath.ToString(), ExpectedSource)
			&& Test.TestFalse(TEXT("level export package is clean"),
				Reloaded->GetOutermost()->IsDirty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPcgDomainClosureTest,
	"UeAgentInterface.Composition.GraphCompound.PcgAi.DomainClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPcgDomainClosureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(TEXT("PCG/AI descriptor registry builds"),
		TryCreatePcgAiGraphDescriptorRegistry(Registry, RegistryError))
		|| !TestNotNull(TEXT("PCG/AI descriptor registry exists"), Registry.Get()))
	{
		AddError(RegistryError);
		return false;
	}

	FGraphCompoundRuntimeConfig RuntimeConfig;

	FGraphCompoundPcgAiProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TestTrue(TEXT("production PCG/AI runtime builds"),
		TryCreatePcgAiGraphProductionRuntime(
			RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
		|| !TestNotNull(TEXT("production PCG/AI runtime exists"), Runtime.Get()))
	{
		AddError(RuntimeError.Code + TEXT(":") + RuntimeError.Detail);
		return false;
	}

	FScopedSourceWorld SourceWorld(*this);
	FScopedPcgDataAsset ApplyAsset(TEXT("PCGD_W01_Apply"));
	FScopedPcgDataAsset FromLevelAsset(TEXT("PCGD_W01_FromLevel"));
	FScopedPcgDataAsset InvalidSourceAsset(TEXT("PCGD_W01_InvalidSource"));
	if (!SourceWorld.IsValid()
		|| !ApplyAsset.CreateAndSave(
			*this, TEXT("/Game/UAI/Tests/Baseline.Baseline"), 1))
	{
		return false;
	}

	const FString ApplyPayload = FString::Printf(
		TEXT("{\"data_type_ref\":\"/Script/PCG.PCGPointData\",\"source_ref\":\"%s\",\"target_ref\":\"%s\",\"value\":{\"point_count\":3}}"),
		*SourceWorld.ObjectPath(), *ApplyAsset.ObjectPath());
	const FString InvalidSchemaPayload = ApplyPayload.LeftChop(1)
		+ TEXT(",\"unexpected\":true}");
	FString Code;
	FString Detail;
	const FUeAgentOperationResult InvalidSchema = Submit(
		*Runtime, TEXT("pcg.data_asset.apply"),
		TEXT("apply-schema-invalid"), InvalidSchemaPayload);
	TestTrue(TEXT("data apply unknown field returns operation_error"),
		TryGetOperationError(InvalidSchema, Code, Detail));
	TestEqual(TEXT("data apply unknown field fails at public schema"),
		Code, FString(TEXT("contract_input_schema_mismatch")));
	const FString LegacyRevisionPayload = ApplyPayload.LeftChop(1)
		+ TEXT(",\"content_revision\":\"legacy\"}");
	const FUeAgentOperationResult LegacyRevision = Submit(
		*Runtime, TEXT("pcg.data_asset.apply"),
		TEXT("apply-legacy-revision"), LegacyRevisionPayload);
	TestTrue(TEXT("data apply legacy revision returns operation_error"),
		TryGetOperationError(LegacyRevision, Code, Detail));
	TestEqual(TEXT("data apply legacy revision fails at public schema"),
		Code, FString(TEXT("contract_input_schema_mismatch")));

	const FUeAgentOperationResult ApplyResult = Submit(
		*Runtime, TEXT("pcg.data_asset.apply"),
		TEXT("apply-valid"), ApplyPayload);
	if (TestSucceeded(*this, TEXT("PCG data asset apply"), ApplyResult))
	{
		TestPointDataReadback(
			*this, ApplyAsset, SourceWorld.ObjectPath(), 3);
		const FString UnsupportedPayload = FString::Printf(
			TEXT("{\"data_type_ref\":\"/Script/PCG.PCGParamData\",\"source_ref\":\"%s\",\"target_ref\":\"%s\",\"value\":{\"point_count\":9}}"),
			*SourceWorld.ObjectPath(), *ApplyAsset.ObjectPath());
		const FUeAgentOperationResult Unsupported = Submit(
			*Runtime, TEXT("pcg.data_asset.apply"),
			TEXT("apply-unsupported-type"), UnsupportedPayload);
		TestTrue(TEXT("unsupported data type returns operation_error"),
			TryGetOperationError(Unsupported, Code, Detail));
		TestEqual(TEXT("unsupported data type has an explicit error"),
			Code, FString(TEXT("graph_compound_pcg_data_type_unsupported")));
		TestPointDataReadback(
			*this, ApplyAsset, SourceWorld.ObjectPath(), 3);
	}

	const FString MissingSourcePayload = FString::Printf(
		TEXT("{\"data_type_ref\":\"/Script/PCG.PCGPointData\",\"source_level_ref\":\"/Game/UAI/Missing.Missing\",\"target_ref\":\"%s\"}"),
		*InvalidSourceAsset.PackageName);
	const FUeAgentOperationResult MissingSource = Submit(
		*Runtime, TEXT("pcg.data_asset.create_from_level"),
		TEXT("from-level-missing-source"), MissingSourcePayload);
	FString MissingSourceCode;
	FString MissingSourceDetail;
	TestTrue(TEXT("missing source level returns operation_error"),
		TryGetOperationError(
			MissingSource, MissingSourceCode, MissingSourceDetail));
	TestEqual(TEXT("missing source level has an explicit error"),
		MissingSourceCode,
		FString(TEXT("graph_compound_pcg_source_level_invalid")));
	TestFalse(TEXT("missing source leaves no target package"),
		FPaths::FileExists(InvalidSourceAsset.Filename()));
	TestNull(TEXT("missing source leaves no loaded target package"),
		FindPackage(nullptr, *InvalidSourceAsset.PackageName));

	const FString FromLevelPayload = FString::Printf(
		TEXT("{\"data_type_ref\":\"/Script/PCG.PCGPointData\",\"source_level_ref\":\"%s\",\"target_ref\":\"%s\"}"),
		*SourceWorld.ObjectPath(), *FromLevelAsset.PackageName);
	const FUeAgentOperationResult FromLevelResult = Submit(
		*Runtime, TEXT("pcg.data_asset.create_from_level"),
		TEXT("from-level-valid"), FromLevelPayload);
	if (TestSucceeded(
			*this, TEXT("PCG data asset create from level"), FromLevelResult))
	{
		TestLevelExportReadback(
			*this, FromLevelAsset, SourceWorld.ObjectPath());
	}
	return !HasAnyErrors();
}
