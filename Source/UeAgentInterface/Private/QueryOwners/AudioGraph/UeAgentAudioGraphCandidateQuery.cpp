#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Templates/UniquePtr.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"

#include "Algo/Sort.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundEditorGraphSchema.h"
#include "MetasoundSource.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCuePalette.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::AudioGraph
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString SoundCueCandidateKind = TEXT("sound_cue_node");
		const FString SoundCueInputContract =
			TEXT("uai.audio.sound_cue_node.create/1");
		const FString SoundCuePlacementContract =
			TEXT("uai.audio.sound-cue-node-identity/1");
		const FString SoundCueRequiredContext = TEXT("target_ref:object_path");
		const FString MetaSoundCandidateKind = TEXT("metasound_node");
		const FString MetaSoundInputContract =
			TEXT("uai.audio.metasound_node.create/1");
		const FString MetaSoundPlacementContract =
			TEXT("uai.audio.metasound-node-identity/1");
		const FString MetaSoundRequiredContext = TEXT("target_ref:node_guid");

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

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject,
			const UE::UAI::Contract::FDiagnostic& Cause, FString FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty()
				? CandidateDiagnostic(MoveTemp(Code), Subject, MoveTemp(FallbackReason))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
		}

		Contract::FDiagnostic GameThreadFailure(const UObject& Target)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_game_thread_required"), Target.GetPathName(),
				TEXT("Audio graph candidates must be queried on the Editor game thread."));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other thread"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread"));
			return Diagnostic;
		}

		Contract::FDiagnostic TargetTypeFailure(const FString& Code, const UObject& Target,
			const FString& Reason, const TArray<FString>& Allowed)
		{
			auto Diagnostic = CandidateDiagnostic(Code, Target.GetPathName(), Reason);
			Diagnostic.PropertyPath = TEXT("target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetClass()->GetName());
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Target.GetClass()->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const FString& Type : Allowed) Expected.Add(MakeShared<FJsonValueString>(Type));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Expected));
			return Diagnostic;
		}

		Contract::FDiagnostic ResolvedIndexFailure(const FString& Name, int32 Index, int32 Count)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_not_found"), Name,
				TEXT("The resolved audio candidate index is outside the current catalog."));
			Diagnostic.CandidateName = Name;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("resolved_index"), Index);
			Actual->SetNumberField(TEXT("candidate_count"), Count);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("index within the current candidate catalog"));
			return Diagnostic;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool MatchesKeywordSearch(
			const FString& Query,
			const FString& Identity,
			const FString& DisplayName,
			const TArray<FString>& ExtraTerms)
		{
			// Keep the technical class name searchable for compatibility, but do not
			// use it as the public selector.  Ranking below prefers the display label.
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 Score = 0;
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Identity,
				DisplayName,
				ExtraTerms,
				Score,
				MatchReason);
		}

		bool MatchesKeywordSearch(
			const FString& Query,
			const FAudioGraphCandidate& Candidate)
		{
			return MatchesKeywordSearch(
				Query,
				Candidate.NativeClassName.IsEmpty() ? Candidate.Id : Candidate.NativeClassName,
				Candidate.Label.IsEmpty() ? Candidate.Name : Candidate.Label,
				{
					Candidate.Name,
					Candidate.Kind,
					Candidate.ActionKind,
					Candidate.Category,
					Candidate.Summary,
					Candidate.WriteSupport,
					Candidate.SupportReason,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
				});
		}

		int32 DisplayMatchRank(const FString& Query, const FAudioGraphCandidate& Candidate)
		{
			const FString Needle = Query.TrimStartAndEnd();
			if (Needle.IsEmpty()) return 100;
			const FString Label = Candidate.Label.IsEmpty() ? Candidate.Name : Candidate.Label;
			const FString Native = Candidate.NativeClassName;
			if (Label.Equals(Needle, ESearchCase::IgnoreCase)) return 0;
			if (Label.StartsWith(Needle, ESearchCase::IgnoreCase)) return 1;
			FString SegmentedNative = Native;
			for (const TCHAR* Separator : {TEXT("."), TEXT(":"), TEXT("/")})
			{
				SegmentedNative.ReplaceInline(Separator, TEXT("|"));
			}
			TArray<FString> NativeSegments;
			SegmentedNative.ParseIntoArray(NativeSegments, TEXT("|"), true);
			for (const FString& Segment : NativeSegments)
			{
				if (Segment.Equals(Needle, ESearchCase::IgnoreCase)) return 2;
			}
			if (Label.Contains(Needle, ESearchCase::IgnoreCase)) return 3;
			if (Native.Equals(Needle, ESearchCase::IgnoreCase)) return 4;
			if (Native.StartsWith(Needle, ESearchCase::IgnoreCase)) return 5;
			return 6;
		}

		// Metadata display names are localized. Build the public selector from the
		// stable frontend class-name field, rather than parsing ToString(). This
		// keeps Name, Namespace and Variant distinct for enum and Init Variable
		// classes whose textual form contains additional separators.
		FString StableMetaSoundCandidateName(
			const FMetasoundFrontendClassName& ClassName)
		{
			const FString StableName = ClassName.Name.ToString().TrimStartAndEnd();
			return StableName.IsEmpty()
				? FString()
				: FName::NameToDisplayString(StableName, false);
		}

		FString ShortMetaSoundScope(const FString& Value)
		{
			FString Scope = Value.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Scope.FindLastChar(TEXT('.'), Dot) && Dot + 1 < Scope.Len())
			{
				Scope.RightChopInline(Dot + 1);
			}
			return Scope.IsEmpty()
				? FString()
				: FName::NameToDisplayString(Scope, false);
		}

		FString ShortSoundCueScope(const UClass& Class)
		{
			FString Scope = Class.GetOutermost()->GetName();
			Scope.RemoveFromStart(TEXT("/Script/"), ESearchCase::IgnoreCase);
			return Scope.TrimStartAndEnd();
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == USoundCue::StaticClass()
				|| Target.GetClass() == UMetaSoundSource::StaticClass();
		}

		FString CandidateRevision(
			const FString& TargetRef,
			const FString& TargetRevision,
			const FString& Name,
			const FString& Kind,
			const FString& InputContract,
			const FString& PlacementContract,
			const FString& RequiredContext,
			const int32 MajorVersion,
			const int32 MinorVersion)
		{
			return Sha256(FString::Printf(
				TEXT("%s|%s|%s|%s|%d|%d|%s|%s|%s"),
				*TargetRef,
				*TargetRevision,
				*Name,
				*Kind,
				MajorVersion,
				MinorVersion,
				*InputContract,
				*PlacementContract,
				*RequiredContext));
		}

		bool SetMetaSoundCandidateIdentity(
			FAudioGraphCandidate& Candidate,
			FAudioGraphCandidateQueryError& OutError,
			bool bForCreation)
		{
			const FString Signature = bForCreation
				? FString::Printf(TEXT("metasound_create|%s|%d"), *Candidate.NativeClassName, Candidate.MajorVersion)
				: FString::Printf(TEXT("metasound_dependency|%s|%d|%d"), *Candidate.NativeClassName, Candidate.MajorVersion, Candidate.MinorVersion);
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = TEXT("metasound_frontend_class");
			Identity.Name = Candidate.NativeClassName;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Signature;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("metasound_frontend_class"),
					Signature,
					Candidate.Name,
					Identity,
					IdentityError))
			{
				return ForwardDiagnostic(OutError, CandidateCause(TEXT("audio_graph_candidate_identity_failed"),
					Candidate.Name, IdentityError, TEXT("The MetaSound candidate could not be assigned an identity.")), Candidate.Name);
			}
			Candidate.Identity = Identity.Identity;
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool BuildMetaSoundCandidates(
			const FString& TargetRef,
			const FString& TargetRevision,
			TArray<FAudioGraphCandidate>& OutCandidates,
			FAudioGraphCandidateQueryError& OutError,
			bool bForCreation)
		{
			OutCandidates.Reset();
#if WITH_EDITORONLY_DATA
			if (!ContentDiscovery::TryEnsureAssetDiscovery(OutError)) return false;
			TArray<TSharedPtr<Metasound::Editor::ISchemaQueryResult>> Classes;
			GetDefault<UMetasoundEditorGraphSchema>()->QueryNodeClasses(
				[&](TUniquePtr<Metasound::Editor::ISchemaQueryResult>&& Result)
				{
					// Context-only variable/interface classes have no asset key. Apply
					// the native class contract before asking for key or asset metadata.
					const auto Type = Result->GetRegistryClassType();
					if (Type == EMetasoundFrontendClassType::External || Type == EMetasoundFrontendClassType::Graph)
						Classes.Add(MakeShareable(Result.Release()));
				}, true, true);
			// The Builder creates the highest minor within a selected major. Include
			// asset tags in that decision without loading every candidate graph.
			TMap<FString, int32> HighestMinors;
			auto CreationKey = [](const FMetaSoundAssetKey& Key)
			{ return Key.ClassName.ToString() + TEXT("|") + FString::FromInt(Key.Version.Major); };
			for (const auto& Class : Classes)
			{
				const auto Key = Class->GetAssetKey();
				if (!Key.IsValid()) continue;
				int32& Minor = HighestMinors.FindOrAdd(CreationKey(Key), Key.Version.Minor);
				Minor = FMath::Max(Minor, Key.Version.Minor);
			}
			for (const auto& Class : Classes)
			{
				const auto Key = Class->GetAssetKey();
				const auto& ClassName = Key.ClassName;
				const auto& Version = Key.Version;
				if (!ClassName.IsValid() || !Version.IsValid()
					|| (bForCreation && Version.Minor != HighestMinors.FindRef(CreationKey(Key))))
				{
					continue;
				}
				FAudioGraphCandidate Candidate;
				Candidate.TargetRef = TargetRef;
				Candidate.TargetRevision = TargetRevision;
				Candidate.NativeClassName = ClassName.ToString();
				Candidate.NativeQueryResult = Class;
				Candidate.Label = Class->GetDisplayName().ToString();
				if (Candidate.Label.IsEmpty())
				{
					Candidate.Label = FName::NameToDisplayString(Candidate.NativeClassName, false);
				}
				Candidate.Name = StableMetaSoundCandidateName(ClassName);
				TArray<FString> ClassParts;
				bool bReadableClass = true;
				for (const FName Part : {ClassName.Namespace, ClassName.Name, ClassName.Variant})
				{
					if (Part.IsNone()) continue;
					const FString Value = Part.ToString();
					bReadableClass &= ContentDiscovery::IsReadableDiscoveryName(Value);
					ClassParts.Add(Value);
				}
				if (bReadableClass && !ClassParts.IsEmpty())
					Candidate.SelectionFields.Add(TEXT("class"), FString::Join(ClassParts, TEXT(" · ")));
				// The native record retains asset provenance even before registration.
				if (const FTopLevelAssetPath AssetPath = Class->GetAssetPath(); AssetPath.IsValid())
				{
					Candidate.Name = AssetPath.GetAssetName().ToString();
					Candidate.SelectionFields.Remove(TEXT("class"));
					const FString Source = ContentDiscovery::MakeReadableDiscoveryPath(AssetPath.ToString());
					if (!Source.IsEmpty()) Candidate.SelectionFields.Add(TEXT("asset_source"), Source);
					ContentDiscovery::AppendReadableDiscoveryPathScopes(AssetPath.GetPackageName().ToString(), Candidate.Scopes);
				}
				if (Candidate.Name.IsEmpty())
				{
					auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_name_unresolved"), Candidate.NativeClassName,
						TEXT("The MetaSound frontend class has no readable candidate name or registered asset name."));
					Diagnostic.PropertyPath = TEXT("name");
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable candidate name"));
					Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Candidate.NativeClassName);
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				if (const FString Scope = ShortMetaSoundScope(ClassName.Variant.ToString()); !Scope.IsEmpty())
				{
					Candidate.Scopes.Add(Scope);
				}
				if (const FString Scope = ShortMetaSoundScope(ClassName.Namespace.ToString()); !Scope.IsEmpty())
				{
					Candidate.Scopes.Add(Scope);
				}
				Candidate.Kind = MetaSoundCandidateKind;
				const FString ClassType = LexToString(Class->GetRegistryClassType());
				Candidate.SelectionFields.Add(TEXT("type"), ClassType);
				const FString Definition = Candidate.SelectionFields.Contains(TEXT("asset_source"))
					? Candidate.SelectionFields.FindRef(TEXT("asset_source")) : Candidate.SelectionFields.FindRef(TEXT("class"));
				if (!Definition.IsEmpty()) Candidate.SelectionFields.Add(TEXT("definition"), Definition + TEXT(" [") + ClassType + TEXT("]"));
				Candidate.ActionKind = TEXT("metasound_frontend_class");
				TArray<FString> Categories;
				for (const auto& Category : Class->GetCategoryHierarchy()) Categories.Add(Category.ToString());
				Candidate.Category = Categories.IsEmpty() ? TEXT("MetaSound") : FString::Join(Categories, TEXT("|"));
				Candidate.Summary = Class->GetTooltip().ToString();
				Candidate.WriteSupport = TEXT("frontend_builder_create");
				Candidate.SupportReason =
					TEXT("native_schema_class_binding");
				Candidate.bSupported = true;
				Candidate.SupportedOperations = {TEXT("create")};
				Candidate.InputContract = MetaSoundInputContract;
				Candidate.PlacementContract = MetaSoundPlacementContract;
				Candidate.RequiredContext = MetaSoundRequiredContext;
				Candidate.MajorVersion = Version.Major;
				Candidate.MinorVersion = Version.Minor;
				if (bForCreation) Candidate.BindingRevision = CandidateRevision(
					Candidate.TargetRef,
					Candidate.TargetRevision,
					Candidate.NativeClassName,
					Candidate.Kind,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
					Candidate.MajorVersion,
					Candidate.MinorVersion);
				if (!SetMetaSoundCandidateIdentity(
						Candidate, OutError, bForCreation))
				{
					return false;
				}
				OutCandidates.Add(MoveTemp(Candidate));
			}
#endif
			return true;
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FAudioGraphCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty()) return ForwardDiagnostic(OutError, MoveTemp(ReadError));
				auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_target_readback_failed"), Target.GetPathName(),
					bCaptured ? TEXT("The audio graph readback did not provide a target revision.")
						: TEXT("The audio graph readback failed without a diagnostic."));
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("revision");
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("target revision"));
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	static ContentDiscovery::FNameSelectionEntry AudioSelectionEntry(const FAudioGraphCandidate& Candidate)
	{
		ContentDiscovery::FNameSelectionEntry Entry;
		Entry.Name = Candidate.Name;
		Entry.Identity = Candidate.Identity.CanonicalKey.IsEmpty()
			? Candidate.NativeClassName : Candidate.Identity.CanonicalKey;
		Entry.Select = Candidate.SelectionFields;
		Entry.Select.Add(TEXT("source_kind"), Candidate.Kind);
		Entry.Select.Add(TEXT("version"), FString::Printf(TEXT("%d.%d"), Candidate.MajorVersion, Candidate.MinorVersion));
		if (!Candidate.Scopes.IsEmpty() && ContentDiscovery::IsReadableDiscoveryName(Candidate.Scopes[0]))
			Entry.Select.Add(TEXT("source"), Candidate.Scopes[0]);
		return Entry;
	}

	static bool TryQueryAudioGraphCandidatesFiltered(
		UObject& Target,
		const FString& Query,
		FAudioGraphCandidateQueryResult& OutResult,
		FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch,
		TFunctionRef<bool(const FAudioGraphCandidate&)> MatchesNativeContext,
		bool bForCreation = true)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return ForwardDiagnostic(OutError, TargetTypeFailure(TEXT("audio_graph_candidate_target_type_unsupported"), Target,
				TEXT("Audio graph candidates require a SoundCue or MetaSoundSource target."), {TEXT("SoundCue"), TEXT("MetaSoundSource")}));
		}
		struct FPreparedQueryCatalog
		{
			FAudioGraphCandidateQueryResult Metadata;
			TArray<FAudioGraphCandidate> Matches;
		};
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FPreparedQueryCatalog>(
			Batch, &Target, bForCreation ? FString() : FString(TEXT("native_dependency_metadata")), [&](FPreparedQueryCatalog& Value)
		{
			Value.Metadata.Target = Target.GetPathName();
			// Native dependency projection resolves class/version metadata during
			// target capture. It does not create a mutation binding or require a
			// target snapshot; capturing that snapshot here would recurse through
			// the same authoring document. Mutation catalogs retain their revision.
			if (bForCreation && !ReadTargetRevision(Target, Value.Metadata.TargetRevision, OutError)) return false;
			auto& Matches = Value.Matches;

			if (Target.GetClass() == USoundCue::StaticClass())
			{
				TArray<UClass*> NativeClasses;
				AudioEditor::GetSoundCueNodeClasses(NativeClasses);
				for (UClass* Class : NativeClasses)
				{
					const FString ClassPath = Class->GetPathName();
					FAudioGraphCandidate Candidate;
					Candidate.TargetRef = Value.Metadata.Target;
					Candidate.TargetRevision = Value.Metadata.TargetRevision;
					// The public selector is a stable semantic class name.  Keep the
					// native path server-side so the shared binding normalizer can pass
					// an exact loadable reference to the native mutation hook.
					FString SemanticName = Class->GetName();
					SemanticName.RemoveFromStart(TEXT("SoundNode"), ESearchCase::IgnoreCase);
					Candidate.NativeClassName = ClassPath;
					Candidate.SelectionFields.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(ClassPath));
					Candidate.Name = FName::NameToDisplayString(SemanticName, false);
					Candidate.Label = Class->GetDisplayNameText().ToString();
					if (Candidate.Label.IsEmpty()) Candidate.Label = Candidate.Name;
					Candidate.Kind = SoundCueCandidateKind;
					Candidate.ActionKind = TEXT("native_sound_cue_class");
					if (const FString Scope = ShortSoundCueScope(*Class); !Scope.IsEmpty())
					{
						Candidate.Scopes.Add(Scope);
					}
					Candidate.WriteSupport = TEXT("native_sound_cue_create");
					Candidate.SupportReason = TEXT("native_sound_cue_class_binding");
					Candidate.bSupported = true;
					Candidate.SupportedOperations = {TEXT("create")};
					Candidate.InputContract = SoundCueInputContract;
					Candidate.PlacementContract = SoundCuePlacementContract;
					Candidate.RequiredContext = SoundCueRequiredContext;
					if (bForCreation) Candidate.BindingRevision = CandidateRevision(
						Candidate.TargetRef,
						Candidate.TargetRevision,
						Candidate.NativeClassName,
						Candidate.Kind,
						Candidate.InputContract,
						Candidate.PlacementContract,
						Candidate.RequiredContext,
						Candidate.MajorVersion,
						Candidate.MinorVersion);
					Matches.Add(MoveTemp(Candidate));
				}
			}
			else
			{
				TArray<FAudioGraphCandidate> Catalog;
				if (!BuildMetaSoundCandidates(
						Value.Metadata.Target,
						Value.Metadata.TargetRevision,
						Catalog,
						OutError, bForCreation))
				{
					return false;
				}
				for (FAudioGraphCandidate& Candidate : Catalog) Matches.Add(MoveTemp(Candidate));
			}

			Algo::SortBy(Matches, [](const FAudioGraphCandidate& Candidate)
			{
				return Candidate.Name + FString::Printf(TEXT("|%08d|%08d"), Candidate.MajorVersion, Candidate.MinorVersion);
		});
		return true;
		});
		if (!Prepared) return false;
		OutResult = Prepared->Metadata;
		const auto& Matches = Prepared->Matches;
		TArray<int32> MatchedIndices;
		for (int32 CandidateIndex = 0; CandidateIndex < Matches.Num(); ++CandidateIndex)
		{
			if (MatchesNativeContext(Matches[CandidateIndex])
				&& MatchesKeywordSearch(Query, Matches[CandidateIndex]))
			{
				MatchedIndices.Add(CandidateIndex);
			}
		}
		Algo::Sort(MatchedIndices, [&Matches, &Query](const int32 A, const int32 B)
		{
			const int32 RankA = DisplayMatchRank(Query, Matches[A]);
			const int32 RankB = DisplayMatchRank(Query, Matches[B]);
			if (RankA != RankB) return RankA < RankB;
			const FString ScopeA = Matches[A].Scopes.IsEmpty() ? FString() : Matches[A].Scopes[0];
			const FString ScopeB = Matches[B].Scopes.IsEmpty() ? FString() : Matches[B].Scopes[0];
			const int32 ScopeOrder = ScopeA.Compare(ScopeB, ESearchCase::IgnoreCase);
			if (ScopeOrder != 0) return ScopeOrder < 0;
			const int32 NameOrder = Matches[A].Name.Compare(Matches[B].Name, ESearchCase::IgnoreCase);
			if (NameOrder != 0) return NameOrder < 0;
			// Only used as a server-side deterministic tie-breaker when two entries
			// have the same readable label and semantic scope.
			return Matches[A].NativeClassName.Compare(Matches[B].NativeClassName, ESearchCase::IgnoreCase) < 0;
		});
		OutResult.TotalMatches = MatchedIndices.Num();
		for (int32 MatchIndex = 0; MatchIndex < MatchedIndices.Num(); ++MatchIndex)
		{
			FAudioGraphCandidate Candidate = Matches[MatchedIndices[MatchIndex]];
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		return true;
	}

	bool TryQueryAudioGraphCandidates(
		UObject& Target, const FString& Query, FAudioGraphCandidateQueryResult& OutResult,
		FAudioGraphCandidateQueryError& OutError, ContentDiscovery::FReadOnlyCandidateBatch* Batch)
	{
		return TryQueryAudioGraphCandidatesFiltered(Target, Query, OutResult, OutError, Batch,
			[](const FAudioGraphCandidate&) { return true; });
	}

	bool TryResolveAudioGraphNativeCandidate(
		UObject& Target, const FString& NativeClassName, const int32 MajorVersion, const int32 MinorVersion,
		FAudioGraphCandidate& OutCandidate, FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch)
	{
		OutCandidate = {};
		FAudioGraphCandidateQueryResult Result;
		if (!TryQueryAudioGraphCandidatesFiltered(Target, FString(), Result, OutError, Batch,
			[&](const FAudioGraphCandidate& Candidate)
			{
				return Candidate.NativeClassName == NativeClassName
					&& Candidate.MajorVersion == MajorVersion && Candidate.MinorVersion == MinorVersion;
			}, false)) return false;
		if (Result.Candidates.Num() != 1)
		{
			auto Diagnostic = CandidateDiagnostic(Result.Candidates.IsEmpty()
				? TEXT("audio_graph_native_candidate_not_found") : TEXT("audio_graph_native_candidate_ambiguous"), Target.GetPathName(),
				TEXT("The native audio class and version must resolve to exactly one current catalog candidate."));
			Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Result.Candidates.Num());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			auto Evidence = MakeShared<FJsonObject>();
			Evidence->SetStringField(TEXT("class"), NativeClassName);
			Evidence->SetNumberField(TEXT("major_version"), MajorVersion);
			Evidence->SetNumberField(TEXT("minor_version"), MinorVersion);
			Diagnostic.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutCandidate = MoveTemp(Result.Candidates[0]);
		return true;
	}

	bool TryResolveSoundCueCandidate(UObject& Target, const FString& Name,
		FAudioGraphCandidate& OutCandidate, FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch, const TMap<FString, FString>& Select)
	{
		if (Target.GetClass() != USoundCue::StaticClass())
			return ForwardDiagnostic(OutError, TargetTypeFailure(TEXT("audio_graph_candidate_target_type_unsupported"), Target,
				TEXT("SoundCue candidate selection requires a SoundCue target."), {TEXT("SoundCue")}));
		FAudioGraphCandidateQueryResult Query;
		if (!TryQueryAudioGraphCandidates(Target, FString(), Query, OutError, Batch)) return false;
		TArray<ContentDiscovery::FNameSelectionEntry> Entries;
		for (const FAudioGraphCandidate& Candidate : Query.Candidates)
		{
			Entries.Add(AudioSelectionEntry(Candidate));
		}
		int32 Index = INDEX_NONE;
		UE::UAI::Contract::FDiagnostic Error;
		if (!ContentDiscovery::TryResolveNameSelection(Entries, Name, Select, Index, Error, ContentDiscovery::ENameComparison::IgnoreCase))
		{
			return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(Error.Code, Name, Target.GetPathName(), TEXT("SoundCue graph menu"), TEXT("audio.graph.candidates"), Error), Name);
		}
		if (!Query.Candidates.IsValidIndex(Index)) return ForwardDiagnostic(OutError, ResolvedIndexFailure(Name, Index, Query.Candidates.Num()), Name);
		OutCandidate = Query.Candidates[Index];
		return true;
	}

	bool TryResolveAudioGraphCandidate(
		UObject& Target,
		const FString& Name,
		FAudioGraphCandidate& OutCandidate,
		FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch, const TMap<FString, FString>& Select)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return ForwardDiagnostic(OutError, GameThreadFailure(Target), Target.GetPathName());
		}
		if (Target.GetClass() != UMetaSoundSource::StaticClass())
		{
			return ForwardDiagnostic(OutError, TargetTypeFailure(TEXT("audio_graph_candidate_compact_selector_unsupported"), Target,
				TEXT("Readable MetaSound candidate selection requires a MetaSoundSource target."), {TEXT("MetaSoundSource")}));
		}
		struct FPreparedResolveCatalog
		{
			TArray<FAudioGraphCandidate> Candidates;
			TArray<ContentDiscovery::FNameSelectionEntry> Names;
		};
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FPreparedResolveCatalog>(
			Batch, &Target, FString(), [&](FPreparedResolveCatalog& Value)
		{
			FString TargetRevision;
			if (!ReadTargetRevision(Target, TargetRevision, OutError)
				|| !BuildMetaSoundCandidates(Target.GetPathName(), TargetRevision, Value.Candidates, OutError, true)) return false;
			for (const FAudioGraphCandidate& Candidate : Value.Candidates)
			{
				Value.Names.Add(AudioSelectionEntry(Candidate));
			}
			return true;
		});
		if (!Prepared) return false;
		int32 ResolvedIndex = INDEX_NONE;
		UE::UAI::Contract::FDiagnostic ResolveError;
		if (!ContentDiscovery::TryResolveNameSelection(Prepared->Names, Name, Select, ResolvedIndex, ResolveError, ContentDiscovery::ENameComparison::IgnoreCase))
		{
			return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(ResolveError.Code, Name, Target.GetPathName(), TEXT("MetaSound graph menu"), TEXT("audio.graph.candidates"), ResolveError), Name);
		}
		if (!Prepared->Candidates.IsValidIndex(ResolvedIndex)) return ForwardDiagnostic(OutError,
			ResolvedIndexFailure(Name, ResolvedIndex, Prepared->Candidates.Num()), Name);
		OutCandidate = Prepared->Candidates[ResolvedIndex];
		const FMetasoundFrontendClass* Loaded = OutCandidate.NativeQueryResult.IsValid()
			? OutCandidate.NativeQueryResult->FindClass() : nullptr;
		if (!Loaded || Loaded->Metadata.GetClassName().ToString() != OutCandidate.NativeClassName
			|| Loaded->Metadata.GetVersion().Major != OutCandidate.MajorVersion
			|| Loaded->Metadata.GetVersion().Minor != OutCandidate.MinorVersion)
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("audio_graph_candidate_class_load_failed"), Name,
				Loaded ? TEXT("The selected MetaSound class changed since its catalog declaration; query the candidate again.")
					: TEXT("The selected MetaSound class could not be loaded and registered by its native schema owner."));
			Diagnostic.TargetPath = Target.GetPathName();
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(OutCandidate.NativeQueryResult.IsValid()
				? OutCandidate.NativeQueryResult->GetAssetKey().ToString() : OutCandidate.NativeClassName);
			if (Loaded) Diagnostic.ActualValue = MakeShared<FJsonValueString>(FMetaSoundAssetKey(Loaded->Metadata).ToString());
			else Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Name);
		}
		return true;
	}

}
