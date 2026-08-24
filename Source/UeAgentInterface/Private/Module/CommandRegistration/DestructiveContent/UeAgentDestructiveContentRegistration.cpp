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
			FString& OutError)
		{
			const FUeAgentCommandContract* Legacy =
				LegacyRegistry.FindExact(CommandId);
			if (!Legacy || !Legacy->IsValid())
			{
				OutError = TEXT("destructive_content_contract_missing:") + CommandId;
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
				const FString Description = Parameter.GetName() == TEXT("managed_id")
					? TEXT("Optional exact UAI managed actor identity; when present, folder_path and managed_id must resolve one actor and broad folder deletion is forbidden.")
					: TEXT("Exact destructive-content mechanics parameter.");
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					Description,
				});
			}
			if (!Legacy->GetParameters().ContainsByPredicate(
					[](const FUeAgentCommandParameter& Parameter)
					{
						return Parameter.GetName()
							== TEXT("destructive_authorization_id");
					}))
			{
				Result.InputFields.Add(Field(
					TEXT("destructive_authorization_id"),
					TEXT("string"),
					false,
					TEXT("Omit to receive the immutable plan and impact scope; repeat the unchanged command with the returned identifier to authorize execution.")));
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier derived from the exact destructive plan.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("planned, committed, rolled_back, failed, rejected, or state_unknown.")),
				Field(TEXT("destructive_authorization_id"), TEXT("string"), true,
					TEXT("Identifier bound to the complete impact scope, plan fingerprint, and target revisions.")),
				Field(TEXT("plan_fingerprint"), TEXT("string"), true,
					TEXT("SHA-256 identity of the canonical plan.")),
				Field(TEXT("impact_scope"), TEXT("array"), true,
					TEXT("Exact packages, referencers, world state, and command authority affected by the plan.")),
				Field(TEXT("revision_snapshot"), TEXT("array"), true,
					TEXT("Persisted target revisions used by the authorization fence.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Native mechanics result and independent readback facts.")),
			};
			Result.Errors = {
				{TEXT("destructive_content_authority_required"),
					TEXT("The caller does not possess uai:operate authority.")},
				{TEXT("destructive_plan_rejected"),
					TEXT("The complete impact scope or current target revisions cannot be established before mutation.")},
				{TEXT("destructive_authorization_mismatch"),
					TEXT("The authorization does not match the current plan, scope, or target revisions.")},
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
				TEXT("First omit destructive_authorization_id and review the complete returned plan."),
				TEXT("Do not authorize while an affected package has unsaved changes."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Repeat the exact unchanged parameters with the returned destructive_authorization_id; any plan or revision change invalidates it.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat only committed with verified native projection as successful mutation; planned performs no mutation.");
			Result.Purpose.FailureGuidance =
				TEXT("Rejected plans are retry-safe after reconciliation; state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Uses Unreal Engine semantic operations and persists only the authorized impact scope.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params_without_authorization), then repeat with returned authorization");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.detail"),
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
				FString& OutError) const override
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
		FString& OutError)
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
		if (Declarations.Num() != 14)
		{
			OutError = TEXT("destructive_content_declaration_count_mismatch");
			return false;
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
