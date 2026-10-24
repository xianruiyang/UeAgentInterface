#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
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
#include "TestSupport/UeAgentFixtureDirectoryLifecycle.h"
#include "UObject/GarbageCollection.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
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
			TEXT("world.wall.create_with_opening"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedDormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("world.actor_hlod_layer.update"),
			TEXT("world.actor_streaming_policy.update"),
			TEXT("world.data_layer_instance.create"),
			TEXT("world.data_layer_relation.update"),
			TEXT("world.data_layer_state.update"),
			TEXT("world.level_game_mode.update"),
			TEXT("world.streaming_relation.create"),
			TEXT("world.streaming_relation.state.update"),
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

	bool CleanupPackage(
		const FString& PackageName,
		const FString& PackageExtension = FPackageName::GetAssetPackageExtension())
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, PackageExtension);
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			FlushAsyncLoading();
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *PackageName))
			{
				return false;
			}
		}
		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Filename});
		TArray<FAssetData> RemainingAssets;
		FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
			FName(*PackageName), RemainingAssets, false);
		return !IFileManager::Get().FileExists(*Filename)
			&& !FindPackage(nullptr, *PackageName)
			&& RemainingAssets.IsEmpty();
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
					TEXT("\"unit\":\"centimeter\"},")
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
				if (!TryAddOwnedExternalPackage(
						ResaveActor->GetExternalPackage()->GetName()))
				{
					return FinishWithCleanup();
				}
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

		bool TryGetOwnedExternalRoots(TArray<FString>& OutRoots) const
		{
			OutRoots.Reset();
			FString RelativeWorldPath = TemporaryWorld;
			if (!RelativeWorldPath.RemoveFromStart(TEXT("/Game/"))
				|| RelativeWorldPath.IsEmpty())
			{
				return false;
			}
			OutRoots = {
				FString(TEXT("/Game/__ExternalActors__/")) + RelativeWorldPath,
				FString(TEXT("/Game/__ExternalObjects__/")) + RelativeWorldPath,
			};
			return true;
		}

		bool TryAddOwnedExternalPackage(const FString& PackageName)
		{
			TArray<FString> ExternalRoots;
			if (!TryGetOwnedExternalRoots(ExternalRoots))
			{
				Test->AddError(TEXT("temporary World external root is invalid: ")
					+ TemporaryWorld);
				return false;
			}
			for (const FString& ExternalRoot : ExternalRoots)
			{
				if (PackageName.StartsWith(ExternalRoot + TEXT("/")))
				{
					ExternalPackageNames.Add(PackageName);
					return true;
				}
			}
			Test->AddError(TEXT("external package is outside the temporary World owner: ")
				+ PackageName);
			return false;
		}

		bool TryCollectOwnedExternalPackagesOnDisk()
		{
			TArray<FString> ExternalRoots;
			if (!TryGetOwnedExternalRoots(ExternalRoots))
			{
				Test->AddError(TEXT("temporary World external root is invalid: ")
					+ TemporaryWorld);
				return false;
			}
			for (const FString& ExternalRoot : ExternalRoots)
			{
				FString Directory;
				if (!FPackageName::TryConvertLongPackageNameToFilename(
						ExternalRoot, Directory))
				{
					Test->AddError(TEXT("temporary World external path is unavailable: ")
						+ ExternalRoot);
					return false;
				}
				TArray<FString> Files;
				IFileManager::Get().FindFilesRecursive(
					Files, *Directory, TEXT("*.uasset"), true, false, false);
				Files.Sort();
				for (const FString& Filename : Files)
				{
					FString PackageName;
					if (!FPackageName::TryConvertFilenameToLongPackageName(
							Filename, PackageName)
						|| !TryAddOwnedExternalPackage(PackageName))
					{
						return false;
					}
				}
			}
			return true;
		}

		bool TryRemoveOwnedExternalDirectories()
		{
			TArray<FString> ExternalRoots;
			if (!TryGetOwnedExternalRoots(ExternalRoots))
			{
				Test->AddError(TEXT("temporary World external root is invalid: ")
					+ TemporaryWorld);
				return false;
			}
			for (const FString& ExternalRoot : ExternalRoots)
			{
				TSet<FString> OwnedDirectories;
				for (const FString& ExternalPackageName : ExternalPackageNames)
				{
					if (!ExternalPackageName.StartsWith(
						ExternalRoot + TEXT("/"), ESearchCase::CaseSensitive))
					{
						continue;
					}
					FString DirectoryPackage =
						FPackageName::GetLongPackagePath(ExternalPackageName);
					while (DirectoryPackage.StartsWith(
						ExternalRoot + TEXT("/"), ESearchCase::CaseSensitive))
					{
						OwnedDirectories.Add(DirectoryPackage);
						DirectoryPackage =
							FPackageName::GetLongPackagePath(DirectoryPackage);
					}
				}
				TArray<FString> OrderedDirectories = OwnedDirectories.Array();
				OrderedDirectories.Sort(
					[](const FString& Left, const FString& Right)
					{
						return Left.Len() == Right.Len()
							? Left < Right
							: Left.Len() > Right.Len();
					});
				for (const FString& DirectoryPackage : OrderedDirectories)
				{
					FString Error;
					if (!UE::UAI::TestSupport::
						TryRemoveEmptyFixturePackageDirectory(
							ExternalRoot, DirectoryPackage, Error))
					{
						Test->AddError(
							TEXT("temporary World external directory cleanup failed: ")
							+ Error);
						return false;
					}
				}

				const FString AllowedOwnerRoot =
					FPackageName::GetLongPackagePath(ExternalRoot);
				FString Error;
				if (!UE::UAI::TestSupport::
					TryRemoveEmptyFixturePackageDirectory(
						AllowedOwnerRoot, ExternalRoot, Error))
				{
					Test->AddError(
						TEXT("temporary World external root cleanup failed: ")
						+ Error);
					return false;
				}
			}
			return true;
		}

		void RestoreAndCleanup()
		{
			if (OriginalWorld.IsEmpty() || !GEditor)
			{
				Test->AddError(TEXT("original editor World is unavailable for safe cleanup"));
				return;
			}
			UWorld* Current = GEditor->GetEditorWorldContext().World();
			if (Current && Current->GetPackage()
				&& Current->GetPackage()->GetName() == TemporaryWorld)
			{
				for (TActorIterator<AActor> It(Current); It; ++It)
				{
					if (UPackage* ExternalPackage = It->GetExternalPackage())
					{
						if (!TryAddOwnedExternalPackage(ExternalPackage->GetName()))
						{
							return;
						}
					}
				}
			}
			if (!TryCollectOwnedExternalPackagesOnDisk())
			{
				return;
			}
			if (!Current || !Current->GetPackage()
				|| Current->GetPackage()->GetName() != OriginalWorld)
			{
				const FString OriginalFile = FPackageName::LongPackageNameToFilename(
					OriginalWorld, FPackageName::GetMapPackageExtension());
				if (!UEditorLoadingAndSavingUtils::LoadMap(OriginalFile))
				{
					Test->AddError(TEXT("original editor World failed to load: ")
						+ OriginalWorld);
					return;
				}
			}
			UWorld* RestoredWorld = GEditor->GetEditorWorldContext().World();
			if (!RestoredWorld || !RestoredWorld->GetPackage()
				|| RestoredWorld->GetPackage()->GetName() != OriginalWorld)
			{
				Test->AddError(TEXT("original editor World was not restored: ")
					+ OriginalWorld);
				return;
			}
			if (UPackage* Package = FindPackage(nullptr, *TemporaryWorld))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Package}, Error, true))
				{
					Test->AddError(TEXT("temporary World package failed to unload: ")
						+ Error.ToString());
					return;
				}
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *TemporaryWorld))
			{
				Test->AddError(TEXT("temporary World package remains loaded: ")
					+ TemporaryWorld);
				return;
			}
			for (const FString& ExternalPackageName : ExternalPackageNames)
			{
				Test->TestTrue(
					*FString::Printf(
						TEXT("temporary external package is removed: %s"),
						*ExternalPackageName),
					CleanupPackage(ExternalPackageName));
			}
			Test->TestTrue(
				TEXT("temporary World lifecycle fixture is removed"),
				CleanupPackage(
					TemporaryWorld, FPackageName::GetMapPackageExtension()));
			Test->TestTrue(
				TEXT("temporary instanced HLOD layer fixture is removed"),
				CleanupPackage(TemporaryWorld + TEXT("_HLODLayer_Instanced")));
			Test->TestTrue(
				TEXT("temporary merged HLOD layer fixture is removed"),
				CleanupPackage(TemporaryWorld + TEXT("_HLODLayer_Merged")));
			TryRemoveOwnedExternalDirectories();
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString OriginalWorld;
		FString TemporaryWorld;
		FString ResaveActorGuid;
		TSet<FString> ExternalPackageNames;
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
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is enabled"), *CommandId),
				Contract->GetAvailability(),
				EUeAgentCommandAvailability::Enabled);
			TestEqual(*FString::Printf(TEXT("%s has one T59 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("world-command-owner")));
			const auto& Fields = Contract->GetModelResultFields();
			TestEqual(TEXT("wall create has two model fields"), Fields.Num(), 2);
			if (Fields.Num() == 2)
			{
				TestEqual(TEXT("wall create status source"),
					Fields[0].SourcePath,
					TArray<FString>{TEXT("projection"), TEXT("payload"),
						TEXT("status")});
				TestEqual(TEXT("wall create outcome source"),
					Fields[1].SourcePath,
					TArray<FString>{TEXT("projection"), TEXT("payload"),
						TEXT("effect_outcome")});
			}
		}
	}
	for (const FString& CommandId : ExpectedDormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered as dormant"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is dormant"), *CommandId),
				Contract->GetAvailability(),
				EUeAgentCommandAvailability::Dormant);
			if (CommandId.EndsWith(TEXT(".create")))
			{
				TestEqual(TEXT("dormant create keeps compact model projection"),
					Contract->GetModelResultFields().Num(), 2);
			}
		}
	}
	const auto* ActorCreate = Registry->FindExact(TEXT("world.actor.create"));
	if (TestNotNull(TEXT("world.actor.create remains compatibility-executable"),
		ActorCreate))
	{
		TestEqual(TEXT("world.actor.create is centrally dormant"),
			ActorCreate->GetAvailability(),
			CommandContract::EUeAgentCommandAvailability::Dormant);
		TestEqual(TEXT("world.actor.create keeps the internal World owner"),
			ActorCreate->GetPublicOwner(), FString(TEXT("world-command-owner")));
		TestEqual(TEXT("world.actor.create keeps compact model projection"),
			ActorCreate->GetModelResultFields().Num(), 2);
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
	TestTrue(TEXT("World exposes the HLOD Reflection workspace provider"),
		Declaration.Hjson.Support == EDomainCapabilitySupport::Supported);
	TestEqual(TEXT("World owns one HLOD workspace provider"),
		Declaration.Hjson.Providers.Num(), 1);
	TestEqual(TEXT("World owns one HLOD semantic workspace route"),
		Declaration.Hjson.Routes.Num(), 1);
	if (Declaration.Hjson.Routes.Num() == 1)
	{
		TestEqual(TEXT("HLOD route targets the native asset class"),
			Declaration.Hjson.Routes[0].RootTypeIdentity,
			FString(TEXT("/Script/Engine.HLODLayer")));
	}
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
	"UAI.StrictV2.T59.WORLD.COMMAND.MIGRATION.HlodUsesReflectionAuthoring",
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

	const UeAgentReflectionAuthoring::FProfile* Profile =
		UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("hlod_layer"));
	if (!TestNotNull(TEXT("HLOD registers the common Reflection authoring profile"),
		Profile)) return false;
	TestEqual(TEXT("HLOD Reflection profile owns the native asset class"),
		Profile->AssetClassPath, FString(TEXT("/Script/Engine.HLODLayer")));
	TestTrue(TEXT("common semantic export remains on the public command surface"),
		Registry->FindExact(TEXT("content_graph.export_workspace")) != nullptr);
	TestTrue(TEXT("common semantic import remains on the public command surface"),
		Registry->FindExact(TEXT("content_graph.import_workspace")) != nullptr);
	TestTrue(TEXT("independent World HLOD build command remains registered"),
		Registry->FindExact(TEXT("job.compile_build")) != nullptr);
	return !HasAnyErrors();
}
