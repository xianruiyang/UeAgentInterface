// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Dom/JsonValue.h"

#include "Animation/AnimBlueprint.h"
#include "BaseWidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "ComponentInstanceDataCache.h"
#include "GameFramework/Actor.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/GCObject.h"
#include "UObject/GarbageCollection.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentBlueprintAssetLifecycle
{
	struct FComponentInstanceRollback::FImpl : FGCObject
	{
		struct FEntry
		{
			TWeakObjectPtr<AActor> Actor;
			FString Path;
			TArray<uint8> Before;
			TUniquePtr<FComponentInstanceDataCache> NativeCache;
		};
		TArray<FEntry> Entries;
		virtual FString GetReferencerName() const override { return TEXT("UAI.BlueprintComponentInstanceRollback"); }
		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (FEntry& Entry : Entries) if (Entry.NativeCache) Entry.NativeCache->AddReferencedObjects(Collector);
		}
	};
	FComponentInstanceRollback::FComponentInstanceRollback() : Impl(MakeUnique<FImpl>()) {}
	FComponentInstanceRollback::~FComponentInstanceRollback() = default;
	void FComponentInstanceRollback::SetActors(const TArray<AActor*>& Actors)
	{
		for (AActor* Actor : Actors) { auto& Entry = Impl->Entries.AddDefaulted_GetRef(); Entry.Actor = Actor; Entry.Path = Actor->GetPathName(); }
	}
	void FComponentInstanceRollback::CollectObjects(TArray<UObject*>& OutObjects) const
	{
		for (const auto& Entry : Impl->Entries) if (Entry.Actor.IsValid()) OutObjects.AddUnique(Entry.Actor.Get());
	}
	bool FComponentInstanceRollback::Capture(FDiagnostic& OutError)
	{
		for (auto& Entry : Impl->Entries)
		{
			AActor* Actor = Entry.Actor.Get();
			if (!Actor) { OutError = FDiagnostic::Failure(TEXT("blueprint_instance_checkpoint_missing"), Entry.Path); return false; }
			Entry.NativeCache = MakeUnique<FComponentInstanceDataCache>(Actor);
			FMemoryWriter Writer(Entry.Before);
			FObjectAndNameAsStringProxyArchive Archive(Writer, false);
			Entry.NativeCache->Serialize(Archive);
			if (Archive.IsError()) { OutError = FDiagnostic::Failure(TEXT("blueprint_instance_checkpoint_failed"), Entry.Path); return false; }
		}
		return true;
	}
	bool FComponentInstanceRollback::Restore(FDiagnostic& OutError)
	{
		for (auto& Entry : Impl->Entries)
		{
			if (!Entry.NativeCache) continue;
			AActor* Actor = FindObject<AActor>(nullptr, *Entry.Path);
			if (!Actor) { OutError = FDiagnostic::Failure(TEXT("blueprint_instance_restore_missing"), Entry.Path); return false; }
			// Resolve original template paths only after the Blueprint checkpoint is
			// restored. Keeping the native cache alive also preserves its duplicated
			// subobjects; no reflected property conversion is implemented here.
			FComponentInstanceDataCache Restored(Actor);
			FMemoryReader Reader(Entry.Before);
			FObjectAndNameAsStringProxyArchive Archive(Reader, false);
			Restored.Serialize(Archive);
			if (Archive.IsError()) { OutError = FDiagnostic::Failure(TEXT("blueprint_instance_restore_failed"), Entry.Path); return false; }
			Restored.ApplyToActor(Actor, ECacheApplyPhase::NonConstructionScript);
		}
		return true;
	}

	namespace
	{
		struct FAuthoringContext
		{
			FString TargetPath;
			TWeakObjectPtr<UBlueprint> ExistingTarget;
		};
		TMap<TWeakObjectPtr<const UBlueprint>, FAuthoringContext> AuthoringContexts;
		bool bRetiredScratchPendingCollection = false;

		void RestoreAuthoredPreviewSettings(UBlueprint* Source, UBlueprint* Duplicate)
		{
			UAnimBlueprint* SourceAnimation = Cast<UAnimBlueprint>(Source);
			UAnimBlueprint* DuplicateAnimation = Cast<UAnimBlueprint>(Duplicate);
			if (!SourceAnimation || !DuplicateAnimation) return;
			// UE resets these duplicate-transient fields, but authoring staging must
			// preserve the source document through scratch and published copies.
			DuplicateAnimation->SetPreviewMesh(SourceAnimation->GetPreviewMesh(), false);
			DuplicateAnimation->SetPreviewAnimationBlueprint(SourceAnimation->GetPreviewAnimationBlueprint());
		}

		TMap<FName, FGuid> CaptureAuthoredMemberGuids(const UBlueprint* Blueprint)
		{
			TMap<FName, FGuid> Result;
			if (Blueprint)
			{
				for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				{
					Result.Add(Variable.VarName, Variable.VarGuid);
				}
			}
			return Result;
		}

		void RestoreAuthoredMemberGuids(
			UBlueprint& Blueprint,
			const TMap<FName, FGuid>& AuthoredMemberGuids)
		{
			for (FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				if (const FGuid* Guid = AuthoredMemberGuids.Find(Variable.VarName)) Variable.VarGuid = *Guid;
			}
			TArray<UK2Node_Variable*> VariableNodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, VariableNodes);
			for (UK2Node_Variable* Node : VariableNodes)
			{
				if (!Node || !Node->VariableReference.IsSelfContext()) continue;
				if (const FGuid* Guid = AuthoredMemberGuids.Find(Node->GetVarName()))
				{
					Node->VariableReference.SetSelfMember(Node->GetVarName(), *Guid);
				}
			}
			TArray<UK2Node_BaseMCDelegate*> DelegateNodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, DelegateNodes);
			for (UK2Node_BaseMCDelegate* Node : DelegateNodes)
			{
				if (!Node || !Node->DelegateReference.IsSelfContext()) continue;
				const FName MemberName = Node->GetPropertyName();
				if (const FGuid* Guid = AuthoredMemberGuids.Find(MemberName))
				{
					Node->DelegateReference.SetSelfMember(MemberName, *Guid);
				}
			}
		}

		void RestoreAuthoredCopy(UBlueprint* Source, UBlueprint& Copy,
			const TMap<FName, FGuid>& AuthoredMemberGuids)
		{
			RestoreAuthoredPreviewSettings(Source, &Copy);
			RestoreAuthoredMemberGuids(Copy, AuthoredMemberGuids);
			if (Source)
			{
				// Generated classes are package siblings of their Blueprint. A native
				// duplicate can retain source-class references in graph nodes (for
				// example a cast target) while member references already use the copy.
				// Preserve the authored self type consistently within the copied graph.
				TMap<UObject*, UObject*> Classes;
				if (Source->GeneratedClass && Copy.GeneratedClass && Source->GeneratedClass != Copy.GeneratedClass)
					Classes.Add(Source->GeneratedClass, Copy.GeneratedClass);
				if (Source->SkeletonGeneratedClass && Copy.SkeletonGeneratedClass && Source->SkeletonGeneratedClass != Copy.SkeletonGeneratedClass)
					Classes.Add(Source->SkeletonGeneratedClass, Copy.SkeletonGeneratedClass);
				FArchiveReplaceObjectRef<UObject> Rebind(&Copy, Classes,
					EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
			}
			RebindBlueprintSelfMemberReferences(Copy);
			// PostDuplicate changes member GUIDs before reconstructing nodes. Restore
			// authored identities before settling context-dependent native interfaces
			// and performing UE's optional final compilation.
			FBlueprintEditorUtils::ReconstructAllNodes(&Copy);
			FKismetEditorUtilities::CompileBlueprint(&Copy, EBlueprintCompileOptions::SkipGarbageCollection);
		}

		// Only the root's working-copy lifetime is suppressed. The native duplicate
		// still owns construction, subobject mapping and PostDuplicate/PostLoad.
		class FScopedRootPersistence
		{
		public:
			explicit FScopedRootPersistence(UBlueprint& InSource)
				: Source(InSource), bWasTransient(Source.HasAnyFlags(RF_Transient))
			{
				Source.ClearFlags(RF_Transient);
			}
			~FScopedRootPersistence()
			{
				if (bWasTransient) Source.SetFlags(RF_Transient);
			}
		private:
			UBlueprint& Source;
			bool bWasTransient;
		};

		UBlueprint* DuplicateAuthoredGraph(FObjectDuplicationParameters& Parameters)
		{
			UBlueprint& Source = *CastChecked<UBlueprint>(Parameters.SourceObject);
			TMap<UObject*, UObject*> Copies;
			Parameters.CreatedObjects = &Copies;
			UBlueprint* Copy = nullptr;
			{
				FScopedRootPersistence RootScope(Source);
				Copy = Cast<UBlueprint>(StaticDuplicateObjectEx(Parameters));
			}
			// Native duplication reuses constructor-created default subobjects.
			// Preserve explicit transient intent on those source objects as well;
			// ordinary nested objects already receive their own source flags.
			for (const auto& Entry : Copies)
			{
				if (Entry.Key != &Source && Entry.Key->HasAllFlags(RF_DefaultSubObject | RF_Transient)
					&& IsValid(Entry.Value))
				{
					Entry.Value->SetFlags(RF_Transient);
				}
			}
			return Copy;
		}

		bool RequiredObject(const UBlueprint& Blueprint, const UObject* Object,
			const UObject* Outer, const TCHAR* Property, const bool bPersistent, FDiagnostic& OutError)
		{
			const bool bPresent = IsValid(Object);
			const bool bOuterMatches = bPresent && Object->GetOuter() == Outer;
			const bool bTransient = bPresent && Object->HasAnyFlags(RF_Transient);
			if (bPresent && bOuterMatches && (!bPersistent || !bTransient)) return true;

			const TCHAR* Code = !bPresent ? TEXT("blueprint_required_object_missing")
				: !bOuterMatches ? TEXT("blueprint_required_object_outer_invalid")
				: TEXT("blueprint_required_object_transient");
			const FString Reason = !bPresent
				? FString::Printf(TEXT("The Blueprint's required %s object is missing."), Property)
				: !bOuterMatches
					? FString::Printf(TEXT("The Blueprint's %s object has the wrong owner."), Property)
					: FString::Printf(TEXT("The Blueprint's required %s object is transient and would be omitted when saving."), Property);
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("blueprint_structure"));
			OutError.TargetPath = GetAuthoringTargetPath(Blueprint);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Property;
			OutError.ActualValuePresent = true;
			if (bPresent)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("object"), Object->GetPathName());
				Actual->SetStringField(TEXT("outer"), GetPathNameSafe(Object->GetOuter()));
				Actual->SetBoolField(TEXT("transient"), bTransient);
				OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
			}
			else OutError.ActualValue = MakeShared<FJsonValueNull>();
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetStringField(TEXT("outer"), GetPathNameSafe(Outer));
			Expected->SetBoolField(TEXT("present"), true);
			if (bPersistent) Expected->SetBoolField(TEXT("transient"), false);
			OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			return false;
		}
	}

	struct FScratchBlueprintPreflightTransaction::FImpl
	{
		TUniquePtr<FScopedTransaction> Transaction;
	};

	FScratchBlueprintPreflightTransaction::FScratchBlueprintPreflightTransaction()
		: Impl(MakeUnique<FImpl>()) {}

	FScratchBlueprintPreflightTransaction::~FScratchBlueprintPreflightTransaction()
	{
		if (Impl->Transaction) Impl->Transaction->Cancel();
	}

	bool FScratchBlueprintPreflightTransaction::Begin(const FString& TargetPath, FDiagnostic& OutError)
	{
		if (GEditor && (GEditor->IsTransactionActive() || GIsTransacting))
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_preflight_transaction_active"),
				TEXT("No Blueprint mutation was started because an Editor transaction is active. Finish the current editing action, then retry."), TEXT("preflight"));
			OutError.TargetPath = TargetPath;
			OutError.SourceKind = TEXT("native_state");
			OutError.PropertyPath = TEXT("editor_transaction_active");
			OutError.ActualValuePresent = true;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			OutError.bRetrySafe = true;
			return false;
		}
		Impl->Transaction = MakeUnique<FScopedTransaction>(
			NSLOCTEXT("UeAgentInterface", "BlueprintPreflight", "Validate Blueprint Authoring"));
		return true;
	}

	struct FScratchBlueprintPackages::FImpl
	{
		TArray<TStrongObjectPtr<UPackage>> Packages;
	};

	FScratchBlueprintPackages::FScratchBlueprintPackages() : Impl(MakeUnique<FImpl>()) {}
	FScratchBlueprintPackages::~FScratchBlueprintPackages() { Reset(); }

	UPackage* FScratchBlueprintPackages::Create(const TCHAR* Purpose)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/UAI/Blueprint/%s_%s"),
			Purpose, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Package->SetFlags(RF_Transient);
		Impl->Packages.Emplace(Package);
		return Package;
	}

	void FScratchBlueprintPackages::Reset()
	{
		for (const auto& Owned : Impl->Packages)
		{
			UPackage* Package = Owned.Get();
			if (!Package) continue;
			TArray<UObject*> Objects;
			GetObjectsWithPackage(Package, Objects, true);
			for (UObject* Object : Objects)
			{
				Object->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
				Object->SetFlags(RF_Transient);
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
				{
					AuthoringContexts.Remove(Blueprint);
					// Match native Blueprint retirement: invalidate the authoring root,
					// not its generated class, functions or CDO. Those dependencies
					// must stay valid until native instance/frame destruction finishes.
					Blueprint->MarkAsGarbage();
				}
			}
			Package->SetDirtyFlag(false);
			Package->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
			bRetiredScratchPendingCollection = true;
		}
		Impl->Packages.Reset();
	}

	void CollectRetiredScratchBlueprints()
	{
		check(IsInGameThread());
		if (bRetiredScratchPendingCollection)
		{
			bRetiredScratchPendingCollection = false;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	void SetAuthoringContext(UBlueprint& Working, const FString& TargetPath, UBlueprint* ExistingTarget)
	{
		// Prepared factory seeds and duplicates are not native content assets.
		// They remain discoverable until GC, so mark their real lifetime before
		// native dependency scans can mistake them for persistent consumers.
		Working.SetFlags(RF_Transient);
		for (auto It = AuthoringContexts.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid()) It.RemoveCurrent();
		}
		AuthoringContexts.Add(&Working, {TargetPath, ExistingTarget});
	}

	bool IsAuthoringWorkingCopy(const UBlueprint& Blueprint)
	{
		return AuthoringContexts.Contains(&Blueprint) || Blueprint.HasAnyFlags(RF_Transient);
	}

	bool ValidateBlueprintStructure(const UBlueprint& Blueprint, FDiagnostic& OutError)
	{
		OutError.Reset();
		if (const auto* WidgetBlueprint = Cast<UBaseWidgetBlueprint>(&Blueprint))
		{
			return RequiredObject(Blueprint, WidgetBlueprint->WidgetTree, &Blueprint,
				TEXT("WidgetTree"), false, OutError);
		}
		return true;
	}

	bool ValidateBlueprintPersistence(const UBlueprint& Blueprint, FDiagnostic& OutError)
	{
		if (!ValidateBlueprintStructure(Blueprint, OutError)) return false;
		if (!RequiredObject(Blueprint, &Blueprint, Blueprint.GetOutermost(), TEXT("asset"), true, OutError))
			return false;
		if (const auto* WidgetBlueprint = Cast<UBaseWidgetBlueprint>(&Blueprint))
		{
			if (!RequiredObject(Blueprint, WidgetBlueprint->WidgetTree, &Blueprint,
				TEXT("WidgetTree"), true, OutError)) return false;
		}
		UClass* GeneratedClass = Blueprint.GeneratedClass;
		// Generated-class/CDO availability belongs to the native Blueprint kind
		// and compiler. Validate existing objects without inventing a requirement
		// for graph-only Blueprint kinds or constructing a CDO during validation.
		if (GeneratedClass)
		{
			if (!RequiredObject(Blueprint, GeneratedClass, Blueprint.GetOutermost(),
				TEXT("GeneratedClass"), true, OutError)) return false;
			if (const UObject* Defaults = GeneratedClass->GetDefaultObject(false))
				return RequiredObject(Blueprint, Defaults, Blueprint.GetOutermost(),
					TEXT("GeneratedClass.ClassDefaultObject"), true, OutError);
		}
		return true;
	}

	FString GetAuthoringTargetPath(const UBlueprint& Blueprint)
	{
		const FAuthoringContext* Context = AuthoringContexts.Find(&Blueprint);
		return Context ? Context->TargetPath : Blueprint.GetPathName();
	}

	FString GetAuthoringClassPath(const UBlueprint& Blueprint, const UClass& Class)
	{
		const FAuthoringContext* Context = AuthoringContexts.Find(&Blueprint);
		if (!Context || Class.ClassGeneratedBy != &Blueprint) return Class.GetPathName();
		const bool bSkeleton = &Class == Blueprint.SkeletonGeneratedClass;
		const bool bGenerated = &Class == Blueprint.GeneratedClass;
		if (!bSkeleton && !bGenerated) return Class.GetPathName();
		if (const UBlueprint* Existing = Context->ExistingTarget.Get())
		{
			const UClass* TargetClass = bSkeleton ? Existing->SkeletonGeneratedClass : Existing->GeneratedClass;
			if (TargetClass) return TargetClass->GetPathName();
		}
		const FString PackagePath = UeAgentAuthoringAssetIdentity::NormalizePackagePath(Context->TargetPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		FName GeneratedName, SkeletonName;
		Blueprint.GetBlueprintClassNames(GeneratedName, SkeletonName, FName(*AssetName));
		return PackagePath + TEXT(".") + (bSkeleton ? SkeletonName : GeneratedName).ToString();
	}

	FString GetAuthoringTypeObjectPath(const UBlueprint* Blueprint, const UObject& Object,
		const FString& CanonicalAssetPath)
	{
		const UClass* Class = Cast<UClass>(&Object);
		if (!Blueprint || !Class) return Object.GetPathName();
		const FAuthoringContext* Context = AuthoringContexts.Find(Blueprint);
		const UBlueprint* Existing = Context ? Context->ExistingTarget.Get() : nullptr;
		const bool bOwned = Class == Blueprint->GeneratedClass || Class == Blueprint->SkeletonGeneratedClass
			|| (Existing && (Class == Existing->GeneratedClass || Class == Existing->SkeletonGeneratedClass));
		if (!bOwned) return Object.GetPathName();
		const FString PackagePath = UeAgentAuthoringAssetIdentity::NormalizePackagePath(
			CanonicalAssetPath.IsEmpty() ? GetAuthoringTargetPath(*Blueprint) : CanonicalAssetPath);
		FName GeneratedName, SkeletonName;
		Blueprint->GetBlueprintClassNames(GeneratedName, SkeletonName,
			FName(*FPackageName::GetLongPackageAssetName(PackagePath)));
		return PackagePath + TEXT(".") + GeneratedName.ToString();
	}

	bool TryResolveAuthoringSelfClass(UBlueprint& Blueprint, const FString& ClassPath, UClass*& OutClass)
	{
		OutClass = nullptr;
		const FString Path = ClassPath.TrimStartAndEnd();
		const FString PackagePath = UeAgentAuthoringAssetIdentity::NormalizePackagePath(GetAuthoringTargetPath(Blueprint));
		FName GeneratedName, SkeletonName;
		Blueprint.GetBlueprintClassNames(GeneratedName, SkeletonName,
			FName(*FPackageName::GetLongPackageAssetName(PackagePath)));
		bool bSelf = Path.Equals(PackagePath + TEXT(".") + GeneratedName.ToString(), ESearchCase::IgnoreCase)
			|| Path.Equals(PackagePath + TEXT(".") + SkeletonName.ToString(), ESearchCase::IgnoreCase);
		const FAuthoringContext* Context = AuthoringContexts.Find(&Blueprint);
		for (const UBlueprint* Owner : {&Blueprint, Context ? Context->ExistingTarget.Get() : nullptr})
		{
			if (!Owner) continue;
			for (const UClass* Class : {Owner->GeneratedClass.Get(), Owner->SkeletonGeneratedClass.Get()})
				bSelf |= Class && Path.Equals(Class->GetPathName(), ESearchCase::IgnoreCase);
		}
		if (bSelf) OutClass = Blueprint.SkeletonGeneratedClass ? Blueprint.SkeletonGeneratedClass : Blueprint.GeneratedClass;
		return bSelf;
	}

	UBlueprint* LoadBlueprint(const FString& InPath)
	{
		const FString PackagePath =
			UeAgentAuthoringAssetIdentity::NormalizePackagePath(InPath);
		return PackagePath.IsEmpty()
			? nullptr
			: Cast<UBlueprint>(StaticLoadObject(
				UBlueprint::StaticClass(),
				nullptr,
				*UeAgentAuthoringAssetIdentity::ToObjectPath(PackagePath)));
	}

	UBlueprint* DuplicateBlueprint(UBlueprint* Source, UObject* Outer, const FName Name,
		const bool bPreserveTextIdentity, FDiagnostic* OutError)
	{
		FDiagnostic Error;
		if (!Source || !Outer)
			Error = FDiagnostic::Failure(TEXT("blueprint_duplicate_input_missing"),
				TEXT("Blueprint duplication requires both a source Blueprint and a destination owner."), TEXT("blueprint_duplicate"));
		else ValidateBlueprintStructure(*Source, Error);
		if (!Error.IsEmpty()) { if (OutError) *OutError = MoveTemp(Error); return nullptr; }
		if (OutError) OutError->Reset();
		const TMap<FName, FGuid> AuthoredMemberGuids = CaptureAuthoredMemberGuids(Source);
		FBlueprintDuplicationScopeFlags DuplicationScope(
			FBlueprintDuplicationScopeFlags::TheSameNodeGuid
				| FBlueprintDuplicationScopeFlags::TheSameTimelineGuid
				| FBlueprintDuplicationScopeFlags::NoExtraCompilation);
		UBlueprint* Duplicate = nullptr;
		if (Source && Outer)
		{
			FObjectDuplicationParameters Parameters(Source, Outer);
			Parameters.DestName = Name;
			// This helper makes working/rollback copies, never published assets.
			// Strip the editor GC keep flag from the entire duplicated object graph,
			// including copies abandoned before an operation state takes ownership.
			Parameters.FlagMask = RF_AllFlags & ~RF_Standalone;
			if (bPreserveTextIdentity)
				Parameters.PortFlags |= PPF_DuplicateVerbatim;
			Duplicate = DuplicateAuthoredGraph(Parameters);
		}
		if (Duplicate)
		{
			Duplicate->SetFlags(RF_Transient);
			RestoreAuthoredCopy(Source, *Duplicate, AuthoredMemberGuids);
		}
		else if (OutError)
		{
			*OutError = FDiagnostic::Failure(TEXT("blueprint_duplicate_failed"),
				TEXT("The native editor returned no Blueprint while duplicating the source object."), TEXT("blueprint_duplicate"));
			OutError->TargetPath = GetAuthoringTargetPath(*Source);
			OutError->SourceKind = TEXT("native_result");
			OutError->PropertyPath = TEXT("asset");
			OutError->ActualValue = MakeShared<FJsonValueNull>();
		}
		return Duplicate;
	}

	UBlueprint* PublishBlueprint(UBlueprint* Source, UPackage* Package, const FString& Name, FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Source || !Package || Name.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_publish_input_missing"),
				TEXT("Blueprint publication requires a source Blueprint, destination package and asset name."), TEXT("blueprint_publish"));
			OutError.PropertyPath = TEXT("asset");
			return nullptr;
		}
		if (!ValidateBlueprintStructure(*Source, OutError)) return nullptr;
		if (const auto* WidgetBlueprint = Cast<UBaseWidgetBlueprint>(Source))
		{
			if (!RequiredObject(*Source, WidgetBlueprint->WidgetTree, Source,
				TEXT("WidgetTree"), true, OutError)) return nullptr;
		}
		const TMap<FName, FGuid> AuthoredMemberGuids = CaptureAuthoredMemberGuids(Source);

		FBlueprintDuplicationScopeFlags DuplicationScope(
			FBlueprintDuplicationScopeFlags::TheSameNodeGuid
				| FBlueprintDuplicationScopeFlags::TheSameTimelineGuid
				| FBlueprintDuplicationScopeFlags::NoExtraCompilation);
		FObjectDuplicationParameters Parameters(Source, Package);
		Parameters.DestName = FName(*Name);
		Parameters.FlagMask = RF_AllFlags;
		UBlueprint* Published = DuplicateAuthoredGraph(Parameters);
		if (!Published)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_asset_publish_failed"),
				TEXT("The native editor returned no Blueprint while duplicating the prepared asset."), TEXT("blueprint_publish"));
			OutError.TargetPath = Package->GetName();
			OutError.PropertyPath = TEXT("asset");
			OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.SourceKind = TEXT("native_result");
			return nullptr;
		}

		Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		RestoreAuthoredCopy(Source, *Published, AuthoredMemberGuids);
		return Published;
	}

	void RebindBlueprintSelfMemberReferences(UBlueprint& Blueprint)
	{
		TMap<FName, FGuid> SelfMemberGuids;
		for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
		{
			SelfMemberGuids.Add(Variable.VarName, Variable.VarGuid);
		}
		if (Blueprint.SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (Node) SelfMemberGuids.Add(Node->GetVariableName(), Node->VariableGuid);
			}
		}

		TArray<UK2Node_Variable*> VariableNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, VariableNodes);
		for (UK2Node_Variable* Node : VariableNodes)
		{
			if (!Node || !Node->VariableReference.IsSelfContext()) continue;
			if (const FGuid* Guid = SelfMemberGuids.Find(Node->GetVarName()))
			{
				Node->VariableReference.SetSelfMember(Node->GetVarName(), *Guid);
			}
		}
		TArray<UK2Node_BaseMCDelegate*> DelegateNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, DelegateNodes);
		for (UK2Node_BaseMCDelegate* Node : DelegateNodes)
		{
			if (!Node || !Node->DelegateReference.IsSelfContext()) continue;
			const FName MemberName = Node->GetPropertyName();
			if (const FGuid* Guid = SelfMemberGuids.Find(MemberName))
			{
				Node->DelegateReference.SetSelfMember(MemberName, *Guid);
			}
		}
	}

	bool DeleteBlueprint(UBlueprint* Blueprint, const FString& TargetPath, FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_delete_target_missing"),
				TEXT("No Blueprint object was supplied for deletion."), TEXT("asset_delete"));
			OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = TargetPath; OutError.PropertyPath = TEXT("asset");
			OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(UBlueprint::StaticClass()->GetPathName());
			return false;
		}
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		if (!ObjectTools::DeleteSingleObject(Blueprint, true))
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_delete_rejected"),
				TEXT("The editor rejected deletion of the Blueprint."), TEXT("asset_delete"),
				TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
			OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = TargetPath; OutError.PropertyPath = TEXT("asset.deleted");
			OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		return true;
	}

	FString GetStoredCompileObservationStatus(const UBlueprint& Blueprint)
	{
		return Blueprint.IsUpToDate() || Blueprint.Status == BS_Error ? TEXT("observed")
			: Blueprint.Status == BS_Unknown ? TEXT("unknown") : TEXT("stale");
	}

	bool CompileBlueprint(UBlueprint* Blueprint, FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_compile_target_missing"),
				TEXT("No Blueprint object was supplied for compilation."), TEXT("native_compile"));
			OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = TEXT("asset");
			OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(UBlueprint::StaticClass()->GetPathName());
			return false;
		}
		if (!ValidateBlueprintStructure(*Blueprint, OutError)) return false;
		FCompilerResultsLog CompilerLog;
		CompilerLog.bLogInfoOnly = false;
		FKismetEditorUtilities::CompileBlueprint(
			Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
		{
			OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(
				TEXT("blueprint_compile_failed"), GetAuthoringTargetPath(*Blueprint), CompilerLog, Blueprint);
			return false;
		}
		return true;
	}
}
