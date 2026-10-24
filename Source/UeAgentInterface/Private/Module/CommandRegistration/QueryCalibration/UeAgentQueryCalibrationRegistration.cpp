#include "Module/CommandRegistration/QueryCalibration/UeAgentQueryCalibrationRegistration.h"

#include "CommandHandlers/QueryCalibration/UeAgentQueryCalibrationHandlers.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"

namespace UE::UAI::Module::CommandRegistration::QueryCalibration
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::QueryCalibration;
	using namespace CommandRegistry;
	using namespace Discovery::Command;
	using namespace Module::CommandRegistration::Active;

	namespace
	{
		const FString Owner = TEXT("query-calibration-command-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {
				Name,
				WireType,
				bRequired,
				false,
				Description,
			};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		FUeAgentCommandDeclaration BaseDeclaration(
			const TCHAR* CommandId,
			const TCHAR* PythonFunction)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("query");
			Result.PythonFunction = PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_current_state")};
			return Result;
		}

		FUeAgentCommandDeclaration SearchDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("query.command.search"), TEXT("command_search"));
			Result.InputFields = {
				Field(TEXT("intent"), TEXT("string"), true, TEXT("Space-separated positive keywords for command ID, registered keywords or registered synonyms.")),
			};
			Result.ResultFields = {
				Field(TEXT("commands"), TEXT("array"), true, TEXT("Canonical command names only, deduplicated in result order.")),
			};
			Result.Errors = {
				Error(TEXT("command_intent_missing"), TEXT("The request contains no searchable keyword.")),
				Error(TEXT("query_authority_required"), TEXT("The authenticated caller lacks read authority.")),
			};
			Result.Purpose.Summary = TEXT("Find exact UAI command candidates from positive keywords.");
			Result.Purpose.Target = TEXT("The immutable current command registry and its complete purpose contracts.");
			Result.Purpose.Preconditions = {TEXT("The caller is authenticated and has uai:query authority.")};
			Result.Purpose.ParameterGuidance = TEXT("Pass short positive action, object, domain or UE-term keywords; do not pass a task sentence.");
			Result.Purpose.ResultGuidance = TEXT("Use a returned command name with query.command.usage.");
			Result.Purpose.FailureGuidance = TEXT("Missing keywords fail without fuzzy execution or fallback.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only registry lookup; no Editor, asset, world or external state changes.");
			Result.Purpose.MinimumExample = TEXT("client.query.command_search('editor runtime inspect')");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage")};
			Result.Purpose.Keywords = {TEXT("discover"), TEXT("keyword"), TEXT("command"), TEXT("search"), TEXT("read-only")};
			Result.Purpose.Synonyms = {TEXT("find command"), TEXT("command lookup")};
			return Result;
		}

		FUeAgentCommandDeclaration UsageDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("query.command.usage"), TEXT("command_usage"));
			Result.InputFields = {
				Field(TEXT("commands"), TEXT("array"), true, TEXT("One or more canonical command names returned by command search.")),
			};
			Result.ResultFields = {
				Field(TEXT("usage"), TEXT("object"), true, TEXT("Command name to minimal Python named SDK usage with required top-level parameters.")),
			};
			Result.Errors = {
				Error(TEXT("command_usage_params_invalid"), TEXT("commands must contain non-empty command names.")),
				Error(TEXT("command_not_registered_exact"), TEXT("A supplied command name is not registered.")),
				Error(TEXT("command_usage_inactive"), TEXT("A supplied command is not active.")),
				Error(TEXT("command_usage_unavailable"), TEXT("A supplied command has no generated Python named method.")),
				Error(TEXT("query_authority_required"), TEXT("The authenticated caller lacks read authority.")),
			};
			Result.Purpose.Summary = TEXT("Get minimal Python named SDK usage for several command names.");
			Result.Purpose.Target = TEXT("The immutable current command registry.");
			Result.Purpose.Preconditions = {TEXT("Use active canonical command names returned by query.command.search.")};
			Result.Purpose.ParameterGuidance = TEXT("Pass canonical command names from command search; duplicate names are collapsed while preserving first-seen order.");
			Result.Purpose.ResultGuidance = TEXT("Use the returned Python call shape and fill only its required top-level arguments; optional behavior remains documented by the selected command contract and result.");
			Result.Purpose.FailureGuidance = TEXT("Any invalid, inactive or unmapped name fails the whole batch without a partial result.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only registry lookup; no Editor, asset, world or external state changes.");
			Result.Purpose.MinimumExample = TEXT("client.query.command_usage(['editor.session', 'content_graph.items'])");
			Result.Purpose.RelatedFlows = {TEXT("query.command.search")};
			Result.Purpose.Keywords = {TEXT("usage"), TEXT("python"), TEXT("command"), TEXT("batch")};
			Result.Purpose.Synonyms = {TEXT("command help"), TEXT("batch command usage")};
			return Result;
		}

		class FQueryCalibrationContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			virtual const FString& GetContributionId() const override
			{
				static const FString Value = TEXT("query-calibration");
				return Value;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>&
					OutDeclarations) const override
			{
				OutDeclarations.Add(SearchDeclaration());
				OutDeclarations.Add(UsageDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry>
					Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>&
					OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TSharedPtr<const FCommandDiscoveryService> Discovery;
				FCommandDiscoveryError DiscoveryError;
				if (!FCommandDiscoveryService::TryCreate(
					Registry,
					Discovery,
					DiscoveryError))
				{
					OutError = DiscoveryError;
					return false;
				}

				TArray<TSharedRef<IUeAgentExactCommandHandler>>
					ContributionHandlers;
				if (!TryCreateQueryCalibrationHandlers(
					Discovery.ToSharedRef(),
					ContributionHandlers,
					OutError))
				{
					return false;
				}
				OutHandlers.Append(ContributionHandlers);
				return true;
			}

		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GQueryCalibrationContributionRegistration(
				TEXT("query-calibration"),
				&TryCreateProductionQueryCalibrationContribution);
	}

	bool TryCreateQueryCalibrationContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		OutContribution = MakeShared<FQueryCalibrationContribution>();
		return true;
	}

	bool TryCreateProductionQueryCalibrationContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		return TryCreateQueryCalibrationContribution(OutContribution, OutError);
	}

	bool TryCreateQueryCalibrationRegistration(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateQueryCalibrationContribution(
				Contribution,
				OutError)
			|| !Composition.TryAdd(
				Contribution.ToSharedRef(),
				OutError)
			|| !Composition.TryCompose(
				OutRegistry,
				OutDispatch,
				OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}
