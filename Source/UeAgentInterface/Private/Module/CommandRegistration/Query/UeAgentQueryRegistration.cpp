#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"

#include "CommandHandlers/Query/UeAgentQueryHandlers.h"

namespace UE::UAI::Module::CommandRegistration::Query
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Query;
	using namespace CommandRegistry;
	using namespace DomainAdapters::Query;
	using namespace Module::CommandRegistration::Active;

	namespace
	{
		const FString Owner = TEXT("query-command-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		FUeAgentCommandDeclaration Declaration(const FQueryRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("query");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {
				Route.CommandId.StartsWith(TEXT("asset."))
					|| Route.CommandId.StartsWith(TEXT("reference."))
					? FString(TEXT("asset_current_state"))
					: FString(TEXT("editor_current_state")),
			};

			if (Route.bUsesObjectPath)
			{
				Result.InputFields.Add(Field(
					TEXT("object_path"),
					TEXT("string"),
					true,
					TEXT("Exact object path used unchanged by the server-owned asset read mechanics.")));
			}
			else if (Route.bRequiresTarget)
			{
				Result.InputFields.Add(Field(
					TEXT("target"),
					TEXT("string"),
					true,
					TEXT("Exact target identity; no client or server alias guessing is permitted.")));
			}
			if (!Route.bUsesObjectPath)
			{
				Result.InputFields.Add(Field(
					TEXT("input"),
					TEXT("object"),
					false,
					TEXT("Command-specific query fields consumed by the exact UE provider.")));
			}
			Result.InputFields.Add(Field(
				TEXT("context"),
				TEXT("object"),
				false,
				TEXT("Explicit execution context and state-plane overrides.")));
			Result.InputFields.Add(Field(
				TEXT("projection"),
				TEXT("object"),
				false,
				TEXT("Field, provenance and bounded paging projection; no hard total-token rejection.")));

			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("Exact public command identity.")),
				Field(TEXT("mechanics_command_id"), TEXT("string"), true, TEXT("Exact server-owned mechanics route used for the read.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true for a successful Query.")),
				Field(TEXT("query_result"), TEXT("object"), true, TEXT("Typed projection with source identity, state revision and completeness.")),
			};
			Result.Errors = {
				Error(TEXT("query_authority_required"), TEXT("The caller lacks uai:query authority.")),
				Error(TEXT("query_cancelled"), TEXT("The request was cancelled before execution.")),
				Error(TEXT("query_params_invalid"), TEXT("The public parameter object is invalid.")),
				Error(TEXT("query_target_required"), TEXT("The exact target or object_path is missing.")),
				Error(TEXT("query_thread_invalid"), TEXT("Real UE mechanics were requested outside the game thread.")),
				Error(TEXT("query_source_not_read_only"), TEXT("A provider could not attest read-only behavior.")),
				Error(TEXT("query_command_not_supported"), TEXT("The exact route has no active provider mechanics.")),
			};

			Result.Purpose.Summary = FString::Printf(
				TEXT("Read %s from current Unreal Engine state through the unique QueryService owner."),
				*Route.CommandId);
			Result.Purpose.Target = Route.bRequiresTarget
				? TEXT("One exact UE target in an explicit execution context and state layer.")
				: TEXT("The current Editor, World, project or configured runtime state.");
			Result.Purpose.Preconditions = {
				TEXT("The UAI Editor service is active and the caller has uai:query authority."),
			};
			Result.Purpose.ParameterGuidance = Route.bRequiresTarget
				? TEXT("Pass the exact target returned by a public search or known UE identity; never rewrite it into an alias.")
				: TEXT("Pass only command-specific input, explicit context and bounded projection fields when needed.");
			Result.Purpose.ResultGuidance = TEXT("Use command_id, mechanics_command_id, state revision and projection completeness to bound every conclusion.");
			Result.Purpose.FailureGuidance = TEXT("Missing targets, unsupported mechanics, provider errors, cancellation and stale context remain distinct typed failures.");
			Result.Purpose.SideEffectGuidance = TEXT("Strictly read-only; a provider that cannot attest this is rejected.");
			Result.Purpose.MinimumExample = Route.bUsesObjectPath
				? FString::Printf(TEXT("%s(client, object_path)"), *Route.PythonFunction)
				: Route.bRequiresTarget
					? FString::Printf(TEXT("%s(client, target)"), *Route.PythonFunction)
					: FString::Printf(TEXT("%s(client)"), *Route.PythonFunction);
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
			};
			Result.Purpose.Keywords = {
				TEXT("query"),
				TEXT("read-only"),
				Route.CommandId,
			};
			Result.Purpose.Synonyms = {Route.PythonFunction};
			return Result;
		}

		class FQueryContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FQueryContribution(
				TSharedRef<IUeAgentQueryService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Value = TEXT("query");
				return Value;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				for (const FQueryRoute& Route : GetNonHjsonQueryRoutes())
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return TryCreateQueryHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentQueryService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GQueryContributionRegistration(
				TEXT("query"),
				&TryCreateProductionQueryContribution);
	}

	bool TryCreateQueryContribution(
		TSharedRef<IUeAgentQueryService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FQueryContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionQueryContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentQueryService> Service;
		if (!TryCreateProductionQueryService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		return TryCreateQueryContribution(
			Service.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateQueryRegistration(
		TSharedRef<IUeAgentQueryService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateQueryContribution(Service, Contribution, OutError)
			|| !Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			|| !Composition.TryCompose(OutRegistry, OutDispatch, OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}
