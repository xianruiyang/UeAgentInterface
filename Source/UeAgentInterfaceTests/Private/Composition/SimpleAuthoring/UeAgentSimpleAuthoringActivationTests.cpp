#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"

// The public edge must bind only to the shared operation application port.
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringCommandHandlers.h"
#include "CommandEdge/UeAgentCommandProtocol.h"

#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::Contract;
	namespace CommandEdge = UE::UAI::CommandEdge;

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
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	class FRecordingOperationPort final
		: public IUeAgentOperationApplicationPort
	{
	public:
		FUeAgentOperationResult Submit(
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>&
				Cancellation) override
		{
			++Calls;
			LastDescriptor = Request.GetDescriptorIdentity();
			LastRequestId = Request.GetRequestId();
			LastIdempotencyKey = Request.GetIdempotencyKey();
			LastPayloadJson = Request.GetPayload().GetJsonValue();
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(
					TEXT("{\"operation_id\":\"op-command-edge\","
						 "\"projection\":{\"profile_id\":\"default\","
						 "\"payload\":{\"applied\":true}}}")));
		}

		int32 Calls = 0;
		TOptional<FUeAgentDescriptorIdentity> LastDescriptor;
		FString LastRequestId;
		FString LastIdempotencyKey;
		FString LastPayloadJson;
	};

	TOptional<CommandEdge::FUeAgentCommandRequest> RequestFor(
		const FString& CommandId,
		const FString& BindingHash,
		const FString& Params)
	{
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ BindingHash
			+ TEXT("\",\"command_id\":\"")
			+ CommandId
			+ TEXT("\",\"correlation_id\":\"corr-sa008\","
				  "\"message_kind\":\"command_request\",\"params\":")
			+ Params
			+ TEXT(",\"request_id\":\"req-sa008\","
				  "\"schema\":\"uai.command.v1\",\"version\":1}");
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
	FUeAgentSimpleAuthoringCommandContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.CommandContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringCommandContractTest::RunTest(
	const FString& Parameters)
{
	const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
		GetSimpleAuthoringCommandRoutes();
	TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
	TSharedPtr<const FUeAgentCommandContractRegistry> Commands;
	FString Error;
	TestTrue(
		TEXT("five family descriptor registries compose exactly once"),
		TryCreateSimpleAuthoringDescriptorRegistry(
			Descriptors,
			Error));
	TestTrue(
		TEXT("production command registry composes from Core plus Simple Authoring"),
		TryCreateProductionCommandContractRegistry(
			Commands,
			Error));
	if (!Descriptors.IsValid() || !Commands.IsValid())
	{
		AddError(TEXT("simple_authoring_registry_unavailable:") + Error);
		return false;
	}

	TSet<FString> Families;
	TSet<FString> CommandIds;
	TSet<FString> DescriptorIds;
	TSet<FString> PythonBindings;
	for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
	{
		TestTrue(TEXT("route is complete"), Route.IsValid());
		TestEqual(
			TEXT("command id is the capability id"),
			Route.CommandId,
			Route.CapabilityId);
		TestFalse(
			TEXT("command id has one owner"),
			CommandIds.Contains(Route.CommandId));
		TestFalse(
			TEXT("descriptor has one public authoring route"),
			DescriptorIds.Contains(
				Route.DescriptorIdentity.GetDescriptorId()));
		const FString PythonBinding =
			Route.Namespace + TEXT(".") + Route.Method;
		TestFalse(
			TEXT("Python namespace and method are collision-free"),
			PythonBindings.Contains(PythonBinding));
		TestTrue(
			TEXT("namespace is a static Python identifier"),
			IsPythonIdentifier(Route.Namespace));
		TestTrue(
			TEXT("method is a static Python identifier"),
			IsPythonIdentifier(Route.Method));

		CommandIds.Add(Route.CommandId);
		DescriptorIds.Add(
			Route.DescriptorIdentity.GetDescriptorId());
		PythonBindings.Add(PythonBinding);
		Families.Add(Route.FamilyId);

		const FUeAgentOperationDescriptor* Descriptor =
			Descriptors->FindExact(Route.DescriptorIdentity);
		const FUeAgentCommandContract* Command =
			Commands->FindExact(Route.CommandId);
		TestNotNull(
			TEXT("route resolves one exact operation descriptor"),
			Descriptor);
		TestNotNull(
			TEXT("route exists in the production command registry"),
			Command);
		if (Descriptor)
		{
			TestEqual(
				TEXT("route capability matches the descriptor"),
				Route.CapabilityId,
				Descriptor->GetCapabilityId());
			TestEqual(
				TEXT("route family matches the descriptor domain"),
				Route.FamilyId,
				Descriptor->GetDomainId());
		}
		if (Command)
		{
			TestEqual(
				TEXT("manifest namespace comes from the route"),
				Command->GetNamespace(),
				Route.Namespace);
			TestEqual(
				TEXT("manifest method comes from the route"),
				Command->GetMethod(),
				Route.Method);
		}
	}

	for (const FString& Family : TArray<FString>{
		TEXT("world"),
		TEXT("landscape"),
		TEXT("mesh"),
		TEXT("asset_data"),
		TEXT("editor_project"),
	})
	{
		TestTrue(
			TEXT("Simple Authoring family is present: ") + Family,
			Families.Contains(Family));
	}

	FString Manifest;
	Error.Reset();
	TestTrue(
		TEXT("Simple Authoring registry builds its local manifest"),
		Commands->BuildManifest(
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			Manifest,
			Error));
	TestFalse(
		TEXT("manifest does not expose descriptor identities"),
		Manifest.Contains(TEXT("descriptor"), ESearchCase::IgnoreCase));
	TestFalse(
		TEXT("manifest does not expose family routing"),
		Manifest.Contains(TEXT("family"), ESearchCase::IgnoreCase));
	TestFalse(
		TEXT("manifest does not expose tickets"),
		Manifest.Contains(TEXT("ticket"), ESearchCase::IgnoreCase));
	TestFalse(
		TEXT("manifest does not expose server sessions"),
		Manifest.Contains(TEXT("session"), ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringPublicCommandHandlersTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.PublicCommandHandlers",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringPublicCommandHandlersTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingOperationPort> OperationPort =
		MakeShared<FRecordingOperationPort>();
	TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
	FString Error;
	TestTrue(
		TEXT("one thin handler is created for every completed route"),
		TryCreateSimpleAuthoringCommandHandlers(
			GetSimpleAuthoringCommandRoutes(),
			OperationPort,
			Handlers,
			Error));
	TestEqual(
		TEXT("every route has one public handler"),
		Handlers.Num(),
		GetSimpleAuthoringCommandRoutes().Num());
	if (Handlers.Num() != GetSimpleAuthoringCommandRoutes().Num())
	{
		AddError(Error);
		return false;
	}

	TSharedPtr<const FUeAgentCommandContractRegistry> Commands;
	TestTrue(
		TEXT("production command registry composes"),
		TryCreateProductionCommandContractRegistry(Commands, Error));
	const FUeAgentSimpleAuthoringCommandRoute& Route =
		GetSimpleAuthoringCommandRoutes()[0];
	const FUeAgentCommandContract* Contract =
		Commands.IsValid()
			? Commands->FindExact(Route.CommandId)
			: nullptr;
	TestNotNull(TEXT("selected route has one command contract"), Contract);
	if (!Contract)
	{
		return false;
	}
	TSharedPtr<CommandEdge::IUeAgentCommandHandler> Selected;
	TSet<FString> CommandIds;
	for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler
		: Handlers)
	{
		TestFalse(
			TEXT("handler command ownership is unique"),
			CommandIds.Contains(Handler->GetCommandId()));
		CommandIds.Add(Handler->GetCommandId());
		if (Handler->GetCommandId() == Route.CommandId)
		{
			Selected = Handler;
		}
	}
	TestTrue(TEXT("selected exact handler exists"), Selected.IsValid());
	const FString Params = TEXT("{\"test_value\":\"forwarded-once\"}");
	const TOptional<CommandEdge::FUeAgentCommandRequest> Request =
		RequestFor(Route.CommandId, Contract->GetBindingHash(), Params);
	TestTrue(TEXT("public command fixture decodes"), Request.IsSet());
	if (!Selected.IsValid() || !Request.IsSet())
	{
		return false;
	}
	const FUeAgentCallerContext Caller(
		TEXT("sa008-public-edge"),
		{TEXT("uai:authoring")},
		Request->GetCorrelationId());
	const CommandEdge::FUeAgentCommandResult Result = Selected->Submit(
		Request.GetValue(),
		Caller,
		MakeShared<FNeverCancelled>());
	TestTrue(TEXT("thin handler returns the application result"), Result.IsOk());
	TestEqual(TEXT("one command submits once"), OperationPort->Calls, 1);
	TestTrue(
		TEXT("handler forwards the exact descriptor identity"),
		OperationPort->LastDescriptor.IsSet()
			&& OperationPort->LastDescriptor.GetValue()
				== Route.DescriptorIdentity);
	TestEqual(
		TEXT("handler forwards canonical request id"),
		OperationPort->LastRequestId,
		Request->GetRequestId());
	TestEqual(
		TEXT("handler owns only deterministic idempotency projection"),
		OperationPort->LastIdempotencyKey,
		FString(TEXT("uai-command:")) + Request->GetRequestId());
	TestEqual(
		TEXT("handler forwards params without business defaults"),
		OperationPort->LastPayloadJson,
		Request->GetParamsJson());
	return true;
}
