#include "Utilities/AuthoringObjectApply/UeAgentTemplatePropertyPropagation.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentEnvTraceDataValue.h"

namespace UeAgentTemplatePropertyPropagation
{
namespace {
thread_local FScope* ActiveScope = nullptr;

void RefreshInstances(const TSet<UObject*>& Instances)
{
    for (UObject* Instance : Instances)
    {
        // Match ComponentEditorUtils default propagation, after all related
        // fields are installed. Registration rebuilds render/physics state.
        auto* Component = Cast<UActorComponent>(Instance);
        if (Component && Component->IsRegistered() && Component->AllowReregistration())
            Component->ReregisterComponent();
        else if (auto* SceneComponent = Cast<USceneComponent>(Instance))
            SceneComponent->UpdateComponentToWorld();
    }
}
}

void FSnapshot::Capture(UObject& Object, FProperty& Property)
{
    const FString Path = Object.GetPathName();
    if (Values.ContainsByPredicate([&](const FValue& Value) {
        return Value.ObjectPath == Path && Value.PropertyName == Property.GetFName();
    })) return;
    FValue& Value = Values.AddDefaulted_GetRef();
    Value.ObjectPath = Path;
    Value.PropertyName = Property.GetFName();
    Value.Type = Property.GetCPPType();
    for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
        Property.ExportText_Direct(Value.Elements.AddDefaulted_GetRef(),
            Property.ContainerPtrToValuePtr<void>(&Object, Index), nullptr, &Object, PPF_None);
    UPackage* Package = Object.GetPackage();
    PackageDirtyStates.FindOrAdd(Package->GetName(), Package->IsDirty());
}

bool FSnapshot::Restore(UE::UAI::Contract::FDiagnostic& OutError)
{
    TSet<UObject*> Restored;
    for (int32 Index = Values.Num() - 1; Index >= 0; --Index)
    {
        const FValue& Value = Values[Index];
        UObject* Object = FindObject<UObject>(nullptr, *Value.ObjectPath);
        FProperty* Property = Object ? FindFProperty<FProperty>(Object->GetClass(), Value.PropertyName) : nullptr;
        if (!Property || Property->GetCPPType() != Value.Type || Property->ArrayDim != Value.Elements.Num())
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("template_instance_restore_target_changed"),
                !Object ? TEXT("The instance to restore no longer exists.") : !Property ? TEXT("The property to restore no longer exists.")
                    : Property->GetCPPType() != Value.Type ? TEXT("The property type changed after the snapshot.") : TEXT("The property array size changed after the snapshot."), TEXT("template_restore"));
            OutError.TargetPath = Value.ObjectPath;
            OutError.PropertyPath = Value.PropertyName.ToString();
            if (Property) {
                const auto Expected = MakeShared<FJsonObject>();
                Expected->SetStringField(TEXT("type"), Value.Type); Expected->SetNumberField(TEXT("array_size"), Value.Elements.Num());
                const auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("type"), Property->GetCPPType()); Actual->SetNumberField(TEXT("array_size"), Property->ArrayDim);
                OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected); OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
            } else OutError.ActualValuePresent = false;
            return false;
        }
        UE::UAI::Contract::FDiagnostic Error;
        auto Storage = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CreateInitialized(Property, Error);
        if (!Storage)
        {
            OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("template_instance_restore_value_failed"), Error,
                TEXT("template_restore"), TEXT("Storage for the instance property snapshot could not be initialized."));
            OutError.TargetPath = Value.ObjectPath;
            OutError.PropertyPath = Value.PropertyName.ToString();
            return false;
        }
        for (int32 Element = 0; Element < Value.Elements.Num(); ++Element)
            if (!Property->ImportText_Direct(*Value.Elements[Element],
                static_cast<uint8*>(Storage->GetMutableValuePtr()) + Element * Property->ElementSize, Object, PPF_None))
            {
                OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("template_instance_restore_value_failed"), TEXT("The native property parser rejected a saved snapshot element."), TEXT("template_restore"));
                OutError.TargetPath = Value.ObjectPath;
                OutError.PropertyPath = FString::Printf(TEXT("%s[%d]"), *Value.PropertyName.ToString(), Element);
                OutError.ExpectedValue = MakeShared<FJsonValueString>(Value.Type);
                OutError.ActualValue = MakeShared<FJsonValueString>(Value.Elements[Element]);
                return false;
            }
        Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Object), Storage->GetValuePtr());
        Restored.Add(Object);
    }
    RefreshInstances(Restored);
    for (const auto& Pair : PackageDirtyStates)
        if (UPackage* Package = FindPackage(nullptr, *Pair.Key)) Package->SetDirtyFlag(Pair.Value);
    Values.Reset();
    PackageDirtyStates.Reset();
    return true;
}

FScope::FScope() : Previous(ActiveScope) { ActiveScope = this; }
FScope::~FScope() { check(ActiveScope == this); ActiveScope = Previous; }
bool FScope::Restore(UE::UAI::Contract::FDiagnostic& OutError) { return Before.Restore(OutError); }
void FScope::Capture(UObject& Object, FProperty& Property)
{
    Before.Capture(Object, Property);
    if (Previous) Previous->Capture(Object, Property);
}

void Gather(UObject& Template, FProperty& Member, const FString& PropertyPath, FSnapshot& Before,
    TArray<TWeakObjectPtr<UObject>>& OutInstances)
{
    UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Source;
    UeAgentAuthoringTargetLocator::FResolveResult Resolution;
    if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(&Template, PropertyPath, Source, Resolution)) return;
    TArray<UObject*> Candidates;
    if (PropertyPath == Member.GetName())
        PropertyAccessUtil::GetArchetypeInstancesInheritingPropertyValue(&Member, &Template, Candidates);
    else
    {
        // The object-level engine helper compares the complete member. For a
        // nested edit compare the addressed value along the same archetype chain,
        // so an override in a sibling field does not suppress this field's update.
        Template.GetArchetypeInstances(Candidates);
    }
    TArray<UObject*> Instances;
    UeAgentPropertyValueCodec::FOptions StoredValues;
    StoredValues.bCompareStoredValues = true;
    for (UObject* Candidate : Candidates)
    {
        UObject* Current = Candidate;
        while (Current && Current != &Template)
        {
            UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Field;
            if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(Current, PropertyPath, Field, Resolution)
                || Field.NotificationOwner != Current
                || !PropertyAccessUtil::IsCompletePropertyIdentical(Source.Property, Source.ValuePtr, Field.Property, Field.ValuePtr)
                // Native inheritance remains necessary. The shared value codec
                // additionally preserves authored case-only string overrides.
                || !UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Source.Property,
                    Source.ValuePtr, Field.ValuePtr, &Template, StoredValues)) break;
            Current = Current->GetArchetype();
        }
        if (Current == &Template) Instances.Add(Candidate);
    }
    for (UObject* Instance : Instances)
    {
        Before.Capture(*Instance, Member);
        if (ActiveScope) ActiveScope->Capture(*Instance, Member);
        OutInstances.Add(Instance);
    }
}

bool Apply(UObject& Template, const TArray<FInheritedField>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
{
    TSet<UObject*> Updated;
    for (const FInheritedField& Inherited : Fields)
    {
        const FString& PropertyPath = Inherited.PropertyPath;
        UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Source;
        UeAgentAuthoringTargetLocator::FResolveResult Resolution;
        if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(&Template, PropertyPath, Source, Resolution)) {
            OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("template_property_changed_during_apply"), Resolution.Diagnostic,
                TEXT("template_propagation"), TEXT("The template property could not be resolved during propagation."));
            OutError.TargetPath = Template.GetPathName(); OutError.PropertyPath = PropertyPath; return false;
        }
        for (int32 InstanceIndex = 0; InstanceIndex < Inherited.Instances.Num(); ++InstanceIndex)
        {
            UObject* Instance = Inherited.Instances[InstanceIndex].Get();
            if (!Instance) {
                OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("template_instance_changed_during_apply"), TEXT("An inherited instance no longer exists during template propagation."), TEXT("template_propagation"));
                OutError.TargetPath = Template.GetPathName(); OutError.PropertyPath = PropertyPath;
                OutError.ActualValue = MakeShared<FJsonValueNumber>(InstanceIndex); return false;
            }
            UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Field;
            if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(Instance, PropertyPath, Field, Resolution)) {
                OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("template_instance_property_changed_during_apply"), Resolution.Diagnostic,
                    TEXT("template_propagation"), TEXT("The inherited instance property could not be resolved during propagation."));
                OutError.TargetPath = Instance->GetPathName(); OutError.PropertyPath = PropertyPath; return false;
            }
            Instance->Modify();
            if (auto* Component = Cast<UActorComponent>(Instance))
                if (AActor* Actor = Component->GetOwner()) Actor->Modify();
            // This is inheritance from an already validated template edit, not a
            // direct edit of the instance. Match UE's archetype propagation branch:
            // copy compatible values without reapplying instance edit permissions
            // (EditDefaultsOnly must still propagate). Gather preserved overrides.
            if (!PropertyAccessUtil::CopyCompletePropertyValue(
                Source.Property, Source.ValuePtr, Field.Property, Field.ValuePtr))
            {
                OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("template_instance_property_apply_failed"), TEXT("The native property copier rejected the template value for the inherited instance."), TEXT("template_propagation"));
                OutError.TargetPath = Instance->GetPathName(); OutError.PropertyPath = PropertyPath;
                OutError.CandidateName = Template.GetPathName();
                OutError.ExpectedValue = MakeShared<FJsonValueString>(Field.Property->GetCPPType());
                OutError.ActualValue = MakeShared<FJsonValueString>(Source.Property->GetCPPType());
                return false;
            }
            UeAgentEnvTraceDataValue::FinalizeLinkedMember(Field.Property, Field.ValuePtr);
            Updated.Add(Instance);
            Instance->MarkPackageDirty();
        }
    }
    RefreshInstances(Updated);
    return true;
}
}
