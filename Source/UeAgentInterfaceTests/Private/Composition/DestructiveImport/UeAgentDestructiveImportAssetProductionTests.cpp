#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/SkinWeightProfile.h"
#include "Animation/MorphTarget.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::Tests::PublicCommand;

	FString EscapeJson(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return Value;
	}

	FString TwoParams(
		const TCHAR* AName,
		const FString& AValue,
		const TCHAR* BName,
		const FString& BValue)
	{
		return FString::Printf(
			TEXT("{\"%s\":\"%s\",\"%s\":\"%s\"}"),
			AName, *EscapeJson(AValue), BName, *EscapeJson(BValue));
	}

	bool IsPlanAuthorizedCommand(const FString& CommandId)
	{
		return CommandId == TEXT("asset.move")
			|| CommandId == TEXT("asset.fixup_redirectors")
			|| CommandId == TEXT("skeletal_mesh.remove_morph_target")
			|| CommandId == TEXT("skeletal_mesh.remove_skin_weight_profile");
	}

	bool TryAddDestructiveAuthorization(
		const FString& Params,
		const FString& AuthorizationId,
		FString& OutParams)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Params);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return false;
		}
		Object->SetStringField(
			TEXT("destructive_authorization_id"), AuthorizationId);
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			Writer = TJsonWriterFactory<
				TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutParams);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	bool TryInvoke(
		const FScopedPublicCommandComposition& Harness,
		const FString& CommandId,
		const FString& Params,
		TOptional<FUeAgentCommandResult>& OutResult,
		TSharedPtr<FJsonObject>& OutPayload,
		FString& OutError)
	{
		OutPayload.Reset();
		OutResult.Reset();
		OutError.Reset();
		const TSharedPtr<const UE::UAI::CommandRegistry::
			FUeAgentImmutableCommandRegistry>& Registry =
				Harness.GetCommandRegistry();
		const UE::UAI::CommandContract::FUeAgentFrozenCommandContract*
			const Contract = Registry.IsValid()
				? Registry->FindExact(CommandId) : nullptr;
		if (!Contract)
		{
			OutError = TEXT("command_contract_missing");
			return false;
		}
		const FString RequestId = TEXT("asset-production-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ Contract->GetContractHash()
			+ TEXT("\",\"command_id\":\"") + CommandId
			+ TEXT("\",\"correlation_id\":\"") + CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",\"params\":")
			+ Params
			+ TEXT(",\"request_id\":\"") + RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
		FUeAgentHttpRequest HttpRequest;
		HttpRequest.Method = TEXT("POST");
		HttpRequest.RoutePath = GetUaiCommandRoutePath();
		HttpRequest.Headers.Add(
			TEXT("X-UeAgentInterface-Token"), Harness.GetConfig().AuthToken);
		HttpRequest.Headers.Add(
			TEXT("X-Transport-Correlation"), CorrelationId);
		HttpRequest.Body = Utf8Bytes(Body);
		HttpRequest.PeerId = TEXT("asset-production-test-peer");
		const FUeAgentHttpResponse Response =
			Harness.GetRouteHost().Invoke(HttpRequest);
		if (Response.StatusCode != 200)
		{
			OutError = FString::Printf(
				TEXT("command_http_status:%d"), Response.StatusCode);
			return false;
		}
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		FUeAgentCommandProtocolCodec Codec;
		if (!Codec.DecodeResult(Response.Body, OutResult, Diagnostic)
			|| !OutResult.IsSet())
		{
			OutError = Diagnostic.Code + TEXT(":") + Diagnostic.Message;
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(OutResult->GetPayloadJson());
		return FJsonSerializer::Deserialize(Reader, OutPayload)
			&& OutPayload.IsValid();
	}

	class FOwnedAssetFixture final
	{
	public:
		FOwnedAssetFixture()
			: RootPackage(TEXT("/Game/UAI/Tests/DestructiveImport/W07_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, SourceRoot(FPaths::ProjectIntermediateDir()
				/ TEXT("UAI/W07")
				/ FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
			IFileManager::Get().MakeDirectory(*SourceRoot, true);
		}

		~FOwnedAssetFixture()
		{
			Cleanup();
		}

		bool CreateMoveSource()
		{
			MoveSource = RootPackage + TEXT("/MoveSource");
			MoveDestination = RootPackage + TEXT("/MovedAsset");
			UPackage* const Package = CreatePackage(*MoveSource);
			UCurveFloat* const Asset = NewObject<UCurveFloat>(
				Package, TEXT("MoveSource"),
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Package || !Asset)
			{
				return false;
			}
			Asset->FloatCurve.AddKey(0.0f, 1.0f);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Save(Package, Asset);
		}

		bool CreateDirtyMoveSource()
		{
			DirtyMoveSource = RootPackage + TEXT("/DirtyMoveSource");
			DirtyMoveDestination = RootPackage + TEXT("/DirtyMovedAsset");
			UPackage* const Package = CreatePackage(*DirtyMoveSource);
			UCurveFloat* const Asset = NewObject<UCurveFloat>(
				Package, TEXT("DirtyMoveSource"),
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Package || !Asset)
			{
				return false;
			}
			Asset->FloatCurve.AddKey(0.0f, 1.0f);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			if (!Save(Package, Asset))
			{
				return false;
			}
			Asset->FloatCurve.AddKey(1.0f, 2.0f);
			Package->MarkPackageDirty();
			DirtyMoveAsset.Reset(Asset);
			return Package->IsDirty();
		}

		bool CreateSkeletalAsset()
		{
			SkeletalPackage = RootPackage + TEXT("/OwnedSkeletalMesh");
			UPackage* const Package = CreatePackage(*SkeletalPackage);
			USkeletalMesh* const Template = LoadObject<USkeletalMesh>(
				nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			USkeletalMesh* const Mesh = Template
				? DuplicateObject<USkeletalMesh>(
					Template, Package, TEXT("OwnedSkeletalMesh"))
				: nullptr;
			if (!Package || !Mesh)
			{
				return false;
			}
			Mesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			UMorphTarget* const Morph = NewObject<UMorphTarget>(
				Mesh, TEXT("OwnedMorph"), RF_Public | RF_Transactional);
			FSkeletalMeshModel* const ImportedModel = Mesh->GetImportedModel();
			if (!Morph || !ImportedModel || ImportedModel->LODModels.IsEmpty())
			{
				return false;
			}
			FMorphTargetDelta Delta;
			Delta.SourceIdx = 0;
			Delta.PositionDelta = FVector3f(1.0f, 0.0f, 0.0f);
			Morph->PopulateDeltas(
				{Delta}, 0, ImportedModel->LODModels[0].Sections, false, false);
			if (!Mesh->RegisterMorphTarget(Morph, false))
			{
				return false;
			}
			FSkinWeightProfileInfo Profile;
			Profile.Name = TEXT("OwnedProfile");
			Mesh->GetSkinWeightProfiles().Add(Profile);
			FAssetRegistryModule::AssetCreated(Mesh);
			Package->MarkPackageDirty();
			return Save(Package, Mesh);
		}

		bool CreateReimportFixtures()
		{
			const FString TextureSource = SourceRoot / TEXT("texture.png");
			TArray<uint8> Png;
			if (!FBase64::Decode(
					TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="),
					Png)
				|| !FFileHelper::SaveArrayToFile(Png, *TextureSource))
			{
				return false;
			}
			const FString WaveSource = SourceRoot / TEXT("sound.wav");
			TArray<uint8> Wave;
			auto U16 = [&Wave](uint16 Value)
			{
				Wave.Add(static_cast<uint8>(Value));
				Wave.Add(static_cast<uint8>(Value >> 8));
			};
			auto U32 = [&Wave](uint32 Value)
			{
				for (int32 Index = 0; Index != 4; ++Index)
				{
					Wave.Add(static_cast<uint8>(Value >> (Index * 8)));
				}
			};
			auto FourCc = [&Wave](const ANSICHAR* Value)
			{
				for (int32 Index = 0; Index != 4; ++Index)
				{
					Wave.Add(static_cast<uint8>(Value[Index]));
				}
			};
			constexpr uint32 DataBytes = 160;
			FourCc("RIFF"); U32(36 + DataBytes); FourCc("WAVE");
			FourCc("fmt "); U32(16); U16(1); U16(1); U32(8000);
			U32(16000); U16(2); U16(16); FourCc("data"); U32(DataBytes);
			Wave.AddZeroed(DataBytes);
			if (!FFileHelper::SaveArrayToFile(Wave, *WaveSource))
			{
				return false;
			}
			const FString MeshSource = SourceRoot / TEXT("mesh.obj");
			if (!FFileHelper::SaveStringToFile(
					TEXT("o Owned\nv 0 0 0\nv 10 0 0\nv 0 10 0\nf 1 2 3\n"),
					*MeshSource))
			{
				return false;
			}

			IAssetTools& AssetTools =
				FModuleManager::LoadModuleChecked<FAssetToolsModule>(
					TEXT("AssetTools")).Get();
			const FString Destination = RootPackage + TEXT("/Imported");
			TArray<UObject*> Imported = AssetTools.ImportAssets(
				{TextureSource, WaveSource, MeshSource}, Destination);
			for (UObject* const Asset : Imported)
			{
				if (Asset)
				{
					ImportedByClass.Add(Asset->GetClass()->GetFName(), Asset);
					UPackage* const Package = Asset->GetOutermost();
					Package->MarkPackageDirty();
					if (!Save(Package, Asset))
					{
						return false;
					}
				}
			}
			TexturePackage = PackageForClass(UTexture2D::StaticClass());
			SoundPackage = PackageForClass(USoundWave::StaticClass());
			StaticMeshPackage = PackageForClass(UStaticMesh::StaticClass());
			TextureSourceRef = TextureSource;
			SoundSourceRef = WaveSource;
			StaticMeshSourceRef = MeshSource;
			SparseSourceRef = SourceRoot / TEXT("missing.vdb");
			return !TexturePackage.IsEmpty()
				&& !SoundPackage.IsEmpty()
				&& !StaticMeshPackage.IsEmpty();
		}

		bool IsPackagePresent(const FString& PackageName) const
		{
			FString Filename;
			return FPackageName::DoesPackageExist(PackageName, &Filename);
		}

		bool IsSkeletalEditAbsent(
			const bool bMorph,
			const FName Name) const
		{
			USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
				nullptr, *(SkeletalPackage + TEXT(".OwnedSkeletalMesh")));
			if (!Mesh)
			{
				return false;
			}
			if (bMorph)
			{
				return Mesh->FindMorphTarget(Name) == nullptr;
			}
			return !Mesh->GetSkinWeightProfiles().ContainsByPredicate(
				[Name](const FSkinWeightProfileInfo& Item)
				{
					return Item.Name == Name;
				});
		}

		void Cleanup()
		{
			DirtyMoveAsset.Reset();
			TArray<UObject*> Objects;
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*RootPackage));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);
			for (const FAssetData& Data : Assets)
			{
				if (UObject* const Asset = Data.GetAsset())
				{
					Objects.Add(Asset);
				}
			}
			if (!Objects.IsEmpty())
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				ObjectTools::DeleteObjectsUnchecked(Objects);
			}
			IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);

			FString ContentDirectory;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					RootPackage,
					ContentDirectory))
			{
				return;
			}
			ContentDirectory = FPaths::ConvertRelativePathToFull(
				ContentDirectory);
			if (!IFileManager::Get().DirectoryExists(*ContentDirectory))
			{
				return;
			}
			TArray<FString> RemainingFiles;
			IFileManager::Get().FindFilesRecursive(
				RemainingFiles,
				*ContentDirectory,
				TEXT("*"),
				true,
				false,
				false);
			if (RemainingFiles.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(
					*ContentDirectory,
					false,
					true);
			}
		}

		FString RootPackage;
		FString MoveSource;
		FString MoveDestination;
		FString DirtyMoveSource;
		FString DirtyMoveDestination;
		FString SkeletalPackage;
		FString TexturePackage;
		FString SoundPackage;
		FString StaticMeshPackage;
		FString TextureSourceRef;
		FString SoundSourceRef;
		FString StaticMeshSourceRef;
		FString SparseSourceRef;

	private:
		TStrongObjectPtr<UCurveFloat> DirtyMoveAsset;

		bool Save(UPackage* Package, UObject* Asset)
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Package, Asset, *Filename, Args);
		}

		FString PackageForClass(UClass* Class) const
		{
			for (const TPair<FName, TObjectPtr<UObject>>& Item : ImportedByClass)
			{
				if (Item.Value && Item.Value->IsA(Class))
				{
					return Item.Value->GetOutermost()->GetName();
				}
			}
			return {};
		}

		FString SourceRoot;
		TMap<FName, TObjectPtr<UObject>> ImportedByClass;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportAssetProductionTest,
	"UeAgentInterface.Composition.DestructiveImport.AssetProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportAssetProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedAssetFixture Fixture;
	if (!TestTrue(TEXT("owned move fixture"), Fixture.CreateMoveSource())
		|| !TestTrue(TEXT("owned dirty move fixture"),
			Fixture.CreateDirtyMoveSource())
		|| !TestTrue(TEXT("owned reimport fixtures"), Fixture.CreateReimportFixtures())
		|| !TestTrue(TEXT("owned skeletal fixture"), Fixture.CreateSkeletalAsset()))
	{
		return false;
	}
	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings()
		: nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}
	FScopedPublicCommandComposition Harness(TEXT("destructive-asset-production"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	const TArray<FString> RetiredLegacyIds{
		TEXT("asset_move"), TEXT("asset_fixup_redirectors"),
		TEXT("sound_wave_reimport"), TEXT("sparse_volume_texture_reimport"),
		TEXT("static_mesh_reimport"), TEXT("texture_reimport"),
		TEXT("skeletal_mesh_remove_morph_target"),
		TEXT("skeletal_mesh_remove_skin_weight_profile"),
	};
	for (const FString& LegacyId : RetiredLegacyIds)
	{
		TestNull(*FString::Printf(
			TEXT("legacy command is absent from the formal registry: %s"),
			*LegacyId),
			Harness.GetCommandRegistry()->FindExact(LegacyId));
	}

	auto Invoke = [this, &Harness, &Error](
		const FString& CommandId,
		const FString& Params,
		bool bExpectedSuccess,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		TOptional<FUeAgentCommandResult> Result;
		const bool bInvoked = TryInvoke(
			Harness, CommandId, Params, Result, OutPayload, Error);
		TestTrue(*FString::Printf(TEXT("%s reaches formal handler"), *CommandId),
			bInvoked && Result.IsSet());
		if (!bInvoked || !Result.IsSet())
		{
			AddError(Error);
			return false;
		}
		if (IsPlanAuthorizedCommand(CommandId) && Result->IsOk())
		{
			FString AuthorizationId;
			FString TerminalStatus;
			FString AuthorizedParams;
			if (!OutPayload.IsValid()
				|| !OutPayload->TryGetStringField(
					TEXT("terminal_status"), TerminalStatus)
				|| TerminalStatus != TEXT("planned")
				|| !OutPayload->TryGetStringField(
					TEXT("destructive_authorization_id"), AuthorizationId)
				|| AuthorizationId.IsEmpty()
				|| !TryAddDestructiveAuthorization(
					Params, AuthorizationId, AuthorizedParams)
				|| !TryInvoke(
					Harness, CommandId, AuthorizedParams,
					Result, OutPayload, Error)
				|| !Result.IsSet())
			{
				AddError(CommandId + TEXT(":destructive_authorization_failed:")
					+ Error);
				return false;
			}
		}
		TestEqual(*FString::Printf(TEXT("%s success state"), *CommandId),
			Result->IsOk(), bExpectedSuccess);
		FString TerminalStatus;
		if (Result->IsOk())
		{
			TestTrue(*FString::Printf(TEXT("%s success terminal status"),
					*CommandId),
				OutPayload.IsValid()
					&& OutPayload->TryGetStringField(
						TEXT("terminal_status"), TerminalStatus)
					&& TerminalStatus == (IsPlanAuthorizedCommand(CommandId)
						? TEXT("committed") : TEXT("succeeded")));
		}
		else
		{
			TSharedPtr<FJsonObject> ErrorObject;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result->GetErrorJson());
			TestTrue(*FString::Printf(TEXT("%s typed terminal failure"),
					*CommandId),
				FJsonSerializer::Deserialize(Reader, ErrorObject)
					&& ErrorObject.IsValid()
					&& ErrorObject->TryGetStringField(
						TEXT("terminal_status"), TerminalStatus)
					&& !TerminalStatus.IsEmpty()
					&& TerminalStatus != TEXT("state_unknown"));
		}
		if (Result->IsOk() != bExpectedSuccess)
		{
			AddError(FString::Printf(
				TEXT("%s unexpected result payload=%s error=%s"),
				*CommandId, *Result->GetPayloadJson(), *Result->GetErrorJson()));
		}
		return Result->IsOk() == bExpectedSuccess;
	};

	TSharedPtr<FJsonObject> Payload;
	Invoke(TEXT("asset.move"),
		TwoParams(TEXT("target_ref"), Fixture.MoveSource,
			TEXT("destination_ref"), Fixture.MoveDestination), true, Payload);
	TestTrue(TEXT("asset move durable destination readback"),
		Fixture.IsPackagePresent(Fixture.MoveDestination)
			&& !Fixture.IsPackagePresent(Fixture.MoveSource));
	Invoke(TEXT("asset.fixup_redirectors"),
		FString::Printf(TEXT("{\"root_ref\":\"%s\"}"), *Fixture.RootPackage),
		true, Payload);
	Invoke(TEXT("asset.move"),
		TwoParams(TEXT("target_ref"), Fixture.DirtyMoveSource,
			TEXT("destination_ref"), Fixture.DirtyMoveDestination), false, Payload);
	TestTrue(TEXT("dirty-source move fails before mutation"),
		Fixture.IsPackagePresent(Fixture.DirtyMoveSource)
			&& !Fixture.IsPackagePresent(Fixture.DirtyMoveDestination));

	Invoke(TEXT("asset.reimport_texture"),
		TwoParams(TEXT("target_ref"), Fixture.TexturePackage,
			TEXT("source_ref"), Fixture.TextureSourceRef), true, Payload);
	Invoke(TEXT("asset.reimport_sound_wave"),
		TwoParams(TEXT("target_ref"), Fixture.SoundPackage,
			TEXT("source_ref"), Fixture.SoundSourceRef), true, Payload);
	Invoke(TEXT("asset.reimport_static_mesh"),
		TwoParams(TEXT("target_ref"), Fixture.StaticMeshPackage,
			TEXT("source_ref"), Fixture.StaticMeshSourceRef), true, Payload);
	const FString MissingSparseTarget = Fixture.RootPackage + TEXT("/MissingSparse");
	Invoke(TEXT("asset.reimport_sparse_volume_texture"),
		TwoParams(TEXT("target_ref"), MissingSparseTarget,
			TEXT("source_ref"), Fixture.SparseSourceRef), false, Payload);

	Invoke(TEXT("skeletal_mesh.remove_morph_target"),
		TwoParams(TEXT("target_ref"), Fixture.SkeletalPackage,
			TEXT("name"), TEXT("OwnedMorph")), true, Payload);
	Invoke(TEXT("skeletal_mesh.remove_skin_weight_profile"),
		TwoParams(TEXT("target_ref"), Fixture.SkeletalPackage,
			TEXT("name"), TEXT("OwnedProfile")), true, Payload);
	TestTrue(TEXT("skeletal destructive edits survive save/reload readback"),
		Fixture.IsSkeletalEditAbsent(true, TEXT("OwnedMorph"))
			&& Fixture.IsSkeletalEditAbsent(false, TEXT("OwnedProfile")));
	TestTrue(TEXT("composition and owned fixtures clean up"), Harness.Stop());
	return !HasAnyErrors();
}
