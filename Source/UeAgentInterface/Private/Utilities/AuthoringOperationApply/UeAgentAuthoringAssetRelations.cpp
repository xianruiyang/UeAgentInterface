#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRelations.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringAssetRelations
{
namespace
{
FString Path(const UObject* Object) { return Object ? Object->GetPathName() : FString(); }
UObject* Resolve(const FString& ObjectPath) { return ObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *ObjectPath); }
UObject* Parent(UObject& Object, FName Name)
{
	const auto* Property = FindFProperty<FObjectPropertyBase>(Object.GetClass(), Name);
	return Property ? Property->GetObjectPropertyValue_InContainer(&Object) : nullptr;
}
TArray<FString> Children(UObject& Object, FName Name)
{
	const auto* Property = FindFProperty<FArrayProperty>(Object.GetClass(), Name);
	const auto* Inner = CastFieldChecked<FObjectPropertyBase>(Property->Inner);
	FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(&Object));
	TArray<FString> Result;
	for (int32 Index = 0; Index < Array.Num(); ++Index) Result.Add(Path(Inner->GetObjectPropertyValue(Array.GetRawPtr(Index))));
	return Result;
}
void SetChildren(UObject& Object, FName Name, const TArray<FString>& Paths)
{
	const auto* Property = FindFProperty<FArrayProperty>(Object.GetClass(), Name);
	const auto* Inner = CastFieldChecked<FObjectPropertyBase>(Property->Inner);
	FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(&Object));
	Array.Resize(Paths.Num());
	for (int32 Index = 0; Index < Paths.Num(); ++Index) Inner->SetObjectPropertyValue(Array.GetRawPtr(Index), Resolve(Paths[Index]));
}
void SetParent(UObject& Object, FName Name, const FString& ObjectPath)
{
	if (const auto* Property = FindFProperty<FObjectPropertyBase>(Object.GetClass(), Name))
		Property->SetObjectPropertyValue_InContainer(&Object, Resolve(ObjectPath));
}
}

struct FPlan::FImpl
{
	struct FNode
	{
		FString ObjectPath, BeforeParent, AfterParent;
		TArray<FString> BeforeChildren, AfterChildren;
		TWeakObjectPtr<UObject> Actual;
		TStrongObjectPtr<UObject> Expected;
		bool bHasParent = false;
	};
	TArray<FNode> Nodes;
	FString RootPath;
	FName ParentProperty, ChildrenProperty;
	TArray<FString> DesiredChildren;
	TFunction<void(UObject&, UObject*)> SetNativeParent;
	bool bApplied = false;
	bool Add(UObject* Object, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Object || Nodes.ContainsByPredicate([&](const auto& Node) { return Node.ObjectPath == Path(Object); })) return true;
		const auto* Property = FindFProperty<FProperty>(Object->GetClass(), ChildrenProperty);
		const auto* Array = CastField<FArrayProperty>(Property);
		if (!Array || !CastField<FObjectPropertyBase>(Array->Inner)) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_relation_property_invalid"),
				!Array ? TEXT("The asset children relation is not an array property.") : TEXT("The asset children array does not contain object references."), TEXT("asset_relation"));
			OutError.TargetPath = Path(Object); OutError.PropertyPath = ChildrenProperty.ToString();
			OutError.SourceKind = TEXT("native_result");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array of object references"));
			if (Property) OutError.ActualValue = MakeShared<FJsonValueString>(Property->GetCPPType());
			else OutError.ActualValuePresent = false;
			return false;
		}
		auto& Node = Nodes.AddDefaulted_GetRef();
		Node.ObjectPath = Path(Object);
		Node.BeforeParent = Node.AfterParent = Path(Parent(*Object, ParentProperty));
		Node.BeforeChildren = Node.AfterChildren = Children(*Object, ChildrenProperty);
		Node.Actual = Object;
		Node.bHasParent = FindFProperty<FObjectPropertyBase>(Object->GetClass(), ParentProperty) != nullptr;
		return true;
	}
	FNode& Find(const FString& ObjectPath)
	{ return *Nodes.FindByPredicate([&](const auto& Node) { return Node.ObjectPath == ObjectPath; }); }
};

FPlan::FPlan() : Impl(MakeUnique<FImpl>()) {}
FPlan::~FPlan() = default;

bool FPlan::Prepare(UObject& Root, const TArray<UObject*>& Desired, FName ParentProperty, FName ChildrenProperty,
	TFunction<void(UObject&, UObject*)> SetNativeParent, UE::UAI::Contract::FDiagnostic& OutError)
{
	Impl->RootPath = Root.GetPathName();
	Impl->ParentProperty = ParentProperty; Impl->ChildrenProperty = ChildrenProperty;
	Impl->SetNativeParent = MoveTemp(SetNativeParent);
	if (!Impl->Add(&Root, OutError)) return false;
	for (UObject* Child : Desired)
	{
		Impl->DesiredChildren.Add(Path(Child));
		if (Child && Parent(*Child, ParentProperty) != &Root)
			if (!Impl->Add(Child, OutError) || !Impl->Add(Parent(*Child, ParentProperty), OutError)) return false;
	}
	for (const auto& ChildPath : Children(Root, ChildrenProperty))
		if (UObject* Child = Resolve(ChildPath); Child && !Desired.Contains(Child) && Parent(*Child, ParentProperty) == &Root)
			if (!Impl->Add(Child, OutError)) return false;
	Impl->Find(Impl->RootPath).AfterChildren = Impl->DesiredChildren;
	for (auto& Node : Impl->Nodes)
	{
		if (Node.ObjectPath == Impl->RootPath) continue;
		if (Desired.Contains(Node.Actual.Get()))
		{
			if (!Node.BeforeParent.IsEmpty() && Node.BeforeParent != Impl->RootPath)
				Impl->Find(Node.BeforeParent).AfterChildren.Remove(Node.ObjectPath);
			Node.AfterParent = Impl->RootPath;
		}
		else if (Node.BeforeParent == Impl->RootPath && !Impl->DesiredChildren.Contains(Node.ObjectPath)) Node.AfterParent.Reset();
	}
	for (auto& Node : Impl->Nodes)
	{
		Node.Expected.Reset(NewObject<UObject>(GetTransientPackage(), Node.Actual->GetClass(), NAME_None, RF_Transient));
		SetParent(*Node.Expected.Get(), ParentProperty, Node.AfterParent);
		SetChildren(*Node.Expected.Get(), ChildrenProperty, Node.AfterChildren);
	}
	return true;
}
void FPlan::ApplyPreview(UObject& Root) const { SetChildren(Root, Impl->ChildrenProperty, Impl->DesiredChildren); }
bool FPlan::Apply(UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!Rebind(OutError)) return false;
	Impl->bApplied = true;
	for (auto& Node : Impl->Nodes) Node.Actual->Modify();
	for (auto& Node : Impl->Nodes)
		if (Node.ObjectPath != Impl->RootPath && Node.BeforeParent != Node.AfterParent)
			Impl->SetNativeParent(*Node.Actual.Get(), Resolve(Node.AfterParent));
	SetChildren(*Resolve(Impl->RootPath), Impl->ChildrenProperty, Impl->DesiredChildren);
	return true;
}
bool FPlan::Rollback(UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!Impl->bApplied) return true;
	if (!Rebind(OutError)) return false;
	for (auto& Node : Impl->Nodes)
	{
		SetParent(*Node.Actual.Get(), Impl->ParentProperty, Node.BeforeParent);
		SetChildren(*Node.Actual.Get(), Impl->ChildrenProperty, Node.BeforeChildren);
	}
	Impl->bApplied = false;
	return true;
}
void FPlan::Release()
{
	for (auto& Node : Impl->Nodes)
	{
		Node.Actual.Reset();
		if (Node.Expected.IsValid()) Node.Expected->MarkAsGarbage();
		Node.Expected.Reset();
	}
}
bool FPlan::Rebind(UE::UAI::Contract::FDiagnostic& OutError)
{
	for (auto& Node : Impl->Nodes)
	{
		Node.Actual = Resolve(Node.ObjectPath);
		if (!Node.Actual.IsValid()) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_relation_target_missing"), TEXT("The asset relation target could not be resolved after replacement."), TEXT("asset_relation_rebind"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Node.ObjectPath; OutError.ActualValuePresent = false; return false;
		}
	}
	return true;
}
void FPlan::CollectRollbackObjects(TArray<UObject*>& OutObjects) const
{ for (const auto& Node : Impl->Nodes) if (Node.Actual.IsValid()) OutObjects.AddUnique(Node.Actual.Get()); }
bool FPlan::AppendPackagePlans(TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& OutPlans,
	UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const
{
	using namespace UeAgentAuthoringOperationApply;
	for (const auto& Node : Impl->Nodes)
	{
		const FString Package = FPackageName::ObjectPathToPackageName(Node.ObjectPath);
		if (OutPlans.ContainsByPredicate([&](const auto& Plan) { return Plan.PackageName == Package; })) continue;
		if (!BuildPackageDurablePlan(Package, Package, Node.Actual.Get(), EPackageDurableIntent::Save, OutPlans, OutFailure)) return false;
	}
	for (auto& Plan : OutPlans) Plan.ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
	return true;
}
bool FPlan::AppendReadRequests(bool bExpected, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
	UeAgentAuthoringOperationApply::FOperationFailure& OutFailure) const
{
	for (const auto& Node : Impl->Nodes)
	{
		UObject* Object = bExpected ? Node.Expected.Get() : Resolve(Node.ObjectPath);
		if (!Object)
		{
			OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("asset_relation_snapshot_missing"),
				bExpected ? TEXT("The expected asset relation snapshot is unavailable.")
					: TEXT("The asset relation target could not be resolved for readback."),
				TEXT("asset_relation_readback"));
			OutFailure.Diagnostic.SourceKind = TEXT("native_result");
			OutFailure.Diagnostic.TargetPath = Node.ObjectPath;
			OutFailure.Diagnostic.ActualValuePresent = false;
			return false;
		}
		for (FName Property : {Impl->ParentProperty, Impl->ChildrenProperty})
		{
			if (Property == Impl->ParentProperty && !Node.bHasParent) continue;
			UeAgentAuthoringState::FReadRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult Result;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentAuthoringState::BuildPropertyReadRequest(Object, Property.ToString(), TEXT("core.property"), Request, Result, Node.ObjectPath))
			{
				OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("asset_relation_snapshot_invalid"), Result.Diagnostic, TEXT("asset_relation_readback"));
				return false;
			}
			if (!UeAgentAuthoringTargetLocator::BuildPropertyIdentity(Node.ObjectPath, Property.ToString(), Node.ObjectPath, Identity, Error))
			{
				OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("asset_relation_snapshot_invalid"), Error, TEXT("asset_relation_readback"));
				return false;
			}
			Request.SnapshotIdentityOverride = Identity;
			OutRequests.Add(MoveTemp(Request));
		}
	}
	return true;
}
}
