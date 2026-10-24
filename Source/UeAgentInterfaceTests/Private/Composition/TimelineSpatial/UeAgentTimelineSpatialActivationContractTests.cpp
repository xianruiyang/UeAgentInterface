#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Module/CommandRegistration/AnimationCurveCommands/UeAgentAnimationCurveCommandRegistration.h"
#include "Module/CommandRegistration/AnimationRigCommands/UeAgentAnimationRigCommandRegistration.h"
#include "Module/CommandRegistration/TimelineSequenceCommands/UeAgentTimelineSequenceCommandRegistration.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialCommandHandlers.h"
#include "CommandEdge/UeAgentCommandProtocol.h"


namespace
{
	FString FixturePath(const FString& RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private"),
			RelativePath));
	}

	TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test,
		const FString& RelativePath,
		const FString& Label)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load ") + Label,
				FFileHelper::LoadFileToString(
					Text,
					*FixturePath(RelativePath))))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse ") + Label,
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TMap<FString, TSharedPtr<FJsonObject>> ObjectsByStringField(
		FAutomationTestBase* Test,
		const TArray<TSharedPtr<FJsonValue>>& Values,
		const TCHAR* Field,
		const FString& Label)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Result;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Test->TestTrue(
					Label + TEXT(" entry is an object"),
					Object.IsValid()))
			{
				continue;
			}
			FString Key;
			if (!Test->TestTrue(
					Label + TEXT(" entry has key ") + Field,
					Object->TryGetStringField(Field, Key)
						&& !Key.IsEmpty()))
			{
				continue;
			}
			Test->TestFalse(
				Label + TEXT(" key is unique: ") + Key,
				Result.Contains(Key));
			Result.Add(Key, Object);
		}
		return Result;
	}

	TSet<FString> ReadStringSet(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TSet<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	bool IsPythonIdentifier(const FString& Value)
	{
		if (Value.IsEmpty()
			|| !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
		{
			return false;
		}
		for (int32 Index = 1; Index < Value.Len(); ++Index)
		{
			if (!(FChar::IsAlnum(Value[Index])
				|| Value[Index] == TEXT('_')))
			{
				return false;
			}
		}
		return true;
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

	class FRecordingQueryPort final
		: public UE::UAI::Contract::IUeAgentQueryApplicationPort
	{
	public:
		UE::UAI::Contract::FUeAgentQueryResult Submit(
			const UE::UAI::Contract::FUeAgentQueryRequest& Request,
			const UE::UAI::Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				UE::UAI::Contract::IUeAgentCancellationSignal>&
				Cancellation) override
		{
			++Calls;
			LastDescriptor = Request.GetDescriptorIdentity();
			LastRequestId = Request.GetRequestId();
			LastPayloadJson = Request.GetPayload().GetJsonValue();
			Descriptors.Add(Request.GetDescriptorIdentity());
			RequestIds.Add(Request.GetRequestId());
			Payloads.Add(Request.GetPayload().GetJsonValue());
			return UE::UAI::Contract::FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				UE::UAI::Contract::FUeAgentContractValue(
					TEXT("{\"projection\":{\"payload\":{\"ok\":true}}}")));
		}

		int32 Calls = 0;
		TOptional<UE::UAI::Contract::FUeAgentDescriptorIdentity>
			LastDescriptor;
		FString LastRequestId;
		FString LastPayloadJson;
		TArray<UE::UAI::Contract::FUeAgentDescriptorIdentity> Descriptors;
		TArray<FString> RequestIds;
		TArray<FString> Payloads;
	};

	class FRecordingOperationPort final
		: public UE::UAI::Contract::IUeAgentOperationApplicationPort
	{
	public:
		UE::UAI::Contract::FUeAgentOperationResult Submit(
			const UE::UAI::Contract::FUeAgentOperationRequest& Request,
			const UE::UAI::Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				UE::UAI::Contract::IUeAgentCancellationSignal>&
				Cancellation) override
		{
			++Calls;
			LastDescriptor = Request.GetDescriptorIdentity();
			LastRequestId = Request.GetRequestId();
			LastIdempotencyKey = Request.GetIdempotencyKey();
			LastPayloadJson = Request.GetPayload().GetJsonValue();
			Descriptors.Add(Request.GetDescriptorIdentity());
			RequestIds.Add(Request.GetRequestId());
			IdempotencyKeys.Add(Request.GetIdempotencyKey());
			Payloads.Add(Request.GetPayload().GetJsonValue());
			return UE::UAI::Contract::FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				UE::UAI::Contract::FUeAgentContractValue(
					TEXT("{\"operation_id\":\"op-ts008\"," 
						"\"projection\":{\"payload\":{\"ok\":true}}}")));
		}

		int32 Calls = 0;
		TOptional<UE::UAI::Contract::FUeAgentDescriptorIdentity>
			LastDescriptor;
		FString LastRequestId;
		FString LastIdempotencyKey;
		FString LastPayloadJson;
		TArray<UE::UAI::Contract::FUeAgentDescriptorIdentity> Descriptors;
		TArray<FString> RequestIds;
		TArray<FString> IdempotencyKeys;
		TArray<FString> Payloads;
	};

	TOptional<UE::UAI::CommandEdge::FUeAgentCommandRequest>
	RequestFor(
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
			+ TEXT("\",\"correlation_id\":\"corr-ts008\"," 
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

	bool ValidateActivationFixture(
		FAutomationTestBase* Test,
		TSharedPtr<FJsonObject>& OutActivation,
		TMap<FString, TSharedPtr<FJsonObject>>& OutProfiles)
	{
		OutActivation = LoadFixture(
			Test,
			TEXT(
				"Composition/TimelineSpatial/Fixtures/"
				"timeline_spatial_activation_contract_cases.json"),
			TEXT("TimelineSpatial activation fixture"));
		if (!OutActivation.IsValid())
		{
			return false;
		}

		Test->TestEqual(
			TEXT("activation fixture schema"),
			OutActivation->GetStringField(TEXT("schema")),
			FString(TEXT(
				"uai.composition.timeline_spatial.activation_fixture.v1")));
		const TArray<TSharedPtr<FJsonValue>>& ProfileValues =
			OutActivation->GetArrayField(TEXT("profiles"));
		Test->TestFalse(
			TEXT("TimelineSpatial activation fixture is non-empty"),
			ProfileValues.IsEmpty());
		OutProfiles = ObjectsByStringField(
			Test,
			ProfileValues,
			TEXT("capability_id"),
			TEXT("activation profile"));
		Test->TestEqual(
			TEXT("activation capability ids are unique"),
			OutProfiles.Num(),
			ProfileValues.Num());

		TSet<FString> DescriptorIds;
		TSet<FString> TypedOperationIds;
		TSet<FString> ProviderBindings;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair
			: OutProfiles)
		{
			const TSharedPtr<FJsonObject>& Profile = Pair.Value;
			const FString DescriptorId =
				Profile->GetStringField(TEXT("descriptor_id"));
			const FString TypedOperationId =
				Profile->GetStringField(TEXT("typed_operation_id"));
			const FString ProviderBinding =
				Profile->GetStringField(TEXT("provider_binding"));
			const FString ContextSource =
				Profile->GetStringField(TEXT("context_source"));
			const FString OracleKind =
				Profile->GetStringField(TEXT("oracle_kind"));
			Test->TestFalse(
				TEXT("descriptor id is unique: ") + DescriptorId,
				DescriptorIds.Contains(DescriptorId));
			Test->TestFalse(
				TEXT("typed operation id is unique: ") + TypedOperationId,
				TypedOperationIds.Contains(TypedOperationId));
			Test->TestFalse(
				TEXT("provider binding is unique: ") + ProviderBinding,
				ProviderBindings.Contains(ProviderBinding));
			Test->TestTrue(
				TEXT("context source is explicit: ") + Pair.Key,
				ContextSource == TEXT("public_required")
					|| ContextSource == TEXT("immutable_application"));
			Test->TestFalse(
				TEXT("oracle is explicit: ") + Pair.Key,
				OracleKind.IsEmpty());

			const TSet<FString> Required =
				ReadStringSet(Profile, TEXT("input_required"));
			const TSet<FString> Optional =
				ReadStringSet(Profile, TEXT("input_optional"));
			for (const FString& Field : Required)
			{
				Test->TestFalse(
					TEXT("input field has one requiredness: ")
						+ Pair.Key + TEXT(".") + Field,
					Optional.Contains(Field));
			}

			DescriptorIds.Add(DescriptorId);
			TypedOperationIds.Add(TypedOperationId);
			ProviderBindings.Add(ProviderBinding);
		}

		struct FSourceFixture
		{
			const TCHAR* AdapterId;
			const TCHAR* Path;
			const TCHAR* Label;
		};
		const FSourceFixture Sources[] = {
			{
				TEXT("world_spatial"),
				TEXT(
					"DomainAdapters/WorldSpatial/Fixtures/"
					"world_spatial_contract_cases.json"),
				TEXT("WorldSpatial source fixture"),
			},
			{
				TEXT("timeline_animation"),
				TEXT(
					"DomainAdapters/TimelineAnimation/Fixtures/"
					"timeline_animation_contract_cases.json"),
				TEXT("TimelineAnimation source fixture"),
			},
			{
				TEXT("rig_simulation_spatial"),
				TEXT(
					"DomainAdapters/RigSimulationSpatial/Fixtures/"
					"rig_simulation_spatial_contract_cases.json"),
				TEXT("RigSimulationSpatial source fixture"),
			},
		};
		int32 SourceCapabilityCount = 0;
		for (const FSourceFixture& SourceSpec : Sources)
		{
			const TSharedPtr<FJsonObject> Source = LoadFixture(
				Test,
				SourceSpec.Path,
				SourceSpec.Label);
			if (!Source.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& CapabilityValues =
				Source->GetArrayField(TEXT("capabilities"));
			SourceCapabilityCount += CapabilityValues.Num();
			for (const TSharedPtr<FJsonValue>& Value : CapabilityValues)
			{
				const TSharedPtr<FJsonObject> Capability = Value->AsObject();
				const FString CapabilityId =
					Capability->GetStringField(TEXT("capability_id"));
				const TSharedPtr<FJsonObject>* ProfilePtr =
					OutProfiles.Find(CapabilityId);
				Test->TestNotNull(
					TEXT("source capability has one activation profile: ")
						+ CapabilityId,
					ProfilePtr);
				if (!ProfilePtr)
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& Profile = *ProfilePtr;
				Test->TestEqual(
					TEXT("source adapter remains authoritative: ")
						+ CapabilityId,
					Profile->GetStringField(TEXT("adapter_id")),
					FString(SourceSpec.AdapterId));
				Test->TestEqual(
					TEXT("descriptor identity is preserved: ")
						+ CapabilityId,
					Profile->GetStringField(TEXT("descriptor_id")),
					Capability->GetStringField(TEXT("descriptor_id")));
				Test->TestEqual(
					TEXT("operation kind is preserved: ") + CapabilityId,
					Profile->GetStringField(TEXT("operation_kind")),
					Capability->GetStringField(TEXT("operation_kind")));
				Test->TestEqual(
					TEXT("effect class is preserved: ") + CapabilityId,
					Profile->GetStringField(TEXT("effect_class")),
					Capability->GetStringField(TEXT("effect_class")));
				Test->TestEqual(
					TEXT("typed family is preserved: ") + CapabilityId,
					Profile->GetStringField(TEXT("family_id")),
					Capability->GetStringField(TEXT("port_family_id")));
				FString SourceOracle;
				if (Capability->TryGetStringField(
						TEXT("oracle_kind"),
						SourceOracle))
				{
					Test->TestEqual(
						TEXT("existing oracle is preserved: ") + CapabilityId,
						Profile->GetStringField(TEXT("oracle_kind")),
						SourceOracle);
				}
			}
		}
		Test->TestEqual(
			TEXT("activation fixture is exactly the three source inventories"),
			SourceCapabilityCount,
			OutProfiles.Num());
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialActivationContractTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3.ActivationContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialActivationContractTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> Activation;
	TMap<FString, TSharedPtr<FJsonObject>> Profiles;
	if (!ValidateActivationFixture(this, Activation, Profiles))
	{
		return false;
	}

	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Composition::TimelineSpatial;
	using namespace UE::UAI::Contract;
	const TArray<FUeAgentTimelineSpatialActivationProfile>* CachedProfiles = nullptr;
	FDiagnostic ProfileError;
	if (!TryGetTimelineSpatialActivationProfiles(CachedProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const auto& Routes = *CachedProfiles;
	TestEqual(
		TEXT("production routes match the activation fixture"),
		Routes.Num(),
		Profiles.Num());
	TSet<FString> ProductionCapabilities;
	for (const FUeAgentTimelineSpatialActivationProfile& Route : Routes)
	{
		TestTrue(TEXT("production activation profile is complete"), Route.IsValid());
		TestFalse(
			TEXT("production capability owner is unique: ") + Route.CapabilityId,
			ProductionCapabilities.Contains(Route.CapabilityId));
		ProductionCapabilities.Add(Route.CapabilityId);
		const TSharedPtr<FJsonObject>* Expected = Profiles.Find(Route.CapabilityId);
		TestNotNull(
			TEXT("production route is declared by the activation fixture: ")
				+ Route.CapabilityId,
			Expected);
		if (!Expected)
		{
			continue;
		}
		TestEqual(
			TEXT("production descriptor id"),
			Route.DescriptorIdentity.GetDescriptorId(),
			(*Expected)->GetStringField(TEXT("descriptor_id")));
		TestEqual(
			TEXT("production descriptor version"),
			Route.DescriptorIdentity.GetVersion(),
			1);
		TestEqual(
			TEXT("production adapter binding"),
			Route.AdapterId,
			(*Expected)->GetStringField(TEXT("adapter_id")));
		TestEqual(
			TEXT("production family binding"),
			Route.FamilyId,
			(*Expected)->GetStringField(TEXT("family_id")));
		TestEqual(
			TEXT("production typed operation"),
			Route.TypedOperationId,
			(*Expected)->GetStringField(TEXT("typed_operation_id")));
		TestEqual(
			TEXT("production provider binding"),
			Route.ProviderBinding,
			(*Expected)->GetStringField(TEXT("provider_binding")));
		TestEqual(
			TEXT("production operation kind"),
			Route.OperationKind,
			(*Expected)->GetStringField(TEXT("operation_kind")));
		TestEqual(
			TEXT("production effect class"),
			Route.EffectClass,
			(*Expected)->GetStringField(TEXT("effect_class")));
		TestEqual(
			TEXT("production context source"),
			Route.ContextSource,
			(*Expected)->GetStringField(TEXT("context_source")));
		TestEqual(
			TEXT("production target requirement"),
			Route.bRequiresTarget,
			(*Expected)->GetBoolField(TEXT("requires_target")));
		TestEqual(
			TEXT("production input requirement"),
			Route.bRequiresInput,
			(*Expected)->GetBoolField(TEXT("requires_input")));
		TestEqual(
			TEXT("production projection support"),
			Route.bSupportsProjection,
			(*Expected)->GetBoolField(TEXT("supports_projection")));
		TestEqual(
			TEXT("production oracle"),
			Route.OracleKind,
			(*Expected)->GetStringField(TEXT("oracle_kind")));
		TestEqual(
			TEXT("command id equals capability id"),
			Route.CommandId,
			Route.CapabilityId);
		TestTrue(
			TEXT("Python namespace is static"),
			IsPythonIdentifier(Route.Namespace));
		TestTrue(
			TEXT("Python method is static"),
			IsPythonIdentifier(Route.Method));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Commands;
	FDiagnostic Error;
	const bool bCommandsCreated = TestTrue(
		TEXT("base and TimelineSpatial command contracts compose"),
		TryCreateTimelineSpatialProductionCommandContractRegistry(
			Commands,
			Error));
	if (bCommandsCreated && Commands.IsValid())
	{
		for (const FUeAgentTimelineSpatialActivationProfile& Route : Routes)
		{
			const UE::UAI::CommandContract::FUeAgentCommandContract* Command =
				Commands->FindExact(Route.CommandId);
			TestNotNull(
				TEXT("production command is registered: ") + Route.CapabilityId,
				Command);
			if (!Command)
			{
				continue;
			}
			TestEqual(TEXT("manifest namespace"), Command->GetNamespace(), Route.Namespace);
			TestEqual(TEXT("manifest method"), Command->GetMethod(), Route.Method);
			TMap<FString, const UE::UAI::CommandContract::FUeAgentCommandParameter*>
				CommandParameters;
			for (const UE::UAI::CommandContract::FUeAgentCommandParameter& Parameter
				: Command->GetParameters())
			{
				CommandParameters.Add(Parameter.GetName(), &Parameter);
			}
			TestEqual(
				TEXT("target parameter presence"),
				CommandParameters.Contains(TEXT("target")),
				Route.bRequiresTarget);
			const auto* Input = CommandParameters.FindRef(TEXT("input"));
			TestNotNull(TEXT("input wrapper is present"), Input);
			if (Input)
			{
				TestEqual(TEXT("input requiredness"), Input->IsRequired(), Route.bRequiresInput);
			}
			const auto* Context = CommandParameters.FindRef(TEXT("context"));
			TestNotNull(TEXT("context wrapper is present"), Context);
			if (Context)
			{
				TestEqual(
					TEXT("public context requiredness"),
					Context->IsRequired(),
					Route.ContextSource == TEXT("public_required"));
			}
			TestEqual(
				TEXT("projection parameter presence"),
				CommandParameters.Contains(TEXT("projection")),
				Route.bSupportsProjection);
		}
		TestNull(
			TEXT("TimelineSpatial does not re-own the active HJSON command"),
			Commands->FindExact(TEXT("content_graph.import_workspace")));

		FString Manifest;
		Error.Reset();
		TestTrue(
			TEXT("complete production registry builds the formal manifest"),
			Commands->BuildManifest(
				GetUaiCommandManifestPluginVersion(),
				GetUaiCommandManifestBuildId(),
				Manifest,
				Error));
		TestFalse(
			TEXT("TimelineSpatial-local manifest omits the outer active HJSON command"),
			Manifest.Contains(
				TEXT("content_graph.import_workspace"),
				ESearchCase::CaseSensitive));
		TestFalse(
			TEXT("formal manifest does not expose descriptor identities"),
			Manifest.Contains(TEXT("descriptor"), ESearchCase::IgnoreCase));
		TestFalse(
			TEXT("formal manifest does not expose family routing"),
			Manifest.Contains(TEXT("family"), ESearchCase::IgnoreCase));

		// This local registry is the additive base consumed by the
		// LongRunningSession legacy-layer contract. The current HJSON command
		// and complete committed manifest are owned by the outer active
		// composition and verified there.
	}

	Error.Reset();
	const bool bDescriptorsCreated =
		TryCreateTimelineSpatialDescriptorRegistry(Descriptors, Error);
	if (!bDescriptorsCreated || !Descriptors.IsValid())
	{
		AddError(TEXT("timeline_spatial_descriptor_registry_unavailable:") + Error.PublicMessage);
		return true;
	}
	for (const FUeAgentTimelineSpatialActivationProfile& Route : Routes)
	{
		const FUeAgentOperationDescriptor* Descriptor =
			Descriptors->FindExact(Route.DescriptorIdentity);
		TestNotNull(
			TEXT("production descriptor is registered: ") + Route.CapabilityId,
			Descriptor);
		if (Descriptor && Route.OperationKind == TEXT("query"))
		{
			TestTrue(TEXT("query has exact C3Q provider binding"), Descriptor->GetQueryBinding().IsSet());
			if (Descriptor->GetQueryBinding().IsSet())
			{
				TestEqual(
					TEXT("query provider binding matches activation profile"),
					Descriptor->GetQueryBinding()->GetProviderBinding(),
					Route.ProviderBinding);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialPublicCommandHandlersTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3.PublicCommandHandlers",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialPublicCommandHandlersTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::TimelineSpatial;
	const TSharedRef<FRecordingQueryPort> QueryPort =
		MakeShared<FRecordingQueryPort>();
	const TSharedRef<FRecordingOperationPort> OperationPort =
		MakeShared<FRecordingOperationPort>();
	TArray<TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>> Handlers;
	UE::UAI::Contract::FDiagnostic Error;
	const TArray<FUeAgentTimelineSpatialActivationProfile>* Profiles = nullptr;
	if (!TryGetTimelineSpatialActivationProfiles(Profiles, Error))
	{
		AddError(Error.PublicMessage);
		return false;
	}
	TestTrue(
		TEXT("one thin handler is created for every TimelineSpatial route"),
		TryCreateTimelineSpatialCommandHandlers(
			*Profiles,
			QueryPort,
			OperationPort,
			Handlers,
			Error));
	TestEqual(
		TEXT("every production route has one public handler"),
		Handlers.Num(),
		Profiles->Num());
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Commands;
	TestTrue(
		TEXT("production command registry composes"),
		TryCreateTimelineSpatialProductionCommandContractRegistry(
			Commands,
			Error));
	if (!Commands.IsValid()
		|| Handlers.Num() != Profiles->Num())
	{
		AddError(TEXT("timeline_spatial_public_handlers_unavailable:") + Error.PublicMessage);
		return false;
	}

	TMap<FString, TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>>
		HandlerById;
	for (const TSharedRef<UE::UAI::CommandEdge::IUeAgentCommandHandler>& Handler
		: Handlers)
	{
		TestFalse(TEXT("handler command ownership is unique"), HandlerById.Contains(Handler->GetCommandId()));
		HandlerById.Add(Handler->GetCommandId(), Handler);
	}
	const FString ParamsJson =
		TEXT("{\"context\":{\"context_revision\":\"ctx-1\"},"
			 "\"input\":{},\"projection\":{},\"target\":\"/Game/Test\"}");
	const UE::UAI::Contract::FUeAgentCallerContext Caller(
		TEXT("ts008-public-edge"),
		{TEXT("uai:query"), TEXT("uai:authoring")},
		TEXT("corr-ts008"));
	int32 ExpectedQueryCalls = 0;
	int32 ExpectedOperationCalls = 0;
	for (const FUeAgentTimelineSpatialActivationProfile& Route
		: *Profiles)
	{
		const UE::UAI::CommandContract::FUeAgentCommandContract* Contract =
			Commands->FindExact(Route.CommandId);
		TestNotNull(TEXT("selected route has a command contract"), Contract);
		if (!Contract)
		{
			continue;
		}
		const FString RequestId = TEXT("req-ts008-") + Route.Method;
		const auto Request = RequestFor(
			Route.CommandId,
			Contract->GetBindingHash(),
			ParamsJson,
			RequestId);
		TestTrue(TEXT("public command request decodes"), Request.IsSet());
		if (Request.IsSet())
		{
			const auto Result = HandlerById.FindChecked(Route.CommandId)->Submit(
				Request.GetValue(),
				Caller,
				MakeShared<FNeverCancelled>());
			TestTrue(TEXT("thin handler returns application result"), Result.IsOk());
			if (Route.OperationKind == TEXT("query"))
			{
				++ExpectedQueryCalls;
				TestTrue(
					TEXT("query handler forwards exact descriptor"),
					QueryPort->LastDescriptor.IsSet()
						&& QueryPort->LastDescriptor.GetValue()
							== Route.DescriptorIdentity);
				TestEqual(
					TEXT("query handler forwards params unchanged"),
					QueryPort->LastPayloadJson,
					ParamsJson);
			}
			else
			{
				++ExpectedOperationCalls;
				TestTrue(
					TEXT("operation handler forwards exact descriptor"),
					OperationPort->LastDescriptor.IsSet()
						&& OperationPort->LastDescriptor.GetValue()
							== Route.DescriptorIdentity);
				TestEqual(
					TEXT("operation handler forwards params unchanged"),
					OperationPort->LastPayloadJson,
					ParamsJson);
				TestEqual(
					TEXT("operation handler owns deterministic idempotency projection"),
					OperationPort->LastIdempotencyKey,
					FString(TEXT("uai-command:")) + RequestId);
			}
		}
	}
	TestEqual(
		TEXT("all discovered query routes submit to C3Q application"),
		QueryPort->Calls,
		ExpectedQueryCalls);
	TestEqual(
		TEXT("all discovered operation routes submit to C3O application"),
		OperationPort->Calls,
		ExpectedOperationCalls);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialUnifiedOperationBindingContractTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3.UnifiedOperationBinding",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialUnifiedOperationBindingContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::TimelineSpatial;
	const TArray<FUeAgentTimelineSpatialActivationProfile>* Profiles = nullptr;
	UE::UAI::Contract::FDiagnostic Error;
	if (!TryGetTimelineSpatialActivationProfiles(Profiles, Error))
	{
		AddError(Error.PublicMessage);
		return false;
	}
	TestEqual(
		TEXT("all effectful TimelineSpatial profiles use C3O"),
		Profiles->FilterByPredicate(
			[](const UE::UAI::Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile& Profile)
			{
				return Profile.OperationKind == TEXT("operation");
			}).Num(),
		37);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTimelineSpatialCreateModelResultRouteTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3.CreateModelResultRoutes",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTimelineSpatialCreateModelResultRouteTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::TimelineSpatial;
	const TArray<FUeAgentTimelineSpatialActivationProfile>* CachedProfiles = nullptr;
	UE::UAI::Contract::FDiagnostic Error;
	if (!TryGetTimelineSpatialActivationProfiles(CachedProfiles, Error))
	{
		AddError(Error.PublicMessage);
		return false;
	}
	const auto& Profiles = *CachedProfiles;
	const auto AssertRoute = [this, &Profiles](
		const TCHAR* CommandId,
		const TCHAR* FieldName)
	{
		const FUeAgentTimelineSpatialActivationProfile* Profile =
			Profiles.FindByPredicate(
				[CommandId](const auto& Candidate)
				{
					return Candidate.CommandId == CommandId;
				});
		TestNotNull(FString(TEXT("create route exists: ")) + CommandId, Profile);
		if (!Profile) return;
		TestEqual(
			FString(TEXT("one compact result field: ")) + CommandId,
			Profile->ModelResultFields.Num(),
			1);
		if (Profile->ModelResultFields.Num() != 1) return;
		const auto& Field = Profile->ModelResultFields[0];
		TestEqual(TEXT("public result name"), Field.Name, FString(FieldName));
		TestTrue(
			TEXT("public result comes from verified domain result"),
			Field.SourcePath == TArray<FString>{
				TEXT("projection"),
				TEXT("payload"),
				TEXT("result"),
				FString(FieldName),
			});
	};

	AssertRoute(TEXT("animation.montage.create"), TEXT("target_ref"));
	AssertRoute(TEXT("timeline.sequence.create"), TEXT("target_ref"));
	AssertRoute(
		TEXT("rig.retarget.assets.generate"),
		TEXT("generated_asset_refs"));

	using UE::UAI::CommandContract::FUeAgentCommandDeclaration;
	using UE::UAI::Module::CommandRegistration::Active::
		IUeAgentActiveCommandContribution;
	const auto AssertRegistration = [this](
		const TCHAR* CommandId,
		const TCHAR* FieldName,
		const TFunction<bool(
			TSharedPtr<IUeAgentActiveCommandContribution>&,
			FString&)>& Factory)
	{
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FString Error;
		if (!TestTrue(
				FString(TEXT("registration composes: ")) + CommandId,
				Factory(Contribution, Error))
			|| !Contribution.IsValid())
		{
			AddError(Error);
			return;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		Contribution->CollectDeclarations(Declarations);
		const FUeAgentCommandDeclaration* Declaration =
			Declarations.FindByPredicate(
				[CommandId](const auto& Candidate)
				{
					return Candidate.CommandId == CommandId;
				});
		TestNotNull(
			FString(TEXT("registered declaration exists: ")) + CommandId,
			Declaration);
		if (!Declaration) return;
		TestEqual(
			FString(TEXT("registered compact result count: ")) + CommandId,
			Declaration->ModelResultFields.Num(),
			1);
		if (Declaration->ModelResultFields.Num() == 1)
		{
			TestEqual(
				TEXT("registered compact result name"),
				Declaration->ModelResultFields[0].Name,
				FString(FieldName));
		}
	};

	AssertRegistration(
		TEXT("animation.blend_space.asset.create"),
		TEXT("target_ref"),
		UE::UAI::Module::CommandRegistration::AnimationCurveCommands::
			TryCreateContribution);
	AssertRegistration(
		TEXT("animation.montage.create"),
		TEXT("target_ref"),
		UE::UAI::Module::CommandRegistration::AnimationCurveCommands::
			TryCreateContribution);
	AssertRegistration(
		TEXT("timeline.sequence.create"),
		TEXT("target_ref"),
		UE::UAI::Module::CommandRegistration::TimelineSequenceCommands::
			TryCreateContribution);
	AssertRegistration(
		TEXT("rig.retarget.assets.generate"),
		TEXT("generated_asset_refs"),
		UE::UAI::Module::CommandRegistration::AnimationRigCommands::
			TryCreateContribution);
	return true;
}
