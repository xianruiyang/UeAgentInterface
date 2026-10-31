// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"

#include "Components/ActorComponent.h"
#include "CoreGlobals.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
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
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		struct FLevelCaptureReuse
		{
			TWeakObjectPtr<UWorld> World;
			FString WorldPath;
			// Only native Engine classes, never World/Actor/component instances.
			// Declared before Snapshot so detached storage is destroyed first.
			TArray<TStrongObjectPtr<UClass>> NativeClasses;
			UeAgentAuthoringState::FStateSnapshot Snapshot;
			TMap<FString, UClass*> RootClasses;
			TMap<FString, bool> RootTemplates;
			TMap<const FProperty*, FString> PropertySchemas;
			bool bSceneEditing = false;
			bool bActive = false;
		};
		thread_local TUniquePtr<FLevelCaptureReuse> PendingCaptureReuse;
		thread_local TUniquePtr<FLevelCaptureReuse> ObservedCaptureReuse;
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
		bool IsLevelReflectedProperty(const FString& Semantic);

		TUniquePtr<FLevelCaptureReuse> MakeCaptureReuse(UWorld& World,
			const UeAgentAuthoringState::FStateSnapshot& Snapshot, const bool bSceneEditing)
		{
			if (!bCaptureObservationEnabled || World.IsPartitionedWorld() || Snapshot.Num() > 200000) return {};
			auto Next = MakeUnique<FLevelCaptureReuse>();
			Next->World = &World; Next->WorldPath = World.GetPathName();
			Next->bSceneEditing = bSceneEditing; Next->Snapshot = Snapshot;
			TSet<UClass*> NativeClasses;
			for (const auto& Entry : Snapshot.GetEntries()) {
				const FString Path = Entry.NativeTargetIdentity.GetSelector(TEXT("owner_path"));
				if (!Path.IsEmpty() && !Next->RootTemplates.Contains(Path)) {
					UObject* Root = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
					Next->RootTemplates.Add(Path, Root && Root->IsTemplate());
					if (HasStableNativeOwner(Root)) {
						Next->RootClasses.Add(Path, Root->GetClass());
						if (!NativeClasses.Contains(Root->GetClass())) {
							NativeClasses.Add(Root->GetClass()); Next->NativeClasses.Emplace(Root->GetClass());
						}
					}
				}
				const FProperty* Property = Entry.NativeValue ? Entry.NativeValue->GetProperty() : nullptr;
				if (!Property && IsLevelReflectedProperty(Entry.SemanticId))
					if (UClass* Class = Next->RootClasses.FindRef(Path))
						Property = FindFProperty<FProperty>(Class, *Entry.NativeTargetIdentity.GetSelector(TEXT("property_path")));
				if (Property) {
					if (!Next->PropertySchemas.Contains(Property)) Next->PropertySchemas.Add(Property, PropertySchema(Property));
				}
			}
			return Next;
		}

		FString EntryKey(const FString& Semantic, const FString& Identity)
		{ return FString::FromInt(Semantic.Len()) + TEXT(":") + Semantic + Identity; }

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
			return Object && Object->GetClass()->GetOutermost()->GetName() == TEXT("/Script/Engine")
				&& Object->GetClass()->HasAnyClassFlags(CLASS_Native)
				&& !Object->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
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
			Cache->Snapshot.ReleaseNativeValuesIf([&](const UeAgentAuthoringState::FStateEntry& Entry) {
				if (!Entry.NativeValue || !IsLevelReflectedProperty(Entry.SemanticId)) return true;
				// These class pointers are owned by NativeClasses. Pure native values
				// do not refer to the old World instance and may survive its unload.
				if (!Cache->RootClasses.Contains(Entry.NativeTargetIdentity.GetSelector(TEXT("owner_path")))) return true;
				const FProperty* Property = Entry.NativeValue->GetProperty();
				bool* Safe = Comparable.Find(Property);
				if (!Safe) { TSet<const UStruct*> Visiting; Safe = &Comparable.Add(Property, IsDetachedValueComparable(Property, Visiting)); }
				if (*Safe) ++Retained;
				return !*Safe;
			});
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_capture_gc entries=%d retained_detached_values=%d native_classes=%d"),
				Cache->Snapshot.Num(), Retained, Cache->NativeClasses.Num());
		}

		TOptional<bool> TryCaptureWithNativeBaseline(UWorld& World, const bool bSceneEditing,
			const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
			UeAgentAuthoringState::FCaptureResult& OutCapture, UE::UAI::Contract::FDiagnostic& Error)
		{
			auto Baseline = PendingCaptureReuse && PendingCaptureReuse->bActive
				? MoveTemp(PendingCaptureReuse) : MoveTemp(ObservedCaptureReuse);
			if (!Baseline) return {};
			if (World.IsPartitionedWorld() || Baseline->WorldPath != World.GetPathName() || Baseline->bSceneEditing != bSceneEditing
				|| Baseline->Snapshot.Num() != Requests.Num()) return {};
			const double Started = FPlatformTime::Seconds();
			TMap<FString, const UeAgentAuthoringState::FStateEntry*> Entries;
			for (const auto& Entry : Baseline->Snapshot.GetEntries())
				Entries.Add(EntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey), &Entry);
			TMap<FString, const UObject*> Owners;
			TSet<const UObject*> ChangedOwners;
			TSet<int32> ReadIndices;
			TMap<const FProperty*, bool> ComparableProperties;
			TMap<const FProperty*, FString> CurrentSchemas;
			TMap<const UObject*, bool> RootProofs;
			int32 ObservedReferences = 0;
			for (int32 Index = 0; Index < Requests.Num(); ++Index)
			{
				const auto& Request = Requests[Index];
				const auto& Identity = Request.SnapshotIdentityOverride.IsSet()
					? Request.SnapshotIdentityOverride.GetValue() : Request.TargetIdentity;
				const FString Key = EntryKey(Request.SemanticId, Identity.CanonicalKey);
				const auto* Previous = Entries.FindRef(Key);
				if (!Previous || !IsValid(Request.RootObject)) return {};
				const UObject* Owner = CaptureOwner(Request.RootObject);
				Owners.Add(Key, Owner);
				if (!HasStableNativeOwner(Request.RootObject) || !HasStableNativeOwner(Owner)) {
					ChangedOwners.Add(Owner); ReadIndices.Add(Index); continue;
				}
				if (Request.PropertyPath.IsEmpty() || !IsLevelReflectedProperty(Request.SemanticId))
				{ ReadIndices.Add(Index); continue; }
				// Logical identity is already matched against the freshly enumerated
				// complete request set. A reloaded native instance may reuse only pure
				// detached values of the exact protected class/field; every current
				// value and schema is still checked below, refs/metadata read afresh.
				bool* RootValid = RootProofs.Find(Request.RootObject);
				if (!RootValid) {
					const FString Path = Request.RootObject->GetPathName();
					RootValid = &RootProofs.Add(Request.RootObject,
						Baseline->RootClasses.FindRef(Path) == Request.RootObject->GetClass());
					if (*RootValid && Baseline->RootTemplates.FindChecked(Path) != Request.RootObject->IsTemplate()) ChangedOwners.Add(Owner);
				}
				if (!*RootValid) return {};
				FProperty* Property = FindFProperty<FProperty>(Request.RootObject->GetClass(), *Request.PropertyPath);
				if (!Property || (Previous->NativeValue && Property != Previous->NativeValue->GetProperty())) return {};
				const FString* Schema = CurrentSchemas.Find(Property);
				if (!Schema) Schema = &CurrentSchemas.Add(Property, PropertySchema(Property));
				const FString* PreviousSchema = Baseline->PropertySchemas.Find(Property);
				if (!PreviousSchema || *Schema != *PreviousSchema) { ChangedOwners.Add(Owner); ReadIndices.Add(Index); continue; }
				bool* Comparable = ComparableProperties.Find(Property);
				if (!Comparable) {
					TSet<const UStruct*> Visiting;
					Comparable = &ComparableProperties.Add(Property, IsDetachedValueComparable(Property, Visiting));
				}
				const void* Current = Property->ContainerPtrToValuePtr<void>(Request.RootObject);
				if (!*Comparable) {
					TSharedPtr<FJsonValue> ActualReference;
					if (UeAgentPropertyValueCodec::TryEncodePathOnlyAssetReferences(Property, Current,
						const_cast<UObject*>(Request.RootObject), ActualReference)) {
						++ObservedReferences;
						if (UeAgentAuthoringState::MakeStableCanonicalValueKey(ActualReference)
							!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Previous->Value)) ChangedOwners.Add(Owner);
					} else ReadIndices.Add(Index);
					continue;
				}
				if (!Previous->NativeValue) { ReadIndices.Add(Index); continue; }
				for (int32 Element = 0; Element < Property->ArrayDim; ++Element)
					if (!Property->Identical(static_cast<const uint8*>(Previous->NativeValue->GetValuePtr()) + Element * Property->ElementSize,
						static_cast<const uint8*>(Current) + Element * Property->ElementSize)) { ChangedOwners.Add(Owner); break; }
			}
			const double Compared = FPlatformTime::Seconds();
			TArray<UeAgentAuthoringState::FReadRequest> ProbeRequests;
			for (int32 Index = 0; Index < Requests.Num(); ++Index)
				if (ReadIndices.Contains(Index) || ChangedOwners.Contains(CaptureOwner(Requests[Index].RootObject))) {
					ReadIndices.Add(Index); ProbeRequests.Add(Requests[Index]);
				}
			UeAgentAuthoringState::FCaptureResult Probed;
			if (!UeAgentAuthoringState::CaptureSnapshot(ProbeRequests, Probed)) { Error = Probed.Diagnostic; return false; }
			const double ProbedAt = FPlatformTime::Seconds();
			// Baseline was consumed from the thread-local cache above. Retain only
			// observation tokens for change detection, not another full snapshot.
			TArray<TSharedPtr<const uint8>> PreviousObservations;
			PreviousObservations.Reserve(Baseline->Snapshot.Num());
			for (const auto& Entry : Baseline->Snapshot.GetEntries()) PreviousObservations.Add(Entry.Observation);
			Entries.Reset();
			UeAgentAuthoringState::FStateSnapshot Merged = MoveTemp(Baseline->Snapshot);
			if (!Merged.TryOverlay(Probed.Snapshot, Merged, Error, true)) return false;
			const auto& MergedEntries = Merged.GetEntries();
			for (int32 Index = 0; Index < MergedEntries.Num(); ++Index)
			{
				// TryOverlay already compared complete metadata/value exactly and
				// preserves baseline order as it replaces existing identities only.
				const auto& Entry = MergedEntries[Index];
				if (Entry.Observation != PreviousObservations[Index])
					ChangedOwners.Add(Owners.FindRef(EntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey)));
			}
			const double Overlaid = FPlatformTime::Seconds();
			TArray<UeAgentAuthoringState::FReadRequest> ClosureRequests;
			for (int32 Index = 0; Index < Requests.Num(); ++Index)
				if (!ReadIndices.Contains(Index) && ChangedOwners.Contains(CaptureOwner(Requests[Index].RootObject)))
					ClosureRequests.Add(Requests[Index]);
			if (!ClosureRequests.IsEmpty()) {
				UeAgentAuthoringState::FCaptureResult Closure;
				if (!UeAgentAuthoringState::CaptureSnapshot(ClosureRequests, Closure)) { Error = Closure.Diagnostic; return false; }
				if (!Merged.TryOverlay(Closure.Snapshot, Merged, Error, true)) return false;
			}
			OutCapture = {}; OutCapture.Status = TEXT("captured"); OutCapture.Snapshot = MoveTemp(Merged);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_native_delta target=%s total_requests=%d captured_requests=%d changed_owners=%d reused_entries=%d observed_references=%d compare_s=%.6f probe_s=%.6f overlay_s=%.6f closure_s=%.6f total_s=%.6f"),
				*World.GetPathName(), Requests.Num(), ProbeRequests.Num() + ClosureRequests.Num(), ChangedOwners.Num(),
				Requests.Num() - ProbeRequests.Num() - ClosureRequests.Num(), ObservedReferences,
				Compared - Started, ProbedAt - Compared, Overlaid - ProbedAt, FPlatformTime::Seconds() - Overlaid, FPlatformTime::Seconds() - Started);
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
			int32& OutActorCount,
			int32& OutComponentCount,
			UE::UAI::Contract::FDiagnostic& OutError,
			bool bSceneEditing)
		{
			const double CaptureStarted = FPlatformTime::Seconds();
			OutActorCount = 0;
			OutComponentCount = 0;
			// Export and Content Graph reads share this capture. Retain descriptors
			// until all Actor/Component values have been captured, regardless of
			// which cells the user currently has loaded in the Editor.
			FWorldPartitionHelpers::FForEachActorWithLoadingResult LoadedActors;
			if (UWorldPartition* Partition = World.GetWorldPartition())
			{
				FWorldPartitionHelpers::FForEachActorWithLoadingParams LoadParams;
				LoadParams.bKeepReferences = true;
				LoadParams.ActorClasses = {AActor::StaticClass()};
				FWorldPartitionHelpers::ForEachActorWithLoading(Partition,
					[](const FWorldPartitionActorDescInstance*) { return true; }, LoadParams, LoadedActors);
			}
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			TArray<UeAgentAuthoringState::FReadRequest> TargetRequests;
			if (!BuildWorldExportReadRequests(
				World,
				TargetRequests,
				OutError))
			{
				return false;
			}
			Requests.Append(MoveTemp(TargetRequests));

			TArray<AActor*> Actors;
			for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor
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
			for (AActor* Actor : Actors)
			{
				if (!BuildActorExportReadRequests(
					*Actor,
					TargetRequests,
					OutError, bSceneEditing))
				{
					return false;
				}
				Requests.Append(MoveTemp(TargetRequests));
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
						|| !EvaluateComponentAuthorability(
							World,
							*Component).bAuthorable)
					{
						continue;
					}
					if (!BuildComponentExportReadRequests(
						*Component,
						TargetRequests,
						OutError, bSceneEditing))
					{
						return false;
					}
					Requests.Append(MoveTemp(TargetRequests));
					++OutComponentCount;
				}
			}
			const double RequestsReady = FPlatformTime::Seconds();
			const TOptional<bool> Incremental = TryCaptureWithNativeBaseline(World, bSceneEditing, Requests, OutCapture, OutError);
			const bool bCaptured = Incremental.IsSet() ? Incremental.GetValue()
				: UeAgentAuthoringState::CaptureSnapshot(Requests, OutCapture);
			const double CaptureFinished = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=native_capture target=%s actors=%d components=%d requests=%d requests_s=%.6f snapshot_s=%.6f total_s=%.6f success=%s"),
				*World.GetPathName(), OutActorCount, OutComponentCount, Requests.Num(),
				RequestsReady - CaptureStarted, CaptureFinished - RequestsReady,
				CaptureFinished - CaptureStarted, bCaptured ? TEXT("true") : TEXT("false"));
			if (!bCaptured)
			{
				if (OutError.IsEmpty()) OutError = OutCapture.Diagnostic;
				return false;
			}
			return true;
		}

	}

	void PrepareLevelCaptureReuse(UWorld& World, FLevelAuthoringCapture& Capture, const FString& ProfileId)
	{
		PendingCaptureReuse = MakeCaptureReuse(World, Capture.Snapshot, ProfileId == SceneEditingProfileId);
	}

	void ActivateLevelCaptureReuse(UWorld& World)
	{
		if (PendingCaptureReuse && PendingCaptureReuse->World.Get() == &World)
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
				ObservedCaptureReuse.Reset(); PendingCaptureReuse.Reset();
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
		CapturePreGcHandle.Reset(); ObservedCaptureReuse.Reset(); PendingCaptureReuse.Reset();
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

		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		FLevelAuthoringCapture Capture;
		const FString CaptureProfile = OutResult.ProfileId == SceneEditingProfileId
			? OutResult.ProfileId : TEXT("llm_authoring");
		if (!TryCaptureLevelAuthoringState(World, Capture, OutError, CaptureProfile))
		{
			SetFailure(OutResult, OutError);
			return false;
		}
		OutResult.ActorCount = Capture.ActorCount;
		OutResult.ComponentCount = Capture.ComponentCount;
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
				{Capture.CanonicalSnapshot->GetSnapshotId(), Capture.CanonicalSnapshot->GetRevision(),
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

	bool TryCaptureLevelAuthoringState(
		UWorld& World,
		FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId)
	{
		OutCapture = FLevelAuthoringCapture();
		OutError.Reset();
		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!CaptureLevelExportState(
				World,
				Capture,
				OutCapture.ActorCount,
				OutCapture.ComponentCount,
				OutError, ProfileId == SceneEditingProfileId))
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
		UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!UE::UAI::HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
				TEXT("ue.level.asset"),
				ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId,
				OutCapture.Snapshot,
				Resources,
				OutCapture.CanonicalSnapshot,
				OutCapture.Model,
				WorkspaceError))
		{
			OutError = WorkspaceError.GetDiagnostic();
			return false;
		}
		if (OutCapture.IsValid())
			ObservedCaptureReuse = MakeCaptureReuse(World, OutCapture.Snapshot, ProfileId == SceneEditingProfileId);
		return OutCapture.IsValid();
	}
}
