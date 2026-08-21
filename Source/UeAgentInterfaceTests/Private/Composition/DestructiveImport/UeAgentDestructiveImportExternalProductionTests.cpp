#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/GarbageCollection.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition::DestructiveImport;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::Tests::PublicCommand;

	const TArray<FString> CanonicalIds{
		TEXT("external.fbx.animation.import"),
		TEXT("external.fbx.skeletal_mesh.import"),
		TEXT("external.geometry_cache.import"),
		TEXT("external.texture.import"),
		TEXT("external.light_profile.import"),
		TEXT("external.sparse_volume_texture.import"),
		TEXT("external.sound_wave.import"),
		TEXT("external.localization.po.import"),
		TEXT("external.skin_weight_profile.import"),
	};

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			Writer = TJsonWriterFactory<
				TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FUeAgentCommandContract ExpectedContract(const FString& CommandId)
	{
		TArray<FUeAgentCommandParameter> Parameters;
		auto String = [&Parameters](const TCHAR* Name)
		{
			Parameters.Emplace(
				Name, EUeAgentCommandWireType::String, true, false);
		};
		String(TEXT("source_ref"));
		if (CommandId == TEXT("external.fbx.animation.import"))
		{
			String(TEXT("destination_ref"));
			String(TEXT("skeleton_ref"));
		}
		else if (CommandId == TEXT("external.localization.po.import"))
		{
			String(TEXT("target_ref"));
		}
		else if (CommandId == TEXT("external.skin_weight_profile.import"))
		{
			String(TEXT("target_ref"));
			String(TEXT("profile_name"));
			Parameters.Emplace(
				TEXT("lod_index"), EUeAgentCommandWireType::Integer, true, false);
		}
		else
		{
			String(TEXT("destination_ref"));
		}
		FString Namespace;
		FString Method;
		CommandId.Split(TEXT("."), &Namespace, &Method);
		Method.ReplaceInline(TEXT("."), TEXT("_"));
		return FUeAgentCommandContract(
			CommandId, Namespace, Method, MoveTemp(Parameters),
			1, 1, EUeAgentCommandStatus::Active);
	}

	bool TryInvoke(
		const FScopedPublicCommandComposition& Harness,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		TOptional<FUeAgentCommandResult>& OutResult,
		TSharedPtr<FJsonObject>& OutPayload,
		FString& OutError)
	{
		OutResult.Reset();
		OutPayload.Reset();
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
		const FString RequestId = TEXT("external-production-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"") + Contract->GetContractHash()
			+ TEXT("\",\"command_id\":\"") + CommandId
			+ TEXT("\",\"correlation_id\":\"") + CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",\"params\":")
			+ Serialize(Params)
			+ TEXT(",\"request_id\":\"") + RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
		FUeAgentHttpRequest Request;
		Request.Method = TEXT("POST");
		Request.RoutePath = GetUaiCommandRoutePath();
		Request.Headers.Add(
			TEXT("X-UeAgentInterface-Token"), Harness.GetConfig().AuthToken);
		Request.Headers.Add(
			TEXT("X-Transport-Correlation"), CorrelationId);
		Request.Body = Utf8Bytes(Body);
		Request.PeerId = TEXT("external-production-test-peer");
		const FUeAgentHttpResponse Response = Harness.GetRouteHost().Invoke(Request);
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

	class FOwnedExternalFixture final
	{
	public:
		FOwnedExternalFixture()
			: RootPackage(TEXT("/Game/UAI/Tests/ExternalDataImport/W09_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, SourceRoot(FPaths::ProjectIntermediateDir()
				/ TEXT("UAI/W09")
				/ FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
			IFileManager::Get().MakeDirectory(*SourceRoot, true);
			TextureSource = SourceRoot / TEXT("OwnedTexture.png");
			SoundSource = SourceRoot / TEXT("OwnedSound.wav");
			LightSource = SourceRoot / TEXT("OwnedLight.ies");
			MissingSource = SourceRoot / TEXT("missing.source");
		}

		~FOwnedExternalFixture()
		{
			TArray<UObject*> Objects;
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*RootPackage));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssets(Filter, Assets);
			for (const FAssetData& Data : Assets)
			{
				if (UObject* const Asset = Data.GetAsset()) Objects.Add(Asset);
			}
			if (!Objects.IsEmpty())
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				ObjectTools::DeleteObjectsUnchecked(Objects);
			}
			IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
		}

		bool CreateSources()
		{
			TArray<uint8> Png;
			if (!FBase64::Decode(
					TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="),
					Png)
				|| !FFileHelper::SaveArrayToFile(Png, *TextureSource))
			{
				return false;
			}
			TArray<uint8> Wave;
			auto U16 = [&Wave](uint16 Value)
			{
				Wave.Add(static_cast<uint8>(Value));
				Wave.Add(static_cast<uint8>(Value >> 8));
			};
			auto U32 = [&Wave](uint32 Value)
			{
				for (int32 Index = 0; Index != 4; ++Index)
					Wave.Add(static_cast<uint8>(Value >> (Index * 8)));
			};
			auto FourCc = [&Wave](const ANSICHAR* Value)
			{
				for (int32 Index = 0; Index != 4; ++Index)
					Wave.Add(static_cast<uint8>(Value[Index]));
			};
			constexpr uint32 DataBytes = 160;
			FourCc("RIFF"); U32(36 + DataBytes); FourCc("WAVE");
			FourCc("fmt "); U32(16); U16(1); U16(1); U32(8000);
			U32(16000); U16(2); U16(16); FourCc("data"); U32(DataBytes);
			Wave.AddZeroed(DataBytes);
			if (!FFileHelper::SaveArrayToFile(Wave, *SoundSource)) return false;
			return FFileHelper::SaveStringToFile(
				TEXT("IESNA:LM-63-1995\n[TEST] UAI\nTILT=NONE\n")
				TEXT("1 1000 1 1 1 1 2 0 0 0\n1 1 1\n0\n0\n1000\n"),
				*LightSource);
		}

		bool PackageAbsent(const FString& PackageName) const
		{
			FString Filename;
			return !FPackageName::DoesPackageExist(PackageName, &Filename)
				&& FindPackage(nullptr, *PackageName) == nullptr;
		}

		UObject* Reload(const FString& PackageName) const
		{
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Package}, Error, true))
					return nullptr;
			}
			CollectGarbage(RF_NoFlags, true);
			return LoadObject<UObject>(
				nullptr, *(PackageName + TEXT(".")
					+ FPackageName::GetLongPackageAssetName(PackageName)));
		}

		FString RootPackage;
		FString SourceRoot;
		FString TextureSource;
		FString SoundSource;
		FString LightSource;
		FString MissingSource;
	};

	TSharedRef<FJsonObject> Params()
	{
		return MakeShared<FJsonObject>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportExternalProductionTest,
	"UeAgentInterface.Composition.DestructiveImport.ExternalProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportExternalProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedExternalFixture Fixture;
	if (!TestTrue(TEXT("owned external sources"), Fixture.CreateSources()))
		return false;
	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings)) return false;
	FScopedPublicCommandComposition Harness(TEXT("external-import-production"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	for (int32 Index = 0; Index < CanonicalIds.Num(); ++Index)
	{
		const FString& CommandId = CanonicalIds[Index];
		const FString Destination = Fixture.RootPackage
			+ FString::Printf(TEXT("/Missing_%02d"), Index);
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("source_ref"), Fixture.MissingSource);
		if (CommandId == TEXT("external.fbx.animation.import"))
		{
			Input->SetStringField(TEXT("destination_ref"), Destination);
			Input->SetStringField(
				TEXT("skeleton_ref"), TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton"));
		}
		else if (CommandId == TEXT("external.localization.po.import"))
		{
			Input->SetStringField(TEXT("target_ref"), TEXT("UAI_W09_Target"));
		}
		else if (CommandId == TEXT("external.skin_weight_profile.import"))
		{
			Input->SetStringField(
				TEXT("target_ref"), TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			Input->SetStringField(TEXT("profile_name"), TEXT("W09Missing"));
			Input->SetNumberField(TEXT("lod_index"), 0);
		}
		else
		{
			Input->SetStringField(TEXT("destination_ref"), Destination);
		}
		TOptional<FUeAgentCommandResult> Result;
		TSharedPtr<FJsonObject> Payload;
		const bool bInvoked = TryInvoke(
			Harness, CommandId, Input, Result, Payload, Error);
		TestTrue(TEXT("missing source reaches formal route: ") + CommandId,
			bInvoked && Result.IsSet() && Payload.IsValid());
		if (bInvoked && Result.IsSet() && Payload.IsValid())
		{
			TestFalse(TEXT("missing source fails closed: ") + CommandId,
				Result->IsOk());
			TSharedPtr<FJsonObject> ErrorObject;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result->GetErrorJson());
			FString TerminalStatus;
			bool bMutationStarted = true;
			bool bRetrySafe = false;
			TestTrue(TEXT("missing source has typed pre-mutation failure: ")
					+ CommandId,
				FJsonSerializer::Deserialize(Reader, ErrorObject)
					&& ErrorObject.IsValid()
					&& ErrorObject->TryGetStringField(
						TEXT("terminal_status"), TerminalStatus)
					&& TerminalStatus == TEXT("rejected")
					&& ErrorObject->TryGetBoolField(
						TEXT("mutation_started"), bMutationStarted)
					&& !bMutationStarted
					&& ErrorObject->TryGetBoolField(
						TEXT("retry_safe"), bRetrySafe)
					&& bRetrySafe);
		}
		if (CommandId != TEXT("external.localization.po.import")
			&& CommandId != TEXT("external.skin_weight_profile.import"))
		{
			TestTrue(TEXT("missing source leaves no package: ") + CommandId,
				Fixture.PackageAbsent(Destination));
		}
	}

	struct FPositive
	{
		FString CommandId;
		FString Source;
		FString PackageName;
		UClass* ExpectedClass = nullptr;
	};
	const TArray<FPositive> Positives{
		{TEXT("external.texture.import"), Fixture.TextureSource,
			Fixture.RootPackage + TEXT("/OwnedTexture"), UTexture2D::StaticClass()},
		{TEXT("external.sound_wave.import"), Fixture.SoundSource,
			Fixture.RootPackage + TEXT("/OwnedSound"), USoundWave::StaticClass()},
		{TEXT("external.light_profile.import"), Fixture.LightSource,
			Fixture.RootPackage + TEXT("/OwnedLight"), UTextureLightProfile::StaticClass()},
	};
	for (const FPositive& Positive : Positives)
	{
		const TSharedRef<FJsonObject> Input = Params();
		Input->SetStringField(TEXT("source_ref"), Positive.Source);
		Input->SetStringField(TEXT("destination_ref"), Positive.PackageName);
		TOptional<FUeAgentCommandResult> Result;
		TSharedPtr<FJsonObject> Payload;
		const bool bInvoked = TryInvoke(
			Harness, Positive.CommandId, Input, Result, Payload, Error);
		TestTrue(TEXT("real importer reaches formal route: ") + Positive.CommandId,
			bInvoked && Result.IsSet() && Result->IsOk() && Payload.IsValid());
		if (bInvoked && Result.IsSet() && Payload.IsValid())
		{
			TestEqual(TEXT("real import commits: ") + Positive.CommandId,
				Payload->GetStringField(TEXT("terminal_status")), TEXT("succeeded"));
			const TSharedPtr<FJsonObject>* Readback = nullptr;
			FString SourceRef;
			FString SourceHash;
			bool bReloaded = false;
			TestTrue(TEXT("source provenance retained by independent readback: ")
					+ Positive.CommandId,
				Payload->TryGetObjectField(TEXT("independent_readback"), Readback)
					&& Readback && Readback->IsValid()
					&& (*Readback)->TryGetStringField(
						TEXT("source_ref"), SourceRef)
					&& SourceRef == Positive.Source
					&& (*Readback)->TryGetStringField(
						TEXT("source_sha256"), SourceHash)
					&& !SourceHash.IsEmpty()
					&& (*Readback)->TryGetBoolField(
						TEXT("reloaded"), bReloaded)
					&& bReloaded);
		}
		UObject* const Reloaded = Fixture.Reload(Positive.PackageName);
		TestTrue(TEXT("durable class reload readback: ") + Positive.CommandId,
			Reloaded && Reloaded->IsA(Positive.ExpectedClass));
	}

	TestTrue(TEXT("formal public composition stops"), Harness.Stop());
	TestEqual(TEXT("public route cleanup"), Harness.GetRouteHost().Handlers.Num(), 0);
	return !HasAnyErrors();
}
