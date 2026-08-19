#include "Core/HjsonWorkspace/Workspace/UeAgentUnifiedHjsonWorkspace.h"

#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceBudgetExceeded& Error)
		{
			OutError.Set(
				Error.Code,
				Error.Path,
				TEXT("/"),
				Error.Observed,
				Error.bStoppedEarly);
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
		, bValid(
			Model->IsValid()
			&& Layout->GetSemanticFingerprint() == SemanticFingerprint
			&& Index.UnitIds.Num()
				== Model->GetCoreModel()->GetUnits().Num())
	{
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
		return bValid;
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
			OutError.Set(TEXT("hjson_unified_model_invalid"));
			return false;
		}
		if (Layout->GetSemanticFingerprint()
				!= Model->GetCoreModel()->GetSemanticFingerprint()
			|| Layout->GetModel()->GetSemanticFingerprint()
				!= Model->GetCoreModel()->GetSemanticFingerprint())
		{
			OutError.Set(TEXT("hjson_workspace_model_mismatch"));
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
					OutError.Set(
						TEXT("hjson_workspace_field_owner_conflict"),
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

		FUeAgentResourceBudgetExceeded ResourceError;
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
			OutError.Set(TEXT("hjson_unified_workspace_invalid"));
			return false;
		}
		OutWorkspace = MoveTemp(Candidate);
		return true;
	}
}
