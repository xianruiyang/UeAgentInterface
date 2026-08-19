#include "Module/CommandRegistration/AnimationRigCommands/UeAgentAnimationRigCommandRegistration.h"

#include "CommandHandlers/AnimationRigCommands/UeAgentAnimationRigCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::AnimationRigCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner(TEXT("animation-rig-command-owner"));

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
				OutError = TEXT("animation_rig_contract_missing:") + CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::AnimationRigCommands::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("t57_animation_rig_command_migration");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None : EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact animation rig mechanics parameter.")});
			}
			Result.ResultFields = {{TEXT("result"), TEXT("object"), true, false,
				TEXT("Native rig, IK, skeleton, hierarchy, or slot result.")}};
			Result.Errors = {
				{TEXT("animation_rig_authority_required"), TEXT("Query or Operation authority is missing.")},
				{TEXT("animation_rig_runtime_unavailable"), TEXT("Native rig mechanics are unavailable.")},
				{TEXT("operation_state_unknown"), TEXT("Reconcile before retrying the operation.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native animation rig mechanics.");
			Result.Purpose.Target = TEXT("One exact Control Rig, IK Rig, retargeter, skeleton, hierarchy, shape, or slot target.");
			Result.Purpose.Preconditions = {TEXT("Resolve exact asset dependencies, identity, and expected revision before mutation.")};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Verify writes with persisted asset readback.");
			Result.Purpose.FailureGuidance = TEXT("Failed writes preserve lifecycle diagnostics and owned cleanup.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only the explicit persisted rig target.")
				: TEXT("Mutates only the explicit animation rig asset through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = QueryOwners::AnimationRigCommands::GetReadbackFlows();
			Result.Purpose.Keywords = {TEXT("animation"), TEXT("rig"), TEXT("ik"), TEXT("skeleton")};
			Result.Purpose.Synonyms = {TEXT("Control Rig"), TEXT("retargeter")};
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
				static const FString Id(TEXT("animation-rig-command-migration"));
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
				return CommandHandlers::AnimationRigCommands::TryCreateHandlers(OutHandlers, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("animation-rig-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile> GraphProfiles;
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile> TimelineProfiles;
		if (!QueryOwners::AnimationRigCommands::TryGetGraphProfiles(GraphProfiles, OutError)
			|| !QueryOwners::AnimationRigCommands::TryGetTimelineProfiles(TimelineProfiles, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::GraphCompound::TryCreateGraphCompoundProductionCommandContractRegistry(
				Legacy, OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::AnimationRigCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(CommandId, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
