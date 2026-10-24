#include "Module/CommandRegistration/EditorProject/UeAgentEditorProjectRegistration.h"

#include "CommandHandlers/EditorProject/UeAgentEditorProjectCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"

namespace UE::UAI::Module::CommandRegistration::EditorProject
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("editor-project-operation-owner");

		FString WireType(EUeAgentCommandWireType Type)
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
			const TCHAR* Name, const TCHAR* Type, bool Required,
			const TCHAR* Description)
		{
			return {Name, Type, Required, false, Description};
		}

		bool TryDeclaration(
			const FString& Id,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("editor_project_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_project_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t39_editor_project_operation");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("project_persisted_state")};
			for (const FUeAgentCommandParameter& P : Contract->GetParameters())
			{
				Result.InputFields.Add({P.GetName(), WireType(P.GetWireType()),
					P.IsRequired(), P.IsNullable(),
					TEXT("Exact Editor project mechanics parameter."), P.GetSchemaJson()});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier of the completed operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Verified native project result and evidence identities.")),
			};
			Result.Errors = {
				{TEXT("editor_project_authority_required"), TEXT("uai:operate is required.")},
				{TEXT("simple_authoring_input_invalid"), TEXT("The exact Editor project schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"), TEXT("The final project state cannot be proven and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common OperationService and Editor project mechanics.");
			Result.Purpose.Target = TEXT("One exact project configuration or project-owned asset target declared by the contract.");
			Result.Purpose.Preconditions = {TEXT("Resolve the exact project setting before mutation; the server binds its current revision internally.")};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance = TEXT("Success requires persisted and verified terminal projection evidence.");
			Result.Purpose.FailureGuidance = TEXT("state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance = TEXT("Uses the formal native EditorProject simple effect.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("project readback")};
			Result.Purpose.Keywords = {TEXT("editor"), TEXT("project"), TEXT("config")};
			Result.Purpose.Synonyms = {TEXT("project settings operation")};
			Out = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> In)
				: Declarations(MoveTemp(In)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("editor-project-operation"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::EditorProject::TryCreateHandlers(Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("editor-project-operation"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Legacy, OutError) || !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : DomainAdapters::EditorProject::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
