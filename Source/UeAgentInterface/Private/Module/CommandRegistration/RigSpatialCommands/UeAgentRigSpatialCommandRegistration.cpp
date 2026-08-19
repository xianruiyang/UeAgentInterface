#include "Module/CommandRegistration/RigSpatialCommands/UeAgentRigSpatialCommandRegistration.h"

#include "CommandHandlers/RigSpatialCommands/UeAgentRigSpatialCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "QueryOwners/RigSpatialCommands/UeAgentRigSpatialCommandQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::RigSpatialCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner(TEXT("rig-spatial-command-owner"));

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

		bool TryDeclaration(
			const FString& CommandId,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("rig_spatial_contract_missing:") + CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::RigSpatialCommands::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("t58_rig_spatial_command_migration");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None : EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("runtime_or_asset_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact rig/spatial mechanics parameter.")});
			}
			Result.ResultFields = {{TEXT("result"), TEXT("object"), true, false,
				TEXT("Native rig simulation, spatial, pose, retarget, socket, or bake result.")}};
			Result.Errors = {
				{TEXT("rig_spatial_authority_required"), TEXT("Query or Operation authority is missing.")},
				{TEXT("rig_spatial_runtime_unavailable"), TEXT("Native rig/spatial mechanics are unavailable.")},
				{TEXT("operation_state_unknown"), TEXT("Reconcile before retrying the operation.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native rig/spatial mechanics.");
			Result.Purpose.Target = TEXT("One explicit rig, skeleton, subject, pose, retargeter, control, socket, or bake target.");
			Result.Purpose.Preconditions = {TEXT("Resolve exact asset or runtime subject dependencies and spatial contracts before mutation.")};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Verify with the returned native state or persisted asset readback.");
			Result.Purpose.FailureGuidance = TEXT("Failed writes preserve lifecycle diagnostics and owned cleanup.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only the explicit rig/spatial target.")
				: TEXT("Mutates only the explicit rig/spatial target through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = QueryOwners::RigSpatialCommands::GetReadbackFlows();
			Result.Purpose.Keywords = {TEXT("rig"), TEXT("spatial"), TEXT("pose"), TEXT("retarget")};
			Result.Purpose.Synonyms = {TEXT("Control Rig"), TEXT("IK"), TEXT("socket")};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations)) {}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("rig-spatial-command-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return CommandHandlers::RigSpatialCommands::TryCreateHandlers(OutHandlers, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("rig-spatial-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile> Profiles;
		if (!QueryOwners::RigSpatialCommands::TryGetTimelineProfiles(Profiles, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::TimelineSpatial::TryCreateTimelineSpatialProductionCommandContractRegistry(
				Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::RigSpatialCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(CommandId, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
