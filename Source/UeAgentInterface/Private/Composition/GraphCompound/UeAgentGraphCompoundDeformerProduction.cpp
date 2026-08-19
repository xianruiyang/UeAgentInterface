#include "Composition/GraphCompound/UeAgentGraphCompoundDeformerProduction.h"

#include "Animation/MeshDeformerCollection.h"
#include "Animation/MeshDeformer.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace DomainAdapters::AnimationRigGraph;
	using namespace StateSemantic;

	namespace
	{
		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryStringField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* const Value = FindField(Object, Name);
			if (!Value || Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		bool TryIntegerField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value = FindField(Object, Name);
			if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			const double Number = FCString::Atod(*Value->GetLexeme());
			const double Rounded = FMath::RoundToDouble(Number);
			if (!FMath::IsFinite(Number)
				|| !FMath::IsNearlyEqual(Number, Rounded)
				|| Rounded < 0.0
				|| Rounded > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Rounded);
			return true;
		}

		FString EntryRef(const FGraphCompoundCanonicalEdit& Edit)
		{
			const FString Prefix = TEXT("item:collection_entry:");
			const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
			return SemanticId.StartsWith(Prefix)
				? SemanticId.Mid(Prefix.Len()) : FString();
		}

		FString ObjectPath(const FString& TargetRef)
		{
			if (TargetRef.Contains(TEXT(".")))
			{
				return TargetRef;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(TargetRef);
			return TargetRef + TEXT(".") + AssetName;
		}

		bool TryValidateCollectionAcyclic(
			UMeshDeformerCollection& Root,
			FGraphCompoundRuntimeError& OutError)
		{
			struct FFrame final
			{
				UMeshDeformerCollection* Collection = nullptr;
				int32 NextChild = 0;
			};
			TSet<const UMeshDeformerCollection*> Visiting;
			TSet<const UMeshDeformerCollection*> Visited;
			TArray<FFrame> Stack;
			Stack.Add({&Root, 0});
			Visiting.Add(&Root);
			while (!Stack.IsEmpty())
			{
				FFrame& Frame = Stack.Last();
				if (Frame.NextChild >= Frame.Collection->MeshDeformerCollections.Num())
				{
					Visiting.Remove(Frame.Collection);
					Visited.Add(Frame.Collection);
					Stack.Pop(EAllowShrinking::No);
					continue;
				}
				UMeshDeformerCollection* const Child =
					Frame.Collection->MeshDeformerCollections[Frame.NextChild++];
				if (!Child)
				{
					SetError(OutError,
						TEXT("animation_rig_graph_collection_entry_invalid"),
						Frame.Collection->GetPathName());
					return false;
				}
				if (Visiting.Contains(Child))
				{
					SetError(OutError,
						TEXT("animation_rig_graph_collection_cycle"),
						Child->GetPathName());
					return false;
				}
				if (!Visited.Contains(Child))
				{
					Visiting.Add(Child);
					Stack.Add({Child, 0});
				}
			}
			return true;
		}

		TArray<const FGraphCompoundDeformerEntrySpec*> OrderedPresentEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			const FString& Kind,
			TSet<FString>& OutMentioned)
		{
			TArray<const FGraphCompoundDeformerEntrySpec*> Present;
			OutMentioned.Reset();
			for (const FGraphCompoundDeformerEntrySpec& Spec : Specs)
			{
				if (Spec.EntryKind != Kind)
				{
					continue;
				}
				OutMentioned.Add(Spec.EntryRef);
				if (Spec.Intent != EChangeIntent::Delete)
				{
					Present.Add(&Spec);
				}
			}
			Present.Sort([](
				const FGraphCompoundDeformerEntrySpec& Left,
				const FGraphCompoundDeformerEntrySpec& Right)
			{
				return Left.RequestedOrderIndex == Right.RequestedOrderIndex
					? Left.EntryRef < Right.EntryRef
					: Left.RequestedOrderIndex < Right.RequestedOrderIndex;
			});

			return Present;
		}

		void ApplyOrderedDeformerEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			TArray<TSoftObjectPtr<UMeshDeformer>>& InOutEntries)
		{
			TSet<FString> Mentioned;
			const TArray<const FGraphCompoundDeformerEntrySpec*> Present =
				OrderedPresentEntries(Specs, TEXT("deformer"), Mentioned);
			TArray<TSoftObjectPtr<UMeshDeformer>> Result;
			for (const FGraphCompoundDeformerEntrySpec* Spec : Present)
			{
				Result.Add(LoadObject<UMeshDeformer>(nullptr, *Spec->EntryRef));
			}
			for (const TSoftObjectPtr<UMeshDeformer>& Existing : InOutEntries)
			{
				const UMeshDeformer* const Object = Existing.Get();
				if (Object && !Mentioned.Contains(Object->GetPathName()))
				{
					Result.Add(Existing);
				}
			}
			InOutEntries = MoveTemp(Result);
		}

		void ApplyOrderedCollectionEntries(
			const TArray<FGraphCompoundDeformerEntrySpec>& Specs,
			TArray<TObjectPtr<UMeshDeformerCollection>>& InOutEntries)
		{
			TSet<FString> Mentioned;
			const TArray<const FGraphCompoundDeformerEntrySpec*> Present =
				OrderedPresentEntries(Specs, TEXT("child_collection"), Mentioned);
			TArray<TObjectPtr<UMeshDeformerCollection>> Result;
			for (const FGraphCompoundDeformerEntrySpec* Spec : Present)
			{
				Result.Add(LoadObject<UMeshDeformerCollection>(
					nullptr, *Spec->EntryRef));
			}
			for (UMeshDeformerCollection* const Existing : InOutEntries)
			{
				if (Existing && !Mentioned.Contains(Existing->GetPathName()))
				{
					Result.Add(Existing);
				}
			}
			InOutEntries = MoveTemp(Result);
		}

		int32 ExpectedNativeIndex(
			const FGraphCompoundDeformerReadbackSpec& Spec,
			const FGraphCompoundDeformerEntrySpec& Entry)
		{
			if (Entry.Intent == EChangeIntent::Delete)
			{
				return INDEX_NONE;
			}
			int32 Index = 0;
			for (const FGraphCompoundDeformerEntrySpec& Candidate : Spec.Entries)
			{
				if (Candidate.EntryKind == Entry.EntryKind
					&& Candidate.Intent != EChangeIntent::Delete
					&& (Candidate.RequestedOrderIndex < Entry.RequestedOrderIndex
						|| (Candidate.RequestedOrderIndex == Entry.RequestedOrderIndex
							&& Candidate.EntryRef < Entry.EntryRef)))
				{
					++Index;
				}
			}
			return Index;
		}

		TSharedRef<FJsonObject> EntryJson(
			const FGraphCompoundDeformerEntrySpec& Entry,
			const int32 NativeIndex,
			const bool bPresent)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), Entry.EntryKind);
			Value->SetStringField(TEXT("entry_ref"), Entry.EntryRef);
			Value->SetNumberField(TEXT("native_order_index"), NativeIndex);
			Value->SetBoolField(TEXT("present"), bPresent);
			return Value;
		}
	}

	bool IsDeformerCreateProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id == TEXT("animation.deformer_graph.asset.create")
			|| Id == TEXT("animation.deformer_source_library.asset.create")
			|| Id == TEXT("animation.mesh_deformer_collection.asset.create")
			|| Id == TEXT("animation.ml_deformer.asset.create");
	}

	bool IsDeformerMutationProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id == TEXT("animation.deformer_graph.topology.apply")
			|| Id == TEXT("animation.deformer_source_library.bindings.apply")
			|| Id == TEXT("animation.mesh_deformer_collection.entries.apply")
			|| Id == TEXT("animation.ml_deformer.configuration.apply");
	}

	bool IsDeformerProductionProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		return IsDeformerCreateProfile(Profile)
			|| IsDeformerMutationProfile(Profile);
	}

	bool IsUnavailableConcreteDeformerProfile(
		const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Id = Profile.GetCapabilityId();
		return Id.StartsWith(TEXT("animation.deformer_source_library."))
			|| Id.StartsWith(TEXT("animation.ml_deformer."));
	}

	bool TrySetUnavailableConcreteDeformerError(
		const FPcgAnimationGraphRouteProfile& Profile,
		FGraphCompoundRuntimeError& OutError)
	{
		if (!IsUnavailableConcreteDeformerProfile(Profile))
		{
			return false;
		}
		SetError(OutError,
			TEXT("animation_rig_graph_concrete_type_unavailable"),
			Profile.GetCapabilityId(),
			TEXT("No enabled plugin provides an adjudicated concrete native owner for this family."));
		return true;
	}

	bool TryParseDeformerReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FGraphCompoundDeformerReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError)
	{
		OutSpec = {};
		OutSpec.TargetKind = Request.GetTargetKind();
		if (TrySetUnavailableConcreteDeformerError(Profile, OutError))
		{
			return false;
		}
		const TArray<FGraphCompoundCanonicalEdit>& Edits =
			Request.GetPayload().GetEdits();
		if (Edits.IsEmpty())
		{
			SetError(OutError, TEXT("graph_compound_deformer_plan_invalid"),
				Request.GetTargetRef());
			return false;
		}

		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.deformer_graph.asset.create"))
		{
			OutSpec.Kind = EGraphCompoundDeformerReadbackKind::DeformerGraphCreate;
			OutSpec.NativeClassPath = UOptimusDeformer::StaticClass()->GetPathName();
			if (Edits.Num() != 1 || !Edits[0].GetProposedAfter().IsValid()
				|| !TryStringField(*Edits[0].GetProposedAfter(),
					TEXT("skeletal_mesh_ref"), OutSpec.SkeletalMeshRef)
				|| !LoadObject<USkeletalMesh>(nullptr, *OutSpec.SkeletalMeshRef))
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_skeletal_mesh_invalid"),
					Request.GetTargetRef(), OutSpec.SkeletalMeshRef);
				return false;
			}
			return true;
		}
		if (Capability
			== TEXT("animation.mesh_deformer_collection.asset.create"))
		{
			OutSpec.Kind =
				EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionCreate;
			OutSpec.NativeClassPath =
				UMeshDeformerCollection::StaticClass()->GetPathName();
			return Edits.Num() == 1;
		}
		if (Capability == TEXT("animation.deformer_graph.topology.apply"))
		{
			OutSpec.Kind = EGraphCompoundDeformerReadbackKind::DeformerGraphTopology;
			OutSpec.NativeClassPath = UOptimusDeformer::StaticClass()->GetPathName();
			if (Edits.Num() != 1 || !Edits[0].GetProposedAfter().IsValid()
				|| Edits[0].GetElementKind() != EGraphCompoundElementKind::Item)
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_topology_edit_unsupported"),
					Request.GetTargetRef());
				return false;
			}
			FString NodeName;
			if (!TryStringField(
					*Edits[0].GetProposedAfter(), TEXT("node_name"), NodeName))
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_node_identity_invalid"),
					Edits[0].GetIdentity().GetSemanticId());
				return false;
			}
			OutSpec.CommentNodeName = *NodeName;
			return true;
		}

		OutSpec.Kind =
			EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionEntries;
		OutSpec.NativeClassPath =
			UMeshDeformerCollection::StaticClass()->GetPathName();
		TSet<FString> Seen;
		for (const FGraphCompoundCanonicalEdit& Edit : Edits)
		{
			FGraphCompoundDeformerEntrySpec Entry;
			Entry.EntryRef = EntryRef(Edit);
			Entry.Intent = Edit.GetIntent();
			if (Entry.EntryRef.IsEmpty()
				|| (Entry.Intent != EChangeIntent::Delete
					&& (!Edit.GetProposedAfter().IsValid()
						|| !TryStringField(*Edit.GetProposedAfter(),
							TEXT("entry_kind"), Entry.EntryKind)
						|| !TryIntegerField(*Edit.GetProposedAfter(),
							TEXT("order_index"), Entry.RequestedOrderIndex)))
				|| Seen.Contains(Entry.EntryRef))
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_collection_entry_invalid"),
					Edit.GetIdentity().GetSemanticId());
				return false;
			}
			Seen.Add(Entry.EntryRef);
			if (Entry.Intent == EChangeIntent::Delete)
			{
				if (UObject* Existing = LoadObject<UObject>(nullptr, *Entry.EntryRef))
				{
					Entry.EntryKind = Existing->IsA<UMeshDeformerCollection>()
						? TEXT("child_collection") : TEXT("deformer");
				}
			}
			if (Entry.EntryKind == TEXT("deformer"))
			{
				if (!LoadObject<UMeshDeformer>(nullptr, *Entry.EntryRef))
				{
					SetError(OutError,
						TEXT("graph_compound_deformer_collection_entry_invalid"),
						Entry.EntryRef, Entry.EntryKind);
					return false;
				}
			}
			else if (Entry.EntryKind == TEXT("child_collection"))
			{
				if (!LoadObject<UMeshDeformerCollection>(nullptr, *Entry.EntryRef))
				{
					SetError(OutError,
						TEXT("graph_compound_deformer_collection_entry_invalid"),
						Entry.EntryRef, Entry.EntryKind);
					return false;
				}
			}
			else
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_collection_kind_invalid"),
					Entry.EntryRef, Entry.EntryKind);
				return false;
			}
			OutSpec.Entries.Add(MoveTemp(Entry));
		}
		return true;
	}

	bool TryCaptureDeformerValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FGraphCompoundDeformerCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError)
	{
		OutValues.Reset();
		if (Profile.GetCapabilityId()
			== TEXT("animation.deformer_graph.topology.apply"))
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(&Target);
			const UOptimusNodeGraph* const Graph =
				Deformer ? Deformer->GetUpdateGraph() : nullptr;
			if (!Graph)
			{
				SetError(OutError,
					TEXT("graph_compound_deformer_target_class_mismatch"),
					Target.GetPathName());
				return false;
			}
			for (const UOptimusNode* Node : Graph->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("node_name"), Node->GetName());
				Value->SetStringField(TEXT("native_object_path"), Node->GetPathName());
				OutValues.Emplace(TEXT("item:") + Node->GetName(), Value);
			}
			return true;
		}

		const UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(&Target);
		if (!Collection)
		{
			SetError(OutError,
				TEXT("graph_compound_deformer_target_class_mismatch"),
				Target.GetPathName());
			return false;
		}
		for (int32 Index = 0; Index < Collection->MeshDeformers.Num(); ++Index)
		{
			const FString Ref =
				Collection->MeshDeformers[Index].ToSoftObjectPath().ToString();
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), TEXT("deformer"));
			Value->SetNumberField(TEXT("order_index"), Index);
			OutValues.Emplace(TEXT("item:collection_entry:") + Ref, Value);
		}
		for (int32 Index = 0;
			Index < Collection->MeshDeformerCollections.Num(); ++Index)
		{
			const UMeshDeformerCollection* const Child =
				Collection->MeshDeformerCollections[Index];
			if (!Child)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("entry_kind"), TEXT("child_collection"));
			Value->SetNumberField(TEXT("order_index"), Index);
			OutValues.Emplace(
				TEXT("item:collection_entry:") + Child->GetPathName(), Value);
		}
		return true;
	}

	TSharedRef<FJsonObject> DeformerExpectedJson(
		const FGraphCompoundDeformerReadbackSpec& Spec)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		Root->SetStringField(TEXT("native_class"), Spec.NativeClassPath);
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			Root->SetStringField(TEXT("skeletal_mesh_ref"), Spec.SkeletalMeshRef);
			Root->SetBoolField(TEXT("primary_binding_present"), true);
		}
		else if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::DeformerGraphTopology)
		{
			Root->SetStringField(TEXT("node_name"), Spec.CommentNodeName.ToString());
			Root->SetBoolField(TEXT("present"), true);
		}
		else if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionEntries)
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
			{
				Entries.Add(MakeShared<FJsonValueObject>(EntryJson(
					Entry, ExpectedNativeIndex(Spec, Entry),
					Entry.Intent != EChangeIntent::Delete)));
			}
			Root->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		}
		return Root;
	}

	bool TryBuildDeformerActualJson(
		const FString& TargetRef,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		FString& OutError)
	{
		OutError.Reset();
		OutRoot = MakeShared<FJsonObject>();
		UObject* const Target = LoadObject<UObject>(nullptr, *ObjectPath(TargetRef));
		if (!Target || Target->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			OutError = TEXT("deformer_readback_target_class_mismatch");
			return false;
		}
		OutRoot->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		OutRoot->SetStringField(TEXT("native_class"), Target->GetClass()->GetPathName());
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(Target);
			OutRoot->SetStringField(TEXT("skeletal_mesh_ref"),
				Deformer && Deformer->Mesh ? Deformer->Mesh->GetPathName() : FString());
			OutRoot->SetBoolField(TEXT("primary_binding_present"),
				Deformer && Deformer->GetComponentBindings().ContainsByPredicate(
					[](const UOptimusComponentSourceBinding* Binding)
					{
						return Binding && Binding->GetFName()
							== UOptimusComponentSourceBinding::GetPrimaryBindingName();
					}));
			return true;
		}
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphTopology)
		{
			const UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(Target);
			const UOptimusNodeGraph* const Graph =
				Deformer ? Deformer->GetUpdateGraph() : nullptr;
			const UOptimusNode* const* Found = Graph
				? Graph->GetAllNodes().FindByPredicate(
					[&Spec](const UOptimusNode* Node)
					{
						return Node && Node->GetFName() == Spec.CommentNodeName;
					}) : nullptr;
			OutRoot->SetStringField(TEXT("node_name"), Spec.CommentNodeName.ToString());
			OutRoot->SetBoolField(TEXT("present"), Found && *Found);
			return true;
		}
		if (Spec.Kind
			== EGraphCompoundDeformerReadbackKind::MeshDeformerCollectionCreate)
		{
			return true;
		}
		const UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(Target);
		if (!Collection)
		{
			OutError = TEXT("deformer_readback_collection_missing");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
		{
			int32 NativeIndex = INDEX_NONE;
			if (Entry.EntryKind == TEXT("deformer"))
			{
				NativeIndex = Collection->MeshDeformers.IndexOfByPredicate(
					[&Entry](const TSoftObjectPtr<UMeshDeformer>& Value)
					{
						return Value.ToSoftObjectPath().ToString() == Entry.EntryRef;
					});
			}
			else
			{
				NativeIndex = Collection->MeshDeformerCollections.IndexOfByPredicate(
					[&Entry](const UMeshDeformerCollection* Value)
					{
						return Value && Value->GetPathName() == Entry.EntryRef;
					});
			}
			Entries.Add(MakeShared<FJsonValueObject>(
				EntryJson(Entry, NativeIndex, NativeIndex != INDEX_NONE)));
		}
		OutRoot->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		return true;
	}

	void AddDeformerDependencyAnchors(
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors)
	{
		if (!Spec.SkeletalMeshRef.IsEmpty())
		{
			OutAnchors.Emplace(LoadObject<UObject>(nullptr, *Spec.SkeletalMeshRef));
		}
		for (const FGraphCompoundDeformerEntrySpec& Entry : Spec.Entries)
		{
			if (UObject* const Object = LoadObject<UObject>(nullptr, *Entry.EntryRef))
			{
				OutAnchors.Emplace(Object);
			}
		}
	}

	UObject* TryCreateDeformerAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError)
	{
		(void)Profile;
		const FName AssetName(*FPackageName::ObjectPathToObjectName(
			ObjectPath(Request.GetTargetRef())));
		if (Spec.Kind == EGraphCompoundDeformerReadbackKind::DeformerGraphCreate)
		{
			USkeletalMesh* const Mesh =
				LoadObject<USkeletalMesh>(nullptr, *Spec.SkeletalMeshRef);
			UOptimusDeformer* const Deformer = NewObject<UOptimusDeformer>(
				&Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Mesh || !Deformer)
			{
				SetError(OutError, TEXT("graph_compound_deformer_factory_failed"),
					Request.GetTargetRef());
				return nullptr;
			}
			Deformer->Mesh = Mesh;
			if (!Deformer->AddComponentBinding(
					nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName())
				|| !Deformer->Compile())
			{
				SetError(OutError,
					TEXT("animation_rig_graph_compile_incomplete"),
					Request.GetTargetRef());
				return nullptr;
			}
			return Deformer;
		}
		UMeshDeformerCollection* const Collection =
			NewObject<UMeshDeformerCollection>(
				&Package, AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
		if (!Collection)
		{
			SetError(OutError, TEXT("graph_compound_deformer_factory_failed"),
				Request.GetTargetRef());
		}
		return Collection;
	}

	bool TryApplyDeformerMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError)
	{
		if (Profile.GetCapabilityId()
			== TEXT("animation.deformer_graph.topology.apply"))
		{
			UOptimusDeformer* const Deformer = Cast<UOptimusDeformer>(&Target);
			FAnimationRigGraphNativeMutationReceipt Apply;
			FAnimationRigGraphBarrierReceipt Barrier;
			FAnimationRigGraphTopology Topology;
			FString Evidence;
			FAnimationRigGraphAdapterError NativeError;
			if (!Deformer || !Hook.TryEnsureCompileAndCaptureDeformerGraph(
					*Deformer, Spec.CommentNodeName, Apply, Barrier,
					Topology, OutRevision, Evidence, NativeError)
				|| !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
			{
				SetError(OutError,
					NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_deformer_native_failed")
						: NativeError.GetCode(),
					Target.GetPathName(), NativeError.GetDetail());
				return false;
			}
			return true;
		}

		UMeshDeformerCollection* const Collection =
			Cast<UMeshDeformerCollection>(&Target);
		if (!Collection)
		{
			SetError(OutError,
				TEXT("graph_compound_deformer_target_class_mismatch"),
				Target.GetPathName());
			return false;
		}
		Collection->Modify();
		ApplyOrderedDeformerEntries(Spec.Entries, Collection->MeshDeformers);
		ApplyOrderedCollectionEntries(
			Spec.Entries, Collection->MeshDeformerCollections);
		if (!TryValidateCollectionAcyclic(*Collection, OutError))
		{
			return false;
		}
		FString Identity = Collection->GetPathName();
		for (const TSoftObjectPtr<UMeshDeformer>& Entry : Collection->MeshDeformers)
		{
			Identity += TEXT("|D:") + Entry.ToSoftObjectPath().ToString();
		}
		for (const UMeshDeformerCollection* Entry
			: Collection->MeshDeformerCollections)
		{
			Identity += TEXT("|C:")
				+ (Entry ? Entry->GetPathName() : FString(TEXT("null")));
		}
		OutRevision = TEXT("md5:") + FMD5::HashAnsiString(*Identity);
		return true;
	}
}
