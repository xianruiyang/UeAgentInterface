#include "Module/CommandRegistration/DestructiveContent/UeAgentDestructiveContentRegistration.h"

#include "CommandHandlers/DestructiveContent/UeAgentDestructiveContentCommandHandlers.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "OperationOwners/DestructiveContent/UeAgentDestructiveContentOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::DestructiveContent
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::DestructiveContent;
	using namespace CommandRegistry;
	using namespace Composition::DestructiveImport;
	using namespace OperationOwners::DestructiveContent;

	namespace
	{
		const FString Owner = TEXT("destructive-content-owner");

		FString WireType(const EUeAgentCommandWireType Type)
		{
			switch (Type)
			{
			case EUeAgentCommandWireType::Boolean: return TEXT("boolean");
			case EUeAgentCommandWireType::Integer: return TEXT("integer");
			case EUeAgentCommandWireType::Number: return TEXT("number");
			case EUeAgentCommandWireType::String: return TEXT("string");
			case EUeAgentCommandWireType::Object: return TEXT("object");
			case EUeAgentCommandWireType::Array: return TEXT("array");
			default: return TEXT("any");
			}
		}

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* Type,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, Type, bRequired, false, Description};
		}

		bool TryDeclaration(
			const FString& CommandId,
			const FUeAgentCommandContractRegistry& LegacyRegistry,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Legacy =
				LegacyRegistry.FindExact(CommandId);
			if (!Legacy || !Legacy->TryValidate(OutError))
			{
				OutError = Legacy
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("destructive_content_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_content_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("destructive_content");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Legacy->GetParameters())
			{
				if (Parameter.GetName() == TEXT("destructive_authorization_id"))
				{
					continue;
				}
				const FString Description = Parameter.GetName() == TEXT("managed_id")
					? TEXT("Optional exact UAI managed actor identity; when present, folder_path and managed_id must resolve one actor and broad folder deletion is forbidden.")
					: TEXT("Exact destructive-content mechanics parameter.");
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					Description,
					Parameter.GetSchemaJson(),
				});
			}
			Result.InputFields.Add(Field(
				TEXT("confirm"),
				TEXT("boolean"),
				false,
				TEXT("Clone and move execute directly when omitted; false explicitly requests a preview. Destructive removal requires confirmation of its preceding plan.")));
			Result.ResultFields = {
				Field(TEXT("status"), TEXT("string"), true,
					TEXT("planned, committed, rolled_back, failed, rejected, or state_unknown.")),
				Field(TEXT("impact_scope"), TEXT("array"), true,
					TEXT("Readable summary of the packages, references and world objects affected by the plan.")),
				Field(TEXT("targets"), TEXT("array"), false,
					TEXT("Readable affected-target summary.")),
				Field(TEXT("deleted_count"), TEXT("integer"), false,
					TEXT("Number of targets proven deleted by a batch operation.")),
				Field(TEXT("blocked_count"), TEXT("integer"), false,
					TEXT("Number of targets blocked by a current dependency or safety condition.")),
				Field(TEXT("remaining_count"), TEXT("integer"), false,
					TEXT("Number of requested targets still present after a batch operation.")),
				Field(TEXT("recursive"), TEXT("boolean"), false,
					TEXT("Whether the completed batch used recursive selection.")),
				Field(TEXT("instruction"), TEXT("string"), false,
					TEXT("Short next action for a planned confirmation.")),
			};
			Result.Errors = {
				{TEXT("destructive_content_authority_required"),
					TEXT("The caller does not possess uai:operate authority.")},
				{TEXT("destructive_plan_rejected"),
					TEXT("The complete impact scope or current target revisions cannot be established before mutation.")},
				{TEXT("destructive_authorization_mismatch"),
					TEXT("The authorization does not match the current plan, scope, or target revisions.")},
				{TEXT("destructive_confirmation_stale"),
					TEXT("The current plan no longer matches the reviewed plan.")},
				{TEXT("destructive_confirmation_missing"),
					TEXT("No immediately preceding matching plan is available for confirmation.")},
				{TEXT("destructive_content_mechanics_unavailable"),
					TEXT("The exact Unreal Engine destructive mechanics could not be bound.")},
				{TEXT("operation_state_unknown"),
					TEXT("The final state cannot be proven and the request must not be replayed automatically.")},
			};
			Result.Purpose.Summary = TEXT("Plan, authorize, and execute ")
				+ CommandId + TEXT(" through the unique destructive-content owner.");
			Result.Purpose.Target =
				TEXT("Only the exact assets, references, world objects, and persisted packages listed by the returned impact scope.");
			Result.Purpose.Preconditions = {
				TEXT("Clone and move execute directly; destructive removal requires review of the returned impact summary."),
				TEXT("Do not authorize while an affected package has unsaved changes."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("For clone and move, omit confirm to execute or set false to preview. For destructive removal, repeat the reviewed parameters with confirm=true.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat only committed with verified native projection as successful mutation; planned performs no mutation.");
			Result.Purpose.FailureGuidance =
				TEXT("Rejected plans are retry-safe after reconciliation; state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Uses Unreal Engine semantic operations and persists only the authorized impact scope.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.usage"),
				TEXT("destructive plan review"),
				TEXT("independent persisted readback"),
			};
			Result.Purpose.Keywords = {
				TEXT("destructive"), TEXT("delete"), TEXT("move"),
				TEXT("authorization"), TEXT("impact"),
			};
			Result.Purpose.Synonyms = {
				TEXT("destructive content operation"),
				TEXT("plan-bound asset mutation"),
			};
			Result.TutorialReferences.Add(
				TEXT("uai-docs:t31-destructive-content-ops#destructive-plan-authorize-commit"));
			if (CommandId == TEXT("world.data_layer.delete"))
			{
				Result.TutorialReferences.Add(
					TEXT("uai-docs:t31-destructive-content-ops#world-data-layer-delete"));
			}
			if (CommandId == TEXT("asset.object.clone"))
			{
				Result.Purpose.Keywords.Append({
					TEXT("clone"),
					TEXT("duplicate"),
					TEXT("copy"),
					TEXT("asset"),
					TEXT("object"),
				});
				Result.Purpose.Synonyms.Append({
					TEXT("clone asset object"),
					TEXT("duplicate asset object"),
					TEXT("copy asset object"),
					TEXT("clone duplicate or copy an owned asset object including a map asset"),
				});
			}
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("destructive-content");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateDestructiveContentCommandHandlers(
					OutHandlers, OutError);
			}

		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GDestructiveContentContributionRegistration(
				TEXT("destructive-content"),
				&TryCreateDestructiveContentContribution);
	}

	bool TryCreateDestructiveContentContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> LegacyRegistry;
		if (!TryCreateDestructiveImportProductionCommandContractRegistry(
				LegacyRegistry, OutError)
			|| !LegacyRegistry.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : GetDestructiveContentCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(
					CommandId, *LegacyRegistry, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
