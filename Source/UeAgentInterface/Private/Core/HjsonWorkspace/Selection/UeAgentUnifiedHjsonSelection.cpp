#include "Core/HjsonWorkspace/Selection/UeAgentUnifiedHjsonSelection.h"

#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		void CopySelectionError(
			const FEditBundleSelectionError& Error,
			FWorkspaceError& OutError)
		{
			OutError.SetFailure(Error);
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}
	}

	FUnifiedHjsonSelectionPackage::FUnifiedHjsonSelectionPackage(
		FUnifiedHjsonSemanticModelRef InModel,
		FSelectionClosure InClosure)
		: Model(MoveTemp(InModel))
		, Closure(MoveTemp(InClosure))
		, ModelFingerprint(Model->GetModelFingerprint())
		, bValid(
			Model->IsValid()
			&& Closure.IsValid()
			&& ModelFingerprint.StartsWith(TEXT("sha256:")))
	{
	}

	const FUnifiedHjsonSemanticModelRef&
	FUnifiedHjsonSelectionPackage::GetModel() const
	{
		return Model;
	}

	const FSelectionClosure&
	FUnifiedHjsonSelectionPackage::GetClosure() const
	{
		return Closure;
	}

	const FString&
	FUnifiedHjsonSelectionPackage::GetModelFingerprint() const
	{
		return ModelFingerprint;
	}

	bool FUnifiedHjsonSelectionPackage::IsValid() const
	{
		return bValid;
	}

	bool TryResolveUnifiedHjsonSelection(
		FUnifiedHjsonSemanticModelRef Model,
		const FWorkspaceSelection& Selection,
		const FSelectionPolicy& Policy,
		FUeAgentResourceTracker& Resources,
		FUnifiedHjsonSelectionPackagePtr& OutPackage,
		FWorkspaceError& OutError)
	{
		OutPackage.Reset();
		OutError.Reset();
		if (!Model->IsValid())
		{
			OutError.SetFailure(Model->GetValidationDiagnostic());
			return false;
		}

		TOptional<FSelectionClosure> InitialClosure;
		FEditBundleSelectionError SelectionError;
		if (!TryResolveSelectionClosure(
			Model->GetCoreModel(),
			Selection,
			Policy,
			InitialClosure,
			SelectionError))
		{
			CopySelectionError(SelectionError, OutError);
			return false;
		}
		if (!InitialClosure.IsSet())
		{
			OutError.SetSourceFailure(TEXT("hjson_selection_closure_missing"), TEXT("Selection resolution reported success but returned no closure."));
			return false;
		}

		TSet<FString> DirectRelationIds;
		for (const FString& RelationId : Selection.GetSelectedRelationIds())
		{
			DirectRelationIds.Add(RelationId);
		}
		for (const FString& UnitId : InitialClosure->GetUnitIds())
		{
			const FSemanticUnit* Unit =
				Model->GetCoreModel()->FindUnit(UnitId);
			if (Unit)
			{
				for (const FString& RelationId : Unit->GetRelationRefs())
				{
					DirectRelationIds.Add(RelationId);
				}
			}
		}

		TOptional<FSelectionClosure> FinalClosure = InitialClosure;
		if (DirectRelationIds.Num()
			!= Selection.GetSelectedRelationIds().Num())
		{
			TArray<FString> SortedRelations = DirectRelationIds.Array();
			SortedRelations.Sort();
			const FWorkspaceSelection ExpandedSelection(
				Selection.GetSelectionId(),
				Selection.GetSelectionKind(),
				Selection.GetSelectedUnitIds(),
				Selection.GetSelectedFieldIds(),
				MoveTemp(SortedRelations),
				Selection.GetCandidateBindingId());
			FinalClosure.Reset();
			SelectionError.Reset();
			if (!TryResolveSelectionClosure(
				Model->GetCoreModel(),
				ExpandedSelection,
				Policy,
				FinalClosure,
				SelectionError))
			{
				CopySelectionError(SelectionError, OutError);
				return false;
			}
		}
		if (!FinalClosure.IsSet())
		{
			OutError.SetSourceFailure(TEXT("hjson_selection_closure_missing"), TEXT("Selection resolution returned no closure after expanding the selected relations."));
			return false;
		}

		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Nodes,
					FinalClosure->GetUnitIds().Num()},
				{EUeAgentResourceDimension::Items,
					FinalClosure->GetFieldIds().Num()},
				{EUeAgentResourceDimension::ReferenceEdges,
					FinalClosure->GetRelationIds().Num()},
				{EUeAgentResourceDimension::WorkUnits,
					FinalClosure->GetUnitIds().Num()
					+ FinalClosure->GetFieldIds().Num()
					+ FinalClosure->GetRelationIds().Num()},
			},
			TEXT("hjson.unified_selection"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}

		FUnifiedHjsonSelectionPackagePtr Candidate =
			MakeShareable(new FUnifiedHjsonSelectionPackage(
				MoveTemp(Model),
				MoveTemp(FinalClosure.GetValue())));
		if (!Candidate->IsValid())
		{
			if (!Candidate->GetModel()->IsValid()) OutError.SetFailure(Candidate->GetModel()->GetValidationDiagnostic());
			else OutError.SetSourceFailure(TEXT("hjson_selection_package_invalid"),
				!Candidate->GetClosure().IsValid() ? TEXT("The resolved selection closure is invalid.")
				: TEXT("The selected model has no valid SHA-256 fingerprint."));
			return false;
		}
		OutPackage = MoveTemp(Candidate);
		return true;
	}
}
