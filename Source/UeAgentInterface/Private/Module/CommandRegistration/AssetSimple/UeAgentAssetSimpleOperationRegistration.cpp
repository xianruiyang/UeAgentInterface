#include "Module/CommandRegistration/AssetSimple/UeAgentAssetSimpleOperationRegistration.h"

#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleOperationCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AssetSimple;
	using namespace CommandRegistry;
	using namespace Composition::SimpleAuthoring;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::AssetSimple;

	namespace
	{
		const FString Owner = TEXT("asset-simple-operation-owner");

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
			const FUeAgentSimpleAuthoringCommandRoute& Route,
			const FUeAgentCommandContractRegistry& LegacyRegistry,
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			const FUeAgentCommandContract* Legacy =
				LegacyRegistry.FindExact(Route.CommandId);
			if (!Legacy || !Legacy->IsValid())
			{
				OutError = TEXT("asset_simple_contract_missing:") + Route.CommandId;
				return false;
			}

			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("t37_asset_simple_operation");
			Result.PythonFunction = Route.CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Legacy->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact AssetSimple operation parameter consumed by the declared domain mechanics."),
				});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier of the one completed SimpleAuthoring operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Canonical verified outcome containing status, evidence identities and persisted or current-state readback.")),
			};
			Result.Errors = {
				{TEXT("asset_simple_authority_required"),
					TEXT("The caller does not possess uai:operate authority.")},
				{TEXT("asset_simple_runtime_unavailable"),
					TEXT("The process Operation host cannot bind the current Editor world.")},
				{TEXT("simple_authoring_input_invalid"),
					TEXT("The exact domain schema rejects the request before mutation.")},
				{TEXT("operation_deadline_exceeded"),
					TEXT("The bounded operation did not reach its required terminal stage.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure cannot prove the final state and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Route.CommandId
				+ TEXT(" through the common OperationService and exact AssetSimple mechanics.");
			Result.Purpose.Target =
				TEXT("One exact persisted asset or project-owned asset setting declared by the command contract.");
			Result.Purpose.Preconditions = {
				TEXT("Use the exact public command identity and contract hash unchanged."),
				TEXT("Resolve all asset and type identities before the first mutation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Pass only fields declared by query.command.detail; the SDK performs no domain remapping.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat success as terminal only when projection.payload reports a verified outcome and evidence identities.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; state_unknown is non-replayable and requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Creates or updates only the exact declared asset target through formal Unreal Engine mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("asset persisted-state query"),
			};
			Result.Purpose.Keywords = {
				TEXT("asset"), TEXT("create"), TEXT("update"), TEXT("OperationService"),
			};
			Result.Purpose.Synonyms = {
				TEXT("simple asset authoring"), TEXT("asset settings mutation"),
			};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TArray<FUeAgentCommandDeclaration> InDeclarations,
				TArray<FUeAgentSimpleAuthoringCommandRoute> InRoutes)
				: Declarations(MoveTemp(InDeclarations))
				, Routes(MoveTemp(InRoutes))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("asset-simple-operation");
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
				return TryCreateAssetSimpleOperationCommandHandlers(
					Routes, OutHandlers, OutError);
			}

		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GAssetSimpleOperationContributionRegistration(
				TEXT("asset-simple-operation"),
				&TryCreateAssetSimpleOperationContribution);
	}

	bool TryCreateAssetSimpleOperationContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			GetAssetSimpleOperationRoutes();
		if (Routes.Num() != 52)
		{
			OutError = TEXT("asset_simple_route_count_mismatch");
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> LegacyRegistry;
		if (!TryCreateProductionCommandContractRegistry(LegacyRegistry, OutError)
			|| !LegacyRegistry.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		Declarations.Reserve(Routes.Num());
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Route, *LegacyRegistry, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(Declarations, Routes);
		return true;
	}

	bool TryCreateAssetSimpleOperationRegistration(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		return TryCreateAssetSimpleOperationContribution(Contribution, OutError)
			&& Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			&& Composition.TryCompose(OutRegistry, OutDispatch, OutError);
	}
}
