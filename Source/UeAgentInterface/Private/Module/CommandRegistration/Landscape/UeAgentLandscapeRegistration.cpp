#include "Module/CommandRegistration/Landscape/UeAgentLandscapeRegistration.h"

#include "CommandHandlers/Landscape/UeAgentLandscapeCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeMechanicsBoundary.h"
#include "OperationOwners/Landscape/UeAgentLandscapeOperationOwner.h"
#include "QueryOwners/Landscape/UeAgentLandscapeQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::Landscape
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		const FString Owner(TEXT("landscape-operation-owner"));

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
				OutError = TEXT("landscape_contract_missing:") + Route.CommandId;
				return false;
			}

			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("landscape");
			Result.PythonFunction = Route.CommandId.RightChop(
				FString(TEXT("landscape.")).Len()).Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {
				QueryOwners::Landscape::GetPersistedStateLayer(),
			};
			for (const FUeAgentCommandParameter& Parameter
				: Legacy->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(),
					WireType(Parameter.GetWireType()),
					Parameter.IsRequired(),
					Parameter.IsNullable(),
					TEXT("Exact Landscape mechanics parameter."),
				});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identity of the completed Landscape operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Verified native outcome and persisted readback evidence.")),
			};
			Result.Errors = {
				{TEXT("landscape_authority_required"),
					TEXT("The caller lacks uai:operate authority.")},
				{TEXT("landscape_runtime_unavailable"),
					TEXT("The process Operation host cannot bind the current Editor world.")},
				{TEXT("landscape_geometry_payload_invalid"),
					TEXT("The exact Landscape schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"),
					TEXT("A post-mutation failure cannot prove final state and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Route.CommandId
				+ TEXT(" through the common OperationService and native Landscape mechanics.");
			Result.Purpose.Target =
				TEXT("One exact Landscape actor and bounded local sample region.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the target world and exact landscape identity before mutation."),
				TEXT("Declare the local sample-grid frame, scale, and conversion evidence."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.detail fields and explicit Landscape coordinate contracts.");
			Result.Purpose.ResultGuidance =
				TEXT("Use the terminal operation projection and an independent persisted-state query.");
			Result.Purpose.FailureGuidance =
				TEXT("Prewrite failures leave no residue; state_unknown requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Creates or edits only the exact declared Landscape target through native editor mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::Landscape::GetIndependentReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("landscape"), TEXT("height"), TEXT("terrain"), TEXT("operation"),
			};
			Result.Purpose.Synonyms = {
				TEXT("terrain creation"), TEXT("heightmap delta"),
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

			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("landscape-operation"));
				return Id;
			}

			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}

			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return CommandHandlers::Landscape::TryCreateHandlers(
					Routes, OutHandlers, OutError);
			}

		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("landscape-operation"), &TryCreateContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			FString& OutError)
		{
			TSharedPtr<IUeAgentActiveCommandContribution> CommandContribution;
			if (!TryCreateContribution(CommandContribution, OutError)
				|| !CommandContribution.IsValid())
			{
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("landscape"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& OperationId
				: DomainAdapters::Landscape::GetCommandIds())
			{
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = OperationId;
				Operation.Kind = OperationId == TEXT("landscape.create")
					? DomainCapability::EDomainOperationKind::Create
					: DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::None;
				Operation.bPersists = true;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory =
					OperationOwners::Landscape::
						FindOperationInvocationFactory(OperationId);
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T41-LANDSCAPE-OPERATION");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("landscape"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
			OperationOwners::Landscape::GetRoutes();
		if (Routes.Num() != 2)
		{
			OutError = TEXT("landscape_route_count_mismatch");
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> LegacyRegistry;
		if (!TryCreateProductionCommandContractRegistry(LegacyRegistry, OutError)
			|| !LegacyRegistry.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
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
}
