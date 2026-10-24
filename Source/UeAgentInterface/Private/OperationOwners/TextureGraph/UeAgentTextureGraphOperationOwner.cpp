#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"

#include "DomainAdapters/TextureGraph/UeAgentTextureGraphAuthoringApply.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	using namespace Composition::GraphCompound;
	using namespace ContextResource;
	using namespace BindingGateway::Operation;

	namespace
	{
		Contract::FDiagnostic NativeConditionFailure(const TCHAR* Code, const FString& Reason, const TCHAR* Condition)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("texture_graph"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Condition;
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Diagnostic;
		}

		Contract::FDiagnostic DuplicateHandlerFailure(const FString& CommandId)
		{
			auto Diagnostic = NativeConditionFailure(TEXT("texture_graph_mechanics_handler_duplicate"),
				TEXT("TextureGraph command registration contains more than one handler for the same command."), TEXT("handlers_per_command"));
			Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(2);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			Diagnostic.SubjectId = CommandId;
			return Diagnostic;
		}

		class FGraphLifecycleResultContext final
			: public DomainCapability::IDomainOperationInvocationContext
		{
		public:
			explicit FGraphLifecycleResultContext(
				TSharedRef<IGraphCompoundAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			bool TryBuildSuccessPayload(
				const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt,
				FString& OutPayloadJson,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				Contract::FUeAgentContractValue Result(TEXT("{}"));
				FGraphCompoundRuntimeError Error;
				if (!Context->TryBuildResult(Receipt, Result, Error))
				{
					OutError = Error;
					if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("texture_graph_result_build_failed"),
						TEXT("The TextureGraph result could not be constructed and no diagnostic was provided."), TEXT("result_built"));
					return false;
				}
				OutPayloadJson = Result.GetJsonValue();
				if (OutPayloadJson.IsEmpty())
				{
					OutError = NativeConditionFailure(TEXT("texture_graph_result_payload_missing"),
						TEXT("The completed TextureGraph operation returned no result payload."), TEXT("result_payload_present"));
					return false;
				}
				return true;
			}

		private:
			TSharedRef<IGraphCompoundAudioMediaLifecycleContext> Context;
		};

		class FGraphMutationInvocationFactory final
			: public DomainCapability::IDomainOperationInvocationFactory
		{
		public:
			explicit FGraphMutationInvocationFactory(FString InOperationId)
				: OperationId(MoveTemp(InOperationId))
			{
			}

			const FString& GetOperationId() const override
			{
				return OperationId;
			}

			bool TryCreate(
				const CommandEdge::FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& CallerContext,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
				DomainCapability::FDomainOperationInvocation& OutInvocation,
				DomainCapability::FDomainOperationInvocationError& OutError)
				const override
			{
				OutInvocation = {};
				OutError = {};
				if (Request.GetCommandId() != OperationId)
				{
					OutError = NativeConditionFailure(TEXT("texture_graph_operation_identity_mismatch"),
						TEXT("The invocation factory does not match the requested TextureGraph operation."), TEXT("operation_identity_matches"));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetCommandId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(OperationId);
					return false;
				}
				TArray<FString> Scopes = CallerContext.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("audio_media_graph.read"));
				Scopes.AddUnique(TEXT("audio_media_graph.write"));
				const Contract::FUeAgentCallerContext EffectiveCaller(
					CallerContext.GetCallerId(),
					MoveTemp(Scopes),
					CallerContext.GetCorrelationId());
				FGraphCompoundAudioMediaProductionConfig Config;
				Config.DurableJournalRoot = FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/GraphCompound/TextureGraph");
				FGraphCompoundAudioMediaLifecycleInvocation GraphInvocation;
				FGraphCompoundRuntimeError GraphError;
				if (!TryCreateAudioMediaGraphOperationLifecycleInvocation(
						OperationId,
						Request,
						EffectiveCaller,
						Cancellation,
						Config,
						GraphInvocation,
						GraphError)
					|| !GraphInvocation.IsValid())
				{
					OutError = MoveTemp(GraphError);
					if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("texture_graph_operation_invocation_failed"),
						TEXT("The TextureGraph lifecycle invocation was not created and no diagnostic was provided."), TEXT("lifecycle_invocation_ready"));
					return false;
				}
				return DomainCapability::FDomainOperationInvocation::TryCreate(
					GraphInvocation.GetPlan(),
					GraphInvocation.GetAuthorization(),
					GraphInvocation.GetService(),
					MakeShared<FGraphLifecycleResultContext>(
						GraphInvocation.GetContext()),
					OutInvocation,
					OutError);
			}

		private:
			FString OperationId;
		};

		const TMap<FString, TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>>& OperationFactories()
		{
			static const TMap<FString, TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> Factories = {
				{TEXT("texture_graph.artifact.bake"),
					GetTextureGraphBakeInvocationFactory()},
				{TEXT("texture_graph.graph.link.mutate"),
					MakeShared<const FGraphMutationInvocationFactory>(
						TEXT("texture_graph.graph.link.mutate"))},
				{TEXT("texture_graph.graph.node.mutate"),
					MakeShared<const FGraphMutationInvocationFactory>(
						TEXT("texture_graph.graph.node.mutate"))},
				{TEXT("texture_graph.graph.parameter.mutate"),
					MakeShared<const FGraphMutationInvocationFactory>(
						TEXT("texture_graph.graph.parameter.mutate"))},
				{TEXT("texture_graph.graph.validate_evaluate_settle"),
					MakeShared<const FGraphMutationInvocationFactory>(
						TEXT("texture_graph.graph.validate_evaluate_settle"))},
			};
			return Factories;
		}
	}

	bool TryApplySemanticTextureGraphComposite(
		const FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UeAgentAuthoringOperationApply::FBackendOptions& Options)
	{
		OutResult = UeAgentAuthoringOperationApply::FApplyWorkspaceResult();
		OutError.Reset();
		return DomainAdapters::TextureGraph::ApplySemanticComposite(
			Request, Plan, Cancellation, Options, OutResult, OutError);
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindTextureGraphOperationInvocationFactory(const FString& OperationId)
	{
		const TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>*
			Factory = OperationFactories().Find(OperationId);
		return Factory
			? TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory>(*Factory)
			: nullptr;
	}

	bool TryCreateMechanicsHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::TextureGraph::TryGetActivationProfiles(
				Profiles, OutError))
		{
			if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("texture_graph_activation_profiles_unavailable"),
				TEXT("TextureGraph activation profiles could not be loaded and no diagnostic was provided."), TEXT("activation_profiles_ready"));
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;

		FGraphCompoundAudioMediaProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/TextureGraph");
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateAudioMediaGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError;
			if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("texture_graph_runtime_unavailable"),
				TEXT("The TextureGraph production runtime was not created and no diagnostic was provided."), TEXT("runtime_ready"));
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Mechanics;
		if (!TryCreateGraphCompoundCommandHandlers(
				Profiles,
				Runtime->GetQueryPort(),
				Runtime->GetOperationPort(),
				Mechanics,
				OutError))
		{
			if (OutError.IsEmpty()) OutError = NativeConditionFailure(TEXT("texture_graph_mechanics_handlers_unavailable"),
				TEXT("TextureGraph mechanics handlers could not be created and no diagnostic was provided."), TEXT("mechanics_handlers_ready"));
			return false;
		}
		for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler
			: Mechanics)
		{
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutHandlers.Reset();
				OutError = DuplicateHandlerFailure(Handler->GetCommandId());
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			ArtifactHandlers;
		if (!TryCreateArtifactHandlers(
			BakeFactory, ArtifactHandlers, OutError))
		{
			OutHandlers.Reset();
			return false;
		}
		for (const TPair<FString,
			TSharedRef<CommandEdge::IUeAgentCommandHandler>>& Pair
			: ArtifactHandlers)
		{
			if (OutHandlers.Contains(Pair.Key))
			{
				OutHandlers.Reset();
				OutError = DuplicateHandlerFailure(Pair.Key);
				return false;
			}
			OutHandlers.Add(Pair.Key, Pair.Value);
		}
		const int32 ExpectedCount = DomainAdapters::TextureGraph::GetCommandIds().Num();
		if (OutHandlers.Num() != ExpectedCount)
		{
			OutError = NativeConditionFailure(TEXT("texture_graph_mechanics_handler_count_invalid"),
				TEXT("The registered TextureGraph handler count does not match the declared command count."), TEXT("handler_count"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(OutHandlers.Num());
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(ExpectedCount);
			return false;
		}
		return true;
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}
