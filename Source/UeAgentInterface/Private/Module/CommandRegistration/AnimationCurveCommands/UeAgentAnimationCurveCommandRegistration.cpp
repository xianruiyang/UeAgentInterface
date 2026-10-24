#include "Module/CommandRegistration/AnimationCurveCommands/UeAgentAnimationCurveCommandRegistration.h"

#include "CommandHandlers/AnimationCurveCommands/UeAgentAnimationCurveCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "QueryOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::AnimationCurveCommands
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner(TEXT("animation-curve-command-owner"));

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
			const TArray<Composition::TimelineSpatial::
				FUeAgentTimelineSpatialActivationProfile>& TimelineProfiles,
			FUeAgentCommandDeclaration& OutDeclaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->TryValidate(OutError))
			{
				OutError = Contract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_curve_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::AnimationCurveCommands::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("t56_animation_curve_command_migration");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None : EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& Parameter : Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact animation/curve mechanics parameter."),
					Parameter.GetSchemaJson(),
				});
			}
			Result.ResultFields = {{TEXT("result"), TEXT("object"), true, false,
				TEXT("Native animation asset result with revision and readback.")}};
			if (const auto* Profile = TimelineProfiles.FindByPredicate(
					[&CommandId](const auto& Candidate)
					{
						return Candidate.CommandId == CommandId;
					}))
			{
				Result.ModelResultFields = Profile->ModelResultFields;
			}
			if (CommandId.EndsWith(TEXT(".asset.create")))
			{
				Result.ModelResultFields = {
					{TEXT("target_ref"), {TEXT("target_ref")}},
				};
			}
			Result.Errors = {
				{TEXT("animation_curve_authority_required"),
					TEXT("The caller lacks Query or Operation authority.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact animation request is invalid.")},
				{TEXT("animation_curve_runtime_unavailable"),
					TEXT("The native TimelineAnimation mechanics are unavailable.")},
				{TEXT("operation_state_unknown"),
					TEXT("The operation requires reconciliation before retry.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native TimelineAnimation mechanics.");
			Result.Purpose.Target =
				TEXT("One exact animation sequence, montage, BlendSpace, track, curve, event, or sample target.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact asset, temporal contract, skeleton dependencies, and expected revision."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters.");
			Result.Purpose.ResultGuidance =
				TEXT("Verify writes with the corresponding persisted animation readback.");
			Result.Purpose.FailureGuidance =
				TEXT("Queries are side-effect free; failed writes preserve lifecycle diagnostics.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only the explicit persisted animation target.")
				: TEXT("Mutates only the explicit animation asset through native UE mechanics.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = QueryOwners::AnimationCurveCommands::GetReadbackFlows();
			Result.Purpose.Keywords = {TEXT("animation"), TEXT("curve"), TEXT("montage")};
			Result.Purpose.Synonyms = {TEXT("animation authoring"), TEXT("BlendSpace")};
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
				static const FString Id(TEXT("animation-curve-command-migration"));
				return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return CommandHandlers::AnimationCurveCommands::TryCreateHandlers(
					OutHandlers, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("animation-curve-command-migration"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile> GraphProfiles;
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile> TimelineProfiles;
		if (!QueryOwners::AnimationCurveCommands::TryGetGraphProfiles(GraphProfiles, OutError)
			|| !QueryOwners::AnimationCurveCommands::TryGetTimelineProfiles(TimelineProfiles, OutError))
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
		for (const FString& CommandId : QueryOwners::AnimationCurveCommands::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(
					CommandId,
					*Legacy,
					TimelineProfiles,
					Declaration,
					OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}
