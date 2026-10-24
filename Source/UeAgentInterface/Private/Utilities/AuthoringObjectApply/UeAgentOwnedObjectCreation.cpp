#include "Utilities/AuthoringObjectApply/UeAgentOwnedObjectCreation.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "SClassViewer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringObjectApply
{
	namespace
	{
		FDiagnostic CreatedObjectFailure(const UObject* Created, const UClass* Class, const UObject* Owner)
		{
			auto Error = FDiagnostic::Failure(TEXT("owned_native_creation_failed"),
				!Created ? TEXT("The native owned-object factory returned no object.")
					: Created->GetClass() != Class ? TEXT("The native owned-object factory returned an object of a different class.")
					: TEXT("The native owned-object factory returned an object with a different outer."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetStringField(TEXT("class"), Class ? Class->GetPathName() : FString());
			Expected->SetBoolField(TEXT("outer_matches"), true);
			Error.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
			Error.ActualValuePresent = Created != nullptr;
			if (Created)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("object"), Created->GetName());
				Actual->SetStringField(TEXT("class"), Created->GetClass()->GetPathName());
				Actual->SetBoolField(TEXT("outer_matches"), Created->GetOuter() == Owner);
				Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
				if (Created->GetOuter()) Error.ActualEvidence = MakeShared<FJsonValueString>(Created->GetOuter()->GetPathName());
			}
			if (Owner) Error.ExpectedEvidence = MakeShared<FJsonValueString>(Owner->GetPathName());
			return Error;
		}
	}

	struct FNativeOwnedCreationContext::FImpl
	{
		UeAgentAuthoringNativeDefaults::FScratchScope Scratch;
		TStrongObjectPtr<UObject> Owner;
		TStrongObjectPtr<UObject> DetailsOwner;
		TSharedPtr<IPropertyRowGenerator> Rows;
		TSharedPtr<IPropertyHandle> Element;
		TSharedPtr<SClassViewer> Picker;
		TSharedPtr<const UE::UAI::HjsonWorkspace::IImportInitializationDomainProvider> Provider;
		TArray<TStrongObjectPtr<UClass>> LoadedClasses;
		FProperty* Property = nullptr;
		FObjectPropertyBase* ObjectProperty = nullptr;
		void* PropertyValuePtr = nullptr;
		int32 Index = INDEX_NONE;
		bool bScratchOwner = false;
		bool bNativeFactory = false;
		~FImpl()
		{
			if (bScratchOwner && Provider && Owner.IsValid())
				Provider->FinishNativeOwnedCreation(Owner.Get());
		}
		bool InitializeDetailsPicker(UObject* ContextOwner, FDiagnostic& Error)
		{
			auto& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
			auto* ArrayProperty = CastField<FArrayProperty>(Property);
			FPropertyRowGeneratorArgs RowArgs;
			RowArgs.bShouldShowHiddenProperties = true;
			Rows = PropertyEditor.CreatePropertyRowGenerator(RowArgs);
			Rows->SetObjects({ContextOwner});
			TFunction<TSharedPtr<IPropertyHandle>(const TArray<TSharedRef<IDetailTreeNode>>&)> FindHandle;
			FindHandle = [&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes) -> TSharedPtr<IPropertyHandle>
			{
				for (const auto& Node : Nodes)
				{
					auto Handle = Node->CreatePropertyHandle();
					for (auto Current = Handle; Current.IsValid(); Current = Current->GetParentHandle())
					{
						void* Address = nullptr;
						if (Current->IsValidHandle() && Current->GetProperty() == Property
							&& Current->GetValueData(Address) == FPropertyAccess::Success && Address == PropertyValuePtr)
							return Current;
					}
					TArray<TSharedRef<IDetailTreeNode>> Children;
					Node->GetChildren(Children, true);
					auto Found = FindHandle(Children);
					if (Found.IsValid()) return Found;
				}
				return nullptr;
			};
			auto Handle = FindHandle(Rows->GetRootTreeNodes());
			if (!Handle || (ArrayProperty && !Handle->AsArray()))
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_handle_unavailable"),
					!Handle ? TEXT("The Details view has no handle for the owned-object property.")
						: TEXT("The Details handle does not expose the expected array interface."),
					TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = Property ? Property->GetName() : FString();
				Error.ActualValuePresent = Handle.IsValid();
				Error.ExpectedValue = MakeShared<FJsonValueString>(ArrayProperty ? TEXT("array property handle") : TEXT("property handle"));
				return false;
			}
			Element = ArrayProperty ? Handle->AsArray()->GetElement(Index) : Handle;
			if (!Element->IsValidHandle())
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_handle_unavailable"),
					TEXT("The Details handle for the owned-object slot is invalid."), TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = Property ? Property->GetName() : FString();
				Error.ActualValue = MakeShared<FJsonValueNumber>(Index);
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("slot with a valid property handle"));
				return false;
			}
			Picker = StaticCastSharedRef<SClassViewer>(PropertyCustomizationHelpers::MakeEditInlineObjectClassPicker(Element.ToSharedRef(), FOnClassPicked()));
			return true;
		}
		bool InitializeNativePicker(FDiagnostic& Error)
		{
			Picker = Provider->MakeNativeOwnedClassPicker(Owner.Get(), ObjectProperty);
			if (Picker) return true;
			FString Path;
			DetailsOwner.Reset(Provider->CreateNativeOwnedDetailsContext(Owner.Get(), ObjectProperty, Path));
			UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
			UeAgentAuthoringTargetLocator::FResolveResult Result;
			if (!DetailsOwner.IsValid())
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
					TEXT("The native provider returned neither a class picker nor a Details context."),
					TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = ObjectProperty ? ObjectProperty->GetName() : FString();
				Error.ActualValuePresent = false;
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native class picker or Details context"));
				return false;
			}
			if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(DetailsOwner.Get(), Path, Target, Result))
			{ Error = MoveTemp(Result.Diagnostic); return false; }
			Property = Target.Property;
			PropertyValuePtr = Target.ValuePtr;
			Index = CastField<FArrayProperty>(Property) ? 0 : INDEX_NONE;
			return InitializeDetailsPicker(DetailsOwner.Get(), Error);
		}
		void* ValueAddress() const
		{
			if (auto* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Array(ArrayProperty, PropertyValuePtr);
				return Array.IsValidIndex(Index) ? Array.GetRawPtr(Index) : nullptr;
			}
			return PropertyValuePtr;
		}
	};
	FNativeOwnedCreationContext::FNativeOwnedCreationContext() : Impl(MakeUnique<FImpl>()) {}
	FNativeOwnedCreationContext::~FNativeOwnedCreationContext() = default;

	bool FNativeOwnedCreationContext::InitializeScopedMember(UObject* Owner, const FString& RootMember,
		FObjectPropertyBase* Property, bool bProspective, FDiagnostic& Error)
	{
		if (!Owner || !Property || !UeAgentAuthoringReflection::IsOwnedObjectProperty(Property))
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
				!Owner ? TEXT("Owned-object creation requires a native owner.")
					: !Property ? TEXT("Owned-object creation requires a property descriptor.")
					: TEXT("The selected property does not own inline object instances."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = RootMember;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("owner_present"), Owner != nullptr);
			Actual->SetBoolField(TEXT("property_present"), Property != nullptr);
			if (Property) Actual->SetStringField(TEXT("property_type"), Property->GetCPPType());
			Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("owned-object property on a native owner"));
			return false;
		}
		TSharedPtr<const UE::UAI::HjsonWorkspace::FImportInitializationDomainProviderRegistry> Providers;
		UE::UAI::HjsonWorkspace::FWorkspaceError Failure;
		if (!UE::UAI::HjsonWorkspace::TryFreezeProcessImportInitializationDomainProviders(Providers, Failure) ||
			!Providers->ResolveNativeOwnedCreationProvider(Owner->GetClass()->GetPathName(), RootMember, Impl->Provider, Failure))
		{ Error = Failure.GetDiagnostic(); return false; }
		Impl->bNativeFactory = Impl->Provider->UsesNativeOwnedFactory(Property);
		if (!Impl->bNativeFactory)
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_scoped_factory_unavailable"),
				TEXT("The registered provider does not supply a native factory for this scoped member."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = RootMember;
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.SubjectId = Owner->GetClass()->GetPathName();
			return false;
		}
		UObject* ContextOwner = Owner;
		if (bProspective)
		{
			auto* Package = Impl->Scratch.CreateScratchPackage(Error);
			if (!Package) return false;
			ContextOwner = StaticDuplicateObject(Owner, Package);
			if (!ContextOwner)
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
					TEXT("The native owner could not be duplicated for prospective owned-object creation."),
					TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.SubjectId = Owner->GetClass()->GetPathName();
				Error.ActualValuePresent = false;
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("prospective owner copy"));
				return false;
			}
			if (!Impl->Scratch.AdoptObjectFromScratchPackage(ContextOwner, Error)) return false;
			Impl->bScratchOwner = true;
		}
		Impl->Owner.Reset(ContextOwner);
		Impl->ObjectProperty = Property;
		return Impl->InitializeNativePicker(Error);
	}

	bool FNativeOwnedCreationContext::Initialize(UObject* Owner, FProperty* Property, bool bProspective, FDiagnostic& Error, int32 Index)
	{
		return Initialize(Owner, Property ? Property->GetName() : FString(), bProspective, Error, Index);
	}

	bool FNativeOwnedCreationContext::Initialize(UObject* Owner, const FString& PropertyPath, bool bProspective, FDiagnostic& Error, int32 Index)
	{
		if (!Owner || PropertyPath.IsEmpty())
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
				!Owner ? TEXT("Owned-object creation requires a native owner.")
					: TEXT("Owned-object creation requires a nonempty property path."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = PropertyPath;
			Error.ActualValuePresent = false;
			Error.ExpectedValue = MakeShared<FJsonValueString>(!Owner ? TEXT("native owner") : TEXT("nonempty property path"));
			return false;
		}
		// Resolve the real owner before a prospective array element is added.
		UeAgentAuthoringTargetLocator::FResolvedPropertyTarget RealTarget;
		UeAgentAuthoringTargetLocator::FResolveResult RealResolveResult;
		const bool bHasRealTarget = UeAgentAuthoringTargetLocator::ResolvePropertyPath(
			Owner, PropertyPath, RealTarget, RealResolveResult);
		UObject* ContextOwner = Owner;
		if (bProspective)
		{
			auto* Package = Impl->Scratch.CreateScratchPackage(Error);
			if (!Package) return false;
			ContextOwner = StaticDuplicateObject(Owner, Package);
			if (!ContextOwner)
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
					TEXT("The native owner could not be duplicated for prospective owned-object creation."),
					TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.SubjectId = Owner->GetClass()->GetPathName();
				Error.ActualValuePresent = false;
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("prospective owner copy"));
				return false;
			}
			if (!Impl->Scratch.AdoptObjectFromScratchPackage(ContextOwner, Error)) return false;
			Impl->bScratchOwner = true;
		}
		UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
		UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
		if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(ContextOwner, PropertyPath, Target,
			ResolveResult, 8, bProspective))
		{ Error = MoveTemp(ResolveResult.Diagnostic); return false; }
		if (Target.NotificationOwner != ContextOwner || Target.NotificationChain.IsEmpty())
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
				Target.NotificationOwner != ContextOwner ? TEXT("The resolved property has a different notification owner.")
					: TEXT("The resolved property has no notification chain."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = PropertyPath;
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("notification_owner_matches"), Target.NotificationOwner == ContextOwner);
			Actual->SetNumberField(TEXT("notification_chain_length"), Target.NotificationChain.Num());
			Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("matching notification owner and nonempty notification chain"));
			return false;
		}
		auto* Property = Target.Property;
		auto* ArrayProperty = CastField<FArrayProperty>(Property);
		auto* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty ? ArrayProperty->Inner : Property);
		FProperty* RootProperty = Target.NotificationChain[0];
		if (!Property || !ObjectProperty || !UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty))
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
				!Property ? TEXT("The resolved owned-object target has no native property.")
					: !ObjectProperty ? TEXT("The resolved property or array element is not an object property.")
					: TEXT("The resolved object property does not own inline instances."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = PropertyPath;
			Error.ActualValuePresent = Property != nullptr;
			if (Property) Error.ActualValue = MakeShared<FJsonValueString>(Property->GetCPPType());
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("owned-object property"));
			return false;
		}
		TSharedPtr<const UE::UAI::HjsonWorkspace::FImportInitializationDomainProviderRegistry> Providers;
		TSharedPtr<const UE::UAI::HjsonWorkspace::IImportInitializationDomainProvider> Provider;
		UE::UAI::HjsonWorkspace::FWorkspaceError ContextError;
		if (!UE::UAI::HjsonWorkspace::TryFreezeProcessImportInitializationDomainProviders(Providers, ContextError)
			|| !Providers->ResolveNativeOwnedCreationProvider(Owner->GetClass()->GetPathName(), RootProperty->GetName(), Provider, ContextError))
		{ Error = ContextError.GetDiagnostic(); return false; }
		Impl->Provider = Provider;
		Impl->bNativeFactory = Provider->UsesNativeOwnedFactory(ObjectProperty);
		if (Impl->bNativeFactory)
		{
			Impl->Owner.Reset(ContextOwner);
			Impl->ObjectProperty = ObjectProperty;
			return Impl->InitializeNativePicker(Error);
		}
		// Dedicated native editors supply their own picker eligibility above. Details-backed
		// properties use the ordinary reflected editability rules and property handles.
		FString AvailabilityReason;
		if (!UeAgentAuthoringReflection::IsEditableProperty(Property, ContextOwner, &AvailabilityReason))
		{ Error = FDiagnostic::Failure(AvailabilityReason, TEXT("The property cannot be edited in this owner context.")); Error.PropertyPath = PropertyPath; return false; }
		for (const FProperty* Member : Target.NotificationChain)
		{
			if (!UeAgentAuthoringReflection::IsEditableProperty(Member, ContextOwner, &AvailabilityReason))
			{ Error = FDiagnostic::Failure(AvailabilityReason, TEXT("A parent property cannot be edited in this owner context.")); Error.PropertyPath = PropertyPath; return false; }
		}
		if (!bProspective && !bHasRealTarget) { Error = MoveTemp(RealResolveResult.Diagnostic); return false; }
		Impl->Owner.Reset(ContextOwner); Impl->Property = Property; Impl->ObjectProperty = ObjectProperty;
		Impl->PropertyValuePtr = Target.ValuePtr; Impl->Index = Index;
		if (ArrayProperty)
		{
			FScriptArrayHelper Array(ArrayProperty, Target.ValuePtr);
			if (bProspective && Index == INDEX_NONE) Index = Array.Num();
			Impl->Index = Index;
			if (bProspective && !UeAgentAuthoringTargetLocator::EnsureArrayElement(Array, Index, Error)) return false;
			if (!Array.IsValidIndex(Index))
			{
				Error = FDiagnostic::Failure(TEXT("owned_creation_index_unavailable"),
					TEXT("The requested owned-object slot is outside the native array."), TEXT("owned_object_creation"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = PropertyPath;
				Error.ActualValue = MakeShared<FJsonValueNumber>(Index);
				Error.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Array.Num()));
				return false;
			}
		}
		if (bProspective) ObjectProperty->SetObjectPropertyValue(Impl->ValueAddress(), nullptr);
		return Impl->InitializeDetailsPicker(ContextOwner, Error);
	}

	bool FNativeOwnedCreationContext::Allows(UClass* Class) const
	{ return Impl->Picker && Class && Impl->Picker->IsClassAllowed(Class); }

	bool FNativeOwnedCreationContext::Collect(TArray<FOwnedClassCandidate>& Candidates, FDiagnostic& Error) const
	{
		Candidates.Reset();
		if (!Impl->Picker)
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_context_unavailable"),
				TEXT("Owned-class candidates cannot be collected because the native class picker is unavailable."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.ActualValuePresent = false;
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native class picker"));
			return false;
		}
		auto* Base = Impl->ObjectProperty->PropertyClass.Get();
		if (!UE::UAI::ContentDiscovery::TryLoadDerivedBlueprintClasses(*Base, Impl->LoadedClasses, Error)) return false;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			// Match ClassViewer's loaded class tree before applying its picker filter.
			if (It->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden)
				|| FBlueprintSupport::IsClassPlaceholder(*It)
				|| FKismetEditorUtilities::IsClassABlueprintSkeleton(*It)) continue;
			if (!It->IsChildOf(Base) || !Allows(*It)) continue;
			FOwnedClassCandidate Candidate;
			Candidate.Class = *It; Candidate.Selection.Name = It->GetDisplayNameText().ToString();
			Candidate.Selection.Identity = It->GetPathName();
			Candidate.Selection.Select.Add(TEXT("source"), It->ClassGeneratedBy ? It->ClassGeneratedBy->GetName() : FPackageName::GetShortName(It->GetOutermost()->GetName()));
			Candidate.Selection.Select.Add(TEXT("class"), It->GetName());
			Candidate.Selection.Select.Add(TEXT("class_source"), UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(It->GetPathName()));
			Candidates.Add(MoveTemp(Candidate));
		}
		Candidates.Sort([](const auto& A, const auto& B) { return A.Selection.Name == B.Selection.Name ? A.Selection.Identity < B.Selection.Identity : A.Selection.Name < B.Selection.Name; });
		return true;
	}
	UObject* FNativeOwnedCreationContext::Create(UClass* Class, FDiagnostic& Error)
	{
		if (!Allows(Class))
		{
			Error = FDiagnostic::Failure(TEXT("owned_candidate_not_allowed"),
				!Impl->Picker ? TEXT("The native class picker is unavailable.")
					: !Class ? TEXT("No class was supplied for owned-object creation.")
					: TEXT("The requested class is not allowed by the native owned-object picker."),
				TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.ActualValuePresent = Class != nullptr;
			if (Class) Error.ActualValue = MakeShared<FJsonValueString>(Class->GetPathName());
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("class accepted by the native owned-object picker"));
			if (Impl->ObjectProperty) Error.PropertyPath = Impl->ObjectProperty->GetName();
			return nullptr;
		}
		if (Impl->bNativeFactory)
		{
			auto* Created = Impl->Provider->CreateNativeOwnedObject(Impl->Owner.Get(), Class);
			if (!Created || Created->GetClass() != Class || Created->GetOuter() != Impl->Owner.Get())
				Error = CreatedObjectFailure(Created, Class, Impl->Owner.Get());
			return Created;
		}
		void* Address = Impl->ValueAddress();
		if (!Address)
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_index_unavailable"),
				TEXT("The requested owned-object slot has no native value storage."), TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.ActualValue = MakeShared<FJsonValueNumber>(Impl->Index);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("slot with native value storage"));
			if (Impl->Property) Error.PropertyPath = Impl->Property->GetName();
			return nullptr;
		}
		if (auto* Existing = Impl->ObjectProperty->GetObjectPropertyValue(Address))
		{
			Error = FDiagnostic::Failure(TEXT("owned_creation_slot_not_empty"),
				TEXT("The owned-object slot already contains an instance."), TEXT("owned_object_creation"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Impl->ObjectProperty->GetName();
			Error.ActualValue = MakeShared<FJsonValueString>(Existing->GetName());
			Error.ExpectedValue = MakeShared<FJsonValueNull>();
			return nullptr;
		}
		PropertyCustomizationHelpers::CreateNewInstanceOfEditInlineObjectClass(Impl->Element.ToSharedRef(), Class);
		auto* Created = Impl->ObjectProperty->GetObjectPropertyValue(Impl->ValueAddress());
		if (!Created || Created->GetClass() != Class || Created->GetOuter() != Impl->Owner.Get())
			Error = CreatedObjectFailure(Created, Class, Impl->Owner.Get());
		return Created;
	}
}
