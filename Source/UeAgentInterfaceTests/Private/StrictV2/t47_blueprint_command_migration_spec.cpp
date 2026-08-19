#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Components/AudioComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "PreviewScene.h"
#include "Slate/SceneViewport.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "Widgets/SViewport.h"

namespace UE::UAI::Tests::StrictV2::T47
{
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	FString CanonicalJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		FString Error;
		return CommandContract::Json::TryCanonicalize(Object, Result, Error)
			? Result : FString(TEXT("{}"));
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t47-product"), TEXT("req-t47-product"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t47_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t47-product-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t47-product\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(),
			*CanonicalJson(Params));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t47-product"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t47-product"),
				{TEXT("uai:query"), TEXT("uai:operate")},
				TEXT("corr-t47-product")),
			MakeShared<FNeverCancelled>());
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
			&& CommandContract::Json::TryParseObject(
				Result.GetPayloadJson(), OutPayload, Error)
			&& OutPayload.IsValid()
			&& OutPayload->TryGetObjectField(TEXT("operation_ref"), Ref)
			&& Ref && Ref->IsValid()
			&& CommandContract::Json::TryCanonicalize(
				(*Ref).ToSharedRef(), OutRefJson, Error);
	}

	TSharedRef<FJsonObject> OperationParams(
		const FString& OperationRef,
		const TOptional<int64> ExpectedGeneration = {})
	{
		TSharedPtr<FJsonObject> Ref;
		FString Error;
		CommandContract::Json::TryParseObject(OperationRef, Ref, Error);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("operation_ref"), Ref.ToSharedRef());
		if (ExpectedGeneration.IsSet())
		{
			Params->SetNumberField(
				TEXT("expected_generation"), ExpectedGeneration.GetValue());
		}
		return Params;
	}

	TSharedRef<FJsonObject> BlueprintViewportParams(
		const FString& BlueprintObjectPath,
		const FString& ViewportIdentity,
		const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(
			TEXT("asset_editor_identity"), BlueprintObjectPath);
		Context->SetStringField(
			TEXT("viewport_identity"), ViewportIdentity);
		Context->SetStringField(TEXT("viewport_scope"), TEXT("asset_editor"));
		Context->SetStringField(TEXT("viewport_role"), TEXT("components"));
		Context->SetStringField(TEXT("world_type"), TEXT("editor"));
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), ViewportIdentity);
		Params->SetObjectField(TEXT("input"), Input);
		Params->SetObjectField(TEXT("context"), Context);
		return Params;
	}

	TSharedRef<FJsonObject> TemporalPoint(const FString& Tick)
	{
		const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetStringField(TEXT("clock_id"), TEXT("editor"));
		Point->SetStringField(TEXT("tick"), Tick);
		Point->SetStringField(TEXT("rate_numerator"), TEXT("60"));
		Point->SetStringField(TEXT("rate_denominator"), TEXT("1"));
		Point->SetStringField(TEXT("state"), TEXT("captured"));
		return Point;
	}

	TSharedRef<FJsonObject> BlueprintPreviewCameraTransform(
		const FString& ViewportIdentity,
		const FVector& Location,
		const FRotator& Rotation)
	{
		const FQuat Quaternion = Rotation.Quaternion();
		const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetStringField(TEXT("frame_id"), TEXT("unreal.world"));
		Frame->SetStringField(
			TEXT("basis_frame_id"), TEXT("blueprint.preview.world"));
		Frame->SetNumberField(TEXT("dimension"), 3);
		Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Frame->SetStringField(
			TEXT("basis_evidence_id"), TEXT("t47-blueprint-preview-camera"));
		Frame->SetObjectField(TEXT("observed_at"), TemporalPoint(TEXT("1")));
		TArray<TSharedPtr<FJsonValue>> Components;
		for (const double Value : {
			Location.X, Location.Y, Location.Z,
			Quaternion.X, Quaternion.Y, Quaternion.Z, Quaternion.W,
			1.0, 1.0, 1.0})
		{
			Components.Add(MakeShared<FJsonValueString>(
				FString::SanitizeFloat(Value, 9)));
		}
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetStringField(
			TEXT("subject"), ViewportIdentity + TEXT(".camera"));
		Transform->SetStringField(TEXT("quantity"), TEXT("transform"));
		Transform->SetObjectField(TEXT("frame"), Frame);
		Transform->SetArrayField(TEXT("components"), MoveTemp(Components));
		Transform->SetArrayField(
			TEXT("conversion_evidence_ids"),
			{MakeShared<FJsonValueString>(TEXT("t47-blueprint-preview-identity"))});
		return Transform;
	}

	class FHeadlessBlueprintSceneViewport final : public FSceneViewport
	{
	public:
		using FSceneViewport::FSceneViewport;

		void SetHeadlessViewportSize(const uint32 Width, const uint32 Height)
		{
			UpdateViewportRHI(
				false,
				Width,
				Height,
				EWindowMode::Windowed,
				PF_Unknown);
		}
	};

	class FBlueprintCompileLifecycleCommand final
		: public IAutomationLatentCommand
	{
	public:
		FBlueprintCompileLifecycleCommand(
			FAutomationTestBase* InTest,
			TSharedPtr<const FUeAgentImmutableCommandRegistry> InRegistry,
			TSharedPtr<const FUeAgentExactCommandDispatch> InDispatch,
			FString InPackageName,
			FString InObjectPath)
			: Test(InTest)
			, Registry(MoveTemp(InRegistry))
			, Dispatch(MoveTemp(InDispatch))
			, PackageName(MoveTemp(InPackageName))
			, ObjectPath(MoveTemp(InObjectPath))
			, StartedAt(FPlatformTime::Seconds())
		{
		}

		bool Update() override
		{
			if (!Test || !Registry.IsValid() || !Dispatch.IsValid())
			{
				Cleanup();
				return true;
			}
			if (FPlatformTime::Seconds() - StartedAt > 90.0)
			{
				Test->AddError(TEXT("T47 Blueprint compile lifecycle timed out"));
				Cleanup();
				return true;
			}
			if (Stage == 0)
			{
				const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
				Policy->SetStringField(TEXT("asset_path"), ObjectPath);
				Policy->SetBoolField(TEXT("include_messages"), true);
				Policy->SetNumberField(TEXT("max_messages"), 1);
				Policy->SetBoolField(TEXT("save_after_compile"), true);
				Policy->SetStringField(
					TEXT("severity_filter"), TEXT("warning_or_error"));
				const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("family_id"), TEXT("asset_compile"));
				Params->SetStringField(
					TEXT("subject_ref"), TEXT("asset:") + ObjectPath);
				Params->SetStringField(
					TEXT("policy_id"),
					TEXT("policy:asset-compile:blueprint/v1"));
				Params->SetObjectField(TEXT("parameters"), Policy);
				TSharedPtr<FJsonObject> Payload;
				const FUeAgentCommandResult Started = SubmitPublic(
					Registry, Dispatch, TEXT("job.compile_build"), Params);
				if (!TryOperationRef(Started, OperationRef, Payload))
				{
					Test->AddError(TEXT("Blueprint compile policy was not accepted: ")
						+ Started.GetErrorJson());
					Cleanup();
					return true;
				}
				Stage = 1;
				return false;
			}

			const FUeAgentCommandResult StatusResult = SubmitPublic(
				Registry, Dispatch, TEXT("operation.status"),
				OperationParams(OperationRef));
			TSharedPtr<FJsonObject> Status;
			FString IgnoredRef;
			if (!TryOperationRef(StatusResult, IgnoredRef, Status))
			{
				Test->AddError(TEXT("Blueprint compile status failed: ")
					+ StatusResult.GetErrorJson());
				Cleanup();
				return true;
			}
			if (!Status->GetBoolField(TEXT("terminal")))
			{
				const int64 Generation =
					Status->GetIntegerField(TEXT("generation"));
				const FUeAgentCommandResult Advanced = SubmitPublic(
					Registry, Dispatch, TEXT("operation.resume"),
					OperationParams(OperationRef, Generation));
				if (!Advanced.IsOk())
				{
					Test->AddError(TEXT("Blueprint compile resume failed: ")
						+ Advanced.GetErrorJson());
					Cleanup();
					return true;
				}
				return false;
			}

			FString ErrorCode;
			Status->TryGetStringField(TEXT("error_code"), ErrorCode);
			if (!ErrorCode.IsEmpty())
			{
				Test->AddError(TEXT("Blueprint compile terminated with ")
					+ ErrorCode + TEXT(": ") + CanonicalJson(Status.ToSharedRef()));
				Cleanup();
				return true;
			}
			const TSharedPtr<FJsonObject>* Signal = nullptr;
			const TSharedPtr<FJsonObject>* SignalPayload = nullptr;
			FString ReportRef;
			if (!Status->TryGetObjectField(TEXT("latest_signal"), Signal)
				|| !Signal || !Signal->IsValid()
				|| !(*Signal)->TryGetObjectField(TEXT("payload"), SignalPayload)
				|| !SignalPayload || !SignalPayload->IsValid()
				|| !(*SignalPayload)->TryGetStringField(
					TEXT("report_ref"), ReportRef)
				|| !ReportRef.RemoveFromStart(TEXT("file:")))
			{
				Test->AddError(TEXT("Blueprint compile terminal state lacks report_ref"));
				Cleanup();
				return true;
			}
			FString ReportText;
			TSharedPtr<FJsonObject> Report;
			FString ParseError;
			const bool bReportValid = FFileHelper::LoadFileToString(
				ReportText, *ReportRef)
				&& CommandContract::Json::TryParseObject(
					ReportText, Report, ParseError)
				&& Report.IsValid();
			Test->TestTrue(TEXT("terminal compile report is independently readable"),
				bReportValid);
			if (bReportValid)
			{
				const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
				Test->TestEqual(TEXT("compile report schema is exact"),
					Report->GetStringField(TEXT("schema")),
					FString(TEXT("uai.blueprint_compile.report.v1")));
				Test->TestTrue(TEXT("compile report confirms save readback"),
					Report->GetBoolField(TEXT("saved"))
						&& Report->GetBoolField(TEXT("package_file_exists"))
						&& !Report->GetBoolField(
							TEXT("package_dirty_after_compile")));
				Test->TestEqual(TEXT("severity filter is canonical"),
					Report->GetStringField(TEXT("severity_filter")),
					FString(TEXT("warning_or_error")));
				Test->TestTrue(TEXT("message projection is bounded"),
					Report->TryGetArrayField(TEXT("messages"), Messages)
						&& Messages && Messages->Num() <= 1);
			}

			const FUeAgentCommandResult Receipt = SubmitPublic(
				Registry, Dispatch, TEXT("operation.receipt"),
				OperationParams(OperationRef));
			TSharedPtr<FJsonObject> ReceiptPayload;
			Test->TestTrue(TEXT("terminal compile receipt is available"),
				Receipt.IsOk()
					&& CommandContract::Json::TryParseObject(
						Receipt.GetPayloadJson(), ReceiptPayload, ParseError)
					&& ReceiptPayload.IsValid()
					&& ReceiptPayload->GetStringField(TEXT("schema"))
						== TEXT("uai.long_running.receipt.v1"));

			const FUeAgentCommandResult Released = SubmitPublic(
				Registry, Dispatch, TEXT("operation.release"),
				OperationParams(OperationRef));
			Test->TestTrue(TEXT("compile lifecycle releases"), Released.IsOk());
			VerifyReload();
			Cleanup();
			return true;
		}

	private:
		void VerifyReload()
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				UPackageTools::UnloadPackages({Package}, Error, true);
				CollectGarbage(RF_NoFlags, true);
			}
			UBlueprint* const Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
			Test->TestNotNull(
				TEXT("saved Blueprint independently reloads"), Reloaded);
			if (Reloaded)
			{
				Test->TestTrue(TEXT("reloaded Blueprint retains compiled status"),
					Reloaded->Status == BS_UpToDate
						|| Reloaded->Status == BS_UpToDateWithWarnings);
			}
		}

		void Cleanup()
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				UPackageTools::UnloadPackages({Package}, Error, true);
				CollectGarbage(RF_NoFlags, true);
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*Filename))
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
			Test->TestFalse(TEXT("owned compile fixture is removed"),
				IFileManager::Get().FileExists(*Filename));
		}

		FAutomationTestBase* Test = nullptr;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString PackageName;
		FString ObjectPath;
		FString OperationRef;
		double StartedAt = 0.0;
		int32 Stage = 0;
	};

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("blueprint.asset.create"),
			TEXT("blueprint.component.events.discover"),
		};
		return Commands;
	}

	const TArray<FString>& HistoricalCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("blueprint_compile"),
			TEXT("blueprint_get_compile_log"),
			TEXT("blueprint_graph_get_view"),
			TEXT("blueprint_graph_set_view"),
			TEXT("blueprint_list_component_events"),
			TEXT("blueprint_viewport_get_camera"),
			TEXT("blueprint_viewport_set_camera"),
		};
		return Commands;
	}

	const TArray<FString>& ConsolidatedSuccessorCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("job.compile_build"),
			TEXT("operation.status"),
			TEXT("operation.receipt"),
			TEXT("operation.release"),
			TEXT("graph.view.read"),
			TEXT("graph.view.update"),
			TEXT("viewport.state.read"),
			TEXT("viewport.state.update"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T47BlueprintCommandMigrationTest,
	"UAI.StrictV2.T47.BLUEPRINT.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T47BlueprintCommandMigrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T47;
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
	TestEqual(TEXT("T47 owns exactly two non-HJSON commands"), ExpectedCommands().Num(), 2);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T47 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("blueprint-command-owner")));
		}
	}
	for (const FString& CommandId : ConsolidatedSuccessorCommands())
	{
		TestNotNull(
			*FString::Printf(TEXT("%s remains an active consolidated successor"),
				*CommandId),
			Registry->FindExact(CommandId));
	}
	const auto* EventContract = Registry->FindExact(
		TEXT("blueprint.component.events.discover"));
	if (EventContract)
	{
		const TSet<FString> RequiredParameterNames = {
			TEXT("asset_path"),
			TEXT("compile_before_query"),
			TEXT("component_name"),
			TEXT("include_hidden"),
			TEXT("limit"),
			TEXT("only_unbound"),
			TEXT("query"),
		};
		TSet<FString> ActualParameterNames;
		for (const auto& Parameter : EventContract->GetInputFields())
		{
			ActualParameterNames.Add(Parameter.Name);
		}
		TestTrue(TEXT("event discovery consumes the complete frozen input set"),
			ActualParameterNames.Includes(RequiredParameterNames));
	}
	for (const FString& HistoricalId : HistoricalCommands())
	{
		TestNull(
			*FString::Printf(TEXT("%s has no active public contract"),
				*HistoricalId),
			Registry->FindExact(HistoricalId));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T47BlueprintComponentEventProductTest,
	"UAI.StrictV2.T47.BLUEPRINT.COMMAND.MIGRATION.PublicComponentEventProduct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T47BlueprintComponentEventProductTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T47;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the public product composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FString PackageName = FString::Printf(
		TEXT("/Temp/UAI/T47/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* const Package = CreatePackage(*PackageName);
	UBlueprint* const Blueprint = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, TEXT("BP_T47Events"),
			BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None)
		: nullptr;
	USCS_Node* AudioNode = nullptr;
	if (Blueprint && Blueprint->SimpleConstructionScript)
	{
		AudioNode = Blueprint->SimpleConstructionScript->CreateNode(
			UAudioComponent::StaticClass(), TEXT("Audio"));
		Blueprint->SimpleConstructionScript->AddNode(AudioNode);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (!TestNotNull(TEXT("owned Blueprint fixture exists"), Blueprint)
		|| !TestNotNull(TEXT("owned assignable-event component exists"), AudioNode))
	{
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		return false;
	}

	const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
	Query->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Query->SetBoolField(TEXT("compile_before_query"), true);
	Query->SetStringField(TEXT("component_name"), AudioNode->GetVariableName().ToString());
	Query->SetBoolField(TEXT("include_hidden"), false);
	Query->SetNumberField(TEXT("limit"), 25);
	Query->SetBoolField(TEXT("only_unbound"), true);
	Query->SetStringField(TEXT("query"), TEXT("finished"));
	const FUeAgentCommandResult Result = SubmitPublic(
		Registry, Dispatch,
		TEXT("blueprint.component.events.discover"), Query);
	TestTrue(TEXT("public component-event query succeeds"), Result.IsOk());
	TestTrue(TEXT("public query uses real assignable delegate reflection"),
		Result.GetPayloadJson().Contains(TEXT("OnAudioFinished"))
			&& Result.GetPayloadJson().Contains(TEXT("returned_event_count"))
			&& Result.GetPayloadJson().Contains(TEXT("can_author_component_bound_event"))
			&& Result.GetPayloadJson().Contains(AudioNode->GetVariableName().ToString()));

	const TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Missing->SetStringField(TEXT("component_name"), TEXT("MissingComponent"));
	const FUeAgentCommandResult MissingResult = SubmitPublic(
		Registry, Dispatch,
		TEXT("blueprint.component.events.discover"), Missing);
	TestFalse(TEXT("missing component fails explicitly"), MissingResult.IsOk());
	TestTrue(TEXT("missing component preserves stable failure semantics"),
		MissingResult.GetErrorJson().Contains(
			TEXT("blueprint_component_event_component_not_found")));

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T47BlueprintCompileProductTest,
	"UAI.StrictV2.T47.BLUEPRINT.COMMAND.MIGRATION.PublicCompileLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T47BlueprintCompileProductTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T47;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the public product composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const FString PackageName = TEXT("/Game/UAI/Tests/T47/Compile_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const Package = CreatePackage(*PackageName);
	UBlueprint* const Blueprint = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, TEXT("BP_CompileFixture"),
			BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None)
		: nullptr;
	if (!TestNotNull(TEXT("owned compile Blueprint exists"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->MarkPackageDirty();
	if (!TestTrue(TEXT("owned compile Blueprint saves before lifecycle"),
		UEditorLoadingAndSavingUtils::SavePackages({Package}, false)))
	{
		Package->SetDirtyFlag(false);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FBlueprintCompileLifecycleCommand(
		this, Registry, Dispatch, PackageName, Blueprint->GetPathName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T47BlueprintPreviewViewportProductTest,
	"UAI.StrictV2.T47.BLUEPRINT.COMMAND.MIGRATION.PublicPreviewViewportCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T47BlueprintPreviewViewportProductTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T47;

	const FString PackageName = FString::Printf(
		TEXT("/Temp/UAI/T47/Preview_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* const Package = CreatePackage(*PackageName);
	UBlueprint* const Blueprint = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, TEXT("BP_PreviewFixture"),
			BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(), NAME_None)
		: nullptr;
	if (!TestNotNull(TEXT("owned preview Blueprint exists"), Blueprint))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues());
	UWorld* const PreviewWorld = PreviewScene->GetWorld();
	TUniquePtr<FEditorViewportClient> Client =
		MakeUnique<FEditorViewportClient>(nullptr, PreviewScene.Get());
	ON_SCOPE_EXIT
	{
		if (Client)
		{
			Client->Viewport = nullptr;
		}
	};
	const TSharedRef<SViewport> ViewportWidget = SNew(SViewport);
	const TSharedRef<FHeadlessBlueprintSceneViewport> SceneViewport =
		MakeShared<FHeadlessBlueprintSceneViewport>(Client.Get(), ViewportWidget);
	ViewportWidget->SetViewportInterface(SceneViewport);
	SceneViewport->SetHeadlessViewportSize(960, 540);
	Client->Viewport = &SceneViewport.Get();
	Client->SetViewportType(LVT_Perspective);
	const FVector BaselineLocation(-420.0, 130.0, 210.0);
	const FRotator BaselineRotation(-12.0, 18.0, 0.0);
	const float BaselineFov = 86.0f;
	Client->SetViewLocation(BaselineLocation);
	Client->SetViewRotation(BaselineRotation);
	Client->ViewFOV = BaselineFov;
	Client->SetRealtime(true);
	AActor* const PreviewActor = PreviewWorld && Blueprint->GeneratedClass
		? PreviewWorld->SpawnActor<AActor>(
			Blueprint->GeneratedClass,
			FVector::ZeroVector,
			FRotator::ZeroRotator)
		: nullptr;
	if (!TestNotNull(TEXT("real Blueprint preview actor exists"), PreviewActor))
	{
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
		return false;
	}

	FString ViewportIdentity;
	FString Error;
	if (!TestTrue(TEXT("Blueprint Components preview session registers"),
		TryRegisterBlueprintComponentsViewport(
			*Blueprint, *Client, *SceneViewport, ViewportIdentity, Error)))
	{
		AddError(Error);
		PreviewActor->Destroy();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (Client && !ViewportIdentity.IsEmpty())
		{
			UnregisterBlueprintComponentsViewport(ViewportIdentity, *Client);
		}
	};

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	const bool bComposed = TryComposeProcessActiveCommandComposition(
		Registry, Dispatch, Error);
	TestTrue(TEXT("public product composition freezes"), bComposed);
	if (!bComposed || !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		UnregisterBlueprintComponentsViewport(ViewportIdentity, *Client);
		PreviewActor->Destroy();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
		return false;
	}

	auto Call = [&](const FString& CommandId,
		const TSharedRef<FJsonObject>& Input)
	{
		return SubmitPublic(
			Registry,
			Dispatch,
			CommandId,
			BlueprintViewportParams(
				Blueprint->GetPathName(), ViewportIdentity, Input));
	};
	const FUeAgentCommandResult Before = Call(
		TEXT("viewport.state.read"), MakeShared<FJsonObject>());
	TestTrue(TEXT("public read resolves the real Blueprint preview viewport"),
		Before.IsOk()
			&& Before.GetPayloadJson().Contains(ViewportIdentity)
			&& Before.GetPayloadJson().Contains(
				TEXT("viewport_state.camera_transform")));

	const FVector UpdatedLocation(315.0, -205.0, 155.0);
	const FRotator UpdatedRotation(-27.0, 122.0, 3.0);
	const float UpdatedFov = 61.0f;
	const TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetObjectField(
		TEXT("camera_transform"),
		BlueprintPreviewCameraTransform(
			ViewportIdentity, UpdatedLocation, UpdatedRotation));
	Patch->SetNumberField(TEXT("fov_degrees"), UpdatedFov);
	const TSharedRef<FJsonObject> UpdateInput = MakeShared<FJsonObject>();
	UpdateInput->SetObjectField(TEXT("state_patch"), Patch);
	const FUeAgentCommandResult Updated = Call(
		TEXT("viewport.state.update"), UpdateInput);
	TestTrue(TEXT("public update changes preview location rotation and FOV"),
		Updated.IsOk()
			&& Client->GetViewLocation().Equals(UpdatedLocation, 0.01)
			&& Client->GetViewRotation().Equals(UpdatedRotation, 0.01)
			&& FMath::IsNearlyEqual(Client->ViewFOV, UpdatedFov, 0.01f));
	const FUeAgentCommandResult IndependentRead = Call(
		TEXT("viewport.state.read"), MakeShared<FJsonObject>());
	TestTrue(TEXT("independent public read observes the preview camera update"),
		IndependentRead.IsOk()
			&& IndependentRead.GetPayloadJson().Contains(TEXT("61")));

	const TSharedRef<FJsonObject> InvalidContext = MakeShared<FJsonObject>();
	InvalidContext->SetStringField(
		TEXT("asset_editor_identity"), Blueprint->GetPathName());
	InvalidContext->SetStringField(TEXT("viewport_identity"), ViewportIdentity);
	InvalidContext->SetStringField(TEXT("viewport_scope"), TEXT("asset_editor"));
	InvalidContext->SetStringField(TEXT("viewport_role"), TEXT("graph"));
	InvalidContext->SetStringField(TEXT("world_type"), TEXT("editor"));
	const TSharedRef<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("target"), ViewportIdentity);
	InvalidParams->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());
	InvalidParams->SetObjectField(TEXT("context"), InvalidContext);
	const FVector BeforeInvalid = Client->GetViewLocation();
	const FUeAgentCommandResult Invalid = SubmitPublic(
		Registry, Dispatch, TEXT("viewport.state.read"), InvalidParams);
	TestFalse(TEXT("wrong Blueprint viewport role fails explicitly"), Invalid.IsOk());
	TestTrue(TEXT("wrong role preserves stable target failure semantics"),
		Invalid.GetErrorJson().Contains(
			TEXT("timeline_spatial_blueprint_viewport_target_invalid"))
			&& Client->GetViewLocation().Equals(BeforeInvalid, 0.01));

	const TSharedRef<FJsonObject> RestorePatch = MakeShared<FJsonObject>();
	RestorePatch->SetObjectField(
		TEXT("camera_transform"),
		BlueprintPreviewCameraTransform(
			ViewportIdentity, BaselineLocation, BaselineRotation));
	RestorePatch->SetNumberField(TEXT("fov_degrees"), BaselineFov);
	const TSharedRef<FJsonObject> RestoreInput = MakeShared<FJsonObject>();
	RestoreInput->SetObjectField(TEXT("state_patch"), RestorePatch);
	const FUeAgentCommandResult Restored = Call(
		TEXT("viewport.state.update"), RestoreInput);
	TestTrue(TEXT("owned preview camera baseline is restored"),
		Restored.IsOk()
			&& Client->GetViewLocation().Equals(BaselineLocation, 0.01)
			&& Client->GetViewRotation().Equals(BaselineRotation, 0.01)
			&& FMath::IsNearlyEqual(Client->ViewFOV, BaselineFov, 0.01f));

	UnregisterBlueprintComponentsViewport(ViewportIdentity, *Client);
	PreviewActor->Destroy();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
