#include "Core/HjsonWorkspace/Workspace/UeAgentUnifiedHjsonWorkspace.h"
#include "Dom/JsonValue.h"

#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}
	}

	FUnifiedHjsonWorkspace::FUnifiedHjsonWorkspace(
		FUnifiedHjsonSemanticModelRef InModel,
		TSharedRef<const FWorkspaceLayoutPlan> InLayout,
		FUnifiedHjsonWorkspaceIndex InIndex)
		: Model(MoveTemp(InModel))
		, Layout(MoveTemp(InLayout))
		, SemanticFingerprint(Model->GetCoreModel()->GetSemanticFingerprint())
		, Index(MoveTemp(InIndex))
	{
		if (!Model->IsValid()) ValidationDiagnostic = Model->GetValidationDiagnostic();
		else if (Layout->GetSemanticFingerprint() != SemanticFingerprint) {
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_workspace_model_mismatch"), TEXT("The layout was built for a different semantic model."), TEXT("workspace_layout"));
			ValidationDiagnostic.PropertyPath = TEXT("layout.model");
		} else if (Index.UnitIds.Num() != Model->GetCoreModel()->GetUnits().Num()) {
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("hjson_unified_workspace_invalid"), TEXT("The workspace unit index does not cover every semantic unit."), TEXT("workspace_layout"));
			ValidationDiagnostic.PropertyPath = TEXT("unit_index");
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Model->GetCoreModel()->GetUnits().Num());
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(Index.UnitIds.Num());
		}

	}

	const FUnifiedHjsonSemanticModelRef&
	FUnifiedHjsonWorkspace::GetModel() const
	{
		return Model;
	}

	const TSharedRef<const FWorkspaceLayoutPlan>&
	FUnifiedHjsonWorkspace::GetLayout() const
	{
		return Layout;
	}

	EWorkspaceSourceMode FUnifiedHjsonWorkspace::GetSourceMode() const
	{
		return Layout->GetSourceMode();
	}

	const FString&
	FUnifiedHjsonWorkspace::GetSemanticFingerprint() const
	{
		return SemanticFingerprint;
	}

	const FUnifiedHjsonWorkspaceIndex&
	FUnifiedHjsonWorkspace::GetIndex() const
	{
		return Index;
	}

	bool FUnifiedHjsonWorkspace::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	bool TryCreateUnifiedHjsonWorkspace(
		FUnifiedHjsonSemanticModelRef Model,
		TSharedRef<const FWorkspaceLayoutPlan> Layout,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		OutWorkspace.Reset();
		OutError.Reset();
		if (!Model->IsValid())
		{
			OutError.SetFailure(Model->GetValidationDiagnostic());
			return false;
		}
		if (Layout->GetSemanticFingerprint()
				!= Model->GetCoreModel()->GetSemanticFingerprint()
			|| Layout->GetModel()->GetSemanticFingerprint()
				!= Model->GetCoreModel()->GetSemanticFingerprint())
		{
			OutError.SetSourceFailure(TEXT("hjson_workspace_model_mismatch"), TEXT("The layout was built for a different semantic model."));
			return false;
		}

		FUnifiedHjsonWorkspaceIndex Index;
		Index.UnitsByComposition = Model->GetCompositions();
		int64 References = 0;
		for (const FSemanticUnit& Unit : Model->GetCoreModel()->GetUnits())
		{
			Index.UnitIds.Add(Unit.GetUnitId());
			if (!Unit.GetOwnerRef().IsEmpty())
			{
				Index.OwnerByUnitId.Add(Unit.GetUnitId(), Unit.GetOwnerRef());
				++References;
			}
			if (!Unit.GetFieldSetRef().IsEmpty())
			{
				if (Index.FieldOwnerByFieldSetId.Contains(
					Unit.GetFieldSetRef()))
				{
					OutError.SetSourceFailure(TEXT("hjson_workspace_field_owner_conflict"), TEXT("A field set is owned by more than one semantic unit."),
						Unit.GetFieldSetRef());
					return false;
				}
				Index.FieldOwnerByFieldSetId.Add(
					Unit.GetFieldSetRef(),
					Unit.GetUnitId());
			}
			if (!Unit.GetRelationRefs().IsEmpty())
			{
				Index.DirectRelationsByUnitId.Add(
					Unit.GetUnitId(),
					Unit.GetRelationRefs());
				References += Unit.GetRelationRefs().Num();
			}
		}
		Index.UnitIds.Sort();

		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Nodes, Index.UnitIds.Num()},
				{EUeAgentResourceDimension::Items,
					Index.FieldOwnerByFieldSetId.Num()},
				{EUeAgentResourceDimension::ReferenceEdges, References},
				{EUeAgentResourceDimension::WorkUnits,
					Index.UnitIds.Num() + References},
			},
			TEXT("hjson.unified_workspace"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}

		FUnifiedHjsonWorkspacePtr Candidate =
			MakeShareable(new FUnifiedHjsonWorkspace(
				MoveTemp(Model),
				MoveTemp(Layout),
				MoveTemp(Index)));
		if (!Candidate->IsValid())
		{
			OutError.SetFailure(Candidate->GetValidationDiagnostic());
			return false;
		}
		OutWorkspace = MoveTemp(Candidate);
		return true;
	}
}
