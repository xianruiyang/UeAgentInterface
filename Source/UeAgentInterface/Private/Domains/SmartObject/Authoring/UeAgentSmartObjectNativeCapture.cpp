#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "UObject/UnrealType.h"

namespace UeAgentSmartObjectAuthoring
{
	namespace
	{
		template<typename T>
		const T* StructValue(const USmartObjectDefinition& Definition, const TCHAR* Name,
			const UScriptStruct* Type)
		{
			const auto* Property = FindFProperty<FStructProperty>(Definition.GetClass(), Name);
			return Property && Property->Struct == Type
				? Property->ContainerPtrToValuePtr<T>(&Definition) : nullptr;
		}
	}

	bool CaptureDefinition(const USmartObjectDefinition& Definition,
		FDefinitionCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = {};
		OutError.Reset();
		FDefinitionCapture Capture;
		Capture.Definition = &Definition;
		Capture.Slots = Definition.GetSlots();
		const auto* RootId = StructValue<FGuid>(Definition, TEXT("RootID"), TBaseStructure<FGuid>::Get());
		const auto* ParametersId = StructValue<FGuid>(Definition, TEXT("ParametersID"), TBaseStructure<FGuid>::Get());
		Capture.Parameters = StructValue<FInstancedPropertyBag>(Definition, TEXT("Parameters"), FInstancedPropertyBag::StaticStruct());
		Capture.Bindings = StructValue<FSmartObjectBindingCollection>(Definition, TEXT("BindingCollection"), FSmartObjectBindingCollection::StaticStruct());
		const auto* Data = FindFProperty<FArrayProperty>(Definition.GetClass(), TEXT("DefinitionData"));
		const auto* DataInner = Data ? CastField<FStructProperty>(Data->Inner) : nullptr;
		const auto* Behaviors = FindFProperty<FArrayProperty>(Definition.GetClass(), TEXT("DefaultBehaviorDefinitions"));
		const auto* BehaviorInner = Behaviors ? CastField<FObjectPropertyBase>(Behaviors->Inner) : nullptr;
		const auto* BindableStructs = FindFProperty<FArrayProperty>(FSmartObjectBindingCollection::StaticStruct(), TEXT("BindableStructs"));
		const auto* BindableInner = BindableStructs ? CastField<FStructProperty>(BindableStructs->Inner) : nullptr;
		const auto FailLayout = [&](const TCHAR* Field, const TCHAR* Expected, const FProperty* Actual)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("smart_object_native_layout_invalid"),
				FString::Printf(TEXT("Native Smart Object field '%s' is missing or has an incompatible type."), Field), TEXT("state_capture"));
			OutError.TargetPath = Definition.GetPathName();
			OutError.PropertyPath = Field;
			OutError.SourceKind = TEXT("native_result");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			OutError.ActualValue = MakeShared<FJsonValueString>(Actual ? Actual->GetCPPType() : TEXT("missing field"));
			return false;
		};
		if (!RootId) return FailLayout(TEXT("RootID"), TEXT("FGuid"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("RootID")));
		if (!ParametersId) return FailLayout(TEXT("ParametersID"), TEXT("FGuid"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("ParametersID")));
		if (!Capture.Parameters) return FailLayout(TEXT("Parameters"), TEXT("FInstancedPropertyBag"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("Parameters")));
		if (!Capture.Bindings) return FailLayout(TEXT("BindingCollection"), TEXT("FSmartObjectBindingCollection"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("BindingCollection")));
		if (!DataInner || DataInner->Struct != FSmartObjectDefinitionDataProxy::StaticStruct())
			return FailLayout(TEXT("DefinitionData"), TEXT("TArray<FSmartObjectDefinitionDataProxy>"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("DefinitionData")));
		if (!BehaviorInner || BehaviorInner->PropertyClass != USmartObjectBehaviorDefinition::StaticClass())
			return FailLayout(TEXT("DefaultBehaviorDefinitions"), TEXT("array of USmartObjectBehaviorDefinition object references"), FindFProperty<FProperty>(Definition.GetClass(), TEXT("DefaultBehaviorDefinitions")));
		if (!BindableInner || BindableInner->Struct != FSmartObjectDefinitionBindableStructDescriptor::StaticStruct())
			return FailLayout(TEXT("BindingCollection.BindableStructs"), TEXT("TArray<FSmartObjectDefinitionBindableStructDescriptor>"), FindFProperty<FProperty>(FSmartObjectBindingCollection::StaticStruct(), TEXT("BindableStructs")));
		Capture.RootId = *RootId;
		Capture.ParametersId = *ParametersId;
		Capture.DefinitionData = *Data->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionDataProxy>>(&Definition);
		Capture.DefaultBehaviors = *Behaviors->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(&Definition);
		Capture.BindableStructs = *BindableStructs->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionBindableStructDescriptor>>(Capture.Bindings);
		OutCapture = MoveTemp(Capture);
		return true;
	}
}
