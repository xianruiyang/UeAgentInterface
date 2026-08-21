#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"
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

// GC-012 requires one production composition owner instead of adding
// domain switches to TimelineSpatial, teaching DomainAdapters to parse HJSON,
// or restoring legacy HTTP dispatch.
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundHjsonTargets.h"

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

	TSet<FString> ExpectedCapabilityIds()
	{
		return {
			TEXT("blueprint.asset.create"),
			TEXT("blueprint.authoring.apply"),
			TEXT("blueprint.component.events.discover"),
			TEXT("umg.asset.create"),
			TEXT("umg.authoring.apply"),
			TEXT("umg.widget.events.discover"),
			TEXT("material.asset.create"),
			TEXT("material.graph.apply"),
			TEXT("material.function.graph.apply"),
			TEXT("material_instance.asset.create"),
			TEXT("material_instance.parameters.apply"),
			TEXT("niagara.emitter.asset.create"),
			TEXT("niagara.system.asset.create"),
			TEXT("niagara.asset.duplicate"),
			TEXT("niagara.stack.reorder"),
			TEXT("niagara.stack.issue_fix"),
			TEXT("niagara.compound.apply"),
			TEXT("niagara.renderer_event_parameter.apply"),
			TEXT("niagara.compile_refresh_settle"),
			TEXT("ai.behavior_tree.asset.create"),
			TEXT("ai.behavior_tree.graph.apply"),
			TEXT("ai.blackboard.asset.create"),
			TEXT("ai.blackboard.schema.apply"),
			TEXT("ai.eqs.asset.create"),
			TEXT("ai.eqs.graph.apply"),
			TEXT("ai.smart_object_definition.asset.create"),
			TEXT("ai.smart_object_definition.apply"),
			TEXT("ai.state_tree.asset.create"),
			TEXT("ai.state_tree.hierarchy.apply"),
			TEXT("pcg.graph.asset.create"),
			TEXT("pcg.graph.apply"),
			TEXT("pcg.graph_instance.apply"),
			TEXT("pcg.graph_parameters.apply"),
			TEXT("pcg.component.apply"),
			TEXT("pcg.data_asset.apply"),
			TEXT("pcg.data_asset.create_from_level"),
			TEXT("pcg.component.world_binding.apply"),
			TEXT("animation.anim_blueprint.graph.apply"),
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.skeleton.definition.apply"),
			TEXT("animation.skeleton.compatibility.apply"),
			TEXT("animation.skeleton.virtual_bone.apply"),
			TEXT("animation.control_rig.hierarchy_graph.apply"),
			TEXT("animation.control_rig.asset.create"),
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.control_rig.shape_library.asset.create"),
			TEXT("animation.deformer_graph.topology.apply"),
			TEXT("animation.deformer_graph.asset.create"),
			TEXT("animation.deformer_source_library.bindings.apply"),
			TEXT("animation.deformer_source_library.asset.create"),
			TEXT("animation.mesh_deformer_collection.entries.apply"),
			TEXT("animation.mesh_deformer_collection.asset.create"),
			TEXT("animation.ml_deformer.configuration.apply"),
			TEXT("animation.ml_deformer.asset.create"),
			TEXT("animation.ik_retargeter.definition.apply"),
			TEXT("animation.ik_retargeter.asset.create"),
			TEXT("animation.ik_retargeter.settings.apply"),
			TEXT("animation.ik_rig.definition.auto_generate"),
			TEXT("animation.ik_rig.definition.apply"),
			TEXT("animation.ik_rig.asset.create"),
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.interface.mutate"),
			TEXT("metasound.graph.node.mutate"),
			TEXT("metasound.graph.link.mutate"),
			TEXT("metasound.graph.default.mutate"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("sound_cue.graph.node.mutate"),
			TEXT("sound_cue.graph.relation.mutate"),
			TEXT("sound_cue.graph.root.mutate"),
			TEXT("sound_cue.graph.compile_settle"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.artifact.capture"),
		};
	}

	TSet<FString> ExpectedGraphHjsonKinds()
	{
		return {
			TEXT("ue.blueprint.asset"),
			TEXT("ue.widget_blueprint.asset"),
			TEXT("ue.niagara_system.asset"),
			TEXT("ue.niagara_emitter.asset"),
			TEXT("ue.niagara_script.asset"),
		};
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundActivationExpectedTest,
	"UeAgentInterface.Composition.GraphCompound.E0.Activation."
		"ExactDescriptorsRoutesAndHjsonTargets",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundActivationExpectedTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Composition::HjsonImport;
	(void)Parameters;

	const TSet<FString> Expected = ExpectedCapabilityIds();
	const TMap<FString, FExpectedCapability> ExpectedContracts =
		ExpectedCapabilityContracts();
	TestEqual(TEXT("Graph/Compound capability count is fixed"),
		Expected.Num(), 78);
	TestEqual(TEXT("Graph/Compound capability contracts are unique"),
		ExpectedContracts.Num(), Expected.Num());
	TestEqual(TEXT("Blueprint/UMG capability count"),
		DomainAdapters::BlueprintUmgGraph::
			GetBlueprintUmgGraphCapabilityContracts().Num(), 6);
	TestEqual(TEXT("Material/Niagara capability count"),
		DomainAdapters::MaterialNiagaraGraph::
			GetMaterialNiagaraGraphCapabilityContracts().Num(), 13);
	TestEqual(TEXT("PCG/AI capability count"),
		DomainAdapters::PcgAiNodeGraph::
			GetPcgAiNodeGraphCapabilityContracts().Num(), 18);
	TestEqual(TEXT("Animation/Rig capability count"),
		DomainAdapters::AnimationRigGraph::
			GetAnimationRigGraphCapabilityContracts().Num(), 26);
	TestEqual(TEXT("Audio/Media capability count"),
		DomainAdapters::AudioMediaGraph::
			GetAudioMediaGraphCapabilityContracts().Num(), 15);

	TSet<FString> ActualHjsonKinds;
	for (const FUeAgentHjsonImportTargetBinding& Binding
		: GetHjsonImportTargetRegistry().GetBindings())
	{
		TestTrue(TEXT("every registered HJSON target is complete"),
			Binding.IsValid());
		TestTrue(TEXT("every registered HJSON target has an operation factory"),
			Binding.HasOperationFactory());
		if (Binding.CompositionKind != TEXT("ue.animation_sequence.asset")
			&& Binding.CompositionKind != TEXT("ue.level_sequence.asset"))
		{
			ActualHjsonKinds.Add(Binding.CompositionKind);
		}
	}
	const TSet<FString> ExpectedHjson = ExpectedGraphHjsonKinds();
	TestEqual(TEXT("five legacy Graph/Compound HJSON kinds are registered"),
		ActualHjsonKinds.Num(), ExpectedHjson.Num());
	for (const FString& Kind : ExpectedHjson)
	{
		TestTrue(TEXT("exact HJSON target exists: ") + Kind,
			ActualHjsonKinds.Contains(Kind));
		TestFalse(TEXT("prefix HJSON target is rejected: ") + Kind,
			GetHjsonImportTargetRegistry().FindExact(Kind + TEXT(".derived"))
				!= nullptr);
	}

	using namespace UE::UAI::Composition::GraphCompound;
	const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles =
		GetGraphCompoundActivationProfiles();
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
	if (Descriptors.IsValid())
	{
		TestEqual(TEXT("one production descriptor per capability"),
			Descriptors->GetDescriptors().Num(), Expected.Num());
	}
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
		TestEqual(TEXT("outer production registry includes 185 base and 78 Graph commands"),
			Commands->GetContracts().Num(), 263);
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

		// The Graph registry owns its 303-command layer and validates the
		// in-memory projection above.  The outer destructive/import activation
		// owns the committed public manifest after it extends this registry.
	}

	using namespace UE::UAI::Composition::GraphCompound;
	TSet<FString> ProductionKinds;
	for (const FString& Kind : GetGraphCompoundHjsonCompositionKinds())
	{
		ProductionKinds.Add(Kind);
	}
	TestEqual(TEXT("production Graph HJSON kind count"),
		ProductionKinds.Num(), ExpectedHjson.Num());
	for (const FString& Kind : ExpectedHjson)
	{
		TestTrue(TEXT("production HJSON kind exists: ") + Kind,
			ProductionKinds.Contains(Kind));
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
	TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
	FString Error;
	TestTrue(TEXT("one thin handler is created for every exact route"),
		TryCreateGraphCompoundCommandHandlers(
			GetGraphCompoundActivationProfiles(),
			QueryPort,
			OperationPort,
			Handlers,
			Error));
	TestEqual(TEXT("public handler count"), Handlers.Num(), 78);

	TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
		Commands;
	TestTrue(TEXT("production command registry composes"),
		TryCreateGraphCompoundProductionCommandContractRegistry(
			Commands, Error));
	if (!Commands.IsValid() || Handlers.Num() != 78)
	{
		AddError(TEXT("graph_compound_public_handlers_unavailable:") + Error);
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
		: GetGraphCompoundActivationProfiles())
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
	TestEqual(TEXT("all query routes submit to C3Q"),
		QueryPort->Calls, 3);
	TestEqual(TEXT("all operation routes submit to C3O"),
		OperationPort->Calls, 74);
	TestEqual(TEXT("fixture query count is stable"),
		ExpectedQueryCalls, 3);
	TestEqual(TEXT("fixture operation count is stable"),
		ExpectedOperationCalls, 74);
	return !HasAnyErrors();
}
