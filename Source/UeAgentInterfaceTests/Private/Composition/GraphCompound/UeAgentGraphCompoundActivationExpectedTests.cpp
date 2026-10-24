#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphContract.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphContract.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphContract.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphContract.h"
#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphContract.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"


namespace
{
	using namespace UE::UAI;

	struct FExpectedCapability final
	{
		FString DescriptorId;
		int32 Version = 0;
		FString OperationKind;
	};

	TMap<FString, FExpectedCapability> ExpectedCapabilityContracts()
	{
		TMap<FString, FExpectedCapability> Result;
		const auto AddContracts = [&Result](const auto& Contracts)
		{
			for (const auto& Contract : Contracts)
			{
				Result.Add(
					Contract.GetCapabilityId(),
					{
						Contract.GetDescriptorId(),
						Contract.GetVersion(),
						Contract.GetOperationKind() == TEXT("query")
							? TEXT("query")
							: TEXT("operation"),
					});
			}
		};
		AddContracts(DomainAdapters::BlueprintUmgGraph::
			GetBlueprintUmgGraphCapabilityContracts());
		AddContracts(DomainAdapters::MaterialNiagaraGraph::
			GetMaterialNiagaraGraphCapabilityContracts());
		AddContracts(DomainAdapters::PcgAiNodeGraph::
			GetPcgAiNodeGraphCapabilityContracts());
		AddContracts(DomainAdapters::AnimationRigGraph::
			GetAnimationRigGraphCapabilityContracts());
		AddContracts(DomainAdapters::AudioMediaGraph::
			GetAudioMediaGraphCapabilityContracts());
		return Result;
	}

	void ExpectedPythonRoute(
		const FString& CommandId,
		FString& OutNamespace,
		FString& OutMethod)
	{
		TArray<FString> Segments;
		CommandId.ParseIntoArray(Segments, TEXT("."), true);
		OutNamespace = Segments.IsEmpty() ? FString() : Segments[0];
		OutMethod.Reset();
		for (int32 Index = 1; Index < Segments.Num(); ++Index)
		{
			if (!OutMethod.IsEmpty())
			{
				OutMethod += TEXT("_");
			}
			OutMethod += Segments[Index];
		}
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
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FRecordingQueryPort final
		: public Contract::IUeAgentQueryApplicationPort
	{
	public:
		Contract::FUeAgentQueryResult Submit(
			const Contract::FUeAgentQueryRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) override
		{
			(void)CallerContext;
			(void)Cancellation;
			++Calls;
			LastDescriptor = Request.GetDescriptorIdentity();
			LastPayloadJson = Request.GetPayload().GetJsonValue();
			return Contract::FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Contract::FUeAgentContractValue(
					TEXT("{\"projection\":{\"payload\":{\"ok\":true}}}")));
		}

		int32 Calls = 0;
		TOptional<Contract::FUeAgentDescriptorIdentity> LastDescriptor;
		FString LastPayloadJson;
	};

	class FRecordingOperationPort final
		: public Contract::IUeAgentOperationApplicationPort
	{
	public:
		Contract::FUeAgentOperationResult Submit(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) override
		{
			(void)CallerContext;
			(void)Cancellation;
			++Calls;
			LastDescriptor = Request.GetDescriptorIdentity();
			LastPayloadJson = Request.GetPayload().GetJsonValue();
			LastIdempotencyKey = Request.GetIdempotencyKey();
			return Contract::FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Contract::FUeAgentContractValue(
					TEXT("{\"operation_id\":\"op-gc013\"," 
						"\"projection\":{\"payload\":{\"ok\":true}}}")));
		}

		int32 Calls = 0;
		TOptional<Contract::FUeAgentDescriptorIdentity> LastDescriptor;
		FString LastPayloadJson;
		FString LastIdempotencyKey;
	};

	TOptional<CommandEdge::FUeAgentCommandRequest> RequestFor(
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
			+ TEXT("\",\"correlation_id\":\"corr-gc013\"," 
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
		CommandEdge::FUeAgentCommandProtocolCodec Codec;
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Result;
		Codec.DecodeRequest(Bytes, Result, Diagnostic);
		return Result;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundActivationExpectedTest,
	"UeAgentInterface.Composition.GraphCompound.E0.Activation."
		"ExactDescriptorsAndRoutes",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundActivationExpectedTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	(void)Parameters;

	const TMap<FString, FExpectedCapability> ExpectedContracts =
		ExpectedCapabilityContracts();
	TSet<FString> Expected;
	for (const TPair<FString, FExpectedCapability>& Contract
		: ExpectedContracts)
	{
		Expected.Add(Contract.Key);
	}

	using namespace UE::UAI::Composition::GraphCompound;
	const TArray<FUeAgentGraphCompoundActivationProfile>* CachedGraphProfiles = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetGraphCompoundActivationProfiles(CachedGraphProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const auto& Profiles = *CachedGraphProfiles;
	TestEqual(TEXT("one activation profile per capability"),
		Profiles.Num(), Expected.Num());
	TSet<FString> ActualCommandIds;
	for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
	{
		TestTrue(TEXT("activation profile is complete"), Profile.IsValid());
		TestEqual(TEXT("command id equals capability id"),
			Profile.GetCommandId(), Profile.GetCapabilityId());
		const FExpectedCapability* ExpectedContract =
			ExpectedContracts.Find(Profile.GetCapabilityId());
		TestNotNull(TEXT("activation profile maps one capability contract"),
			ExpectedContract);
		if (ExpectedContract)
		{
			TestEqual(TEXT("descriptor id comes from capability contract"),
				Profile.GetDescriptorIdentity().GetDescriptorId(),
				ExpectedContract->DescriptorId);
			TestEqual(TEXT("descriptor version comes from capability contract"),
				Profile.GetDescriptorIdentity().GetVersion(),
				ExpectedContract->Version);
			TestEqual(TEXT("query/operation semantics are normalized"),
				Profile.GetOperationKind(),
				ExpectedContract->OperationKind);
		}
		TestTrue(TEXT("descriptor identity has a real schema hash"),
			Profile.GetDescriptorIdentity().IsValid());
		FString ExpectedNamespace;
		FString ExpectedMethod;
		ExpectedPythonRoute(
			Profile.GetCommandId(), ExpectedNamespace, ExpectedMethod);
		TestEqual(TEXT("public namespace follows uniform command mapping"),
			Profile.GetNamespace(), ExpectedNamespace);
		TestEqual(TEXT("public method follows uniform command mapping"),
			Profile.GetMethod(), ExpectedMethod);
		TestTrue(TEXT("public namespace is a Python identifier"),
			IsPythonIdentifier(Profile.GetNamespace()));
		TestTrue(TEXT("public method is a Python identifier"),
			IsPythonIdentifier(Profile.GetMethod()));
		ActualCommandIds.Add(Profile.GetCommandId());
	}
	TestEqual(TEXT("activation command ids are unique"),
		ActualCommandIds.Num(), Expected.Num());
	for (const FString& CommandId : Expected)
	{
		TestTrue(TEXT("capability has exact public route: ") + CommandId,
			ActualCommandIds.Contains(CommandId));
	}

	TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
	TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
		Commands;
	FString Error;
	TestTrue(TEXT("Graph/Compound descriptor registry is created"),
		TryCreateGraphCompoundDescriptorRegistry(Descriptors, Error));
	TestTrue(TEXT("Graph/Compound production command registry is created"),
		TryCreateGraphCompoundProductionCommandContractRegistry(
			Commands, Error));
	for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
	{
		if (Descriptors.IsValid())
		{
			TestNotNull(TEXT("exact production descriptor exists: ")
				+ Profile.GetCapabilityId(),
				Descriptors->FindExact(Profile.GetDescriptorIdentity()));
			TestNotNull(TEXT("exact compiled input schema exists: ")
				+ Profile.GetCapabilityId(),
				Descriptors->FindCompiledInputSchemaExact(
					Profile.GetDescriptorIdentity()));
		}
		if (Commands.IsValid())
		{
			const CommandContract::FUeAgentCommandContract* Command =
				Commands->FindExact(Profile.GetCommandId());
			TestNotNull(TEXT("exact production command exists: ")
				+ Profile.GetCapabilityId(), Command);
			if (Command)
			{
				TestEqual(TEXT("manifest namespace matches activation"),
					Command->GetNamespace(), Profile.GetNamespace());
				TestEqual(TEXT("manifest method matches activation"),
					Command->GetMethod(), Profile.GetMethod());
			}
		}
	}
	if (Commands.IsValid())
	{
		FString Manifest;
		Error.Reset();
		TestTrue(TEXT("production registry builds the formal manifest"),
			Commands->BuildManifest(
				CommandContract::GetUaiCommandManifestPluginVersion(),
				CommandContract::GetUaiCommandManifestBuildId(),
				Manifest,
				Error));
		for (const FString& CommandId : Expected)
		{
			TestTrue(TEXT("formal manifest contains Graph/Compound command: ")
				+ CommandId,
				Manifest.Contains(CommandId, ESearchCase::CaseSensitive));
		}
		TSharedPtr<FJsonObject> ManifestObject;
		Error.Reset();
		TestTrue(TEXT("formal manifest is valid JSON"),
			Contract::Json::TryParseObject(
				Manifest, ManifestObject, Error));
		if (ManifestObject.IsValid())
		{
			for (const TSharedPtr<FJsonValue>& Value
				: ManifestObject->GetArrayField(TEXT("commands")))
			{
				const TSharedPtr<FJsonObject> Command = Value->AsObject();
				FString CommandId;
				if (!Command.IsValid()
					|| !Command->TryGetStringField(TEXT("command_id"), CommandId)
					|| !Expected.Contains(CommandId))
				{
					continue;
				}
				TestFalse(TEXT("Graph manifest command hides descriptor routing: ")
					+ CommandId, Command->HasField(TEXT("descriptor"))
						|| Command->HasField(TEXT("descriptor_id"))
						|| Command->HasField(TEXT("descriptor_hash")));
				TestFalse(TEXT("Graph manifest command hides family routing: ")
					+ CommandId, Command->HasField(TEXT("family"))
						|| Command->HasField(TEXT("family_id"))
						|| Command->HasField(TEXT("provider_binding")));
			}
		}

		// The Graph registry owns its command layer and validates the
		// in-memory projection above.
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPublicCommandHandlersExpectedTest,
	"UeAgentInterface.Composition.GraphCompound.E3.Activation."
		"PublicCommandHandlers",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPublicCommandHandlersExpectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::Composition::GraphCompound;
	const TSharedRef<FRecordingQueryPort> QueryPort =
		MakeShared<FRecordingQueryPort>();
	const TSharedRef<FRecordingOperationPort> OperationPort =
		MakeShared<FRecordingOperationPort>();
	const TArray<FUeAgentGraphCompoundActivationProfile>* CachedGraphProfiles = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetGraphCompoundActivationProfiles(CachedGraphProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const auto& Profiles = *CachedGraphProfiles;
	TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
	Contract::FDiagnostic Error;
	TestTrue(TEXT("one thin handler is created for every exact route"),
		TryCreateGraphCompoundCommandHandlers(
			Profiles,
			QueryPort,
			OperationPort,
			Handlers,
			Error));
	TestEqual(TEXT("one public handler per activation profile"),
		Handlers.Num(), Profiles.Num());

	TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
		Commands;
	TestTrue(TEXT("production command registry composes"),
		TryCreateGraphCompoundProductionCommandContractRegistry(
			Commands, Error));
	if (!Commands.IsValid() || Handlers.Num() != Profiles.Num())
	{
		AddError(TEXT("graph_compound_public_handlers_unavailable:") + Error.PublicMessage);
		return false;
	}

	TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>
		HandlerById;
	for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler
		: Handlers)
	{
		TestFalse(TEXT("handler command ownership is unique"),
			HandlerById.Contains(Handler->GetCommandId()));
		HandlerById.Add(Handler->GetCommandId(), Handler);
	}

	const FString ParamsJson =
		TEXT("{\"context\":{\"context_revision\":\"ctx-gc013\"},"
			 "\"input\":{},\"projection\":{},\"target\":\"/Game/Test\"}");
	const Contract::FUeAgentCallerContext Caller(
		TEXT("gc013-public-edge"),
		{TEXT("uai:query"), TEXT("uai:authoring")},
		TEXT("corr-gc013"));
	int32 ExpectedQueryCalls = 0;
	int32 ExpectedOperationCalls = 0;
	for (const FUeAgentGraphCompoundActivationProfile& Profile
		: Profiles)
	{
		const CommandContract::FUeAgentCommandContract* Command =
			Commands->FindExact(Profile.GetCommandId());
		TestNotNull(TEXT("selected route has a command contract"), Command);
		if (!Command || !HandlerById.Contains(Profile.GetCommandId()))
		{
			continue;
		}
		const FString RequestId = TEXT("req-gc013-")
			+ Profile.GetMethod();
		const TOptional<CommandEdge::FUeAgentCommandRequest> Request =
			RequestFor(
				Profile.GetCommandId(),
				Command->GetBindingHash(),
				ParamsJson,
				RequestId);
		TestTrue(TEXT("public command request decodes"), Request.IsSet());
		if (!Request.IsSet())
		{
			continue;
		}
		const CommandEdge::FUeAgentCommandResult Result =
			HandlerById.FindChecked(Profile.GetCommandId())->Submit(
				Request.GetValue(),
				Caller,
				MakeShared<FNeverCancelled>());
		TestTrue(TEXT("thin handler returns application result"),
			Result.IsOk());
		if (Profile.GetOperationKind() == TEXT("query"))
		{
			++ExpectedQueryCalls;
			TestTrue(TEXT("query handler forwards exact descriptor"),
				QueryPort->LastDescriptor.IsSet()
					&& QueryPort->LastDescriptor.GetValue()
						== Profile.GetDescriptorIdentity());
			TestEqual(TEXT("query handler forwards params unchanged"),
				QueryPort->LastPayloadJson, ParamsJson);
		}
		else
		{
			++ExpectedOperationCalls;
			TestTrue(TEXT("operation handler forwards exact descriptor"),
				OperationPort->LastDescriptor.IsSet()
					&& OperationPort->LastDescriptor.GetValue()
						== Profile.GetDescriptorIdentity());
			TestEqual(TEXT("operation handler forwards params unchanged"),
				OperationPort->LastPayloadJson, ParamsJson);
			TestEqual(TEXT("operation idempotency projection is deterministic"),
				OperationPort->LastIdempotencyKey,
				FString(TEXT("uai-command:")) + RequestId);
		}
	}
	TestEqual(TEXT("all discovered query routes submit to C3Q"),
		QueryPort->Calls, ExpectedQueryCalls);
	TestEqual(TEXT("all discovered operation routes submit to C3O"),
		OperationPort->Calls, ExpectedOperationCalls);
	return !HasAnyErrors();
}
