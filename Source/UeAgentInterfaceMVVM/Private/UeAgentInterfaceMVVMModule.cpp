#include "IUeAgentInterfaceMVVMModule.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintExtension.h"

class FUeAgentInterfaceMVVMModule final : public IUeAgentInterfaceMVVMModule
{
public:
	virtual bool HasFunctionReference(const UBlueprint& Dependent, TFunctionRef<bool(const UFunction*)> Matches) const override
	{
		const auto* Widget = Cast<UWidgetBlueprint>(&Dependent);
		const auto* Extension = Widget ? UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(Widget) : nullptr;
		const UMVVMBlueprintView* View = Extension ? Extension->GetBlueprintView() : nullptr;
		if (!View) return false;
		auto PathUses = [&](const FMVVMBlueprintPropertyPath& Path, bool) {
			// Native deprecated-field migration, if needed, only touches this value copy.
			FMVVMBlueprintPropertyPath Copy = Path;
			for (const FMVVMBlueprintFieldPath& Field : Copy.GetFieldPaths())
			{
				const auto Resolved = Field.GetField(Widget->GeneratedClass);
				if (Resolved.IsFunction() && Matches(Resolved.GetFunction())) return true;
			}
			return false;
		};
		auto ConversionUses = [&](const UMVVMBlueprintViewConversionFunction& Conversion) {
			const auto Reference = Conversion.GetConversionFunction();
			return Matches(Reference.GetFunction(Widget));
		};
		return VisitReferences(*View, PathUses, ConversionUses);
	}

	virtual bool HasExternalRenameEffect(const UBlueprint& Source, const UBlueprint& Dependent,
		const FName OldName, const FName NewName) const override
	{
		const auto* Widget = Cast<UWidgetBlueprint>(&Dependent);
		const auto* Extension = Widget ? UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(Widget) : nullptr;
		const UMVVMBlueprintView* View = Extension ? Extension->GetBlueprintView() : nullptr;
		if (!View) return false;
		// The original callback mutates direct binding paths, but only applies
		// copied event/condition/pin paths when OnFieldRenamed returns true.
		auto Affects = [&](const FMVVMBlueprintPropertyPath& Path, const bool Direct = false) {
			FMVVMBlueprintPropertyPath Copy = Path;
			const UClass* SelfContext = Widget->GeneratedClass;
			const auto PathSource = Copy.GetSource(Widget);
			const UStruct* OwnerStruct = nullptr;
			if (PathSource == EMVVMBlueprintFieldPathSource::SelfContext) OwnerStruct = SelfContext;
			else if (PathSource == EMVVMBlueprintFieldPathSource::ViewModel || PathSource == EMVVMBlueprintFieldPathSource::Widget)
			{
				const auto* Model = PathSource == EMVVMBlueprintFieldPathSource::ViewModel ? View->FindViewModel(Copy.GetViewModelId()) : nullptr;
				const FName OwnerName = PathSource == EMVVMBlueprintFieldPathSource::ViewModel
					? (Model ? Model->GetViewModelName() : NAME_None) : Copy.GetWidgetName();
				const FProperty* OwnerProperty = FBlueprintEditorUtils::GetMostUpToDateClass(SelfContext)->FindPropertyByName(OwnerName);
				if (const auto* Object = CastField<FObjectPropertyBase>(OwnerProperty)) OwnerStruct = Object->PropertyClass;
				else if (const auto* Struct = CastField<FStructProperty>(OwnerProperty)) OwnerStruct = Struct->Struct;
			}
			// This adapter is called only for plain UBlueprint sources. The native
			// same-Widget source/extension rename branch is therefore unreachable.
			// Follow the native visitation order: even a non-matching direct binding
			// can change when the engine resolves a deprecated member or source.
			for (const FMVVMBlueprintFieldPath& Field : Copy.GetFieldPaths())
			{
				if (OwnerStruct)
				{
					const UClass* OwnerClass = Cast<UClass>(OwnerStruct);
					const UStruct* Current = OwnerClass ? FBlueprintEditorUtils::GetMostUpToDateClass(OwnerClass) : OwnerStruct;
					const FProperty* OwnerProperty = Current->FindPropertyByName(Field.GetFieldName(SelfContext));
					if (const auto* Object = CastField<FObjectPropertyBase>(OwnerProperty)) OwnerStruct = Object->PropertyClass;
					else if (const auto* Struct = CastField<FStructProperty>(OwnerProperty)) OwnerStruct = Struct->Struct;
				}
				if (Field.GetParentClass(SelfContext) == Source.GeneratedClass && Field.GetFieldName(SelfContext) == OldName) return true;
			}
			return Direct && !FMVVMBlueprintPropertyPath::StaticStruct()->CompareScriptStruct(&Path, &Copy, 0);
		};
		return VisitReferences(*View, Affects, [](const UMVVMBlueprintViewConversionFunction& Conversion) {
			return Conversion.HasAnyFlags(RF_NeedPostLoad);
		});
	}

private:
	template<typename PathPredicate, typename ConversionPredicate>
	static bool VisitReferences(const UMVVMBlueprintView& View, const PathPredicate& Affects, const ConversionPredicate& ConversionAffects)
	{
		auto PinsAffect = [&](const auto& Pins) {
			for (const FMVVMBlueprintPin& Pin : Pins)
				if (Pin.UsedPathAsValue() && Affects(Pin.GetPath(), false)) return true;
			return false;
		};
		for (const FMVVMBlueprintViewBinding& Binding : View.GetBindings())
		{
			if (Affects(Binding.SourcePath, true) || Affects(Binding.DestinationPath, true)) return true;
			for (const UMVVMBlueprintViewConversionFunction* Conversion : {
				Binding.Conversion.DestinationToSourceConversion.Get(), Binding.Conversion.SourceToDestinationConversion.Get() })
			{
				// Native traversal calls ConditionalPostLoad before inspecting pins.
				if (Conversion && (ConversionAffects(*Conversion) || PinsAffect(Conversion->GetPins()))) return true;
			}
		}
		for (const UMVVMBlueprintViewEvent* Event : View.GetEvents())
			if (Event && (PinsAffect(Event->GetPins()) || Affects(Event->GetEventPath(), false) || Affects(Event->GetDestinationPath(), false))) return true;
		for (const UMVVMBlueprintViewCondition* Condition : View.GetConditions())
			if (Condition && (PinsAffect(Condition->GetPins()) || Affects(Condition->GetConditionPath(), false) || Affects(Condition->GetDestinationPath(), false))) return true;
		return false;
	}
};

IMPLEMENT_MODULE(FUeAgentInterfaceMVVMModule, UeAgentInterfaceMVVM)

#if WITH_DEV_AUTOMATION_TESTS
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMVVMGraphRenameEffectTest,
	"UeAgentInterface.T208.Blueprint.MVVMGraphRenameEffect", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentMVVMGraphRenameEffectTest::RunTest(const FString&)
{
	if (!TestTrue(TEXT("native MVVMEditor was loaded by host plugin lifecycle"), FModuleManager::Get().IsModuleLoaded(TEXT("ModelViewViewModelEditor")))) return false;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* Source = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("RenameSource_") + Token), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	auto* Widget = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), GetTransientPackage(),
		*FString(TEXT("RenameView_") + Token), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!Source || !Widget) return false;
	if (!Widget->WidgetTree) Widget->WidgetTree = NewObject<UWidgetTree>(Widget, TEXT("WidgetTree"));
	for (const FName Name : {FName(TEXT("ComputeStatus")), FName(TEXT("OtherStatus"))})
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Source, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph(Source, Graph, true, static_cast<UFunction*>(nullptr));
		TArray<UK2Node_FunctionEntry*> Entries; Graph->GetNodesOfClass(Entries);
		if (Entries.Num() != 1) return false;
		Entries[0]->AddExtraFlags(FUNC_BlueprintPure | FUNC_Const);
		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
		auto* Result = Creator.CreateNode(); Result->FunctionReference.SetSelfMember(Name); Creator.Finalize();
		FEdGraphPinType Boolean; Boolean.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		Result->CreateUserDefinedPin(TEXT("ReturnValue"), Boolean, EGPD_Input);
	}
	FEdGraphPinType Dispatcher; Dispatcher.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	FBlueprintEditorUtils::AddMemberVariable(Source, TEXT("StatusChanged"), Dispatcher);
	FBlueprintEditorUtils::AddMemberVariable(Source, TEXT("OtherChanged"), Dispatcher);
	for (const FName Name : {FName(TEXT("StatusChanged")), FName(TEXT("OtherChanged"))})
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Source, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		const auto* Schema = GetDefault<UEdGraphSchema_K2>();
		Schema->CreateDefaultNodesForGraph(*Graph);
		Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
		Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		Schema->MarkFunctionEntryAsEditable(Graph, true);
		Graph->bEditable = false;
		Source->DelegateSignatureGraphs.Add(Graph);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Source);
	FKismetEditorUtilities::CompileBlueprint(Source);
	auto* CopySource = DuplicateObject<UBlueprint>(Source, GetTransientPackage(), *FString(TEXT("CopiedSource_") + Token));
	if (!CopySource) return false;
	FKismetEditorUtilities::CompileBlueprint(CopySource);
	FGuid OriginalGuid, CopiedGuid;
	UBlueprint::GetFunctionGuidFromClassByFieldName(Source->GeneratedClass, TEXT("ComputeStatus"), OriginalGuid);
	UBlueprint::GetFunctionGuidFromClassByFieldName(CopySource->GeneratedClass, TEXT("ComputeStatus"), CopiedGuid);
	if (!TestTrue(TEXT("native MVVM fixture copy retains function GUID"), OriginalGuid.IsValid() && OriginalGuid == CopiedGuid)) return false;
	FEdGraphPinType Reference; Reference.PinCategory = UEdGraphSchema_K2::PC_Object; Reference.PinSubCategoryObject = Source->GeneratedClass;
	FBlueprintEditorUtils::AddMemberVariable(Widget, TEXT("SourceRef"), Reference);
	Reference.PinSubCategoryObject = CopySource->GeneratedClass;
	FBlueprintEditorUtils::AddMemberVariable(Widget, TEXT("CopyRef"), Reference);
	FEdGraphPinType Boolean; Boolean.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Widget, TEXT("Status"), Boolean);
	FKismetEditorUtilities::CompileBlueprint(Widget);
	if (!TestTrue(TEXT("real source and Widget Blueprint compile"), Source->Status != BS_Error && Widget->Status != BS_Error)) return false;
	auto* Extension = UWidgetBlueprintExtension::RequestExtension<UMVVMWidgetBlueprintExtension_View>(Widget);
	Extension->CreateBlueprintViewInstance();
	auto* View = Extension->GetBlueprintView();
	if (!View) return false;
	FMVVMBlueprintViewBinding& Binding = View->AddDefaultBinding();
	Binding.BindingType = EMVVMBindingMode::OneTimeToDestination;
	Binding.DestinationPath.SetSelfContext();
	Binding.DestinationPath.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Widget->GeneratedClass->FindPropertyByName(TEXT("Status"))));
	auto MakePath = [&](const FName FunctionName) {
		FMVVMBlueprintPropertyPath Path; Path.SetSelfContext();
		Path.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Widget->GeneratedClass->FindPropertyByName(TEXT("SourceRef"))));
		Path.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Source->GeneratedClass->FindFunctionByName(FunctionName)));
		return Path;
	};
	auto Snapshot = [&]() {
		TArray<uint8> Bytes;
		for (UObject* Object : {static_cast<UObject*>(Source), static_cast<UObject*>(CopySource), static_cast<UObject*>(Widget), static_cast<UObject*>(Extension), static_cast<UObject*>(View)})
		{
			TArray<uint8> ObjectBytes; FObjectWriter Writer(Object, ObjectBytes); Bytes.Append(ObjectBytes);
		}
		return Bytes;
	};
	FUeAgentInterfaceMVVMModule Inspector;
	auto Check = [&](const TCHAR* Label, const FMVVMBlueprintPropertyPath& Path, const bool Expected, const bool ExpectedReference = false, const bool IsDelegate = false) {
		Binding.SourcePath = Path;
		const auto Before = Snapshot();
		int32 Modified = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			if (Object && (Object == Source || Object == Widget || Object->IsIn(Source) || Object->IsIn(Widget))) ++Modified;
		});
		const FName OldName = IsDelegate ? TEXT("StatusChanged") : TEXT("ComputeStatus");
		const FName NewName = IsDelegate ? TEXT("StatusUpdated") : TEXT("EvaluateStatus");
		const bool Predicted = Inspector.HasExternalRenameEffect(*Source, *Widget, OldName, NewName);
		const bool Referenced = Inspector.HasFunctionReference(*Widget, [&](const UFunction* Function) {
			return Function && Function->GetFName() == TEXT("ComputeStatus")
				&& UBlueprint::GetBlueprintFromClass(Function->GetOwnerClass()) == Source;
		});
		FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (!TestTrue(FString(Label) + TEXT(" preflight leaves original objects unchanged"), Modified == 0 && Before == Snapshot())) return false;
		if (!TestEqual(FString(Label) + TEXT(" deletion reference matches the fixture's native function selection"), Referenced, ExpectedReference)) return false;
		AddInfo(FString::Printf(TEXT("%s: deletion_reference=%d modifications=%d"), Label, Referenced, Modified));
		// Independent oracle: the actual registered native subscriber mutates only
		// these owned fixture objects; it never runs during the product preflight.
		bool NativeChangedAtNotification = false;
		const auto NativeHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			// MVVM emits this after updating direct paths and before its structural
			// refresh may resolve those references again. Observe the write here.
			if (Object == View) NativeChangedAtNotification |= Before != Snapshot();
		});
		if (IsDelegate) FBlueprintEditorUtils::OnRenameVariableReferencesEvent.Broadcast(Widget, Source->GeneratedClass, OldName, NewName);
		else FBlueprintEditorUtils::OnRenameFunctionReferencesEvent.Broadcast(Widget, Source->GeneratedClass, OldName, NewName);
		FCoreUObjectDelegates::OnObjectModified.Remove(NativeHandle);
		const bool ChangedAfter = Before != Snapshot();
		const bool Actual = NativeChangedAtNotification || ChangedAfter;
		AddInfo(FString::Printf(TEXT("%s: predicted=%d native_changed=%d at_notification=%d after=%d"), Label, Predicted, Actual, NativeChangedAtNotification, ChangedAfter));
		return TestEqual(FString(Label) + TEXT(" native oracle"), Actual, Expected)
			&& TestEqual(FString(Label) + TEXT(" predicate matches independent native oracle"), Predicted, Actual);
	};
	if (!Check(TEXT("referencing binding"), MakePath(TEXT("ComputeStatus")), true, true)) return false;
	if (!Check(TEXT("unrelated binding"), MakePath(TEXT("OtherStatus")), false)) return false;
	FMVVMBlueprintPropertyPath CopiedPath; CopiedPath.SetSelfContext();
	CopiedPath.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Widget->GeneratedClass->FindPropertyByName(TEXT("CopyRef"))));
	CopiedPath.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(CopySource->GeneratedClass->FindFunctionByName(TEXT("ComputeStatus"))));
	if (!Check(TEXT("copied owner same GUID binding"), CopiedPath, false)) return false;
	if (!Check(TEXT("direct binding source migration"), FMVVMBlueprintPropertyPath(), true)) return false;
	for (const FName Name : {FName(TEXT("StatusChanged")), FName(TEXT("OtherChanged"))})
	{
		FMVVMBlueprintPropertyPath Path; Path.SetSelfContext();
		Path.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Widget->GeneratedClass->FindPropertyByName(TEXT("SourceRef"))));
		Path.AppendPropertyPath(Widget, UE::MVVM::FMVVMConstFieldVariant(Source->GeneratedClass->FindPropertyByName(Name)));
		if (!Check(Name == TEXT("StatusChanged") ? TEXT("delegate variable broadcast hit") : TEXT("unrelated delegate variable broadcast"),
			Path, Name == TEXT("StatusChanged"), false, true)) return false;
	}
	return !HasAnyErrors();
}
#endif
