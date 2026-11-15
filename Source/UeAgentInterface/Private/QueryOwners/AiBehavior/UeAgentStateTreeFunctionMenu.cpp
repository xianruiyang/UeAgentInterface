#include "QueryOwners/AiBehavior/UeAgentStateTreeFunctionMenu.h"

#include "DetailsViewArgs.h"
#include "IDetailPropertyChildrenCustomizationHandler.h"
#include "IDetailPropertyExtensionHandler.h"
#include "IDetailsView.h"
#include "IPropertyAccessEditor.h"
#include "Modules/ModuleManager.h"
#include "Preferences/PersonaOptions.h"
#include "PropertyBindingExtension.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StateTreeAnyEnum.h"
#include "StateTreeDelegate.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreePropertyRefHelpers.h"
#include "StateTreePropertyRef.h"
#include "StateTreeState.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::QueryOwners::AiBehavior
{
namespace
{
	// Observe the same rows that StateTree customizes. Bound function inputs are only
	// created by its children handler on the receiving field's actual Details view.
	class FStateTreeMenuRows final : public IDetailPropertyChildrenCustomizationHandler
	{
	public:
		explicit FStateTreeMenuRows(TSharedRef<IDetailPropertyChildrenCustomizationHandler> InNative) : Native(InNative) {}
		bool ShouldCustomizeChildren(TSharedRef<IPropertyHandle> Handle) override
		{
			Handles.Add(Handle);
			return Native->ShouldCustomizeChildren(Handle);
		}
		void CustomizeChildren(IDetailChildrenBuilder& Children, TSharedPtr<IPropertyHandle> Handle) override
		{
			Native->CustomizeChildren(Children, Handle);
		}
		TArray<TSharedRef<IPropertyHandle>> Handles;
	private:
		TSharedRef<IDetailPropertyChildrenCustomizationHandler> Native;
	};

	const UScriptStruct* StructRefBase(const FProperty& Property)
	{
		const FString& Name = Property.GetMetaData(TEXT("BaseStruct"));
		if (Name.IsEmpty()) return nullptr;
		if (auto* Loaded = UClass::TryFindTypeSlow<UScriptStruct>(Name)) return Loaded;
		return LoadObject<UScriptStruct>(nullptr, *Name);
	}

	// StateTree's field-dependent compatibility precedes ordinary PropertyBinding promotion.
	bool Compatible(const FProperty& Source, const FProperty& Target, const void* SourceValue, const void* TargetValue)
	{
		const auto* Struct = CastField<FStructProperty>(&Target);
		if (Struct && Struct->Struct == FStateTreeAnyEnum::StaticStruct())
		{
			const UEnum* Enum = nullptr;
			if (const auto* Byte = CastField<FByteProperty>(&Source)) Enum = Byte->GetIntPropertyEnum();
			if (const auto* EnumField = CastField<FEnumProperty>(&Source)) Enum = EnumField->GetEnum();
			if (Enum) return Target.HasMetaData(TEXT("AllowAnyBinding")) || static_cast<const FStateTreeAnyEnum*>(TargetValue)->Enum == Enum;
		}
		if (Struct && Struct->Struct == FStateTreeStructRef::StaticStruct())
		{
			if (const auto* From = CastField<FStructProperty>(&Source))
			{
				const auto* Base = StructRefBase(Target);
				const auto* Actual = From->Struct == FStateTreeStructRef::StaticStruct() ? StructRefBase(Source) : From->Struct.Get();
				return Base && Actual && Actual->IsChildOf(Base);
			}
		}
		if (Struct && StateTree::PropertyRefHelpers::IsPropertyRef(Target))
		{
			const auto* Ref = CastField<FStructProperty>(&Source);
			if (!SourceValue && Ref && Ref->Struct == FStateTreeBlueprintPropertyRef::StaticStruct()) return false;
			return StateTree::PropertyRefHelpers::IsPropertyRefCompatibleWithProperty(Target, Source, TargetValue, SourceValue);
		}
		if (Struct && (Struct->Struct == FStateTreeDelegateListener::StaticStruct() || Struct->Struct == FStateTreeTransitionDelegateListener::StaticStruct()))
		{
			const auto* From = CastField<FStructProperty>(&Source);
			return From && From->Struct == FStateTreeDelegateDispatcher::StaticStruct();
		}
		return PropertyBinding::GetPropertyCompatibility(&Source, &Target) != PropertyBinding::EPropertyCompatibility::Incompatible;
	}

	void Outputs(const FStateTreeBindableStructDesc& Desc, const FPropertyBindingDataView View, const FProperty& Target,
		const void* TargetValue, bool bAllowShortcut, TFunctionRef<void(const FString&)> Visit)
	{
		TArray<FBindingChainElement> Chain = {FBindingChainElement(nullptr, 0)};
		auto TryPath = [&](const FString& Text, const FProperty& Leaf, bool bEmit = true)
		{
			FPropertyBindingPath Path(Desc.ID);
			TArray<FPropertyBindingPathIndirection> Steps;
			if (!Path.FromString(Text) || !Path.ResolveIndirectionsWithValue(View, Steps) || Steps.IsEmpty()) return false;
			const auto& Last = Steps.Last();
			if (!Compatible(Leaf, Target, View.GetMemory() ? Last.GetPropertyAddress() : nullptr, TargetValue)) return false;
			if (bEmit) Visit(Text);
			return true;
		};
		if (const FProperty* Single = StateTree::GetStructSingleOutputProperty(*Desc.Struct))
			if (TryPath(Single->GetName(), *Single, bAllowShortcut)) return;

		// The property binding widget does not offer array elements, functions or non-output roots in this placement.
		TSet<const UStruct*> Parents;
		const int32 MaxDepth = FPropertyBindingWidgetArgs().MaxDepth;
		auto Walk = [&](auto&& Self, const UStruct* Type, const FString& Prefix) -> void
		{
			if (!Type || Parents.Contains(Type) || Chain.Num() > MaxDepth) return;
			Parents.Add(Type);
			for (TFieldIterator<FProperty> It(Type, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Field = *It;
				if ((Chain.Num() == 1 && StateTree::GetUsageFromMetaData(Field) != EStateTreePropertyUsage::Output)
					|| !PropertyBinding::IsPropertyBindable(*Field) || Field->HasAnyPropertyFlags(CPF_AdvancedDisplay | CPF_EditorOnly)
					|| !GetDefault<UPersonaOptions>()->IsAllowedProperty(Field)) continue;
				if (StateTree::PropertyRefHelpers::IsPropertyRef(Target)
					&& !StateTree::PropertyRefHelpers::IsPropertyAccessibleForPropertyRef(*Field, Chain, Desc)) continue;
				const auto* To = CastField<FStructProperty>(&Target);
				if (To && (To->Struct == FStateTreeDelegateListener::StaticStruct() || To->Struct == FStateTreeTransitionDelegateListener::StaticStruct()))
				{
					const auto* From = CastField<FStructProperty>(Field);
					if (!From || From->Struct != FStateTreeDelegateDispatcher::StaticStruct()) continue;
				}
				const FString Text = Prefix.IsEmpty() ? Field->GetName() : Prefix + TEXT(".") + Field->GetName();
				TryPath(Text, *Field);
				Chain.Emplace(Field);
				if (const auto* Nested = CastField<FStructProperty>(Field)) Self(Self, Nested->Struct, Text);
				else if (const auto* Object = CastField<FObjectPropertyBase>(Field)) Self(Self, Object->PropertyClass, Text);
				Chain.Pop();
			}
			Parents.Remove(Type);
		};
		Walk(Walk, Desc.Struct, FString());
	}
}

bool VisitStateTreeFunctionMenu(UStateTreeEditorData& Data, FGuid Owner, const FString& TargetPath,
	TFunctionRef<void(const UScriptStruct&, const FString&)> Visit, FString& Failure)
{
	const bool bScoped = Owner.IsValid();
	FPropertyBindingPath Requested(Owner);
	if (bScoped)
	{
		FPropertyBindingDataView View;
		if (!Requested.FromString(TargetPath) || !Data.GetBindingDataViewByID(Owner, View)
			|| !Requested.UpdateSegmentsFromValue(View, &Failure))
		{
			if (Failure.IsEmpty()) Failure = TEXT("The receiving field does not resolve on this StateTree item.");
			return false;
		}
	}
	auto& Editor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	const auto Details = Editor.CreateDetailView(FDetailsViewArgs());
	FStateTreeEditorModule::SetDetailPropertyHandlers(*Details);
	const auto Extension = Details->GetExtensionHandler();
	const auto Rows = MakeShared<FStateTreeMenuRows>(Details->GetChildrenCustomizationHandler().ToSharedRef());
	Details->SetChildrenCustomizationHandler(Rows);
	TArray<UObject*> Owners;
	if (bScoped)
	{
		auto* State = Data.GetStateByStructID(Owner);
		Owners.Add(State ? static_cast<UObject*>(const_cast<UStateTreeState*>(State)) : static_cast<UObject*>(&Data));
	}
	else
	{
		Owners.Add(&Data);
		Data.VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) { Owners.Add(&State); return EStateTreeVisitor::Continue; });
	}
	bool Found = false;
	for (UObject* Container : Owners)
	{
		Rows->Handles.Reset();
		Details->SetObject(Container, true);
		for (const auto& Handle : Rows->Handles)
		{
			if (!Handle->IsValidHandle() || !Handle->GetProperty()) continue;
			FPropertyBindingPath Path;
			PropertyBinding::MakeStructPropertyPathFromPropertyHandle(Handle, Path, Data.GetFallbackStructID());
			if (!Path.GetStructID().IsValid() || Path.NumSegments() == 0
				|| (bScoped && (Path.GetStructID() != Owner || Path.ToString() != Requested.ToString()))) continue;
			FPropertyBindingDataView View;
			TArray<FPropertyBindingPathIndirection> Steps;
			void* Value = nullptr;
			if (!Data.GetBindingDataViewByID(Path.GetStructID(), View) || !Path.ResolveIndirectionsWithValue(View, Steps)
				|| Steps.IsEmpty() || Handle->GetValueData(Value) != FPropertyAccess::Success || !Value
				|| Steps.Last().GetPropertyAddress() != Value || Steps.Last().GetProperty() != Handle->GetProperty()) continue;
			Found = true;
			if (Extension->IsPropertyExtendable(Container->GetClass(), *Handle))
			{
				TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>> Accessible;
				Data.GetBindableStructs(Path.GetStructID(), Accessible);
				static_cast<const IStateTreeEditorPropertyBindingsOwner&>(Data).EnumerateBindablePropertyFunctionNodes([&](const UScriptStruct* Node, const FStateTreeBindableStructDesc& Desc, const FStateTreeDataView Source)
				{
					FPropertyBindingDataView Existing;
					if (Data.GetBindingDataViewByID(Desc.ID, Existing)) return EStateTreeVisitor::Continue;
					const bool bDirect = !Accessible.ContainsByPredicate([&](const auto& Entry) {
						const auto& Access = Entry.template Get<FStateTreeBindableStructDesc>();
						return Access.Struct == Desc.Struct && Access.DataSource != EStateTreeBindableStructSource::Context
							&& Access.DataSource != EStateTreeBindableStructSource::Parameter && Access.DataSource != EStateTreeBindableStructSource::TransitionEvent
							&& Access.DataSource != EStateTreeBindableStructSource::StateEvent && Access.DataSource != EStateTreeBindableStructSource::PropertyFunction;
					});
					Outputs(Desc, Source, *Handle->GetProperty(), Value, bDirect, [&](const FString& Output) { Visit(*Node, Output); });
					return EStateTreeVisitor::Continue;
				});
			}
			if (bScoped) return true;
		}
	}
	if (bScoped && !Found) Failure = TEXT("The receiving field has no matching Details row for its native binding identity and value.");
	return !bScoped || Found;
}
}
