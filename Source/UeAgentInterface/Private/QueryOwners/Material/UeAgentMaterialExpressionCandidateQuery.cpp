#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"

#include "Algo/Sort.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Material
{
	using namespace ContentDiscovery;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("material_expression");
		const FString FunctionCandidateKind = TEXT("material_function_call");
		const FString InputContract = TEXT("uai.material.expression.create/1");
		const FString PlacementContract =
			TEXT("uai.material.expression.graph-placement/1");
		const FString RequiredContext =
			TEXT("editor_game_thread:material_or_material_function");

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString PublicReason, const UObject& Target)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target.GetPathName();
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause, TEXT("candidate_query"),
				Cause.IsEmpty() ? TEXT("The material palette query failed without reporting a diagnostic.") : FString());
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Cause.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.IsA<UMaterial>() || Target.IsA<UMaterialFunction>();
		}

		FString CandidateRevision(
			const UObject& Target,
			const FString& CandidateName)
		{
			return Sha256(
				CandidateName + TEXT("|") + CandidateKind + TEXT("|")
				+ InputContract + TEXT("|") + PlacementContract + TEXT("|")
				+ RequiredContext + TEXT("|")
				+ Target.GetClass()->GetPathName());
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FMaterialExpressionCandidateQueryError& OutError)
		{
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty()) return ForwardDiagnostic(OutError, MoveTemp(ReadError), Target.GetPathName());
				auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_target_readback_failed"),
					bCaptured ? TEXT("Material target readback did not return a revision.")
						: TEXT("Material target readback failed without a diagnostic."), Target);
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("/revision");
					Diagnostic.ExpectedValuePresent = true;
					Diagnostic.ActualValuePresent = false;
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryMaterialExpressionCandidates(
		UObject& Target,
		const FString& Query,
		FMaterialExpressionCandidateQueryResult& OutResult,
		FMaterialExpressionCandidateQueryError& OutError,
		const FMaterialExpressionCandidateContext& Context)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_game_thread_required"),
				TEXT("Material candidate queries require the Editor game thread."), Target);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other_thread"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("material_candidate_target_type_unsupported"),
				TEXT("Material candidate target must be a Material or MaterialFunction."), Target);
			Diagnostic.PropertyPath = TEXT("/target");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(TEXT("Material")), MakeShared<FJsonValueString>(TEXT("MaterialFunction"))});
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Target.GetPathName());
		}
		OutResult.Target = Target.GetPathName();
		OutResult.Context = Context;
		OutResult.Context.GraphName = Context.GraphName.IsEmpty()
			? (Target.IsA<UMaterial>()
				? FString(TEXT("MaterialGraph"))
				: FString(TEXT("MaterialFunctionGraph")))
			: Context.GraphName;
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("material_node");
		// Enumerate the native context once; apply the public keyword predicate below.
		Params.SearchText.Reset();
		Params.SearchMode = TEXT("contains");
		Params.AssetPath = Target.GetPathName();
		Params.GraphName = OutResult.Context.GraphName;
		Params.NodeGuid = Context.NodeGuid;
		Params.PinGuid = Context.PinGuid;
		Params.PinName = Context.PinName;
		TArray<UeAgentPaletteSearch::FCandidate> PaletteCandidates;
		TArray<FString> PaletteWarnings;
		UE::UAI::Contract::FDiagnostic SearchError;
		if (!UeAgentPaletteSearch::CollectMaterialCandidates(
				Params, PaletteCandidates, PaletteWarnings, SearchError))
		{
			return ForwardDiagnostic(OutError, CandidateCause(TEXT("material_candidate_palette_query_failed"), Target.GetPathName(), SearchError), Target.GetPathName());
		}
		PaletteCandidates.RemoveAll(
			[](const UeAgentPaletteSearch::FCandidate& Candidate)
			{
				return Candidate.Kind != CandidateKind
				&& Candidate.Kind != FunctionCandidateKind;
			});
		TArray<int32> MatchedIndices;
		for (int32 CandidateIndex = 0;
			CandidateIndex < PaletteCandidates.Num(); ++CandidateIndex)
		{
			UeAgentPaletteSearch::FCandidate& Candidate =
				PaletteCandidates[CandidateIndex];
			const FString ResolvedNodeGuid =
				Candidate.AuthoringSelectors.FindRef(TEXT("context_node_guid"));
			const FString ResolvedPinGuid =
				Candidate.AuthoringSelectors.FindRef(TEXT("context_pin_guid"));
			const FString ResolvedPinName =
				Candidate.AuthoringSelectors.FindRef(TEXT("context_pin_name"));
			if (!ResolvedNodeGuid.IsEmpty())
			{
				OutResult.Context.NodeGuid = ResolvedNodeGuid;
			}
			if (!ResolvedPinGuid.IsEmpty())
			{
				OutResult.Context.PinGuid = ResolvedPinGuid;
			}
			if (!ResolvedPinName.IsEmpty())
			{
				OutResult.Context.PinName = ResolvedPinName;
			}
			ContentDiscovery::FContentDiscoveryRequest SearchRequest;
			SearchRequest.Query = Query;
			SearchRequest.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 MatchScore = 0;
			FString MatchReason;
			if (ContentDiscovery::TryScoreContentDiscoveryMatch(
					SearchRequest,
					Candidate.GetCandidateId(),
					Candidate.Name.IsEmpty()
						? Candidate.DisplayName
						: Candidate.Name,
					{
						Candidate.DisplayName,
						Candidate.FullName,
						Candidate.Keywords,
						Candidate.CategoryPath,
						Candidate.Tooltip,
					},
					MatchScore,
				MatchReason))
			{
				Candidate.Score = MatchScore;
				if (!Candidate.GetPublicId().IsEmpty())
				{
					MatchedIndices.Add(CandidateIndex);
				}
			}
		}

		MatchedIndices.Sort(
			[&PaletteCandidates](const int32 LeftIndex, const int32 RightIndex)
			{
				const UeAgentPaletteSearch::FCandidate& Left =
					PaletteCandidates[LeftIndex];
				const UeAgentPaletteSearch::FCandidate& Right =
					PaletteCandidates[RightIndex];
				if (Left.UiOrder != Right.UiOrder)
				{
					return Left.UiOrder < Right.UiOrder;
				}
				return Left.GetCandidateId() < Right.GetCandidateId();
			});
		OutResult.TotalMatches = MatchedIndices.Num();
		for (const int32 MatchedIndex : MatchedIndices)
		{
			const UeAgentPaletteSearch::FCandidate& Source =
				PaletteCandidates[MatchedIndex];
			FMaterialExpressionCandidate Candidate;
			Candidate.Id = Source.GetPublicId();
			Candidate.Name = Source.Name;
			Candidate.Scopes.Reset();
			Candidate.Label = Candidate.Name;
			Candidate.Kind = Source.Kind;
			Candidate.Category = Source.CategoryPath;
			Candidate.bSupported = Source.bSupportedInContext;
			Candidate.SupportReason = Source.SupportReason;
			Candidate.SupportDiagnostic = Source.SupportDiagnostic;
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract;
			Candidate.PlacementContract = PlacementContract;
			Candidate.RequiredContext = RequiredContext;
			Candidate.BindingRevision = CandidateRevision(
				Target, Source.FullName);
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}

		return true;
	}

}
