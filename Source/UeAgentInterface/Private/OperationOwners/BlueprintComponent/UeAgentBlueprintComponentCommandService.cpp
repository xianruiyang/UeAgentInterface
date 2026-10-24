#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "QueryOwners/BlueprintComponent/UeAgentBlueprintComponentQueryOwner.h"

namespace UE::UAI::OperationOwners::BlueprintComponent
{
	using namespace UE::UAI::QueryOwners::BlueprintComponent;

	namespace
	{
		const FString CandidateCommandId =
			TEXT("blueprint.component.candidates");

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		

		class FProductionService final
			: public IUeAgentBlueprintComponentCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentBlueprintComponentCommandRequest& Request,
				FUeAgentBlueprintComponentCommandResponse& OutResponse,
				FUeAgentBlueprintComponentCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("blueprint_component_cancelled"), Request.CommandId,
						TEXT("The blueprint component candidate request was cancelled before discovery started."));
					Diagnostic.PropertyPath = TEXT("cancellation_requested");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				if (Request.CommandId != CandidateCommandId)
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("blueprint_component_command_not_supported"), Request.CommandId,
						FString::Printf(TEXT("This service accepts only '%s'."), *CandidateCommandId));
					Diagnostic.PropertyPath = TEXT("command_supported");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Request.CommandId);
				}
				return TryExecuteCandidateQuery(
					Request, OutResponse, OutError);
			}
		};

	}

	bool TryCreateProductionBlueprintComponentCommandService(
		TSharedPtr<IUeAgentBlueprintComponentCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService = MakeShared<FProductionService>();
		OutError.Reset();
		return true;
	}
}
