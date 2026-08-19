#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphMechanicsBoundary.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	using namespace Composition::GraphCompound;

	namespace
	{
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
				FString& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				Contract::FUeAgentContractValue Result(TEXT("{}"));
				FGraphCompoundRuntimeError Error;
				if (!Context->TryBuildResult(Receipt, Result, Error))
				{
					OutError = Error.Code.IsEmpty()
						? FString(TEXT("texture_graph_operation_result_invalid"))
						: Error.Code + TEXT(":") + Error.Detail;
					return false;
				}
				OutPayloadJson = Result.GetJsonValue();
				return !OutPayloadJson.IsEmpty();
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
					OutError = DomainCapability::FDomainOperationInvocationError(
						TEXT("texture_graph_operation_identity_mismatch"),
						Request.GetCommandId(),
						OperationId);
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
				Config.OperationTimeoutSeconds = 120.0;
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
					OutError = DomainCapability::FDomainOperationInvocationError(
						GraphError.Code.IsEmpty()
							? FString(TEXT("texture_graph_operation_invocation_failed"))
							: GraphError.Code,
						GraphError.SubjectId,
						GraphError.Detail);
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
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::TextureGraph::TryGetActivationProfiles(
				Profiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundAudioMediaProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/TextureGraph");
		ProductionConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateAudioMediaGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
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
			return false;
		}
		for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler
			: Mechanics)
		{
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutHandlers.Reset();
				OutError = TEXT("texture_graph_mechanics_handler_duplicate");
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
				OutError = TEXT("texture_graph_mechanics_handler_duplicate:")
					+ Pair.Key;
				return false;
			}
			OutHandlers.Add(Pair.Key, Pair.Value);
		}
		return OutHandlers.Num()
			== DomainAdapters::TextureGraph::GetCommandIds().Num();
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		return TryCreateMechanicsHandlers(
			GetTextureGraphBakeInvocationFactory(),
			OutHandlers,
			OutError);
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
