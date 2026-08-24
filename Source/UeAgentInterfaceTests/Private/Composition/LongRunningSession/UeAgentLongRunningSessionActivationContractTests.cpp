#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionCommandHandlers.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"Composition/LongRunningSession/Fixtures/"
				"long_running_session_activation_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load long-running activation fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse long-running activation fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TMap<FString, TSharedPtr<FJsonObject>> ObjectsByField(
		FAutomationTestBase* Test,
		const TArray<TSharedPtr<FJsonValue>>& Values,
		const TCHAR* Field,
		const FString& Label)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Result;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Test->TestTrue(Label + TEXT(" entry is object"), Object.IsValid()))
			{
				continue;
			}
			FString Key;
			if (!Test->TestTrue(
					Label + TEXT(" entry has key"),
					Object->TryGetStringField(Field, Key) && !Key.IsEmpty()))
			{
				continue;
			}
			Test->TestFalse(Label + TEXT(" key unique: ") + Key, Result.Contains(Key));
			Result.Add(Key, Object);
		}
		return Result;
	}

	TArray<FString> ParameterNames(const TSharedPtr<FJsonObject>& Command)
	{
		TArray<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Command->GetArrayField(TEXT("parameters")))
		{
			Result.Add(Value->AsObject()->GetStringField(TEXT("name")));
		}
		return Result;
	}

	FString JsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	class FNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	class FRecordingOperationPort final
		: public UE::UAI::Contract::IUeAgentOperationApplicationPort
	{
	public:
		UE::UAI::Contract::FUeAgentOperationResult Submit(
			const UE::UAI::Contract::FUeAgentOperationRequest& Request,
			const UE::UAI::Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				UE::UAI::Contract::IUeAgentCancellationSignal>& Cancellation)
			override
		{
			++Calls;
			Descriptors.Add(Request.GetDescriptorIdentity());
			RequestIds.Add(Request.GetRequestId());
			Payloads.Add(Request.GetPayload().GetJsonValue());
			return UE::UAI::Contract::FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				UE::UAI::Contract::FUeAgentContractValue(
					TEXT("{\"operation_id\":\"op-ls008\","
						"\"phase\":\"accepted\"}")));
		}

		int32 Calls = 0;
		TArray<UE::UAI::Contract::FUeAgentDescriptorIdentity> Descriptors;
		TArray<FString> RequestIds;
		TArray<FString> Payloads;
	};

	TOptional<UE::UAI::CommandEdge::FUeAgentCommandRequest> RequestFor(
		const FString& CommandId,
		const FString& BindingHash,
		const FString& Params,
		const FString& RequestId)
	{
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ BindingHash
			+ TEXT("\",\"command_id\":\"")
			+ CommandId
			+ TEXT("\",\"correlation_id\":\"corr-ls008\","
				"\"message_kind\":\"command_request\",\"params\":")
			+ Params
			+ TEXT(",\"request_id\":\"")
			+ RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
		const FTCHARToUTF8 Converted(*Body);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		UE::UAI::CommandEdge::FUeAgentCommandProtocolCodec Codec;
		UE::UAI::CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<UE::UAI::CommandEdge::FUeAgentCommandRequest> Result;
		Codec.DecodeRequest(Bytes, Result, Diagnostic);
		return Result;
	}

	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	class FRecordingRuntimePort final : public ILongRunningOperationPort
	{
	public:
		bool Start(
			const FLongRunningOperationRequest& Request,
			const UE::UAI::ContextResource::FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			LastWorldType = Request.GetExecutionContext().GetWorldType();
			OutIdentity = FLongRunningWorkIdentity(
				TEXT("resource:ls009:") + Request.GetOperationId(),
				41);
			return true;
		}

		bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			++ObserveCalls;
			TOptional<FLongRunningOperationSignal> Signal;
			if (bEmitSignal)
			{
				FUeAgentCanonicalValuePtr ArtifactRef;
				FUeAgentCanonicalValuePtr ByteCount;
				FUeAgentCanonicalValuePtr Payload;
				FUeAgentStateSemanticError CanonicalError;
				if (!FUeAgentCanonicalValue::TryString(
						TEXT("artifact:file:ls013.png"),
						Resources,
						ArtifactRef,
						CanonicalError)
					|| !FUeAgentCanonicalValue::TryInteger(
						TEXT("9007199254740993"),
						Resources,
						ByteCount,
						CanonicalError)
					|| !FUeAgentCanonicalValue::TryObject(
						{
							{TEXT("artifact_ref"), ArtifactRef.ToSharedRef()},
							{TEXT("byte_count"), ByteCount.ToSharedRef()},
						},
						Resources,
						Payload,
						CanonicalError))
				{
					OutError = FExecutionMutationError(
						CanonicalError.Code,
						CanonicalError.Detail);
					return false;
				}
				Signal.Emplace(
					TEXT("uai.capture.artifact.v1"),
					INT64_C(9007199254740993),
					Payload.ToSharedRef());
			}
			OutObservation = FLongRunningWorkObservation(
				Identity.GetGeneration(),
				bComplete
					? ELongRunningWorkState::ExitedSuccess
					: ELongRunningWorkState::Running,
				FString(),
				Signal);
			return true;
		}

		bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			++StopCalls;
			return true;
		}

		bool Join(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++JoinCalls;
			return true;
		}

		bool Release(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			return true;
		}

		bool bComplete = false;
		bool bEmitSignal = false;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		UE::UAI::ContextResource::EUeAgentExecutionWorldType LastWorldType =
			UE::UAI::ContextResource::EUeAgentExecutionWorldType::Invalid;
	};

	class FRecordingRuntimeFactories final
		: public CompileBuildJob::IUeAgentCompileBuildJobPortFactory
		, public EditorSessionOperation::IUeAgentEditorSessionOperationPortFactory
		, public CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortFactory
	{
	public:
		bool TryCreateOperationPort(
			const CompileBuildJob::FUeAgentCompileBuildJobTypedRequest& Request,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			return AddPort(OutPort);
		}

		bool TryCreateOperationPort(
			const EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest& Request,
			const FLongRunningOperationRequest& OperationRequest,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			return AddPort(OutPort);
		}

		bool TryCreateOperationPort(
			const CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest& Request,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			return AddPort(OutPort);
		}

		TSharedRef<FRecordingRuntimePort, ESPMode::ThreadSafe> LastPort() const
		{
			check(!Ports.IsEmpty());
			return Ports.Last();
		}

		TArray<TSharedRef<FRecordingRuntimePort, ESPMode::ThreadSafe>> Ports;

	private:
		bool AddPort(
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort)
		{
			const TSharedRef<FRecordingRuntimePort, ESPMode::ThreadSafe> Port =
				MakeShared<FRecordingRuntimePort, ESPMode::ThreadSafe>();
			Ports.Add(Port);
			OutPort = Port;
			return true;
		}
	};

	const UE::UAI::Composition::LongRunningSession::
		FUeAgentLongRunningSessionActivationProfile* FindProfile(
			const FString& CommandId)
	{
		for (const UE::UAI::Composition::LongRunningSession::
			FUeAgentLongRunningSessionActivationProfile& Profile
			: UE::UAI::Composition::LongRunningSession::
				GetLongRunningSessionActivationProfiles())
		{
			if (Profile.CommandId == CommandId)
			{
				return &Profile;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> OperationPayload(
		const FUeAgentOperationResult& Result)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
			Result.GetValue().GetJsonValue());
		FJsonSerializer::Deserialize(
			Reader,
			Root,
			FJsonSerializer::EFlags::StoreNumbersAsStrings);
		return Root;
	}

	FString OperationErrorCode(const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return Root.IsValid()
			&& Root->TryGetObjectField(TEXT("operation_error"), Error)
			&& Error
			&& Error->IsValid()
			? (*Error)->GetStringField(TEXT("code"))
			: FString();
	}

	FString OperationRefJson(const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject>* OperationRef = nullptr;
		return Root.IsValid()
			&& Root->TryGetObjectField(TEXT("operation_ref"), OperationRef)
			&& OperationRef
			&& OperationRef->IsValid()
			? JsonText(OperationRef->ToSharedRef())
			: FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionPublicActivationContractTest,
	"UeAgentInterface.Composition.LongRunningSession.E0.PublicActivationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionPublicActivationContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("activation fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.long_running_session.public_activation.test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>& CommandValues =
		Fixture->GetArrayField(TEXT("commands"));
	TestEqual(
		TEXT("fixture command count"),
		CommandValues.Num(),
		static_cast<int32>(Fixture->GetNumberField(TEXT("command_count"))));
	const TMap<FString, TSharedPtr<FJsonObject>> Commands =
		ObjectsByField(this, CommandValues, TEXT("command_id"), TEXT("command"));

	const TSet<FString> ExpectedCommandIds = {
		TEXT("job.compile_build"),
		TEXT("editor.session"),
		TEXT("capture.image.execute"),
		TEXT("capture.media.execute"),
		TEXT("capture.render.execute"),
		TEXT("editor.safe_exit"),
		TEXT("operation.status"),
		TEXT("operation.resume"),
		TEXT("operation.cancel"),
		TEXT("operation.receipt"),
		TEXT("operation.reconcile"),
		TEXT("operation.release"),
	};
	TestEqual(TEXT("exact public command count"), Commands.Num(), 12);
	for (const FString& CommandId : ExpectedCommandIds)
	{
		TestTrue(
			TEXT("required command exists: ") + CommandId,
			Commands.Contains(CommandId));
	}

	TSet<FString> DescriptorIds;
	TSet<FString> HandlerBindings;
	int32 StartCount = 0;
	int32 LifecycleCount = 0;
	const TSet<FString> ForbiddenPublicParameters = {
		TEXT("adapter_id"),
		TEXT("command_line"),
		TEXT("executable"),
		TEXT("arguments"),
		TEXT("process_handle"),
		TEXT("session_registry"),
		TEXT("timeout_seconds"),
		TEXT("http_route"),
		TEXT("raw_image_bytes"),
		TEXT("raw_audio_bytes"),
		TEXT("raw_video_bytes"),
		TEXT("business_outcome"),
	};
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Commands)
	{
		const TSharedPtr<FJsonObject>& Command = Pair.Value;
		const FString Kind = Command->GetStringField(TEXT("kind"));
		StartCount += Kind == TEXT("start") ? 1 : 0;
		LifecycleCount += Kind == TEXT("lifecycle") ? 1 : 0;
		TestTrue(TEXT("command kind is supported"),
			Kind == TEXT("start") || Kind == TEXT("lifecycle"));
		TestFalse(TEXT("command namespace is not empty"),
			Command->GetStringField(TEXT("namespace")).IsEmpty());
		TestFalse(TEXT("command method is not empty"),
			Command->GetStringField(TEXT("method")).IsEmpty());

		const FString DescriptorId =
			Command->GetStringField(TEXT("descriptor_id"));
		const FString HandlerBinding =
			Command->GetStringField(TEXT("handler_binding"));
		TestFalse(TEXT("descriptor identity is exact and unique"),
			DescriptorIds.Contains(DescriptorId));
		TestFalse(TEXT("handler binding is exact and unique"),
			HandlerBindings.Contains(HandlerBinding));
		DescriptorIds.Add(DescriptorId);
		HandlerBindings.Add(HandlerBinding);

		TSet<FString> ParameterSet;
		for (const TSharedPtr<FJsonValue>& ParameterValue
			: Command->GetArrayField(TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
			const FString Name = Parameter->GetStringField(TEXT("name"));
			TestFalse(TEXT("parameter is unique: ") + Pair.Key,
				ParameterSet.Contains(Name));
			ParameterSet.Add(Name);
			TestFalse(TEXT("public parameter does not expose native owner: ") + Name,
				ForbiddenPublicParameters.Contains(Name));
			TestTrue(TEXT("all activation parameters are required"),
				Parameter->GetBoolField(TEXT("required")));
			TestFalse(TEXT("activation parameters are non-nullable"),
				Parameter->GetBoolField(TEXT("nullable")));
		}
		TestTrue(TEXT("command has exact example params"),
			Command->HasTypedField<EJson::Object>(TEXT("example_params")));
	}
	TestEqual(TEXT("six start commands"), StartCount, 6);
	TestEqual(TEXT("six lifecycle commands"), LifecycleCount, 6);
	TestEqual(
		TEXT("start count matches fixture"),
		StartCount,
		static_cast<int32>(Fixture->GetNumberField(TEXT("start_command_count"))));
	TestEqual(
		TEXT("lifecycle count matches fixture"),
		LifecycleCount,
		static_cast<int32>(
			Fixture->GetNumberField(TEXT("lifecycle_command_count"))));

	TestTrue(
		TEXT("safe-exit exposes dirty resolution and durable witness"),
		ParameterNames(Commands[TEXT("editor.safe_exit")])
			== TArray<FString>{
				TEXT("dirty_resolution"), TEXT("external_witness")});
	TestTrue(
		TEXT("resume uses operation ref plus generation fence"),
		ParameterNames(Commands[TEXT("operation.resume")])
			== TArray<FString>{
				TEXT("operation_ref"), TEXT("expected_generation")});
	TestTrue(
		TEXT("reconcile uses operation ref plus generation fence"),
		ParameterNames(Commands[TEXT("operation.reconcile")])
			== TArray<FString>{
				TEXT("operation_ref"), TEXT("expected_generation")});
	TestFalse(
		TEXT("reconcile never starts or advances native work"),
		Commands[TEXT("operation.reconcile")]->GetBoolField(
			TEXT("may_start_or_advance_native_work")));

	const TArray<TSharedPtr<FJsonValue>>& OperationRefFields =
		Fixture->GetArrayField(TEXT("operation_ref_fields"));
	TestEqual(TEXT("operation ref has exactly three opaque fields"),
		OperationRefFields.Num(), 3);
	TestEqual(TEXT("operation ref operation id"),
		OperationRefFields[0]->AsString(), FString(TEXT("operation_id")));
	TestEqual(TEXT("operation ref lease"),
		OperationRefFields[1]->AsString(), FString(TEXT("owner_lease_id")));
	TestEqual(TEXT("operation ref server session"),
		OperationRefFields[2]->AsString(), FString(TEXT("server_session_id")));

	const TArray<TSharedPtr<FJsonValue>>& FamilyValues =
		Fixture->GetArrayField(TEXT("family_bindings"));
	TestEqual(
		TEXT("all thirteen Adapter families are bound"),
		FamilyValues.Num(),
		static_cast<int32>(
			Fixture->GetNumberField(TEXT("family_binding_count"))));
	TSet<FString> FamilyKeys;
	for (const TSharedPtr<FJsonValue>& FamilyValue : FamilyValues)
	{
		const TSharedPtr<FJsonObject> Family = FamilyValue->AsObject();
		const FString Key = Family->GetStringField(TEXT("adapter_id"))
			+ TEXT(":") + Family->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("Adapter family binding is unique: ") + Key,
			FamilyKeys.Contains(Key));
		FamilyKeys.Add(Key);
		TestTrue(TEXT("family points to active public command"),
			Commands.Contains(Family->GetStringField(TEXT("command_id"))));
	}

	const TSharedPtr<FJsonObject> ResultContract =
		Fixture->GetObjectField(TEXT("result_contract"));
	TestFalse(TEXT("accepted is not complete"),
		ResultContract->GetBoolField(TEXT("accepted_is_complete")));
	TestFalse(TEXT("exit scheduled is not complete"),
		ResultContract->GetBoolField(TEXT("exit_scheduled_is_complete")));
	TestFalse(TEXT("artifact ready is not complete"),
		ResultContract->GetBoolField(TEXT("artifact_ready_is_complete")));
	TestFalse(TEXT("state unknown is never retry safe"),
		ResultContract->GetBoolField(TEXT("state_unknown_retry_safe")));
	TestFalse(TEXT("disconnect does not imply Outcome"),
		ResultContract->GetBoolField(TEXT("disconnect_implies_outcome")));

	using namespace UE::UAI::Composition::LongRunningSession;
	TestEqual(TEXT("production activation version"),
		UAI_LONG_RUNNING_SESSION_ACTIVATION_V1, 1);
	const TArray<FUeAgentLongRunningSessionActivationProfile>& Profiles =
		GetLongRunningSessionActivationProfiles();
	TestEqual(TEXT("one production profile per public command"),
		Profiles.Num(), Commands.Num());
	TMap<FString, const FUeAgentLongRunningSessionActivationProfile*>
		ProfileById;
	for (const FUeAgentLongRunningSessionActivationProfile& Profile : Profiles)
	{
		TestTrue(TEXT("production activation profile valid"), Profile.IsValid());
		TestFalse(TEXT("production command identity unique"),
			ProfileById.Contains(Profile.CommandId));
		ProfileById.Add(Profile.CommandId, &Profile);
	}
	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Commands)
	{
		const FUeAgentLongRunningSessionActivationProfile* const* Found =
			ProfileById.Find(Pair.Key);
		if (!TestTrue(TEXT("production profile exists: ") + Pair.Key,
				Found != nullptr))
		{
			continue;
		}
		TestEqual(TEXT("production namespace"), (*Found)->Namespace,
			Pair.Value->GetStringField(TEXT("namespace")));
		TestEqual(TEXT("production method"), (*Found)->Method,
			Pair.Value->GetStringField(TEXT("method")));
		TestEqual(TEXT("production descriptor"),
			(*Found)->DescriptorIdentity.GetDescriptorId(),
			Pair.Value->GetStringField(TEXT("descriptor_id")));
		TestEqual(TEXT("production handler binding"),
			(*Found)->HandlerBinding,
			Pair.Value->GetStringField(TEXT("handler_binding")));
	}

	TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>
		DescriptorRegistry;
	FString DescriptorError;
	TestTrue(TEXT("production Descriptor registry composes"),
		TryCreateLongRunningSessionDescriptorRegistry(
			DescriptorRegistry, DescriptorError));
	if (!DescriptorRegistry.IsValid())
	{
		AddError(TEXT("long_running_session_descriptors_missing:")
			+ DescriptorError);
	}
	else
	{
		TestEqual(TEXT("one Descriptor per public command"),
			DescriptorRegistry->GetDescriptors().Num(), Commands.Num());
		for (const FUeAgentLongRunningSessionActivationProfile& Profile
			: Profiles)
		{
			const UE::UAI::Contract::FUeAgentOperationDescriptor* Descriptor =
				DescriptorRegistry->FindExact(Profile.DescriptorIdentity);
			TestNotNull(TEXT("profile exact Descriptor registered: ")
				+ Profile.CommandId, Descriptor);
			if (Descriptor)
			{
				TestEqual(TEXT("long-running Descriptor is an operation"),
					Descriptor->GetOperationKind(),
					UE::UAI::Contract::EUeAgentDescriptorOperationKind::Operation);
				TestNotNull(TEXT("input schema is compiled"),
					DescriptorRegistry->FindCompiledInputSchemaExact(
						Profile.DescriptorIdentity));
			}
		}
	}

	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Registry;
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> BaseRegistry;
	FString RegistryError;
	TestTrue(
		TEXT("timeline/spatial base command registry composes"),
		UE::UAI::Composition::TimelineSpatial::
			TryCreateTimelineSpatialProductionCommandContractRegistry(
				BaseRegistry, RegistryError));
	if (!BaseRegistry.IsValid())
	{
		AddError(TEXT("long_running_session_base_registry_missing:")
			+ RegistryError);
		return false;
	}
	RegistryError.Reset();
	TestTrue(TEXT("production command registry composes"),
		TryCreateLongRunningSessionProductionCommandContractRegistry(
			Registry, RegistryError));
	if (!Registry.IsValid())
	{
		AddError(TEXT("long_running_session_registry_missing:") + RegistryError);
	}
	else
	{
		TestEqual(
			TEXT("long-running composition adds its exact contracts to the current base"),
			Registry->GetContracts().Num(),
			BaseRegistry->GetContracts().Num() + ExpectedCommandIds.Num());
		for (const FString& CommandId : ExpectedCommandIds)
		{
			TestNotNull(TEXT("public command registered: ") + CommandId,
				Registry->FindExact(CommandId));
		}

		FString Manifest;
		RegistryError.Reset();
		TestTrue(
			TEXT("outermost production registry builds formal manifest"),
			Registry->BuildManifest(
				UE::UAI::CommandContract::GetUaiCommandManifestPluginVersion(),
				UE::UAI::CommandContract::GetUaiCommandManifestBuildId(),
				Manifest,
				RegistryError));
		TestTrue(
			TEXT("formal manifest contains long-running commands"),
			Manifest.Contains(TEXT("operation.reconcile"),
				ESearchCase::CaseSensitive));
		// The committed formal manifest is owned by the current outermost
		// activation registry. LongRunningSession proves only its additive
		// in-memory composition over the current TimelineSpatial base here.
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionCulturePreviewContextContractTest,
	"UeAgentInterface.Composition.LongRunningSession.E0.CulturePreviewContextContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionCulturePreviewContextContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::LongRunningSession;
	const FUeAgentLongRunningSessionActivationProfile* const Profile =
		FindProfile(TEXT("editor.session"));
	if (!TestNotNull(TEXT("editor.session profile exists"), Profile))
	{
		return false;
	}

	FUeAgentLongRunningSessionRuntimeConfig RuntimeConfig;
	RuntimeConfig.ProjectInstanceId = TEXT("project:ls-culture");
	RuntimeConfig.EditorInstanceId = TEXT("editor-instance:ls-culture");
	RuntimeConfig.ServerSessionId = TEXT("server-session:ls-culture");
	RuntimeConfig.OwnerLeaseSecret = TEXT("lease-secret:ls-culture");
	RuntimeConfig.EditorWorldIdentity = TEXT("world:/Game/Maps/LSCulture");
	RuntimeConfig.EditorLevelIdentity =
		TEXT("level:/Game/Maps/LSCulture:PersistentLevel");
	RuntimeConfig.TimeoutSeconds = 30.0;
	RuntimeConfig.MaxFrameBytes = 1024 * 1024;
	RuntimeConfig.MaxActiveOperations = 4;
	RuntimeConfig.MaxRetainedOperations = 8;
	const TSharedRef<FRecordingRuntimeFactories, ESPMode::ThreadSafe> Factories =
		MakeShared<FRecordingRuntimeFactories, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
	FUeAgentLongRunningSessionRuntimeError RuntimeError;
	TestTrue(
		TEXT("culture preview runtime composes"),
		FUeAgentLongRunningSessionRuntime::TryCreate(
			RuntimeConfig,
			StaticCastSharedRef<
				CompileBuildJob::IUeAgentCompileBuildJobPortFactory>(Factories),
			StaticCastSharedRef<
				EditorSessionOperation::IUeAgentEditorSessionOperationPortFactory>(
					Factories),
			StaticCastSharedRef<
				CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortFactory>(
					Factories),
			Runtime,
			RuntimeError));
	if (!Runtime.IsValid())
	{
		AddError(RuntimeError.Code);
		return false;
	}

	const FUeAgentCallerContext Caller(
		TEXT("ls-culture-public-edge"),
		{TEXT("uai:operation")},
		TEXT("corr-ls-culture"));
	const TSharedRef<FNeverCancelled> Cancellation = MakeShared<FNeverCancelled>();
	const FString Payload =
		TEXT("{\"family_id\":\"runtime_preview\",")
		TEXT("\"subject_ref\":\"culture:fr\",")
		TEXT("\"policy_id\":\"policy:runtime-session:culture-preview/v1\",")
		TEXT("\"parameters\":{\"culture\":\"fr\",")
		TEXT("\"restore_after\":true,\"scope\":\"session\"}}");
	const FUeAgentOperationResult Result =
		Runtime->GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->DescriptorIdentity,
				TEXT("request:ls-culture:start"),
				TEXT("uai-command:request:ls-culture:start"),
				FUeAgentContractValue(Payload)),
			Caller,
			Cancellation);
	const TSharedPtr<FJsonObject> Root = OperationPayload(Result);
	TestTrue(TEXT("culture preview returns an operation snapshot"), Root.IsValid());
	if (Root.IsValid())
	{
		TestTrue(
			TEXT("the exact culture policy does not require a PIE subject"),
			OperationErrorCode(Root).IsEmpty());
		TestEqual(
			TEXT("culture preview reaches one native session port"),
			Factories->Ports.Num(),
			1);
		const FUeAgentLongRunningSessionActivationProfile* const ResumeProfile =
			FindProfile(TEXT("operation.resume"));
		TestNotNull(TEXT("operation.resume profile exists"), ResumeProfile);
		if (ResumeProfile)
		{
			const FString ResumePayload = TEXT("{\"operation_ref\":")
				+ OperationRefJson(Root)
				+ TEXT(",\"expected_generation\":0}");
			const TSharedPtr<FJsonObject> ResumeRoot = OperationPayload(
				Runtime->GetOperationPort()->Submit(
					FUeAgentOperationRequest(
						ResumeProfile->DescriptorIdentity,
						TEXT("request:ls-culture:resume"),
						TEXT("uai-command:request:ls-culture:resume"),
						FUeAgentContractValue(ResumePayload)),
					Caller,
					Cancellation));
			TestTrue(
				TEXT("culture preview resume starts the native session"),
				ResumeRoot.IsValid()
					&& OperationErrorCode(ResumeRoot).IsEmpty()
					&& Factories->LastPort()->StartCalls == 1);
			TestTrue(
				TEXT("culture preview carries an Editor world context"),
				Factories->LastPort()->LastWorldType
					== UE::UAI::ContextResource::
						EUeAgentExecutionWorldType::Editor);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionPublicCommandHandlersTest,
	"UeAgentInterface.Composition.LongRunningSession.E1.PublicCommandHandlers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionPublicCommandHandlersTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::LongRunningSession;
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TMap<FString, TSharedPtr<FJsonObject>> Commands = ObjectsByField(
		this,
		Fixture->GetArrayField(TEXT("commands")),
		TEXT("command_id"),
		TEXT("command"));
	const TSharedRef<FRecordingOperationPort> OperationPort =
		MakeShared<FRecordingOperationPort>();
	TArray<TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>> Handlers;
	FString Error;
	TestTrue(TEXT("one exact handler per activation profile"),
		TryCreateLongRunningSessionCommandHandlers(
			GetLongRunningSessionActivationProfiles(),
			OperationPort,
			Handlers,
			Error));
	TestEqual(TEXT("handler count"), Handlers.Num(), 12);

	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Registry;
	TestTrue(TEXT("production command registry composes for handler test"),
		TryCreateLongRunningSessionProductionCommandContractRegistry(
			Registry, Error));
	if (!Registry.IsValid() || Handlers.Num() != 12)
	{
		AddError(TEXT("long_running_session_handlers_unavailable:") + Error);
		return false;
	}

	TMap<FString, TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>>
		HandlerById;
	for (const TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>& Handler
		: Handlers)
	{
		TestFalse(TEXT("handler command ownership unique"),
			HandlerById.Contains(Handler->GetCommandId()));
		HandlerById.Add(Handler->GetCommandId(), Handler);
	}

	const UE::UAI::Contract::FUeAgentCallerContext Caller(
		TEXT("ls008-public-edge"),
		{TEXT("uai:operation")},
		TEXT("corr-ls008"));
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	int32 RequestIndex = 0;
	for (const UE::UAI::Composition::LongRunningSession::
		FUeAgentLongRunningSessionActivationProfile& Profile
		: GetLongRunningSessionActivationProfiles())
	{
		const UE::UAI::CommandContract::FUeAgentCommandContract* Contract =
			Registry->FindExact(Profile.CommandId);
		const TSharedPtr<FJsonObject>* FixtureCommand =
			Commands.Find(Profile.CommandId);
		TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>* Handler =
			HandlerById.Find(Profile.CommandId);
		if (!TestNotNull(TEXT("contract exists"), Contract)
			|| !TestTrue(TEXT("fixture command exists"), FixtureCommand != nullptr)
			|| !TestTrue(TEXT("handler exists"), Handler != nullptr))
		{
			continue;
		}
		const FString ParamsJson = JsonText(
			(*FixtureCommand)->GetObjectField(TEXT("example_params")).ToSharedRef());
		const FString RequestId = FString::Printf(
			TEXT("req-ls008-%d"), RequestIndex++);
		const TOptional<UE::UAI::CommandEdge::FUeAgentCommandRequest> Request =
			RequestFor(
				Profile.CommandId,
				Contract->GetBindingHash(),
				ParamsJson,
				RequestId);
		if (!TestTrue(TEXT("wire request decodes"), Request.IsSet()))
		{
			continue;
		}
		const UE::UAI::CommandEdge::FUeAgentCommandResult Result =
			(*Handler)->Submit(Request.GetValue(), Caller, Cancellation);
		TestTrue(TEXT("handler returns exact C3O result"), Result.IsOk());
		TestTrue(TEXT("handler result matches request"),
			Result.MatchesRequest(Request.GetValue()));
	}
	TestEqual(TEXT("each command forwards once"),
		OperationPort->Calls, Handlers.Num());
	TestEqual(TEXT("each command preserves request id"),
		OperationPort->RequestIds.Num(), Handlers.Num());
	TestEqual(TEXT("each command forwards one typed descriptor"),
		OperationPort->Descriptors.Num(), Handlers.Num());
	for (int32 Index = 0;
		Index < OperationPort->Descriptors.Num()
			&& Index < GetLongRunningSessionActivationProfiles().Num();
		++Index)
	{
		TestTrue(TEXT("handler uses profile exact descriptor"),
			OperationPort->Descriptors[Index]
				== GetLongRunningSessionActivationProfiles()[Index]
					.DescriptorIdentity);
	}

	FUeAgentLongRunningSessionRuntimeConfig RuntimeConfig;
	RuntimeConfig.ProjectInstanceId = TEXT("project:ls009");
	RuntimeConfig.EditorInstanceId = TEXT("editor-instance:ls009");
	RuntimeConfig.ServerSessionId = TEXT("server-session:ls009");
	RuntimeConfig.OwnerLeaseSecret = TEXT("lease-secret:ls009");
	RuntimeConfig.EditorWorldIdentity = TEXT("world:/Game/Maps/LS009");
	RuntimeConfig.EditorLevelIdentity = TEXT("level:/Game/Maps/LS009:PersistentLevel");
	RuntimeConfig.TimeoutSeconds = 0.25;
	RuntimeConfig.MaxFrameBytes = 1024 * 1024;
	RuntimeConfig.MaxActiveOperations = 16;
	RuntimeConfig.MaxRetainedOperations = 64;
	const TSharedRef<FRecordingRuntimeFactories, ESPMode::ThreadSafe> Factories =
		MakeShared<FRecordingRuntimeFactories, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
	FUeAgentLongRunningSessionRuntimeError RuntimeError;
	TestTrue(TEXT("one LongRunningSession runtime composes"),
		FUeAgentLongRunningSessionRuntime::TryCreate(
			RuntimeConfig,
			StaticCastSharedRef<
				CompileBuildJob::IUeAgentCompileBuildJobPortFactory>(Factories),
			StaticCastSharedRef<
				EditorSessionOperation::IUeAgentEditorSessionOperationPortFactory>(
					Factories),
			StaticCastSharedRef<
				CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortFactory>(
					Factories),
			Runtime,
			RuntimeError));
	if (!Runtime.IsValid())
	{
		AddError(TEXT("long_running_session_runtime_unavailable:")
			+ RuntimeError.Code);
		return false;
	}

	const auto SubmitRuntime = [
		this,
		&Runtime,
		&Caller,
		&Cancellation
	](
		const FString& CommandId,
		const FString& RequestId,
		const FString& PayloadJson)
	{
		const FUeAgentLongRunningSessionActivationProfile* Profile =
			FindProfile(CommandId);
		if (!Profile)
		{
			AddError(TEXT("runtime profile missing: ") + CommandId);
			return FUeAgentOperationResult(
				FUeAgentDescriptorIdentity(
					TEXT("uai.invalid/invalid"),
					1,
					TEXT("sha256:invalid")),
				RequestId,
				FUeAgentContractValue(TEXT("{}")));
		}
		return Runtime->GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->DescriptorIdentity,
				RequestId,
				TEXT("uai-command:") + RequestId,
				FUeAgentContractValue(PayloadJson)),
			Caller,
			Cancellation);
	};

	const FString FirstStartPayload =
		TEXT("{\"family_id\":\"asset_compile\"," 
			"\"subject_ref\":\"asset:/Game/UAI/Fixture.Asset\"," 
			"\"policy_id\":\"policy:asset-compile:default/v1\"," 
			"\"parameters\":{\"nested\":{\"enabled\":true}}}");
	const FString ReorderedStartPayload =
		TEXT("{\"parameters\":{\"nested\":{\"enabled\":true}}," 
			"\"policy_id\":\"policy:asset-compile:default/v1\"," 
			"\"subject_ref\":\"asset:/Game/UAI/Fixture.Asset\"," 
			"\"family_id\":\"asset_compile\"}");
	const FUeAgentOperationResult FirstStart = SubmitRuntime(
		TEXT("job.compile_build"),
		TEXT("request:ls009:dedupe"),
		FirstStartPayload);
	const TSharedPtr<FJsonObject> FirstStartRoot = OperationPayload(FirstStart);
	TestTrue(TEXT("first start returns accepted snapshot"),
		FirstStartRoot.IsValid() && OperationErrorCode(FirstStartRoot).IsEmpty());
	TestEqual(TEXT("start creates one operation-scoped port"),
		Factories->Ports.Num(), 1);
	TestEqual(TEXT("accept does not start native work"),
		Factories->LastPort()->StartCalls, 0);
	TestEqual(TEXT("start phase is accepted"),
		FirstStartRoot->GetStringField(TEXT("phase")),
		FString(TEXT("accepted")));
	TestFalse(TEXT("accepted remains nonterminal"),
		FirstStartRoot->GetBoolField(TEXT("terminal")));
	FPlatformProcess::Sleep(0.35f);

	const FUeAgentOperationResult DuplicateStart = SubmitRuntime(
		TEXT("job.compile_build"),
		TEXT("request:ls009:dedupe"),
		ReorderedStartPayload);
	const TSharedPtr<FJsonObject> DuplicateStartRoot =
		OperationPayload(DuplicateStart);
	TestEqual(TEXT("canonical duplicate returns the same operation ref"),
		OperationRefJson(DuplicateStartRoot),
		OperationRefJson(FirstStartRoot));
	TestEqual(TEXT("duplicate does not create a second native port"),
		Factories->Ports.Num(), 1);

	const FUeAgentOperationResult DifferentStart = SubmitRuntime(
		TEXT("job.compile_build"),
		TEXT("request:ls009:dedupe"),
		TEXT("{\"family_id\":\"asset_compile\"," 
			"\"subject_ref\":\"asset:/Game/UAI/Fixture.Asset\"," 
			"\"policy_id\":\"policy:asset-compile:alternate/v1\"," 
			"\"parameters\":{}}"));
	const TSharedPtr<FJsonObject> DifferentStartRoot =
		OperationPayload(DifferentStart);
	TestNotEqual(TEXT("same request id with different canonical params is distinct"),
		OperationRefJson(DifferentStartRoot),
		OperationRefJson(FirstStartRoot));
	TestEqual(TEXT("distinct request creates its own operation port"),
		Factories->Ports.Num(), 2);

	const TSharedPtr<FJsonObject> FirstRef =
		FirstStartRoot->GetObjectField(TEXT("operation_ref"));
	const FString OperationId =
		FirstRef->GetStringField(TEXT("operation_id"));
	const FString OwnerLeaseId =
		FirstRef->GetStringField(TEXT("owner_lease_id"));
	const FString ServerSessionId =
		FirstRef->GetStringField(TEXT("server_session_id"));
	const FString FirstRefJson = OperationRefJson(FirstStartRoot);
	const FString StatusPayload =
		TEXT("{\"operation_ref\":") + FirstRefJson + TEXT("}");
	const TSharedPtr<FJsonObject> StatusRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.status"), TEXT("request:ls009:status"), StatusPayload));
	TestTrue(TEXT("correct three-field operation ref reattaches"),
		StatusRoot.IsValid() && OperationErrorCode(StatusRoot).IsEmpty());

	const FString WrongLeasePayload = FString::Printf(
		TEXT("{\"operation_ref\":{\"operation_id\":\"%s\"," 
			"\"owner_lease_id\":\"lease:wrong\"," 
			"\"server_session_id\":\"%s\"}}"),
		*OperationId,
		*ServerSessionId);
	const TSharedPtr<FJsonObject> WrongLeaseRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.status"),
		TEXT("request:ls009:wrong-lease"),
		WrongLeasePayload));
	TestEqual(TEXT("wrong lease fails closed"),
		OperationErrorCode(WrongLeaseRoot),
		FString(TEXT("operation_owner_lease_mismatch")));

	const FString WrongSessionPayload = FString::Printf(
		TEXT("{\"operation_ref\":{\"operation_id\":\"%s\"," 
			"\"owner_lease_id\":\"%s\"," 
			"\"server_session_id\":\"server-session:wrong\"}}"),
		*OperationId,
		*OwnerLeaseId);
	const TSharedPtr<FJsonObject> WrongSessionRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.status"),
		TEXT("request:ls009:wrong-session"),
		WrongSessionPayload));
	TestEqual(TEXT("wrong server session fails closed"),
		OperationErrorCode(WrongSessionRoot),
		FString(TEXT("operation_server_session_mismatch")));

	const FString ResumePayload = TEXT("{\"operation_ref\":")
		+ FirstRefJson + TEXT(",\"expected_generation\":0}");
	const TSharedPtr<FJsonObject> ResumeRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.resume"), TEXT("request:ls009:resume"), ResumePayload));
	TestNotEqual(TEXT("resume does not inherit the start request deadline"),
		ResumeRoot->GetStringField(TEXT("error_code")),
		FString(TEXT("deadline_exceeded")));
	TestTrue(TEXT("resume starts exactly one native operation"),
		ResumeRoot.IsValid() && OperationErrorCode(ResumeRoot).IsEmpty());
	TestEqual(TEXT("native start count"), Factories->Ports[0]->StartCalls, 1);
	TestEqual(TEXT("running generation is returned"),
		static_cast<int64>(ResumeRoot->GetNumberField(TEXT("generation"))),
		int64(41));

	const int32 ObserveBeforeStale = Factories->Ports[0]->ObserveCalls;
	const TSharedPtr<FJsonObject> StaleRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.resume"),
		TEXT("request:ls009:stale"),
		ResumePayload));
	TestEqual(TEXT("stale generation fails closed"),
		OperationErrorCode(StaleRoot),
		FString(TEXT("operation_generation_mismatch")));
	TestEqual(TEXT("stale resume does not observe or advance native work"),
		Factories->Ports[0]->ObserveCalls,
		ObserveBeforeStale);

	const FString ReconcilePayload = TEXT("{\"operation_ref\":")
		+ FirstRefJson + TEXT(",\"expected_generation\":41}");
	const int32 StartsBeforeReconcile = Factories->Ports[0]->StartCalls;
	const int32 ObservesBeforeReconcile = Factories->Ports[0]->ObserveCalls;
	const TSharedPtr<FJsonObject> ReconcileRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.reconcile"),
		TEXT("request:ls009:reconcile"),
		ReconcilePayload));
	TestTrue(TEXT("known reconcile returns current snapshot"),
		ReconcileRoot.IsValid() && OperationErrorCode(ReconcileRoot).IsEmpty());
	TestEqual(TEXT("reconcile never starts native work"),
		Factories->Ports[0]->StartCalls,
		StartsBeforeReconcile);
	TestEqual(TEXT("reconcile never observes or advances native work"),
		Factories->Ports[0]->ObserveCalls,
		ObservesBeforeReconcile);

	const FString ForgedUnknownReconcilePayload = FString::Printf(
		TEXT("{\"operation_ref\":{\"operation_id\":\"operation:unknown\"," 
			"\"owner_lease_id\":\"lease:unknown\"," 
			"\"server_session_id\":\"%s\"},\"expected_generation\":7}"),
		*ServerSessionId);
	const TSharedPtr<FJsonObject> ForgedUnknownReconcileRoot = OperationPayload(
		SubmitRuntime(
			TEXT("operation.reconcile"),
			TEXT("request:ls009:forged-unknown-reconcile"),
			ForgedUnknownReconcilePayload));
	TestEqual(TEXT("unknown operation with forged lease fails closed"),
		OperationErrorCode(ForgedUnknownReconcileRoot),
		FString(TEXT("operation_owner_lease_mismatch")));
	TestEqual(TEXT("forged reconcile does not replay native work"),
		Factories->Ports[0]->StartCalls,
		StartsBeforeReconcile);

	const FString ReceiptPayload = StatusPayload;
	const TSharedPtr<FJsonObject> EarlyReceiptRoot = OperationPayload(
		SubmitRuntime(
			TEXT("operation.receipt"),
			TEXT("request:ls009:early-receipt"),
			ReceiptPayload));
	TestEqual(TEXT("nonterminal receipt fails closed"),
		OperationErrorCode(EarlyReceiptRoot),
		FString(TEXT("operation_not_terminal")));

	Factories->Ports[0]->bComplete = true;
	Factories->Ports[0]->bEmitSignal = true;
	const FString CompletePayload = TEXT("{\"operation_ref\":")
		+ FirstRefJson + TEXT(",\"expected_generation\":41}");
	const TSharedPtr<FJsonObject> CompleteRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.resume"),
		TEXT("request:ls009:complete"),
		CompletePayload));
	TestEqual(TEXT("C7 success is exposed as native exit, not business complete"),
		CompleteRoot->GetStringField(TEXT("lifecycle_status")),
		FString(TEXT("native_exited_success")));
	TestEqual(TEXT("terminal native work joined once"),
		Factories->Ports[0]->JoinCalls, 1);
	TestEqual(TEXT("terminal native work released once"),
		Factories->Ports[0]->ReleaseCalls, 1);
	const TSharedPtr<FJsonObject> LatestSignal =
		CompleteRoot->GetObjectField(TEXT("latest_signal"));
	TestTrue(TEXT("public snapshot exposes the latest C7 signal"),
		LatestSignal.IsValid());
	if (LatestSignal.IsValid())
	{
		TestEqual(TEXT("latest signal preserves its exact type"),
			LatestSignal->GetStringField(TEXT("type_id")),
			FString(TEXT("uai.capture.artifact.v1")));
		FString SignalRevision;
		const TSharedPtr<FJsonValue> RevisionValue =
			LatestSignal->TryGetField(TEXT("revision"));
		TestTrue(TEXT("latest signal revision remains an exact integer"),
			RevisionValue.IsValid()
				&& RevisionValue->TryGetString(SignalRevision));
		TestEqual(TEXT("latest signal revision exceeds IEEE-754 exactly"),
			SignalRevision,
			FString(TEXT("9007199254740993")));
		const TSharedPtr<FJsonObject> SignalPayload =
			LatestSignal->GetObjectField(TEXT("payload"));
		TestTrue(TEXT("latest signal exposes its canonical payload"),
			SignalPayload.IsValid());
		if (SignalPayload.IsValid())
		{
			TestEqual(TEXT("latest signal payload preserves artifact ref"),
				SignalPayload->GetStringField(TEXT("artifact_ref")),
				FString(TEXT("artifact:file:ls013.png")));
			FString ByteCount;
			const TSharedPtr<FJsonValue> ByteCountValue =
				SignalPayload->TryGetField(TEXT("byte_count"));
			TestTrue(TEXT("payload integer remains exact"),
				ByteCountValue.IsValid()
					&& ByteCountValue->TryGetString(ByteCount));
			TestEqual(TEXT("payload integer exceeds IEEE-754 exactly"),
				ByteCount,
				FString(TEXT("9007199254740993")));
		}
	}

	const TSharedPtr<FJsonObject> ReceiptRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.receipt"),
		TEXT("request:ls009:receipt"),
		ReceiptPayload));
	TestEqual(TEXT("terminal receipt uses the receipt schema"),
		ReceiptRoot->GetStringField(TEXT("schema")),
		FString(TEXT("uai.long_running.receipt.v1")));
	TestFalse(TEXT("native-exited receipt does not declare retry safe"),
		ReceiptRoot->GetBoolField(TEXT("retry_safe")));

	const TSharedPtr<FJsonObject> ReleaseRoot = OperationPayload(SubmitRuntime(
		TEXT("operation.release"),
		TEXT("request:ls009:release"),
		StatusPayload));
	TestEqual(TEXT("terminal release is explicit"),
		ReleaseRoot->GetStringField(TEXT("lifecycle_status")),
		FString(TEXT("released")));
	const TSharedPtr<FJsonObject> ReleasedStatusRoot = OperationPayload(
		SubmitRuntime(
			TEXT("operation.status"),
			TEXT("request:ls009:released-status"),
			StatusPayload));
	TestEqual(TEXT("released operation is no longer retained"),
		OperationErrorCode(ReleasedStatusRoot),
		FString(TEXT("operation_not_found")));

	const FString UnknownReconcilePayload = TEXT("{\"operation_ref\":")
		+ FirstRefJson
		+ TEXT(",\"expected_generation\":9007199254740993}");
	const TSharedPtr<FJsonObject> UnknownReconcileRoot = OperationPayload(
		SubmitRuntime(
			TEXT("operation.reconcile"),
			TEXT("request:ls009:released-reconcile"),
			UnknownReconcilePayload));
	TestEqual(TEXT("released server-issued ref reconciles as state_unknown"),
		UnknownReconcileRoot->GetStringField(TEXT("lifecycle_status")),
		FString(TEXT("state_unknown")));
	TestTrue(TEXT("unknown reconcile is terminal"),
		UnknownReconcileRoot->GetBoolField(TEXT("terminal")));
	TestFalse(TEXT("unknown reconcile is never retry safe"),
		UnknownReconcileRoot->GetBoolField(TEXT("retry_safe")));
	FString UnknownGeneration;
	const TSharedPtr<FJsonValue> UnknownGenerationValue =
		UnknownReconcileRoot->TryGetField(TEXT("generation"));
	TestTrue(TEXT("unknown reconcile generation keeps an exact integer lexeme"),
		UnknownGenerationValue.IsValid()
			&& UnknownGenerationValue->TryGetString(UnknownGeneration));
	TestEqual(TEXT("generation beyond IEEE-754 remains exact"),
		UnknownGeneration,
		FString(TEXT("9007199254740993")));
	TestEqual(TEXT("released reconcile does not replay native work"),
		Factories->Ports[0]->StartCalls,
		StartsBeforeReconcile);

	const TArray<FString> RetiredOwnerCommands{
		TEXT("editor_close"),
		TEXT("editor_prepare_exit"),
		TEXT("editor_resolve_dirty_resources"),
		TEXT("audio_submix_record_start"),
		TEXT("audio_submix_record_stop"),
		TEXT("audio_submix_record_cancel"),
		TEXT("packaging_run_build"),
		TEXT("packaging_task_status"),
		TEXT("packaging_cancel_task"),
		TEXT("localization_gather_text"),
		TEXT("localization_compile_text"),
		TEXT("world_partition_resave_actors"),
		TEXT("screenshot_viewport"),
		TEXT("screenshot_viewport_buffer"),
		TEXT("pcg_screenshot"),
		TEXT("editor_viewport_screenshot"),
		TEXT("niagara_screenshot"),
		TEXT("modeling_voxel_grid_begin"),
		TEXT("modeling_voxel_grid_add_bounds"),
		TEXT("modeling_voxel_grid_remove_bounds"),
		TEXT("modeling_voxel_grid_apply"),
	};
	for (const FString& CommandId : RetiredOwnerCommands)
	{
		TestNull(
			TEXT("legacy identity is absent from the sole production registry: ")
				+ CommandId,
			Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr);
	}
	return !HasAnyErrors();
}
