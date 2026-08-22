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
	using namespace DomainAdapters::QueryCalibration;
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
				Field(TEXT("intent"), TEXT("string"), true, TEXT("Unknown-name intent expressed in ordinary task language.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Maximum candidates from 1 through 8; defaults to 3.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Non-negative continuation cursor; defaults to 0.")),
			};
			Result.ResultFields = {
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Small ranked candidates with reasons and exact contract identities.")),
				Field(TEXT("completeness"), TEXT("string"), true, TEXT("complete or paged for this immutable registry revision.")),
			};
			Result.Errors = {
				Error(TEXT("command_intent_missing"), TEXT("The intent contains no searchable term.")),
				Error(TEXT("command_search_page_invalid"), TEXT("The limit or cursor is outside the public bound.")),
				Error(TEXT("query_authority_required"), TEXT("The authenticated caller lacks read authority.")),
			};
			Result.Purpose.Summary = TEXT("Find a small set of exact UAI command candidates from an unknown-name intent.");
			Result.Purpose.Target = TEXT("The immutable current command registry and its complete purpose contracts.");
			Result.Purpose.Preconditions = {TEXT("The caller is authenticated and has uai:query authority.")};
			Result.Purpose.ParameterGuidance = TEXT("Describe the intended task; use limit and cursor only for bounded paging.");
			Result.Purpose.ResultGuidance = TEXT("Use a candidate command_id and contract_hash unchanged with query.command.detail.");
			Result.Purpose.FailureGuidance = TEXT("Invalid intent or paging fails without fuzzy execution or fallback.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only registry lookup; no Editor, asset, world or external state changes.");
			Result.Purpose.MinimumExample = TEXT("client.query.command_search('inspect current editor runtime', limit=3)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("query.calibration.read")};
			Result.Purpose.Keywords = {TEXT("discover"), TEXT("intent"), TEXT("command"), TEXT("search"), TEXT("read-only")};
			Result.Purpose.Synonyms = {TEXT("find command"), TEXT("what command should I call")};
			return Result;
		}

		FUeAgentCommandDeclaration DetailDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("query.command.detail"), TEXT("command_detail"));
			Result.InputFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("Exact command identity returned by command search.")),
				Field(TEXT("contract_hash"), TEXT("string"), true, TEXT("Exact sha256 contract identity returned with that candidate.")),
			};
			Result.ResultFields = {
				Field(TEXT("purpose"), TEXT("object"), true, TEXT("Complete purpose, preconditions, guidance, example and related flows.")),
				Field(TEXT("input_fields"), TEXT("array"), true, TEXT("Exact public input field declarations; schema-capable owners attach a machine-readable JSON Schema to each field.")),
				Field(TEXT("result_fields"), TEXT("array"), true, TEXT("Exact public result field declarations.")),
				Field(TEXT("tutorial_references"), TEXT("array"), true, TEXT("Stable public SDK and installed-skill tutorial references.")),
				Field(TEXT("invocation"), TEXT("object"), true, TEXT("Named, generic, exact and CLI invocation templates for this exact identity.")),
			};
			Result.Errors = {
				Error(TEXT("command_not_registered_exact"), TEXT("The complete command identity does not exist.")),
				Error(TEXT("command_contract_version_mismatch"), TEXT("The candidate hash is stale or belongs to another contract.")),
				Error(TEXT("query_authority_required"), TEXT("The authenticated caller lacks read authority.")),
			};
			Result.Purpose.Summary = TEXT("Read the complete usage detail for one exact command contract.");
			Result.Purpose.Target = TEXT("One immutable command_id and contract_hash pair returned by public discovery.");
			Result.Purpose.Preconditions = {TEXT("Use both identity fields exactly as returned by query.command.search.")};
			Result.Purpose.ParameterGuidance = TEXT("Do not rewrite, alias or infer either identity field.");
			Result.Purpose.ResultGuidance = TEXT("Pass the same identity and parameters admitted by each input_fields[].schema to SDK execute_exact or a named wrapper; never guess values outside a published schema.");
			Result.Purpose.FailureGuidance = TEXT("Unknown and stale identities fail with distinct typed codes and never fall back.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only registry lookup; no persistent or transient UE mutation.");
			Result.Purpose.MinimumExample = TEXT("client.query.command_detail(command_id, contract_hash)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.search"), TEXT("sdk.execute_exact")};
			Result.Purpose.Keywords = {TEXT("detail"), TEXT("purpose"), TEXT("contract"), TEXT("exact"), TEXT("usage")};
			Result.Purpose.Synonyms = {TEXT("command help"), TEXT("show command schema")};
			return Result;
		}

		FUeAgentCommandDeclaration CalibrationDeclaration()
		{
			FUeAgentCommandDeclaration Result = BaseDeclaration(
				TEXT("query.calibration.read"), TEXT("calibration_read"));
			Result.InputFields = {
				Field(TEXT("category"), TEXT("string"), true, TEXT("One exact category: capability, content_search, candidate_catalog, state, reference or runtime_probe.")),
				Field(TEXT("search_text"), TEXT("string"), false, TEXT("Optional case-insensitive filter applied by the real source.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Maximum returned items from 1 through 100; defaults to 50.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Non-negative continuation cursor; defaults to 0.")),
			};
			Result.ResultFields = {
				Field(TEXT("items"), TEXT("array"), true, TEXT("Real UE identities and current fields for the selected query class.")),
				Field(TEXT("completeness"), TEXT("string"), true, TEXT("complete, paged, partial or degraded without silent promotion.")),
				Field(TEXT("revision"), TEXT("string"), true, TEXT("Observed source/world revision used for this read.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true; a non-read-only provider is rejected.")),
			};
			Result.Errors = {
				Error(TEXT("query_category_unknown"), TEXT("The category is not one of the six calibrated state providers.")),
				Error(TEXT("query_params_invalid"), TEXT("The category, limit or cursor is invalid.")),
				Error(TEXT("query_cursor_invalid"), TEXT("The cursor exceeds the current result set.")),
				Error(TEXT("query_source_not_read_only"), TEXT("The selected source violated the read-only contract.")),
				Error(TEXT("query_thread_invalid"), TEXT("Real UE state was requested outside the game thread.")),
				Error(TEXT("query_authority_required"), TEXT("The authenticated caller lacks read authority.")),
			};
			Result.Purpose.Summary = TEXT("Read one of six representative real UE query classes with truthful paging and completeness.");
			Result.Purpose.Target = TEXT("Current engine, asset registry, loaded world, reference graph or Editor runtime state.");
			Result.Purpose.Preconditions = {TEXT("The Editor service is running and the caller has uai:query authority.")};
			Result.Purpose.ParameterGuidance = TEXT("Use an exact category; page with the returned next_cursor and preserve identities unchanged.");
			Result.Purpose.ResultGuidance = TEXT("Treat source_id, state_layer, revision and completeness as part of every bounded claim.");
			Result.Purpose.FailureGuidance = TEXT("Unsupported categories, invalid paging and unavailable mechanics remain typed and explicit.");
			Result.Purpose.SideEffectGuidance = TEXT("Strictly read-only; providers may inspect but never mutate Editor, asset, world or external state.");
			Result.Purpose.MinimumExample = TEXT("client.query.calibration_read('runtime_probe', limit=2)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.search"), TEXT("query.command.detail")};
			Result.Purpose.Keywords = {TEXT("inspect"), TEXT("editor"), TEXT("runtime"), TEXT("state"), TEXT("asset"), TEXT("reference"), TEXT("query")};
			Result.Purpose.Synonyms = {TEXT("read current editor"), TEXT("search assets"), TEXT("runtime probe")};
			return Result;
		}

		class FQueryCalibrationContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FQueryCalibrationContribution(
				TSharedRef<IUeAgentQueryCalibrationSource> InSource)
				: Source(MoveTemp(InSource))
			{
			}

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
				OutDeclarations.Add(DetailDeclaration());
				OutDeclarations.Add(CalibrationDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry>
					Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>&
					OutHandlers,
				FString& OutError) const override
			{
				TSharedPtr<const FCommandDiscoveryService> Discovery;
				FCommandDiscoveryError DiscoveryError;
				if (!FCommandDiscoveryService::TryCreate(
					Registry,
					Discovery,
					DiscoveryError))
				{
					OutError = DiscoveryError.Code
						+ TEXT(":")
						+ DiscoveryError.Detail;
					return false;
				}

				TArray<TSharedRef<IUeAgentExactCommandHandler>>
					ContributionHandlers;
				if (!TryCreateQueryCalibrationHandlers(
					Discovery.ToSharedRef(),
					Source,
					ContributionHandlers,
					OutError))
				{
					return false;
				}
				OutHandlers.Append(ContributionHandlers);
				return true;
			}

		private:
			TSharedRef<IUeAgentQueryCalibrationSource> Source;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GQueryCalibrationContributionRegistration(
				TEXT("query-calibration"),
				&TryCreateProductionQueryCalibrationContribution);
	}

	bool TryCreateQueryCalibrationContribution(
		TSharedRef<IUeAgentQueryCalibrationSource> Source,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		OutContribution = MakeShared<FQueryCalibrationContribution>(Source);
		return true;
	}

	bool TryCreateProductionQueryCalibrationContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentQueryCalibrationSource> Source;
		if (!TryCreateProductionQueryCalibrationSource(Source, OutError))
		{
			return false;
		}
		return TryCreateQueryCalibrationContribution(
			Source.ToSharedRef(),
			OutContribution,
			OutError);
	}

	bool TryCreateQueryCalibrationRegistration(
		TSharedRef<IUeAgentQueryCalibrationSource> Source,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateQueryCalibrationContribution(
				Source,
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
