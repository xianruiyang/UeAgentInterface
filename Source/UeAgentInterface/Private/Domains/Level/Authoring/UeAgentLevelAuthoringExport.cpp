// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"

#include "Components/ActorComponent.h"
#include "Containers/BitArray.h"
#include "CoreGlobals.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentPropertyReadScope.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentThreadCpuTiming.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "Domains/Level/Authoring/UeAgentLevelPartitionReadScope.h"

namespace UeAgentLevelAuthoring
{
	static bool CaptureLevelNativeState(UWorld& World, FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId,
		const TArray<FLevelActorCaptureScope>* Scope, const TSet<FString>* WorldPropertyNames = nullptr,
		bool bAuthoringExportOnly = false);
	void RetainWorldPartitionActorsForAuthoring(UWorld& World,
		FWorldPartitionHelpers::FForEachActorWithLoadingResult& OutLoadedActors)
	{
		if (UWorldPartition* Partition = World.GetWorldPartition()) {
			FWorldPartitionHelpers::FForEachActorWithLoadingParams Params;
			Params.bKeepReferences = true; Params.ActorClasses = {AActor::StaticClass()};
			FWorldPartitionHelpers::ForEachActorWithLoading(Partition,
				[](const FWorldPartitionActorDescInstance*) { return true; }, Params, OutLoadedActors);
		}
	}
	FLevelPartitionReadScope::FLevelPartitionReadScope(UWorld& World)
	{
		RetainWorldPartitionActorsForAuthoring(World, LoadedActors);
	}
	namespace
	{
		bool CompleteLevelCanonicalCapture(FLevelAuthoringCapture& Capture,
			UE::UAI::Contract::FDiagnostic& Error, const FString& ProfileId,
			bool bBuildLegacyFingerprint);
		struct FLevelCaptureReuse
		{
			TWeakObjectPtr<UWorld> World;
			FString WorldPath;
			// Only native Engine classes, never World/Actor/component instances.
			// Declared before Snapshot so detached storage is destroyed first.
			TArray<TStrongObjectPtr<UClass>> NativeClasses;
			UeAgentAuthoringState::FStateSnapshot Snapshot;
			TSharedPtr<const FLevelSemanticFactsSet> SemanticFacts;
			TMap<FString, UClass*> RootClasses;
			TMap<FString, bool> RootTemplates;
			TMap<const FProperty*, FString> PropertySchemas;
			// Call-local handoff only. Cleared before publishing the next candidate.
			TMap<FString, TWeakObjectPtr<UObject>> ObservedRoots;
			TMap<const UClass*, TMap<FString, FProperty*>> ObservedFields;
			bool bSceneEditing = false;
			bool bActive = false;
			bool bAwaitingPersistentReload = false;
		};
		thread_local TUniquePtr<FLevelCaptureReuse> PendingCaptureReuse;
		thread_local TUniquePtr<FLevelCaptureReuse> ObservedCaptureReuse;
		// Positions only, weakly bound to the snapshot's sealed identity order.
		// Every current request still proves its complete identity before use.
		struct FRequestEntryPositions
		{
			TWeakPtr<const uint8> Membership;
			TArray<int32> Entries;
		};
		thread_local FRequestEntryPositions PreviousRequestPositions;
		FDelegateHandle CapturePreGcHandle;
		FDelegateHandle CapturePreExitHandle;
		bool bCaptureObservationEnabled = false;

		void AppendSchemaToken(FString& Key, const FString& Value)
		{ Key += FString::FromInt(Value.Len()) + TEXT(":") + Value; }

		void AppendPropertySchema(const FProperty* Property, FString& Key, TSet<const FProperty*>& Visiting)
		{
			if (!Property || Visiting.Contains(Property)) return;
			Visiting.Add(Property);
			AppendSchemaToken(Key, Property->GetPathName());
			AppendSchemaToken(Key, Property->GetCPPType());
			AppendSchemaToken(Key, FString::Printf(TEXT("%llu:%d:%d"),
				static_cast<uint64>(Property->GetPropertyFlags()), Property->ArrayDim, Property->ElementSize));
			AppendSchemaToken(Key, Property->GetDisplayNameText().ToString());
			AppendSchemaToken(Key, Property->GetToolTipText().ToString());
			if (const auto* Metadata = Property->GetMetaDataMap()) {
				TArray<FName> Names; Metadata->GetKeys(Names);
				Names.Sort(FNameLexicalLess());
				for (FName Name : Names) { AppendSchemaToken(Key, Name.ToString()); AppendSchemaToken(Key, Metadata->FindChecked(Name)); }
			}
			if (const auto Enum = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(
				UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Property), false, Property))
				AppendSchemaToken(Key, UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(Enum)));
			if (const auto* Struct = CastField<FStructProperty>(Property)) {
				AppendSchemaToken(Key, Struct->Struct->GetPathName());
				for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It) AppendPropertySchema(*It, Key, Visiting);
			}
			if (const auto* Array = CastField<FArrayProperty>(Property)) AppendPropertySchema(Array->Inner, Key, Visiting);
			if (const auto* Set = CastField<FSetProperty>(Property)) AppendPropertySchema(Set->ElementProp, Key, Visiting);
			if (const auto* Map = CastField<FMapProperty>(Property)) {
				AppendPropertySchema(Map->KeyProp, Key, Visiting); AppendPropertySchema(Map->ValueProp, Key, Visiting);
			}
			if (const auto* Optional = CastField<FOptionalProperty>(Property)) AppendPropertySchema(Optional->GetValueProperty(), Key, Visiting);
			Visiting.Remove(Property);
		}

		FString PropertySchema(const FProperty* Property)
		{ FString Key; TSet<const FProperty*> Visiting; AppendPropertySchema(Property, Key, Visiting); return Key; }
		bool HasStableNativeOwner(const UObject* Object);
		const UObject* CaptureOwner(const UObject* Object);
		bool IsLevelReflectedProperty(const FString& Semantic);

		TUniquePtr<FLevelCaptureReuse> MakeCaptureReuse(UWorld& World,
			const UeAgentAuthoringState::FStateSnapshot& Snapshot, const bool bSceneEditing,
			const TSharedPtr<const FLevelSemanticFactsSet>& SemanticFacts)
		{
			if (!bCaptureObservationEnabled || World.IsPartitionedWorld() || !Snapshot.GetObservationMembership()) return {};
			auto Next = MakeUnique<FLevelCaptureReuse>();
			Next->World = &World; Next->WorldPath = World.GetPathName();
			Next->bSceneEditing = bSceneEditing; Next->Snapshot = Snapshot;
			Next->SemanticFacts = SemanticFacts;
			TSet<UClass*> NativeClasses;
			TSet<FString> ObservedRootPaths;
			for (int32 EntryIndex = 0; EntryIndex < Snapshot.Num(); ++EntryIndex) {
				const auto& Entry = Snapshot.ReadAuthoringEntry(EntryIndex);
				const auto& Binding = Snapshot.ReadNativeBinding(EntryIndex);
				const FString Path = Binding.NativeTargetIdentity.GetSelector(TEXT("owner_path"));
				if (!Path.IsEmpty() && !ObservedRootPaths.Contains(Path)) {
					ObservedRootPaths.Add(Path);
					UObject* Root = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
					if (HasStableNativeOwner(Root) && HasStableNativeOwner(CaptureOwner(Root))) {
						Next->RootTemplates.Add(Path, Root->IsTemplate());
						Next->RootClasses.Add(Path, Root->GetClass());
						if (!NativeClasses.Contains(Root->GetClass())) {
							NativeClasses.Add(Root->GetClass()); Next->NativeClasses.Emplace(Root->GetClass());
						}
					}
				}
				// Unknown owners remain in the complete snapshot and are probed on
				// every capture. They do not invalidate the separately proven native
				// owner indices or contribute unprotected class/field pointers.
				if (!Next->RootClasses.Contains(Path)) continue;
				const FProperty* Property = Binding.NativeValue ? Binding.NativeValue->GetProperty() : nullptr;
				if (!Property && IsLevelReflectedProperty(Entry.SemanticId))
					if (UClass* Class = Next->RootClasses.FindRef(Path))
						Property = FindFProperty<FProperty>(Class, *Binding.NativeTargetIdentity.GetSelector(TEXT("property_path")));
				if (Property) {
					if (!Next->PropertySchemas.Contains(Property)) Next->PropertySchemas.Add(Property, PropertySchema(Property));
				}
			}
			return Next;
		}

		TUniquePtr<FLevelCaptureReuse> FinishObservedCaptureReuse(UWorld& World,
			const UeAgentAuthoringState::FStateSnapshot& Snapshot, const bool bSceneEditing,
			const TSharedPtr<const FLevelSemanticFactsSet>& SemanticFacts,
			TUniquePtr<FLevelCaptureReuse> Candidate)
		{
			const double Started = FPlatformTime::Seconds();
			bool bComplete = Candidate && bCaptureObservationEnabled && !World.IsPartitionedWorld()
				&& Candidate->WorldPath == World.GetPathName() && Candidate->bSceneEditing == bSceneEditing
				&& Candidate->Snapshot.SharesStorageWith(Snapshot)
				&& Candidate->ObservedRoots.Num() == Candidate->RootClasses.Num()
				&& Candidate->RootClasses.Num() == Candidate->RootTemplates.Num();
			int32 Roots = 0, Fields = 0;
			TMap<const FProperty*, FString> Schemas;
			if (bComplete) for (const auto& Pair : Candidate->ObservedRoots) {
				UObject* Root = Pair.Value.Get();
				if (!HasStableNativeOwner(Root) || !Root->GetPathName().Equals(Pair.Key, ESearchCase::CaseSensitive)
					|| Root->GetClass() != Candidate->RootClasses.FindRef(Pair.Key)) { bComplete = false; break; }
				Candidate->RootTemplates.FindChecked(Pair.Key) = Root->IsTemplate();
				++Roots;
			}
			if (bComplete) for (const auto& ClassFields : Candidate->ObservedFields) {
				for (const auto& Pair : ClassFields.Value) {
					const FProperty* Field = FindFProperty<FProperty>(ClassFields.Key, *Pair.Key);
					if (!Field || Field != Pair.Value) { bComplete = false; break; }
					++Fields;
					if (!Schemas.Contains(Field)) {
						FString Schema = PropertySchema(Field);
						const FString* Previous = Candidate->PropertySchemas.Find(Field);
						if (!Previous || !Schema.Equals(*Previous, ESearchCase::CaseSensitive)) { bComplete = false; break; }
						Schemas.Add(Field, MoveTemp(Schema));
					}
				}
				if (!bComplete) break;
			}
			bComplete = bComplete && Schemas.Num() == Candidate->PropertySchemas.Num();
			if (bComplete) {
				Candidate->World = &World;
				Candidate->SemanticFacts = SemanticFacts;
				Candidate->PropertySchemas = MoveTemp(Schemas);
				Candidate->ObservedRoots.Empty(); Candidate->ObservedFields.Empty();
				Candidate->bActive = false;
			} else Candidate = MakeCaptureReuse(World, Snapshot, bSceneEditing, SemanticFacts);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_candidate indexed=%s entries=%d roots=%d class_fields=%d total_s=%.6f"),
				bComplete ? TEXT("true") : TEXT("false"), Snapshot.Num(), Roots, Fields, FPlatformTime::Seconds() - Started);
			return Candidate;
		}

		const UObject* CaptureOwner(const UObject* Object)
		{
			if (const auto* Component = Cast<UActorComponent>(Object)) return Component->GetOwner();
			return Object;
		}

		bool HasStableNativeOwner(const UObject* Object)
		{
			// Level's three property semantics all use the same reflected accessor.
			// Derived Actor/Component state is a separate, always-read request. Do
			// not force a complete Actor recapture just for an unlisted native class.
			static const FName EnginePackageName(TEXT("/Script/Engine"));
			const UClass* Class = Object ? Object->GetClass() : nullptr;
			return Class && Class->GetOutermost()->GetFName() == EnginePackageName
				&& Class->HasAnyClassFlags(CLASS_Native)
				&& !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
		}
		bool IsLevelReflectedProperty(const FString& Semantic)
		{ return Semantic == WorldPropertySemanticId || Semantic == ActorPropertySemanticId || Semantic == ComponentPropertySemanticId; }

		bool IsDetachedValueComparable(const FProperty* Property, TSet<const UStruct*>& Visiting)
		{
			// Object/owned-object codecs can expose state beyond the copied pointer.
			// Read those values again, including references nested in containers.
			if (!Property || Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)
				|| CastField<FObjectPropertyBase>(Property) || CastField<FInterfaceProperty>(Property)
				|| CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property)) return false;
			if (const auto* Struct = CastField<FStructProperty>(Property))
			{
				if (Visiting.Contains(Struct->Struct)) return false;
				Visiting.Add(Struct->Struct);
				for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
					if (!IsDetachedValueComparable(*It, Visiting)) { Visiting.Remove(Struct->Struct); return false; }
				Visiting.Remove(Struct->Struct);
			}
			if (const auto* Array = CastField<FArrayProperty>(Property)) return IsDetachedValueComparable(Array->Inner, Visiting);
			if (const auto* Set = CastField<FSetProperty>(Property)) return IsDetachedValueComparable(Set->ElementProp, Visiting);
			if (const auto* Map = CastField<FMapProperty>(Property))
				return IsDetachedValueComparable(Map->KeyProp, Visiting) && IsDetachedValueComparable(Map->ValueProp, Visiting);
			if (const auto* Optional = CastField<FOptionalProperty>(Property)) return IsDetachedValueComparable(Optional->GetValueProperty(), Visiting);
			return true;
		}

		void ReleaseCollectableNativeValues(TUniquePtr<FLevelCaptureReuse>& Cache)
		{
			if (!Cache) return;
			TMap<const FProperty*, bool> Comparable;
			int32 Retained = 0;
			Cache->Snapshot.ReleaseNativeValuesIf([&](const UeAgentAuthoringState::FStateEntry& Entry,
				const UeAgentAuthoringState::FStateNativeBinding& Binding) {
				if (!Binding.NativeValue || !IsLevelReflectedProperty(Entry.SemanticId)) return true;
				// Arbitrary policy functions may capture the old World. Only the
				// plain policy flags can accompany detached values across GC/reload.
				if (Binding.NativeReadLimits && (Binding.NativeReadLimits->StructFieldFilter
					|| Binding.NativeReadLimits->AuthorizeProperty)) return true;
				// These class pointers are owned by NativeClasses. Pure native values
				// do not refer to the old World instance and may survive its unload.
				if (!Cache->RootClasses.Contains(Binding.NativeTargetIdentity.GetSelector(TEXT("owner_path")))) return true;
				const FProperty* Property = Binding.NativeValue->GetProperty();
				bool* Safe = Comparable.Find(Property);
				if (!Safe) { TSet<const UStruct*> Visiting; Safe = &Comparable.Add(Property, IsDetachedValueComparable(Property, Visiting)); }
				if (*Safe) ++Retained;
				return !*Safe;
			});
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_gc entries=%d retained_detached_values=%d native_classes=%d"),
				Cache->Snapshot.Num(), Retained, Cache->NativeClasses.Num());
		}

		TOptional<bool> TryCaptureWithNativeBaseline(UWorld& World, const bool bSceneEditing,
			const TArray<UeAgentAuthoringState::FReadRequestBinding>& Requests,
			FScopedLevelSemanticFacts& SemanticFacts,
			UeAgentAuthoringState::FCaptureResult& OutCapture, UE::UAI::Contract::FDiagnostic& Error,
			TUniquePtr<FLevelCaptureReuse>& OutObservation)
		{
			auto Baseline = PendingCaptureReuse && PendingCaptureReuse->bActive
				? MoveTemp(PendingCaptureReuse) : MoveTemp(ObservedCaptureReuse);
			if (!Baseline) return {};
			if (World.IsPartitionedWorld() || Baseline->WorldPath != World.GetPathName() || Baseline->bSceneEditing != bSceneEditing) {
				const int32 RetiredEntries = Baseline->Snapshot.Num();
				const double RetireStarted = FPlatformTime::Seconds();
				Baseline.Reset();
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_miss retired_entries=%d requested_entries=%d retire_s=%.6f reason=world_or_profile"),
					RetiredEntries, Requests.Num(), FPlatformTime::Seconds() - RetireStarted);
				return {};
			}
			const double Started = FPlatformTime::Seconds();
			const auto Entries = Baseline->Snapshot.ReadAuthoringEntries();
			const bool bReusePositions = PreviousRequestPositions.Membership.Pin() == Baseline->Snapshot.GetObservationMembership()
				&& PreviousRequestPositions.Entries.Num() == Requests.Num();
			TArray<int32> CurrentPositions;
			CurrentPositions.SetNumUninitialized(Requests.Num());
			TBitArray<> Visited(false, Entries.Num());
			TArray<const UObject*> EntryOwners;
			EntryOwners.Init(nullptr, Requests.Num());
			int32 ReusedPositions = 0;
			const double Indexed = FPlatformTime::Seconds();
			const auto ObservationCpu = UE::UAI::Timing::SampleThreadCpu();
			TMap<const UObject*, TArray<int32>> OwnerRequestIndices;
			TSet<const UObject*> ChangedOwners;
			TBitArray<> ReadIndices(false, Requests.Num());
			TArray<int32> OrderedProbeIndices;
			const auto MarkProbe = [&](int32 Index) { ReadIndices[Index] = true; OrderedProbeIndices.Add(Index); };
			struct FCurrentPropertyProof
			{
				bool bSchemaMatches = false;
				bool bDetachedComparable = false;
			};
			TMap<const FProperty*, FCurrentPropertyProof> CurrentPropertyProofs;
			TMap<const FProperty*, FString> CurrentSchemas;
			TMap<const UClass*, TMap<FString, FProperty*>> CurrentFields;
			TMap<const UObject*, bool> RootProofs;
			int32 ObservedReferences = 0, ReusedSemanticFacts = 0;
			int32 ForcedOwner = 0, ForcedSemantic = 0, ForcedSchema = 0, ForcedReference = 0, ForcedNativeValue = 0, NewIdentities = 0;
			TMap<const FProperty*, int32> ReferenceFamilies;
			int32 UnclassifiedReferences = 0;
			for (int32 Index = 0; Index < Requests.Num(); ++Index)
			{
				const auto& Binding = Requests[Index];
				const auto& SemanticId = Binding.GetSemanticId();
				const auto& PropertyPath = Binding.GetPropertyPath();
				const UObject* RootObject = Binding.GetRootObject();
				if (!IsValid(RootObject)) return {};
				const UObject* Owner = CaptureOwner(RootObject);
				EntryOwners[Index] = Owner;
				OwnerRequestIndices.FindOrAdd(Owner).Add(Index);
				int32 EntryIndex = bReusePositions ? PreviousRequestPositions.Entries[Index] : INDEX_NONE;
				if (Entries.IsValidIndex(EntryIndex)
					&& Entries[EntryIndex].SemanticId == SemanticId
					&& Binding.MatchesSnapshotIdentityKey(Entries[EntryIndex].TargetIdentity.CanonicalKey)) ++ReusedPositions;
				else EntryIndex = Baseline->Snapshot.FindEntryIndex(SemanticId, Binding.GetSnapshotIdentityKey());
				if (!Entries.IsValidIndex(EntryIndex)) {
					CurrentPositions[Index] = INDEX_NONE;
					++NewIdentities; ChangedOwners.Add(Owner); MarkProbe(Index); continue;
				}
				if (Visited[EntryIndex]) return {};
				Visited[EntryIndex] = true;
				CurrentPositions[Index] = EntryIndex;
				const auto* Previous = &Entries[EntryIndex];
				const auto& PreviousBinding = Baseline->Snapshot.ReadNativeBinding(EntryIndex);
				if (!HasStableNativeOwner(RootObject) || !HasStableNativeOwner(Owner)) {
					++ForcedOwner; ChangedOwners.Add(Owner); MarkProbe(Index); continue;
				}
				const auto& CurrentLimits = Binding.GetReadLimits();
				const auto& PreviousLimits = PreviousBinding.NativeReadLimits;
				if ((!PreviousLimits && IsLevelReflectedProperty(SemanticId))
					|| CurrentLimits.StructFieldFilter || CurrentLimits.AuthorizeProperty
					|| (PreviousLimits && (PreviousLimits->StructFieldFilter || PreviousLimits->AuthorizeProperty))
					|| CurrentLimits.bApplyAuthoringStructFieldPolicy != (PreviousLimits && PreviousLimits->bApplyAuthoringStructFieldPolicy)
					|| CurrentLimits.bShowDefaultsOnly != (PreviousLimits && PreviousLimits->bShowDefaultsOnly)
					|| CurrentLimits.bUseInstanceEditability != (PreviousLimits && PreviousLimits->bUseInstanceEditability)) {
					++ForcedOwner; ChangedOwners.Add(Owner); MarkProbe(Index); continue;
				}
				if (PropertyPath.IsEmpty() || !IsLevelReflectedProperty(SemanticId))
				{
					if (SemanticFacts.ObserveUnchanged(Binding.Materialize(), Baseline->SemanticFacts)) ++ReusedSemanticFacts;
					else { ++ForcedSemantic; MarkProbe(Index); }
					continue;
				}
				// Logical identity is already matched against the freshly enumerated
				// complete request set. A reloaded native instance may reuse only pure
				// detached values of the exact protected class/field; every current
				// value and schema is still checked below, refs/metadata read afresh.
				bool* RootValid = RootProofs.Find(RootObject);
				if (!RootValid) {
					const FString Path = RootObject->GetPathName();
					RootValid = &RootProofs.Add(RootObject,
						Baseline->RootClasses.FindRef(Path) == RootObject->GetClass());
					if (*RootValid && Baseline->RootTemplates.FindChecked(Path) != RootObject->IsTemplate()) ChangedOwners.Add(Owner);
				}
				if (!*RootValid) { ++ForcedOwner; ChangedOwners.Add(Owner); MarkProbe(Index); continue; }
				// Resolve each actual class/field once within this synchronous capture;
				// instances still compare their own live value and protected schema.
				auto& Fields = CurrentFields.FindOrAdd(RootObject->GetClass());
				FProperty** FoundField = Fields.Find(PropertyPath);
				if (!FoundField) FoundField = &Fields.Add(PropertyPath,
						FindFProperty<FProperty>(RootObject->GetClass(), *PropertyPath));
				FProperty* Property = *FoundField;
				if (!Property || (PreviousBinding.NativeValue && Property != PreviousBinding.NativeValue->GetProperty())) {
					++ForcedSchema; ChangedOwners.Add(Owner); MarkProbe(Index); continue;
				}
				auto* PropertyProof = CurrentPropertyProofs.Find(Property);
				if (!PropertyProof) {
					// CurrentSchemas was already captured once per property in this
					// synchronous observation. Compare that same declaration with the
					// immutable baseline once too; values and references below are live.
					const FString& Schema = CurrentSchemas.Add(Property, PropertySchema(Property));
					const FString* PreviousSchema = Baseline->PropertySchemas.Find(Property);
					FCurrentPropertyProof Proof;
					Proof.bSchemaMatches = PreviousSchema && Schema.Equals(*PreviousSchema, ESearchCase::CaseSensitive);
					if (Proof.bSchemaMatches) {
						TSet<const UStruct*> Visiting;
						Proof.bDetachedComparable = IsDetachedValueComparable(Property, Visiting);
					}
					PropertyProof = &CurrentPropertyProofs.Add(Property, Proof);
				}
				if (!PropertyProof->bSchemaMatches) { ++ForcedSchema; ChangedOwners.Add(Owner); MarkProbe(Index); continue; }
				const void* Current = Property->ContainerPtrToValuePtr<void>(RootObject);
				if (!PropertyProof->bDetachedComparable) {
					TSharedPtr<FJsonValue> ActualReference;
					if (UeAgentPropertyValueCodec::TryEncodePathOnlyAssetReferences(Property, Current,
						const_cast<UObject*>(RootObject), ActualReference)) {
						++ObservedReferences;
						// Observe exported spelling too; native FName identity is not
						// proof that the current path text stayed unchanged.
						if (!UeAgentAuthoringState::AreStableCanonicalValuesEqual(ActualReference, Previous->Value)) ChangedOwners.Add(Owner);
					} else {
						++ForcedReference; MarkProbe(Index);
						if (ReferenceFamilies.Contains(Property) || ReferenceFamilies.Num() < 64)
							++ReferenceFamilies.FindOrAdd(Property);
						else ++UnclassifiedReferences;
					}
					continue;
				}
				if (!PreviousBinding.NativeValue) { ++ForcedNativeValue; MarkProbe(Index); continue; }
				for (int32 Element = 0; Element < Property->ArrayDim; ++Element)
					if (!Property->Identical(static_cast<const uint8*>(PreviousBinding.NativeValue->GetValuePtr()) + Element * Property->ElementSize,
						static_cast<const uint8*>(Current) + Element * Property->ElementSize)) { ChangedOwners.Add(Owner); break; }
			}
			const double Compared = FPlatformTime::Seconds();
			const auto ComparedCpu = UE::UAI::Timing::SampleThreadCpu();
			// The native comparison above used only current requests. Select those
			// actually present in the old observation; no outside row becomes current
			// merely because the old full snapshot was available. Missing identities
			// are captured below, without discarding unrelated successful comparisons.
			const int32 PreviousEntryCount = Baseline->Snapshot.Num();
			TArray<int32> MatchedPositions;
			MatchedPositions.Reserve(Requests.Num() - NewIdentities);
			for (int32 Position : CurrentPositions) if (Position != INDEX_NONE) MatchedPositions.Add(Position);
			const bool bSameScope = NewIdentities == 0 && MatchedPositions.Num() == PreviousEntryCount;
			const double SelectionStarted = FPlatformTime::Seconds();
			UeAgentAuthoringState::FStateSnapshot Selected;
			if (bSameScope) Selected = MoveTemp(Baseline->Snapshot);
			else {
				if (!Baseline->Snapshot.TrySelectEntries(MatchedPositions, Selected)) return {};
				// Retire the unused entries now, under the original native-class roots.
				// They are not retained as another generation or moved outside timing.
				Baseline->Snapshot = {};
			}
			TArray<const UObject*> SelectedOwners;
			SelectedOwners.Init(nullptr, Selected.Num());
			for (int32 Index = 0; Index < Requests.Num(); ++Index) {
				if (CurrentPositions[Index] == INDEX_NONE) continue;
				const int32 Position = bSameScope ? CurrentPositions[Index]
					: Selected.FindEntryIndex(Requests[Index].GetSemanticId(), Requests[Index].GetSnapshotIdentityKey());
				SelectedOwners[Position] = EntryOwners[Index];
			}
			const double SelectedAt = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_selection previous_entries=%d selected_entries=%d requested_entries=%d new_identities=%d selection_and_retire_s=%.6f snapshot_outside_scope=false"),
				PreviousEntryCount, Selected.Num(), Requests.Num(), NewIdentities, SelectedAt - SelectionStarted);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_property_observation_proofs requests=%d properties=%d"),
				Requests.Num(), CurrentPropertyProofs.Num());
			TArray<UeAgentAuthoringState::FReadRequest> ProbeRequests;
			for (const UObject* Owner : ChangedOwners)
				if (const auto* Indices = OwnerRequestIndices.Find(Owner))
					for (int32 Index : *Indices) if (!ReadIndices[Index]) MarkProbe(Index);
			// The complete current pass built this index; sorting only the selected
			// positions preserves the original callback order, including unknown owners.
			if (!ChangedOwners.IsEmpty()) OrderedProbeIndices.Sort();
			const int32 ProbeSelectionVisits = OrderedProbeIndices.Num();
			ProbeRequests.Reserve(OrderedProbeIndices.Num());
			for (int32 Index : OrderedProbeIndices) ProbeRequests.Add(Requests[Index].Materialize());
			const double ProbesPrepared = FPlatformTime::Seconds();
			UeAgentAuthoringState::FCaptureResult Probed;
			if (!UeAgentAuthoringState::CaptureSnapshotWithBaseline(ProbeRequests, Selected, Probed)) { Error = Probed.Diagnostic; return false; }
			const double ProbedAt = FPlatformTime::Seconds();
			TArray<int32> KnownProbePositions, NewProbePositions;
			for (int32 Index = 0; Index < Probed.Snapshot.Num(); ++Index) {
				const auto& Entry = Probed.Snapshot.ReadAuthoringEntry(Index);
				(Selected.FindEntryIndex(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey) != INDEX_NONE
					? KnownProbePositions : NewProbePositions).Add(Index);
			}
			UeAgentAuthoringState::FStateSnapshot KnownProbes, NewProbes;
			if (NewProbePositions.IsEmpty()) KnownProbes = MoveTemp(Probed.Snapshot);
			else if (KnownProbePositions.IsEmpty()) NewProbes = MoveTemp(Probed.Snapshot);
			else if (!Probed.Snapshot.TrySelectEntries(KnownProbePositions, KnownProbes)
				|| !Probed.Snapshot.TrySelectEntries(NewProbePositions, NewProbes)) return {};
			UeAgentAuthoringState::FStateSnapshot Merged = MoveTemp(Selected);
			TArray<int32> ChangedEntries;
			if (!Merged.TryOverlay(KnownProbes, Merged, Error, true, ChangedEntries)) return false;
			for (int32 Index : ChangedEntries)
			{
				// Overlay reports only observation changes after its complete strict
				// content comparison; no full snapshot token copy or second scan.
				if (SelectedOwners.IsValidIndex(Index)) ChangedOwners.Add(SelectedOwners[Index]);
			}
			if (!NewProbes.IsEmpty()) Merged = Merged.MergeCapturedObservations(NewProbes, true);
			const double Overlaid = FPlatformTime::Seconds();
			TArray<UeAgentAuthoringState::FReadRequest> ClosureRequests;
			TArray<int32> ClosureIndices;
			for (const UObject* Owner : ChangedOwners)
				if (const auto* Indices = OwnerRequestIndices.Find(Owner))
					for (int32 Index : *Indices) if (!ReadIndices[Index]) ClosureIndices.Add(Index);
			ClosureIndices.Sort();
			const int32 ClosureSelectionVisits = ClosureIndices.Num();
			ClosureRequests.Reserve(ClosureIndices.Num());
			for (int32 Index : ClosureIndices) ClosureRequests.Add(Requests[Index].Materialize());
			if (!ClosureRequests.IsEmpty()) {
				UeAgentAuthoringState::FCaptureResult Closure;
				if (!UeAgentAuthoringState::CaptureSnapshot(ClosureRequests, Closure)) { Error = Closure.Diagnostic; return false; }
				if (!Merged.TryOverlay(Closure.Snapshot, Merged, Error, true)) return false;
			}
			OutCapture = {}; OutCapture.Status = TEXT("captured"); OutCapture.Snapshot = MoveTemp(Merged);
			PreviousRequestPositions.Membership = OutCapture.Snapshot.GetObservationMembership();
			if (!bSameScope) for (int32 Index = 0; Index < Requests.Num(); ++Index)
				CurrentPositions[Index] = OutCapture.Snapshot.FindEntryIndex(Requests[Index].GetSemanticId(), Requests[Index].GetSnapshotIdentityKey());
			PreviousRequestPositions.Entries = MoveTemp(CurrentPositions);
			// Every request and current value was observed above. Transfer the
			// complete unique indices instead of rediscovering them from every row.
			// Recheck declarations after all callbacks/canonical consumers finish.
			if (bSameScope && ForcedSchema == 0
				&& RootProofs.Num() == Baseline->RootClasses.Num()
				&& Baseline->RootClasses.Num() == Baseline->RootTemplates.Num()
				&& CurrentSchemas.Num() == Baseline->PropertySchemas.Num()) {
				for (const auto& Pair : RootProofs)
					Baseline->ObservedRoots.Add(Pair.Key->GetPathName(), const_cast<UObject*>(Pair.Key));
				Baseline->ObservedFields = MoveTemp(CurrentFields);
				Baseline->Snapshot = OutCapture.Snapshot;
				OutObservation = MoveTemp(Baseline);
			}
			const double Completed = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_request_consumption current_observation=%d probe_selection=%d closure_selection=%d"),
				Requests.Num(), ProbeSelectionVisits, ClosureSelectionVisits);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_identity_positions reused=%d requests=%d bytes=%llu"),
				ReusedPositions, Requests.Num(), static_cast<uint64>(PreviousRequestPositions.Entries.GetAllocatedSize()));
			TArray<TPair<const FProperty*, int32>> OrderedFamilies;
			for (const auto& Pair : ReferenceFamilies) OrderedFamilies.Add({Pair.Key, Pair.Value});
			OrderedFamilies.Sort([](const auto& Left, const auto& Right) { return Left.Value > Right.Value; });
			for (int32 I = 0; I < FMath::Min(16, OrderedFamilies.Num()); ++I) {
				const FProperty* Field = OrderedFamilies[I].Key;
				const FString OwnerPath = Field->GetOwnerStruct() ? Field->GetOwnerStruct()->GetPathName() : FString();
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_reference_probe_family owner_type=%s field=%s codec=%s count=%d"),
					*OwnerPath, *Field->GetName(), *Field->GetClass()->GetName(), OrderedFamilies[I].Value);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_native_probe_reasons owner=%d semantic=%d schema=%d reference=%d native_value=%d semantic_facts_reused=%d"),
				ForcedOwner, ForcedSemantic, ForcedSchema, ForcedReference, ForcedNativeValue, ReusedSemanticFacts);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_native_delta target=%s total_requests=%d captured_requests=%d changed_owners=%d reused_entries=%d observed_references=%d compare_s=%.6f index_in_compare_s=%.6f observation_in_compare_s=%.6f selection_s=%.6f probe_s=%.6f request_materialize_in_probe_s=%.6f overlay_s=%.6f closure_s=%.6f reference_families=%d unclassified_references=%d total_s=%.6f"),
				*World.GetPathName(), Requests.Num(), ProbeRequests.Num() + ClosureRequests.Num(), ChangedOwners.Num(),
				Requests.Num() - ProbeRequests.Num() - ClosureRequests.Num(), ObservedReferences,
				Compared - Started, Indexed - Started, Compared - Indexed, SelectedAt - Compared, ProbedAt - SelectedAt, ProbesPrepared - SelectedAt,
				Overlaid - ProbedAt, Completed - Overlaid, ReferenceFamilies.Num(), UnclassifiedReferences, Completed - Started);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_current_observation_cpu wall_s=%.6f cpu_s=%.6f requests=%d"),
				Compared - Indexed, UE::UAI::Timing::ThreadCpuSeconds(ObservationCpu, ComparedCpu), Requests.Num());
			return true;
		}
		void SetFailure(FLevelExportResult& OutResult, const UE::UAI::Contract::FDiagnostic& Diagnostic)
        {
            OutResult.Status = TEXT("failed");
            OutResult.Diagnostic = Diagnostic;
        }

		bool CaptureLevelExportState(
			UWorld& World,
			UeAgentAuthoringState::FCaptureResult& OutCapture,
			TSharedPtr<const FLevelSemanticFactsSet>& OutSemanticFacts,
			TUniquePtr<FLevelCaptureReuse>& OutObservation,
			int32& OutActorCount,
			int32& OutComponentCount,
			UE::UAI::Contract::FDiagnostic& OutError,
			bool bSceneEditing, const TArray<FLevelActorCaptureScope>* Scope = nullptr,
			const TSet<FString>* WorldPropertyNames = nullptr, bool bAuthoringExportOnly = false)
		{
			const double CaptureStarted = FPlatformTime::Seconds();
			OutActorCount = 0;
			OutComponentCount = 0;
			// Export and Content Graph reads share this capture. Retain descriptors
			// until all Actor/Component values have been captured, regardless of
			// which cells the user currently has loaded in the Editor.
			FWorldPartitionHelpers::FForEachActorWithLoadingResult LoadedActors;
			if (!Scope) RetainWorldPartitionActorsForAuthoring(World, LoadedActors);
			const auto BuildRequests = [&](auto& TargetRequests, auto&& ConsumeGroup, UE::UAI::Contract::FDiagnostic& BuildError) -> bool {
			UeAgentAuthoringState::FScopedPropertyReadCandidates PropertyCandidates;
			TArray<UeAgentAuthoringState::FReadRequest> WorldRequests;
			const TSet<FString> NoWorldProperties;
			if (!BuildWorldExportReadRequests(World, WorldRequests, BuildError,
				Scope ? (WorldPropertyNames ? WorldPropertyNames : &NoWorldProperties) : nullptr, !Scope))
			{
				return false;
			}

			TArray<AActor*> Actors;
			for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor && (!Scope || Scope->ContainsByPredicate([&](const auto& Item) {
					return Item.ActorGuid.IsValid() ? Item.ActorGuid == Actor->GetActorGuid()
						: !Item.ManagedId.IsEmpty() && Item.ManagedId.Equals(GetManagedActorId(*Actor), ESearchCase::CaseSensitive);
				}))
					&& EvaluateActorAuthorability(
						World,
						*Actor).bAuthorable)
				{
					Actors.Add(Actor);
				}
			}
			Actors.Sort(
				[](const AActor& Left, const AActor& Right)
				{
					return Left.GetActorGuid().ToString(
							EGuidFormats::DigitsWithHyphensLower)
						< Right.GetActorGuid().ToString(
							EGuidFormats::DigitsWithHyphensLower);
				});
			for (auto& Request : WorldRequests) TargetRequests.Emplace(MoveTemp(Request));
			ConsumeGroup(TargetRequests); TargetRequests.Reset(); WorldRequests.Empty();
			for (AActor* Actor : Actors)
			{
				const auto* ActorScope = Scope ? Scope->FindByPredicate([&](const auto& Item) {
					return Item.ActorGuid.IsValid() ? Item.ActorGuid == Actor->GetActorGuid()
						: !Item.ManagedId.IsEmpty() && Item.ManagedId.Equals(GetManagedActorId(*Actor), ESearchCase::CaseSensitive);
				}) : nullptr;
				if (!BuildActorExportScopedReadRequests(
					*Actor,
					TargetRequests,
					BuildError, bSceneEditing, !ActorScope || ActorScope->bActorState, !ActorScope || ActorScope->bActorProperties))
				{
					return false;
				}
				const auto SelectPropertyFields = [&](auto& SelectedRequests) {
					if (ActorScope && !ActorScope->PropertyNames.IsEmpty())
						SelectedRequests.RemoveAll([&](const auto& Request) {
							return (Request.GetSemanticId() == ActorPropertySemanticId || Request.GetSemanticId() == ComponentPropertySemanticId)
								&& !ActorScope->PropertyNames.Contains(Request.GetPropertyPath());
						});
				};
				SelectPropertyFields(TargetRequests);
				ConsumeGroup(TargetRequests); TargetRequests.Reset();
				++OutActorCount;
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				Components.Sort(
					[](const UActorComponent& Left,
						const UActorComponent& Right)
					{
						return Left.GetName() < Right.GetName();
					});
				for (UActorComponent* Component : Components)
				{
					if (!Component
						|| (ActorScope && !ActorScope->bAllComponents && !ActorScope->ComponentNames.Contains(Component->GetName()))
						|| !EvaluateComponentAuthorability(
							World,
							*Component).bAuthorable)
					{
						continue;
					}
					if (!BuildComponentExportReadRequests(
						*Component,
						TargetRequests,
						BuildError, bSceneEditing, !ActorScope || ActorScope->bComponentProperties))
					{
						return false;
					}
					SelectPropertyFields(TargetRequests);
					ConsumeGroup(TargetRequests); TargetRequests.Reset();
					++OutComponentCount;
				}
			}
			return true;
			};
			FScopedLevelSemanticFacts SemanticFacts;
			bool bCaptured = false;
			int32 RequestCount = 0;
			double RequestsSeconds = 0, SnapshotSeconds = 0;
			if (bAuthoringExportOnly && !Scope) {
				// Complete export has no whole-request-list consumer. Feed the
				// original full builder output to the original reader once, and
				// retain only the resulting entries for the one final seal.
				TArray<UeAgentAuthoringState::FReadRequest> Group;
				double ProducerWall = 0, ReadSeconds = 0;
				const auto Produce = [&](UeAgentAuthoringState::FOwnedReadRequestConsumer Read,
					UE::UAI::Contract::FDiagnostic& BuildError) {
					const double Started = FPlatformTime::Seconds();
					const auto Consume = [&](auto& Requests) { for (auto& Request : Requests) Read(MoveTemp(Request)); };
					const bool bBuilt = BuildRequests(Group, Consume, BuildError);
					ProducerWall = FPlatformTime::Seconds() - Started;
					return bBuilt;
					};
				const double ReaderStarted = FPlatformTime::Seconds();
				bCaptured = UeAgentAuthoringState::CaptureSnapshotOwned(Produce, OutCapture,
					UeAgentAuthoringState::ECaptureValueMode::AuthoringExportLiterals, &ReadSeconds, &RequestCount);
				const double ReaderFinished = FPlatformTime::Seconds();
				RequestsSeconds = ReaderStarted - CaptureStarted + ProducerWall - ReadSeconds;
				SnapshotSeconds = ReaderFinished - ReaderStarted - (ProducerWall - ReadSeconds);
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_request_direct requests=%d compact_produced=0 compact_expanded=0 full_request_retention=one_owner producer_s=%.6f reader_s=%.6f"),
					RequestCount, ProducerWall - ReadSeconds, SnapshotSeconds);
			} else {
				TArray<UeAgentAuthoringState::FReadRequestBinding> Requests, Group;
				const auto Collect = [&](auto& Items) { Requests.Append(MoveTemp(Items)); };
				if (!BuildRequests(Group, Collect, OutError)) return false;
				const double RequestsReady = FPlatformTime::Seconds();
				if (bAuthoringExportOnly) {
					bCaptured = UeAgentAuthoringState::CaptureSnapshot(Requests, OutCapture, nullptr, nullptr,
						UeAgentAuthoringState::ECaptureValueMode::AuthoringExportLiterals);
				} else {
					const TOptional<bool> Incremental = TryCaptureWithNativeBaseline(World, bSceneEditing, Requests, SemanticFacts, OutCapture, OutError, OutObservation);
					bCaptured = Incremental.IsSet() ? Incremental.GetValue()
						: UeAgentAuthoringState::CaptureSnapshot(Requests, OutCapture, nullptr, nullptr, UeAgentAuthoringState::ECaptureValueMode::Authoring);
				}
				RequestsSeconds = RequestsReady - CaptureStarted;
				SnapshotSeconds = FPlatformTime::Seconds() - RequestsReady;
				RequestCount = Requests.Num();
			}
			const double CaptureFinished = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=native_capture target=%s scope=%s actors=%d components=%d requests=%d requests_s=%.6f snapshot_s=%.6f total_s=%.6f success=%s"),
				*World.GetPathName(), Scope ? TEXT("component_closure") : TEXT("complete_world"), OutActorCount, OutComponentCount, RequestCount,
				RequestsSeconds, SnapshotSeconds,
				CaptureFinished - CaptureStarted, bCaptured ? TEXT("true") : TEXT("false"));
			if (!bCaptured)
			{
				if (OutError.IsEmpty()) OutError = OutCapture.Diagnostic;
				return false;
			}
			OutSemanticFacts = SemanticFacts.GetFacts();
			return true;
		}

	}

	void PrepareLevelCaptureReuse(UWorld& World, FLevelAuthoringCapture& Capture, const FString& ProfileId)
	{
		const bool bSameObservation = ObservedCaptureReuse && ObservedCaptureReuse->World.Get() == &World
			&& ObservedCaptureReuse->WorldPath == World.GetPathName()
			&& ObservedCaptureReuse->bSceneEditing == (ProfileId == SceneEditingProfileId)
			&& ObservedCaptureReuse->SemanticFacts == Capture.SemanticFacts
			&& ObservedCaptureReuse->Snapshot.SharesStorageWith(Capture.Snapshot);
		PendingCaptureReuse = bSameObservation ? MoveTemp(ObservedCaptureReuse)
			: MakeCaptureReuse(World, Capture.Snapshot, ProfileId == SceneEditingProfileId, Capture.SemanticFacts);
		if (PendingCaptureReuse) {
			PendingCaptureReuse->bActive = false;
			// The prepare consumer has finished using this raw observation. Retain
			// its canonical/model outputs but transfer native lifetime to Pending.
			Capture.Snapshot = {};
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_prepare reused_observation=%s"),
			bSameObservation ? TEXT("true") : TEXT("false"));
	}

	void ActivateLevelCaptureReuse(UWorld& World)
	{
		if (PendingCaptureReuse && PendingCaptureReuse->World.Get() == &World)
			PendingCaptureReuse->bActive = true;
	}
	void ReleaseLevelCaptureForPersistentReload()
	{
		if (!PendingCaptureReuse || PendingCaptureReuse->bAwaitingPersistentReload) return;
		ReleaseCollectableNativeValues(PendingCaptureReuse);
		PendingCaptureReuse->World.Reset();
		PendingCaptureReuse->SemanticFacts.Reset();
		PendingCaptureReuse->ObservedRoots.Empty();
		PendingCaptureReuse->ObservedFields.Empty();
		PendingCaptureReuse->bActive = false;
		PendingCaptureReuse->bAwaitingPersistentReload = true;
	}
	void RebindLevelCaptureAfterPersistentReload(UWorld& World)
	{
		if (!PendingCaptureReuse || !PendingCaptureReuse->bAwaitingPersistentReload) return;
		if (!PendingCaptureReuse->WorldPath.Equals(World.GetPathName(), ESearchCase::CaseSensitive)) {
			PendingCaptureReuse.Reset(); return;
		}
		// This activates a comparison candidate, not a current-object proof.
		// TryCaptureWithNativeBaseline re-enumerates the entire request set and
		// verifies each current identity/class/schema/value before reusing it.
		PendingCaptureReuse->World = &World;
		PendingCaptureReuse->bAwaitingPersistentReload = false;
		PendingCaptureReuse->bActive = true;
	}
	void DiscardLevelCaptureReuse() { PendingCaptureReuse.Reset(); }
	void StartLevelCaptureObservation()
	{
		bCaptureObservationEnabled = true;
		if (!CapturePreExitHandle.IsValid())
			CapturePreExitHandle = FEditorDelegates::OnEditorPreExit.AddLambda([] {
				// Module shutdown follows UObject shutdown. Destroy native values
				// while their FProperties and native class roots are still valid.
				bCaptureObservationEnabled = false;
				ObservedCaptureReuse.Reset(); PendingCaptureReuse.Reset(); PreviousRequestPositions = {};
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_pre_exit released=true"));
			});
		if (!CapturePreGcHandle.IsValid())
			CapturePreGcHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda([] {
				// Keep only pure detached values protected by native class ownership.
				// Object/owned/delegate/unknown storage is released before collection;
				// Logical target, current class/field, schema and actual values are
				// checked again even when the same map has been loaded into new roots.
				ReleaseCollectableNativeValues(ObservedCaptureReuse);
				ReleaseCollectableNativeValues(PendingCaptureReuse);
			});
	}
	void StopLevelCaptureObservation()
	{
		bCaptureObservationEnabled = false;
		FEditorDelegates::OnEditorPreExit.Remove(CapturePreExitHandle);
		CapturePreExitHandle.Reset();
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(CapturePreGcHandle);
		CapturePreGcHandle.Reset(); ObservedCaptureReuse.Reset(); PendingCaptureReuse.Reset(); PreviousRequestPositions = {};
	}

	TSharedRef<FJsonObject> FLevelExportResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		if (!Diagnostic.IsEmpty()) Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		Result->SetStringField(TEXT("workspace_root"), WorkspaceRoot);
		Result->SetStringField(TEXT("profile"), ProfileId);
		Result->SetNumberField(TEXT("actor_count"), ActorCount);
		Result->SetNumberField(TEXT("component_count"), ComponentCount);
		Result->SetNumberField(TEXT("file_count"), FileCount);
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		Result->SetObjectField(
			TEXT("model_metrics"), ModelMetrics.ToJsonObject());
		if (!EntryFile.IsEmpty()) Result->SetStringField(TEXT("entry_file"), EntryFile);
		if (PublisherReceipt.IsValid())
		{
			Result->SetObjectField(TEXT("publisher"), PublisherReceipt.ToSharedRef());
		}
		Result->SetNumberField(
			TEXT("written_file_count"), WrittenFiles.Num());
		return Result;
	}

	bool ExportLevelWorkspace(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		FLevelExportResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport)
	{
		const double ExportStarted = FPlatformTime::Seconds();
  auto& OutError = OutResult.Diagnostic;
		OutResult = FLevelExportResult();
		OutResult.Status = TEXT("failed");
		OutResult.WorkspaceRoot = WorkspaceRoot;
		OutResult.ProfileId = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		OutError.Reset();
		if (WorkspaceRoot.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_workspace_root_missing"),
				TEXT("Level export requires a non-empty workspace output directory."), TEXT("level_export"));
			OutError.PropertyPath = TEXT("workspace_root");
			OutError.ActualValue = MakeShared<FJsonValueString>(WorkspaceRoot);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty output directory"));
			return false;
		}

		TWeakPtr<const UE::UAI::HjsonWorkspace::FAuthoringModel> CompletedModel;
		TWeakPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> CompletedCanonical;
		ON_SCOPE_EXIT
		{
			// A complete export has consumed these observations synchronously.
			// Publication owns value-free declarations/formatter bytes; keeping a
			// second complete native and canonical value world serves only the next
			// complete-capture memo, not Source or a local edit. Pending belongs to
			// an active operation's save/readback lifetime and is not retired here.
			const int32 ObservedEntries = ObservedCaptureReuse ? ObservedCaptureReuse->Snapshot.Num() : 0;
			ObservedCaptureReuse.Reset();
			PreviousRequestPositions = {};
			UE::UAI::HjsonWorkspace::ResetCanonicalCaptureReuse();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_export_capture_retire observed_entries=%d canonical_memo_released=true pending_operation_preserved=true value_model_alive=%s canonical_snapshot_alive=%s"),
				ObservedEntries, CompletedModel.IsValid() ? TEXT("true") : TEXT("false"), CompletedCanonical.IsValid() ? TEXT("true") : TEXT("false"));
		};

		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		FLevelAuthoringCapture Capture;
		const FString CaptureProfile = OutResult.ProfileId == SceneEditingProfileId
			? OutResult.ProfileId : TEXT("llm_authoring");
		const bool bDirectOutput = PreparedExport && PreparedExport->Publication.bDirectDataflow && CaptureProfile == OutResult.ProfileId;
		if (!CaptureLevelNativeState(World, Capture, OutError, CaptureProfile, nullptr, nullptr, bDirectOutput)
			|| !CompleteLevelCanonicalCapture(Capture, OutError, CaptureProfile,
				!PreparedExport || !PreparedExport->Publication.bDirectDataflow))
		{
			SetFailure(OutResult, OutError);
			return false;
		}
		OutResult.ActorCount = Capture.ActorCount;
		CompletedModel = Capture.Model; CompletedCanonical = Capture.CanonicalSnapshot;
		OutResult.ComponentCount = Capture.ComponentCount;
		if (bDirectOutput) {
			// DirectBinding and Model finished their last snapshot consumer.
			// No later formatter/publisher needs codec JSON or native provenance.
			Capture.Snapshot = {};
		}
		const double CaptureFinished = FPlatformTime::Seconds();
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while capturing the Level state."), TEXT("level_export"));
			Error.TargetPath = GetWorldPath(World);
			Error.PropertyPath = TEXT("package_dirty");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			Error.SourceKind = TEXT("native_result");
			SetFailure(OutResult, Error);
			return false;
		}
		UeAgentExportWorkspaceWriter::FWorkspaceResult WorkspaceResult;
		bool bExported;
		if (Capture.Model->GetProfileId() == OutResult.ProfileId)
		{
			// Capture already built this exact profile from current native state.
			// Reuse that immutable model; do not rebuild it from Snapshot again.
			auto Resources = UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			bExported = UE::UAI::HjsonWorkspace::TryExportCapturedCompositionWorkspace(
				TEXT("ue.level.asset"), OutResult.ProfileId, Capture.Model.ToSharedRef(),
				Capture.DirectBinding.IsSet() ? Capture.DirectBinding.GetValue() : UE::UAI::HjsonWorkspace::FCompositionSnapshotBinding{
					Capture.CanonicalSnapshot->GetSnapshotId(), Capture.CanonicalSnapshot->GetRevision(),
					Capture.CanonicalSnapshot->GetProvenance().GetEvidenceId()},
				WorkspaceRoot, Resources, WorkspaceResult, PreparedExport);
		}
		else
		{
			// Other profiles retain their own projection and validation.
			bExported = UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
				TEXT("ue.level.asset"), OutResult.ProfileId, Capture.Snapshot, WorkspaceRoot, WorkspaceResult, PreparedExport);
		}
		if (!bExported)
		{
			SetFailure(OutResult, WorkspaceResult.Diagnostic);
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while writing the Level workspace."), TEXT("level_export"));
			Error.TargetPath = GetWorldPath(World);
			Error.PropertyPath = TEXT("package_dirty");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			Error.SourceKind = TEXT("native_result");
			SetFailure(OutResult, Error);
			return false;
		}
		OutResult.WorkspaceRoot = WorkspaceResult.WorkspaceRoot;
		OutResult.ExpectedRevision = WorkspaceResult.ExpectedRevision;
		OutResult.Metrics = WorkspaceResult.Metrics;
		OutResult.ModelMetrics = WorkspaceResult.ModelMetrics;
		OutResult.FileCount = WorkspaceResult.Files.Num();
		OutResult.EntryFile = WorkspaceResult.EntryFile;
		OutResult.WrittenFiles = WorkspaceResult.WrittenFiles;
		OutResult.WrittenRelativeFiles =
			WorkspaceResult.WrittenRelativeFiles;
		OutResult.PublisherReceipt =
			WorkspaceResult.PublisherReceipt;
		OutResult.Status = WorkspaceResult.Status;
		OutResult.Diagnostic.Reset();
		OutError.Reset();
		const double ExportFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_export target=%s profile=%s capture_s=%.6f export_s=%.6f total_s=%.6f success=true"),
			*World.GetPathName(), *OutResult.ProfileId, CaptureFinished - ExportStarted,
			ExportFinished - CaptureFinished, ExportFinished - ExportStarted);
		return true;
	}

	bool VerifyLevelAuthoringReadback(UWorld& World,
		const UeAgentAuthoringState::FStateSnapshot& Expected,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FLevelAuthoringCapture Actual;
		if (!TryCaptureLevelAuthoringState(World, Actual, OutError)) return false;
		UeAgentAuthoringSemanticCompare::FComparisonReport Comparison;
		if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(Expected, Actual.Snapshot,
			UeAgentAuthoringSemanticCompare::FCompareOptions(), Comparison))
		{ OutError = Comparison.Diagnostic; return false; }
		if (Comparison.bEquivalent) return true;
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_persistent_readback_mismatch"),
			TEXT("Reloaded level differs from the state verified before saving."), TEXT("persistent_readback"));
		for (const auto& Difference : Comparison.Entries)
		{
			if (Difference.bEquivalent) continue;
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(OutError.Code, Difference.Diagnostic,
				OutError.Stage, OutError.PublicMessage);
			break;
		}
		OutError.TargetPath = World.GetPathName();
		OutError.Stage = TEXT("persistent_readback");
		return false;
	}

	static bool CaptureLevelNativeState(
		UWorld& World,
		FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId, const TArray<FLevelActorCaptureScope>* Scope,
		const TSet<FString>* WorldPropertyNames, bool bAuthoringExportOnly)
	{
		OutCapture = FLevelAuthoringCapture();
		OutError.Reset();
		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		UeAgentAuthoringState::FCaptureResult Capture;
		TUniquePtr<FLevelCaptureReuse> Observation;
		if (!CaptureLevelExportState(
				World,
				Capture,
				OutCapture.SemanticFacts,
				Observation,
				OutCapture.ActorCount,
				OutCapture.ComponentCount,
				OutError, ProfileId == SceneEditingProfileId, Scope, WorldPropertyNames, bAuthoringExportOnly))
		{
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_capture_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while capturing the Level state."), TEXT("level_export"));
			OutError.TargetPath = GetWorldPath(World);
			OutError.PropertyPath = TEXT("package_dirty");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutCapture.Snapshot = MoveTemp(Capture.Snapshot);
		if (!bAuthoringExportOnly) ObservedCaptureReuse = FinishObservedCaptureReuse(World, OutCapture.Snapshot, ProfileId == SceneEditingProfileId,
			OutCapture.SemanticFacts, MoveTemp(Observation));
		else UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_consumer purpose=authoring_export entries=%d shared_property_native_copy=false observation_memo_built=false"), OutCapture.Snapshot.Num());
		return !OutCapture.Snapshot.IsEmpty();
	}

	bool TryCaptureLevelNativeState(UWorld& World, FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId)
	{
		return CaptureLevelNativeState(World, OutCapture, OutError, ProfileId, nullptr);
	}

	bool TryCaptureLevelActorNativeState(UWorld& World, const TArray<FGuid>& ActorGuids,
		FLevelAuthoringCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId)
	{
		TArray<FLevelActorCaptureScope> Scope;
		for (const FGuid& Id : ActorGuids) {
			auto& ActorScope = Scope.AddDefaulted_GetRef();
			ActorScope.ActorGuid = Id;
			ActorScope.bActorState = ActorScope.bActorProperties = ActorScope.bAllComponents = true;
		}
		return CaptureLevelNativeState(World, OutCapture, OutError, ProfileId, &Scope);
	}

	bool TryCaptureLevelScopedNativeState(UWorld& World, const TArray<FLevelActorCaptureScope>& Scope,
		FLevelAuthoringCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId,
		const TSet<FString>& WorldPropertyNames)
	{
		return CaptureLevelNativeState(World, OutCapture, OutError, ProfileId, &Scope, &WorldPropertyNames);
	}

	namespace {
	bool CompleteLevelCanonicalCapture(FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId,
		bool bBuildLegacyFingerprint)
	{
		UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!bBuildLegacyFingerprint) {
			OutCapture.CanonicalSnapshot.Reset();
			return UE::UAI::HjsonWorkspace::TryBuildDirectAuthoringCapture(TEXT("ue.level.asset"), ProfileId,
				OutCapture.Snapshot, Resources, OutCapture.DirectBinding, OutCapture.Model, WorkspaceError)
				? OutCapture.IsValid() : (OutError = WorkspaceError.GetDiagnostic(), false);
		}
		OutCapture.DirectBinding.Reset();
		if (!UE::UAI::HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
				TEXT("ue.level.asset"),
				ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId,
				OutCapture.Snapshot,
				Resources,
				OutCapture.CanonicalSnapshot,
				OutCapture.Model,
				WorkspaceError, bBuildLegacyFingerprint))
		{
			OutError = WorkspaceError.GetDiagnostic();
			return false;
		}
		return OutCapture.IsValid();
	}
	}

	bool TryCompleteLevelCanonicalCapture(FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId)
	{
		return CompleteLevelCanonicalCapture(OutCapture, OutError, ProfileId, true);
	}

	bool TryCaptureLevelAuthoringState(UWorld& World, FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId)
	{
		return TryCaptureLevelNativeState(World, OutCapture, OutError, ProfileId)
			&& TryCompleteLevelCanonicalCapture(OutCapture, OutError, ProfileId);
	}
}
