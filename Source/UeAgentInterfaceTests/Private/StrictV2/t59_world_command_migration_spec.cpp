#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::Tests::StrictV2::T59
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("world.actor.create"),
			TEXT("world.actor_hlod_layer.update"),
			TEXT("world.actor_streaming_policy.update"),
			TEXT("world.data_layer_instance.create"),
			TEXT("world.data_layer_relation.update"),
			TEXT("world.data_layer_state.update"),
			TEXT("world.level_game_mode.update"),
			TEXT("world.streaming_relation.create"),
			TEXT("world.streaming_relation.state.update"),
			TEXT("world.wall.create_with_opening"),
			TEXT("world.world_partition_streaming.update"),
		};
		return Commands;
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FUeAgentFrozenCommandContract* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t59-hjson"), TEXT("req-t59-hjson"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t59_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t59-hjson-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t59-hjson\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t59-hjson"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t59-hjson"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate"),
				 TEXT("uai:authoring:hjson:import")},
				TEXT("corr-t59-hjson")),
			MakeShared<FNeverCancelled>());
	}

	FString CanonicalJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Object, Result, Error) ? Result : TEXT("{}");
	}

	FString HlodDocument()
	{
		return TEXT(R"hjson({
			"schema":"uai.world_hlod_layer.edit/1",
			"root":{
				"type":"/Script/Engine.HLODLayer",
				"layer_type":"Instancing",
				"is_spatially_loaded":true,
				"cell_size":512,
				"loading_range":2048.0
			}
		})hjson");
	}

	FString Sha256Text(const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	bool WriteText(const FString& Path, const FString& Text)
	{
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			&& FFileHelper::SaveStringToFile(
				Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool WriteDirectoryWorkspace(
		const FString& Root,
		const FString& Target,
		const FString& Document)
	{
		const FString Entry = TEXT("asset.hjson");
		const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"), TEXT("uai.content_graph.workspace/1"));
		Manifest->SetNumberField(TEXT("format_version"), 1);
		Manifest->SetStringField(TEXT("target_identity"), Target);
		Manifest->SetStringField(TEXT("base_revision"), TEXT("absent"));
		Manifest->SetStringField(TEXT("entry_file"), Entry);
		const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("path"), Entry);
		File->SetStringField(TEXT("content_hash"), Sha256Text(Document));
		Manifest->SetArrayField(
			TEXT("files"), {MakeShared<FJsonValueObject>(File)});
		Manifest->SetArrayField(
			TEXT("references"), TArray<TSharedPtr<FJsonValue>>{});
		return WriteText(FPaths::Combine(Root, Entry), Document)
			&& WriteText(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				CanonicalJson(Manifest));
	}

	FString ImportParams(
		const FString& Target,
		const TSharedRef<FJsonObject>& Source)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		Params->SetStringField(TEXT("expected_revision"), TEXT("absent"));
		Params->SetStringField(
			TEXT("authorization_id"), TEXT("auth:t59:hlod-hjson-product"));
		return CanonicalJson(Params);
	}

	bool CleanupPackage(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			FlushAsyncLoading();
			FText Error;
			UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
		}
		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}
		return !IFileManager::Get().FileExists(*Filename)
			&& !FindPackage(nullptr, *PackageName);
	}

	bool TryOperationRef(
		const FUeAgentCommandResult& Result,
		FString& OutRefJson,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		OutRefJson.Reset();
		OutPayload.Reset();
		FString Error;
		const TSharedPtr<FJsonObject>* Ref = nullptr;
		return Result.IsOk()
			&& Json::TryParseObject(Result.GetPayloadJson(), OutPayload, Error)
			&& OutPayload.IsValid()
			&& OutPayload->TryGetObjectField(TEXT("operation_ref"), Ref)
			&& Ref && Ref->IsValid()
			&& Json::TryCanonicalize((*Ref).ToSharedRef(), OutRefJson, Error);
	}

	class FWorldLifecyclePolicyLatentCommand final
		: public IAutomationLatentCommand
	{
	public:
		FWorldLifecyclePolicyLatentCommand(
			FAutomationTestBase* InTest,
			TSharedPtr<const FUeAgentImmutableCommandRegistry> InRegistry,
			TSharedPtr<const FUeAgentExactCommandDispatch> InDispatch)
			: Test(InTest), Registry(MoveTemp(InRegistry)), Dispatch(MoveTemp(InDispatch))
			, StartedAt(FPlatformTime::Seconds())
		{
			if (GEditor)
			{
				if (UWorld* World = GEditor->GetEditorWorldContext().World())
				{
					OriginalWorld = World->GetPackage()
						? World->GetPackage()->GetName() : FString();
				}
			}
			TemporaryWorld = TEXT("/Game/UAI/Tests/T59/WorldContext_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		}

		bool Update() override
		{
			if (!Test || !Registry.IsValid() || !Dispatch.IsValid())
			{
				return true;
			}
			if (FPlatformTime::Seconds() - StartedAt > 180.0)
			{
				Test->AddError(FString::Printf(
					TEXT("T59 world lifecycle policy scenario timed out at stage %d"),
					Stage));
				RestoreAndCleanup();
				return true;
			}
			if (Stage == 0)
			{
				if (OriginalWorld.IsEmpty())
				{
					Test->AddError(TEXT("T59 lifecycle scenario has no editor world"));
					return true;
				}
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("job.compile_build"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_build\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":\"policy:world-build:hlod-plan/v1\",")
						TEXT("\"parameters\":{\"step\":\"build\"}}"),
						*OriginalWorld));
				TSharedPtr<FJsonObject> Payload;
				if (!TryOperationRef(Started, OperationRef, Payload))
				{
					Test->AddError(TEXT("HLOD plan policy was not accepted: ")
						+ Started.GetErrorJson());
					return true;
				}
				Stage = 1;
				return false;
			}
			if (Stage == 1)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("HLOD plan"), Status)) return false;
				if (!Status.IsValid()) return FinishWithCleanup();
				Test->TestTrue(TEXT("HLOD plan exposes its durable report"),
					Status->HasField(TEXT("latest_signal"))
					&& CanonicalJson(Status.ToSharedRef()).Contains(TEXT("report_ref")));
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("editor.session"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_context\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":\"policy:world-context:create-blank/v1\",")
						TEXT("\"parameters\":{\"overwrite_policy\":\"fail\",")
						TEXT("\"save_existing_map\":false,\"create_world_partition\":true}}"),
						*TemporaryWorld));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("create-blank policy was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 2;
				return false;
			}
			if (Stage == 2)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("create-blank"), Status)) return false;
				if (!Status.IsValid()) return FinishWithCleanup();
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!Test->TestNotNull(TEXT("create-blank leaves a real current world"), World)
					|| !Test->TestEqual(TEXT("created map becomes the current map"),
						World->GetPackage()->GetName(), TemporaryWorld)
					|| !Test->TestNotNull(TEXT("created fixture owns World Partition"),
						World->GetWorldPartition()))
				{
					return FinishWithCleanup();
				}
				const FString MapFile = FPackageName::LongPackageNameToFilename(
					TemporaryWorld, FPackageName::GetMapPackageExtension());
				Test->TestTrue(TEXT("create-blank persists a map package"),
					IFileManager::Get().FileExists(*MapFile));
				const FUeAgentCommandResult CreatedActor = SubmitPublic(
					Registry, Dispatch, TEXT("world.actor.create"),
					TEXT("{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\",")
					TEXT("\"folder_path\":\"UAI/T59\",\"label\":\"T59ResaveActor\",")
					TEXT("\"placement\":{\"basis_evidence\":\"explicit UE world frame\",")
					TEXT("\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":[0,0,0,1],")
					TEXT("\"scale\":[1,1,1],\"translation\":[0,0,0],")
					TEXT("\"unit\":\"centimeter\"},\"save_level\":true,")
					TEXT("\"tags\":[\"T59ResaveFixture\"]}"));
				if (!Test->TestTrue(
						TEXT("resave fixture is created through the public World command"),
						CreatedActor.IsOk()))
				{
					Test->AddError(CreatedActor.GetErrorJson());
					return FinishWithCleanup();
				}
				AActor* ResaveActor = nullptr;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == TEXT("T59ResaveActor"))
					{
						ResaveActor = *It;
						break;
					}
				}
				if (!Test->TestNotNull(
						TEXT("resave fixture has an independently observed Actor"),
						ResaveActor)
					|| !Test->TestNotNull(
						TEXT("resave fixture uses an external Actor package"),
						ResaveActor ? ResaveActor->GetExternalPackage() : nullptr)
					|| !Test->TestTrue(
						TEXT("resave fixture has a stable Actor GUID"),
						ResaveActor && ResaveActor->GetActorGuid().IsValid()))
				{
					return FinishWithCleanup();
				}
				ResaveActorGuid = ResaveActor->GetActorGuid().ToString(
					EGuidFormats::DigitsWithHyphens);
				ResaveActorPackageName = ResaveActor->GetExternalPackage()->GetName();
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("editor.session"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_context\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":")
						TEXT("\"policy:world-context:partition-region-loader/v1\",")
						TEXT("\"parameters\":{\"bounds\":{\"center\":[0,0,0],")
						TEXT("\"extent\":[100,100,100]},\"label\":\"T59Region\"}}"),
						*TemporaryWorld));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("region-loader policy was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 3;
				return false;
			}
			if (Stage == 3)
			{
				const FUeAgentCommandResult StatusResult = SubmitPublic(
					Registry, Dispatch, TEXT("operation.status"),
					TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
				TSharedPtr<FJsonObject> Status;
				FString IgnoredRef;
				if (!TryOperationRef(StatusResult, IgnoredRef, Status))
				{
					Test->AddError(TEXT("region-loader status failed: ")
						+ StatusResult.GetErrorJson());
					return FinishWithCleanup();
				}
				if (!Status->GetBoolField(TEXT("terminal"))
					&& !CanonicalJson(Status.ToSharedRef()).Contains(
						TEXT("world-partition-loader")))
				{
					if (!TryAdvance(Status))
					{
						return FinishWithCleanup();
					}
				}
				if (!Status->GetBoolField(TEXT("terminal"))
					&& CanonicalJson(Status.ToSharedRef()).Contains(
						TEXT("world-partition-loader")))
				{
					const FUeAgentCommandResult Query = SubmitPublic(
						Registry, Dispatch, TEXT("world.partition"),
						TEXT("{\"include_loaded_regions\":true}"));
					Test->TestTrue(TEXT("active loader is visible in public partition state"),
						Query.IsOk()
						&& Query.GetPayloadJson().Contains(TEXT("user_loaded_region_count")));
					const FUeAgentCommandResult Cancelled = SubmitPublic(
						Registry, Dispatch, TEXT("operation.cancel"),
						TEXT("{\"operation_ref\":") + OperationRef
							+ TEXT(",\"reason\":\"t59_fixture_cleanup\"}"));
					if (!Cancelled.IsOk())
					{
						Test->AddError(TEXT("region-loader cancel failed: ")
							+ Cancelled.GetErrorJson());
						return FinishWithCleanup();
					}
					Stage = 4;
				}
				else if (Status->GetBoolField(TEXT("terminal")))
				{
					Test->AddError(TEXT("region-loader exited before explicit cleanup: ")
						+ StatusResult.GetPayloadJson());
					return FinishWithCleanup();
				}
				return false;
			}
			if (Stage == 4)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("region-loader cleanup"), Status, true))
				{
					return false;
				}
				SubmitPublic(
					Registry, Dispatch, TEXT("operation.release"),
					TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
				const FUeAgentCommandResult ActorLoader = SubmitPublic(
					Registry, Dispatch, TEXT("editor.session"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_context\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":")
						TEXT("\"policy:world-context:partition-actor-loader/v1\",")
						TEXT("\"parameters\":{\"actor_guids\":[\"%s\"]}}"),
						*TemporaryWorld, *ResaveActorGuid));
				if (!TryOperationRef(ActorLoader, OperationRef, Status))
				{
					Test->AddError(TEXT("actor-loader policy was not accepted: ")
						+ ActorLoader.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 5;
				return false;
			}
			if (Stage == 5)
			{
				const FUeAgentCommandResult StatusResult = SubmitPublic(
					Registry, Dispatch, TEXT("operation.status"),
					TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
				TSharedPtr<FJsonObject> Status;
				FString IgnoredRef;
				if (!TryOperationRef(StatusResult, IgnoredRef, Status))
				{
					Test->AddError(TEXT("actor-loader status failed: ")
						+ StatusResult.GetErrorJson());
					return FinishWithCleanup();
				}
				if (!Status->GetBoolField(TEXT("terminal"))
					&& !CanonicalJson(Status.ToSharedRef()).Contains(
						TEXT("world-partition-loader")))
				{
					if (!TryAdvance(Status)) return FinishWithCleanup();
				}
				if (Status->GetBoolField(TEXT("terminal")))
				{
					Test->AddError(TEXT("actor-loader exited before explicit cleanup: ")
						+ StatusResult.GetPayloadJson());
					return FinishWithCleanup();
				}
				if (CanonicalJson(Status.ToSharedRef()).Contains(
						TEXT("world-partition-loader")))
				{
					const FUeAgentCommandResult Cancelled = SubmitPublic(
						Registry, Dispatch, TEXT("operation.cancel"),
						TEXT("{\"operation_ref\":") + OperationRef
							+ TEXT(",\"reason\":\"t59_actor_fixture_cleanup\"}"));
					if (!Cancelled.IsOk())
					{
						Test->AddError(TEXT("actor-loader cancel failed: ")
							+ Cancelled.GetErrorJson());
						return FinishWithCleanup();
					}
					Stage = 6;
				}
				return false;
			}
			if (Stage == 6)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("actor-loader cleanup"), Status, true))
				{
					return false;
				}
				SubmitPublic(
					Registry, Dispatch, TEXT("operation.release"),
					TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("job.compile_build"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_partition_resave\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":")
						TEXT("\"policy:world-partition-resave:actors-plan/v1\",")
						TEXT("\"parameters\":{\"actor_guids\":[\"%s\"]}}"),
						*TemporaryWorld, *ResaveActorGuid));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("Actor GUID resave plan was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 7;
				return false;
			}
			if (Stage == 7)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("Actor GUID resave plan"), Status)) return false;
				if (!Status.IsValid()) return FinishWithCleanup();
				Test->TestTrue(TEXT("Actor GUID resave plan exposes its report"),
					CanonicalJson(Status.ToSharedRef()).Contains(TEXT("report_ref")));
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("job.compile_build"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_partition_resave\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":")
						TEXT("\"policy:world-partition-resave:actors-run/v1\",")
						TEXT("\"parameters\":{\"actor_guids\":[\"%s\"],")
						TEXT("\"allow_commandlet\":true,\"allow_destructive\":true}}"),
						*TemporaryWorld, *ResaveActorGuid));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("Actor GUID resave execute was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 8;
				return false;
			}
			if (Stage == 8)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("Actor GUID resave execute"), Status)) return false;
				if (!Status.IsValid()) return FinishWithCleanup();
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("job.compile_build"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_build\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":\"policy:world-build:hlod-run/v1\",")
						TEXT("\"parameters\":{\"step\":\"setup\",")
						TEXT("\"allow_commandlet\":true,\"allow_destructive\":true,")
						TEXT("\"allow_commandlet_rendering\":true}}"),
						*TemporaryWorld));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("HLOD execute policy was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 9;
				return false;
			}
			if (Stage == 9)
			{
				TSharedPtr<FJsonObject> Status;
				if (!PollTerminal(TEXT("HLOD execute"), Status)) return false;
				if (!Status.IsValid()) return FinishWithCleanup();
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("editor.session"),
					FString::Printf(
						TEXT("{\"family_id\":\"world_context\",")
						TEXT("\"subject_ref\":\"world:%s\",")
						TEXT("\"policy_id\":\"policy:world-context:set-current/v1\",")
						TEXT("\"parameters\":{\"save_existing_map\":false}}"),
						*OriginalWorld));
				if (!TryOperationRef(Started, OperationRef, Status))
				{
					Test->AddError(TEXT("restore-world policy was not accepted: ")
						+ Started.GetErrorJson());
					return FinishWithCleanup();
				}
				Stage = 10;
				return false;
			}
			TSharedPtr<FJsonObject> Status;
			if (!PollTerminal(TEXT("restore-world"), Status)) return false;
			RestoreAndCleanup();
			return true;
		}

	private:
		bool PollTerminal(
			const FString& Label,
			TSharedPtr<FJsonObject>& OutStatus,
			const bool bAllowCancelled = false)
		{
			const FUeAgentCommandResult Result = SubmitPublic(
				Registry, Dispatch, TEXT("operation.status"),
				TEXT("{\"operation_ref\":") + OperationRef + TEXT("}"));
			FString IgnoredRef;
			if (!TryOperationRef(Result, IgnoredRef, OutStatus))
			{
				Test->AddError(Label + TEXT(" status failed: ") + Result.GetErrorJson());
				OutStatus.Reset();
				return true;
			}
			if (!OutStatus->GetBoolField(TEXT("terminal")))
			{
				if (!TryAdvance(OutStatus))
				{
					OutStatus.Reset();
					return true;
				}
				if (!OutStatus->GetBoolField(TEXT("terminal"))) return false;
			}
			FString ErrorCode;
			OutStatus->TryGetStringField(TEXT("error_code"), ErrorCode);
			if (!ErrorCode.IsEmpty() && !bAllowCancelled)
			{
				const TSharedPtr<FJsonObject>* LatestSignal = nullptr;
				const TSharedPtr<FJsonObject>* SignalPayload = nullptr;
					FString ReportRef;
					FString ReportTail;
				if (OutStatus->TryGetObjectField(TEXT("latest_signal"), LatestSignal)
					&& LatestSignal && LatestSignal->IsValid()
					&& (*LatestSignal)->TryGetObjectField(
						TEXT("payload"), SignalPayload)
					&& SignalPayload && SignalPayload->IsValid()
					&& (*SignalPayload)->TryGetStringField(
						TEXT("report_ref"), ReportRef)
					&& ReportRef.RemoveFromStart(TEXT("file:")))
				{
					FString ReportText;
					FFileHelper::LoadFileToString(ReportText, *ReportRef);
					const FString DebugCopy = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectDir()
							/ TEXT("docs/plan/20260806_UAI_REQUIREMENT_DESIGN_REALIGNMENT")
							/ TEXT(".work-cache/t59-hlod-execute-failure.log"));
					FFileHelper::SaveStringToFile(
						ReportText, *DebugCopy,
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
					ReportTail = ReportText.Right(12000);
				}
				Test->AddError(Label + TEXT(" terminated with ") + ErrorCode
					+ TEXT(": ") + CanonicalJson(OutStatus.ToSharedRef())
					+ TEXT(" report_tail: ") + ReportTail);
				OutStatus.Reset();
			}
			return true;
		}

		bool TryAdvance(TSharedPtr<FJsonObject>& InOutStatus)
		{
			if (!InOutStatus.IsValid()) return false;
			const int64 Generation = InOutStatus->GetIntegerField(TEXT("generation"));
			const FUeAgentCommandResult Advanced = SubmitPublic(
				Registry, Dispatch, TEXT("operation.resume"),
				TEXT("{\"expected_generation\":") + LexToString(Generation)
					+ TEXT(",\"operation_ref\":") + OperationRef + TEXT("}"));
			FString IgnoredRef;
			TSharedPtr<FJsonObject> AdvancedStatus;
			if (!TryOperationRef(Advanced, IgnoredRef, AdvancedStatus))
			{
				Test->AddError(TEXT("operation resume failed: ")
					+ Advanced.GetErrorJson());
				return false;
			}
			InOutStatus = MoveTemp(AdvancedStatus);
			return true;
		}

		bool FinishWithCleanup()
		{
			RestoreAndCleanup();
			return true;
		}

		void RestoreAndCleanup()
		{
			if (!OriginalWorld.IsEmpty() && GEditor)
			{
				UWorld* Current = GEditor->GetEditorWorldContext().World();
				if (!Current || !Current->GetPackage()
					|| Current->GetPackage()->GetName() != OriginalWorld)
				{
					const FString OriginalFile = FPackageName::LongPackageNameToFilename(
						OriginalWorld, FPackageName::GetMapPackageExtension());
					UEditorLoadingAndSavingUtils::LoadMap(OriginalFile);
				}
			}
			if (UPackage* Package = FindPackage(nullptr, *TemporaryWorld))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				UPackageTools::UnloadPackages({Package}, Error, true);
			}
			CollectGarbage(RF_NoFlags, true);
			if (!ResaveActorPackageName.IsEmpty())
			{
				const FString ActorFile = FPackageName::LongPackageNameToFilename(
					ResaveActorPackageName,
					FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*ActorFile))
				{
					IFileManager::Get().Delete(*ActorFile, false, true);
				}
				Test->TestFalse(TEXT("temporary external Actor fixture is removed"),
					IFileManager::Get().FileExists(*ActorFile));
			}
			const FString MapFile = FPackageName::LongPackageNameToFilename(
				TemporaryWorld, FPackageName::GetMapPackageExtension());
			if (IFileManager::Get().FileExists(*MapFile))
			{
				IFileManager::Get().Delete(*MapFile, false, true);
			}
			Test->TestFalse(TEXT("temporary World lifecycle fixture is removed"),
				IFileManager::Get().FileExists(*MapFile));
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString OriginalWorld;
		FString TemporaryWorld;
		FString ResaveActorGuid;
		FString ResaveActorPackageName;
		FString OperationRef;
		double StartedAt = 0.0;
		int32 Stage = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T59WorldCommandMigrationTest,
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T59WorldCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T59;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T59 owns exactly eleven domain commands"), ExpectedCommands().Num(), 11);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T59 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("world-command-owner")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T59WorldDomainCapabilityTest,
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.DomainCapability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T59WorldDomainCapabilityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainCapability;
	(void)Parameters;
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	FString Error;
	if (!TestTrue(TEXT("the process domain capabilities freeze"),
		TryFreezeProcessDomainCapabilities(Frozen, Error))
		|| !Frozen.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* Domain =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("world_commands");
			});
	if (!TestNotNull(TEXT("World commands own one typed contribution"), Domain))
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*Domain)->GetDeclaration();
	TestEqual(TEXT("the World migration owner is exact"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T59-WORLD-COMMAND-MIGRATION")));
	TestEqual(TEXT("all World commands use request-scoped operations"),
		Declaration.Operation.Operations.Num(), 11);
	TestTrue(TEXT("HLOD HJSON is explicitly supported"),
		Declaration.Hjson.Support == EDomainCapabilitySupport::Supported);
	TestTrue(TEXT("HLOD HJSON has an exact route"),
		!Declaration.Hjson.Routes.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T59WorldPartitionQuerySuccessorTest,
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.WorldPartitionQuerySuccessors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T59WorldPartitionQuerySuccessorTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T59;
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process public command surface composes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandResult Partition = SubmitPublic(
		Registry, Dispatch, TEXT("world.partition"),
		TEXT("{\"include_loaded_regions\":true}"));
	TestTrue(TEXT("World Partition state is publicly queryable"), Partition.IsOk());
	if (!Partition.IsOk())
	{
		AddError(Partition.GetErrorJson());
	}
	else
	{
		TestTrue(TEXT("partition query reports whether World Partition exists"),
			Partition.GetPayloadJson().Contains(TEXT("partition_enabled")));
		TestTrue(TEXT("partition query reports editor streaming state"),
			Partition.GetPayloadJson().Contains(TEXT("streaming_enabled")));
		TestTrue(TEXT("partition query reports owned user-loaded regions"),
			Partition.GetPayloadJson().Contains(TEXT("user_loaded_region_count")));
	}

	const FUeAgentCommandResult Health = SubmitPublic(
		Registry, Dispatch, TEXT("world.partition_health"),
		TEXT("{\"enable_streaming\":true}"));
	TestTrue(TEXT("World Partition health is a public read-only query"), Health.IsOk());
	if (!Health.IsOk())
	{
		AddError(Health.GetErrorJson());
	}
	else
	{
		TestTrue(TEXT("health query reports an explicit applicability status"),
			Health.GetPayloadJson().Contains(TEXT("health_status")));
		TestTrue(TEXT("health query reports the exact error count"),
			Health.GetPayloadJson().Contains(TEXT("error_count")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T59WorldLifecyclePolicyProductTest,
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.WorldLifecyclePolicyProduct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T59WorldLifecyclePolicyProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T59;
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process public command surface composes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FWorldLifecyclePolicyLatentCommand(this, Registry, Dispatch));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T59WorldHlodHjsonProductTest,
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.HlodHjsonPublicProductRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T59WorldHlodHjsonProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T59;
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process public command surface composes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SourceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T59/Hlod_") + Suffix);
	const FString Document = HlodDocument();
	const TArray<FString> Kinds = {
		TEXT("inline"), TEXT("single_file"), TEXT("directory")};
	TArray<FString> Packages;
	for (const FString& Kind : Kinds)
	{
		const FString PackageName = TEXT("/Game/UAI/Tests/T59/HLOD_")
			+ Kind + TEXT("_") + Suffix;
		Packages.Add(PackageName);
		CleanupPackage(PackageName);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), Kind);
		if (Kind == TEXT("inline"))
		{
			Source->SetStringField(TEXT("document"), Document);
		}
		else if (Kind == TEXT("single_file"))
		{
			const FString Path = FPaths::Combine(
				SourceRoot, Kind, TEXT("asset.hjson"));
			TestTrue(TEXT("single-file HLOD fixture persists"),
				WriteText(Path, Document));
			Source->SetStringField(TEXT("path"), Path);
		}
		else
		{
			const FString Path = FPaths::Combine(SourceRoot, Kind);
			TestTrue(TEXT("directory HLOD workspace persists"),
				WriteDirectoryWorkspace(Path, PackageName, Document));
			Source->SetStringField(TEXT("path"), Path);
		}

		const FUeAgentCommandResult Imported = SubmitPublic(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			ImportParams(PackageName, Source));
		TestTrue(*FString::Printf(
			TEXT("%s creates HLOD through the unique public HJSON ingress"), *Kind),
			Imported.IsOk());
		if (!Imported.IsOk())
		{
			AddError(Imported.GetErrorJson());
			continue;
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		TestTrue(*FString::Printf(TEXT("%s HLOD package persists"), *Kind),
			IFileManager::Get().FileExists(*Filename));
		UHLODLayer* Layer = LoadObject<UHLODLayer>(
			nullptr, *(PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName)));
		if (TestNotNull(*FString::Printf(
			TEXT("%s HLOD independently reloads"), *Kind), Layer))
		{
			TestEqual(TEXT("HLOD layer type persists"),
				Layer->GetLayerType(), EHLODLayerType::Instancing);
			TestTrue(TEXT("HLOD spatial policy persists"), Layer->IsSpatiallyLoaded());
			TestEqual(TEXT("HLOD cell size persists"), Layer->GetCellSize(), 512);
			TestEqual(TEXT("HLOD loading range persists"),
				Layer->GetLoadingRange(), 2048.0);
		}

		const FUeAgentCommandResult Queried = SubmitPublic(
			Registry, Dispatch, TEXT("content_graph.summary"),
			FString::Printf(TEXT("{\"target\":\"%s\"}"), *PackageName));
		TestTrue(*FString::Printf(
			TEXT("%s HLOD has public independent readback"), *Kind), Queried.IsOk());
		if (!Queried.IsOk()) AddError(Queried.GetErrorJson());
	}
	for (const FString& PackageName : Packages)
	{
		TestTrue(TEXT("HLOD product fixture cleanup succeeds"),
			CleanupPackage(PackageName));
	}
	IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
	return !HasAnyErrors();
}
