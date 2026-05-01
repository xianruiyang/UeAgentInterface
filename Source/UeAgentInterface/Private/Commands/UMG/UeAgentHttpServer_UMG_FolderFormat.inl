#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/RichTextBlock.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "MovieScene.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"

namespace UeAgentWidgetBlueprintFolderOps
{
	struct FWidgetPropertySpec
	{
		const TCHAR* PropertyPath;
	};

	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("WidgetBlueprint")));
	}

	static FString GetBlueprintProxyRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return FFileHelper::SaveStringToFile(
			MakeJsonString(Obj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		return true;
	}

	static bool LoadOptionalJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		OutObj.Reset();
		if (!FPaths::FileExists(FilePath))
		{
			return true;
		}

		FString LoadError;
		if (!LoadJsonFile(FilePath, OutObj, LoadError))
		{
			OutError = FString::Printf(TEXT("%s:%s"), *LoadError, *FilePath);
			return false;
		}
		return true;
	}

	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& OutValue)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, FoundObject) || !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutValue = *FoundObject;
		return true;
	}

	static FString PinContainerTypeToString(const EPinContainerType InContainerType)
	{
		switch (InContainerType)
		{
		case EPinContainerType::Array:
			return TEXT("array");
		case EPinContainerType::Set:
			return TEXT("set");
		case EPinContainerType::Map:
			return TEXT("map");
		default:
			return TEXT("");
		}
	}

	static TSharedPtr<FJsonObject> ExportPinTypeJson(const FEdGraphPinType& PinType)
	{
		TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
		TypeObj->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
		if (!PinType.PinSubCategory.IsNone())
		{
			TypeObj->SetStringField(TEXT("pin_subcategory"), PinType.PinSubCategory.ToString());
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			if (const UObject* Obj = PinType.PinSubCategoryObject.Get())
			{
				TypeObj->SetStringField(TEXT("pin_subcategory_object"), Obj->GetPathName());
			}
		}
		const FString ContainerType = PinContainerTypeToString(PinType.ContainerType);
		if (!ContainerType.IsEmpty())
		{
			TypeObj->SetStringField(TEXT("container_type"), ContainerType);
		}
		if (PinType.ContainerType == EPinContainerType::Map && !PinType.PinValueType.TerminalCategory.IsNone())
		{
			TSharedPtr<FJsonObject> ValueTypeObj = MakeShared<FJsonObject>();
			ValueTypeObj->SetStringField(TEXT("pin_category"), PinType.PinValueType.TerminalCategory.ToString());
			if (!PinType.PinValueType.TerminalSubCategory.IsNone())
			{
				ValueTypeObj->SetStringField(TEXT("pin_subcategory"), PinType.PinValueType.TerminalSubCategory.ToString());
			}
			if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
			{
				if (const UObject* Obj = PinType.PinValueType.TerminalSubCategoryObject.Get())
				{
					ValueTypeObj->SetStringField(TEXT("pin_subcategory_object"), Obj->GetPathName());
				}
			}
			TypeObj->SetObjectField(TEXT("value_type"), ValueTypeObj);
		}
		return TypeObj;
	}

	static FString ExportBlueprintVariableDefaultValue(UBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		if (Blueprint && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
			if (CDO)
			{
				if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarDesc.VarName))
				{
					void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
					if (ValuePtr)
					{
						FString ExportedValue;
						Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, CDO, PPF_None);
						return ExportedValue;
					}
				}
			}
		}
		return VarDesc.DefaultValue;
	}

	static TSharedPtr<FJsonObject> ExportVariablesJson(UWidgetBlueprint* WidgetBlueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<FBPVariableDescription> SortedVariables = WidgetBlueprint ? WidgetBlueprint->NewVariables : TArray<FBPVariableDescription>();
		SortedVariables.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B)
		{
			return A.VarName.LexicalLess(B.VarName);
		});

		for (const FBPVariableDescription& VarDesc : SortedVariables)
		{
			if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				continue;
			}

			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			VarObj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
			VarObj->SetObjectField(TEXT("type"), ExportPinTypeJson(VarDesc.VarType));
			VarObj->SetStringField(TEXT("default_value"), ExportBlueprintVariableDefaultValue(WidgetBlueprint, VarDesc));
			VarObj->SetBoolField(TEXT("instance_editable"), (VarDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);
			Variables.Add(MakeShared<FJsonValueObject>(VarObj));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("variables"), Variables);
		return Root;
	}

	static const TArray<FString>& GetCommonWidgetPropertyPaths()
	{
		static const TArray<FString> Paths = {
			TEXT("RenderOpacity"),
			TEXT("Visibility"),
			TEXT("bIsEnabled"),
			TEXT("ToolTipText")
		};
		return Paths;
	}

	static TArray<FString> GetWidgetPropertyPaths(UWidget* Widget)
	{
		TArray<FString> Paths = GetCommonWidgetPropertyPaths();
		if (!Widget)
		{
			return Paths;
		}

		if (Widget->IsA<UTextBlock>())
		{
			Paths.Append({
				TEXT("Text"),
				TEXT("ColorAndOpacity")
			});
		}
		else if (Widget->IsA<UButton>())
		{
			Paths.Append({
				TEXT("ColorAndOpacity"),
				TEXT("BackgroundColor")
			});
		}
		else if (Widget->IsA<UImage>())
		{
			Paths.Append({
				TEXT("Brush.TintColor"),
				TEXT("Brush.ImageSize")
			});
		}
		else if (Widget->IsA<UBorder>())
		{
			Paths.Append({
				TEXT("BrushColor"),
				TEXT("ContentColorAndOpacity"),
				TEXT("Padding")
			});
		}
		else if (Widget->IsA<USizeBox>())
		{
			Paths.Append({
				TEXT("WidthOverride"),
				TEXT("HeightOverride"),
				TEXT("MinDesiredWidth"),
				TEXT("MinDesiredHeight"),
				TEXT("MaxDesiredWidth"),
				TEXT("MaxDesiredHeight")
			});
		}
		else if (Widget->IsA<UProgressBar>())
		{
			Paths.Append({
				TEXT("Percent"),
				TEXT("FillColorAndOpacity")
			});
		}
		else if (Widget->IsA<UEditableTextBox>())
		{
			Paths.Append({
				TEXT("Text"),
				TEXT("HintText")
			});
		}
		else if (Widget->IsA<URichTextBlock>())
		{
			Paths.Append({
				TEXT("Text")
			});
		}
		return Paths;
	}

	static TArray<FString> GetSlotPropertyPaths(UPanelSlot* Slot)
	{
		TArray<FString> Paths;
		if (!Slot)
		{
			return Paths;
		}

		if (Slot->IsA<UCanvasPanelSlot>())
		{
			Paths.Append({
				TEXT("LayoutData.Offsets"),
				TEXT("LayoutData.Anchors"),
				TEXT("LayoutData.Alignment"),
				TEXT("bAutoSize"),
				TEXT("ZOrder")
			});
		}
		else if (Slot->IsA<UOverlaySlot>())
		{
			Paths.Append({
				TEXT("Padding"),
				TEXT("HorizontalAlignment"),
				TEXT("VerticalAlignment")
			});
		}
		else if (Slot->IsA<UHorizontalBoxSlot>() || Slot->IsA<UVerticalBoxSlot>())
		{
			Paths.Append({
				TEXT("Padding"),
				TEXT("HorizontalAlignment"),
				TEXT("VerticalAlignment"),
				TEXT("Size")
			});
		}
		else if (Slot->IsA<UBorderSlot>())
		{
			Paths.Append({
				TEXT("Padding"),
				TEXT("HorizontalAlignment"),
				TEXT("VerticalAlignment")
			});
		}
		else if (Slot->IsA<UGridSlot>())
		{
			Paths.Append({
				TEXT("Row"),
				TEXT("Column"),
				TEXT("RowSpan"),
				TEXT("ColumnSpan"),
				TEXT("Layer"),
				TEXT("Padding"),
				TEXT("Nudge"),
				TEXT("HorizontalAlignment"),
				TEXT("VerticalAlignment")
			});
		}
		else if (Slot->IsA<UUniformGridSlot>())
		{
			Paths.Append({
				TEXT("Row"),
				TEXT("Column"),
				TEXT("HorizontalAlignment"),
				TEXT("VerticalAlignment")
			});
		}
		return Paths;
	}

	static TArray<TSharedPtr<FJsonValue>> ExportPropertyArray(UObject* Obj, const TArray<FString>& PropertyPaths)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (const FString& PropertyPath : PropertyPaths)
		{
			FProperty* Property = nullptr;
			void* ValuePtr = nullptr;
			if (!UeAgentUmgOps::ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
			{
				continue;
			}

			FString ExportedValue;
			Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, Obj, PPF_None);
			TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
			PropertyObj->SetStringField(TEXT("value_text"), ExportedValue);
			Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
		}
		return Properties;
	}

	static void CollectWidgetsPreOrder(UWidget* Widget, TArray<UWidget*>& OutWidgets)
	{
		if (!Widget)
		{
			return;
		}

		OutWidgets.Add(Widget);
		if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
		{
			for (int32 ChildIndex = 0; ChildIndex < PanelWidget->GetChildrenCount(); ++ChildIndex)
			{
				CollectWidgetsPreOrder(PanelWidget->GetChildAt(ChildIndex), OutWidgets);
			}
		}
	}

	static void BuildWidgetDepthMap(UWidget* Widget, int32 Depth, TMap<FString, int32>& OutDepthByName)
	{
		if (!Widget)
		{
			return;
		}
		OutDepthByName.Add(Widget->GetName(), Depth);
		if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
		{
			for (int32 ChildIndex = 0; ChildIndex < PanelWidget->GetChildrenCount(); ++ChildIndex)
			{
				BuildWidgetDepthMap(PanelWidget->GetChildAt(ChildIndex), Depth + 1, OutDepthByName);
			}
		}
	}

	static TSharedPtr<FJsonObject> ExportWidgetTreeJson(UWidgetBlueprint* WidgetBlueprint)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("root_widget"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget
			? WidgetBlueprint->WidgetTree->RootWidget->GetName()
			: FString());

		TArray<TSharedPtr<FJsonValue>> WidgetsArray;
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !WidgetBlueprint->WidgetTree->RootWidget)
		{
			Root->SetArrayField(TEXT("widgets"), WidgetsArray);
			return Root;
		}

		TArray<UWidget*> Widgets;
		CollectWidgetsPreOrder(WidgetBlueprint->WidgetTree->RootWidget, Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (!Widget)
			{
				continue;
			}

			TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
			WidgetObj->SetStringField(TEXT("id"), Widget->GetName());
			WidgetObj->SetStringField(TEXT("widget_name"), Widget->GetName());
			WidgetObj->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetPathName());
			WidgetObj->SetBoolField(TEXT("make_variable"), Widget->bIsVariable);
			WidgetObj->SetArrayField(TEXT("properties"), ExportPropertyArray(Widget, GetWidgetPropertyPaths(Widget)));

			TArray<TSharedPtr<FJsonValue>> ChildrenArray;
			if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
			{
				for (int32 ChildIndex = 0; ChildIndex < PanelWidget->GetChildrenCount(); ++ChildIndex)
				{
					if (UWidget* Child = PanelWidget->GetChildAt(ChildIndex))
					{
						ChildrenArray.Add(MakeShared<FJsonValueString>(Child->GetName()));
					}
				}
			}
			WidgetObj->SetArrayField(TEXT("children"), ChildrenArray);

			if (Widget->Slot)
			{
				TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
				SlotObj->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
				SlotObj->SetArrayField(TEXT("properties"), ExportPropertyArray(Widget->Slot, GetSlotPropertyPaths(Widget->Slot)));
				WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
			}

			WidgetsArray.Add(MakeShared<FJsonValueObject>(WidgetObj));
		}

		Root->SetArrayField(TEXT("widgets"), WidgetsArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportSettingsJson(UWidgetBlueprint* WidgetBlueprint)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		bool bIsFocusable = false;
		if (WidgetBlueprint && WidgetBlueprint->GeneratedClass)
		{
			if (const UUserWidget* WidgetCDO = Cast<UUserWidget>(WidgetBlueprint->GeneratedClass->GetDefaultObject()))
			{
				bIsFocusable = WidgetCDO->IsFocusable();
			}
		}
		Root->SetStringField(TEXT("parent_class"), WidgetBlueprint && WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("root_widget_name"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget ? WidgetBlueprint->WidgetTree->RootWidget->GetName() : TEXT(""));
		Root->SetStringField(TEXT("root_widget_class"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget ? WidgetBlueprint->WidgetTree->RootWidget->GetClass()->GetPathName() : TEXT(""));
		Root->SetBoolField(TEXT("has_named_root"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget != nullptr);
		Root->SetBoolField(TEXT("is_focusable"), bIsFocusable);
		Root->SetStringField(TEXT("description"), TEXT(""));
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportBindingsJson(UWidgetBlueprint* WidgetBlueprint)
	{
		TArray<TSharedPtr<FJsonValue>> BindingsArray;
		if (WidgetBlueprint)
		{
			TArray<FDelegateEditorBinding> SortedBindings = WidgetBlueprint->Bindings;
			SortedBindings.Sort([](const FDelegateEditorBinding& A, const FDelegateEditorBinding& B)
			{
				if (A.ObjectName == B.ObjectName)
				{
					return A.PropertyName.LexicalLess(B.PropertyName);
				}
				return A.ObjectName < B.ObjectName;
			});

			for (const FDelegateEditorBinding& Binding : SortedBindings)
			{
				TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
				BindingObj->SetStringField(TEXT("widget_name"), Binding.ObjectName);
				BindingObj->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
				if (Binding.Kind == EBindingKind::Property)
				{
					BindingObj->SetStringField(TEXT("binding_kind"), TEXT("property_variable"));
					BindingObj->SetStringField(TEXT("source_variable_name"), Binding.SourceProperty.ToString());
				}
				else
				{
					BindingObj->SetStringField(TEXT("binding_kind"), TEXT("function"));
					BindingObj->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
				}
				BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
			}
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("bindings"), BindingsArray);
		return Root;
	}

	static void BuildFloatKeyMap(const FMovieSceneFloatChannel& Channel, TMap<int32, float>& OutValues)
	{
		const TArrayView<const FFrameNumber> Times = Channel.GetData().GetTimes();
		const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetData().GetValues();
		for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
		{
			OutValues.Add(Times[Index].Value, Values[Index].Value);
		}
	}

	static TSharedPtr<FJsonObject> BuildAnimationTrackJson(const FFrameRate& TickResolution, const FString& WidgetName, UMovieSceneTrack* Track)
	{
		if (!Track)
		{
			return nullptr;
		}

		if (UMovieScene2DTransformTrack* TransformTrack = Cast<UMovieScene2DTransformTrack>(Track))
		{
			const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
			UMovieScene2DTransformSection* TransformSection = Sections.Num() > 0 ? Cast<UMovieScene2DTransformSection>(Sections[0]) : nullptr;
			if (!TransformSection)
			{
				return nullptr;
			}

			TMap<int32, float> TranslationX;
			TMap<int32, float> TranslationY;
			TMap<int32, float> ScaleX;
			TMap<int32, float> ScaleY;
			TMap<int32, float> ShearX;
			TMap<int32, float> ShearY;
			TMap<int32, float> Angle;
			BuildFloatKeyMap(TransformSection->Translation[0], TranslationX);
			BuildFloatKeyMap(TransformSection->Translation[1], TranslationY);
			BuildFloatKeyMap(TransformSection->Scale[0], ScaleX);
			BuildFloatKeyMap(TransformSection->Scale[1], ScaleY);
			BuildFloatKeyMap(TransformSection->Shear[0], ShearX);
			BuildFloatKeyMap(TransformSection->Shear[1], ShearY);
			BuildFloatKeyMap(TransformSection->Rotation, Angle);

			TSet<int32> Frames;
			for (const auto& Pair : TranslationX) Frames.Add(Pair.Key);
			for (const auto& Pair : TranslationY) Frames.Add(Pair.Key);
			for (const auto& Pair : ScaleX) Frames.Add(Pair.Key);
			for (const auto& Pair : ScaleY) Frames.Add(Pair.Key);
			for (const auto& Pair : ShearX) Frames.Add(Pair.Key);
			for (const auto& Pair : ShearY) Frames.Add(Pair.Key);
			for (const auto& Pair : Angle) Frames.Add(Pair.Key);

			TArray<int32> SortedFrames = Frames.Array();
			SortedFrames.Sort();

			TArray<TSharedPtr<FJsonValue>> KeysArray;
			for (const int32 FrameValue : SortedFrames)
			{
				TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
				KeyObj->SetNumberField(TEXT("frame"), FrameValue);
				KeyObj->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(FrameValue))));
				if (const float* Value = TranslationX.Find(FrameValue)) KeyObj->SetNumberField(TEXT("translation_x"), *Value);
				if (const float* Value = TranslationY.Find(FrameValue)) KeyObj->SetNumberField(TEXT("translation_y"), *Value);
				if (const float* Value = ScaleX.Find(FrameValue)) KeyObj->SetNumberField(TEXT("scale_x"), *Value);
				if (const float* Value = ScaleY.Find(FrameValue)) KeyObj->SetNumberField(TEXT("scale_y"), *Value);
				if (const float* Value = ShearX.Find(FrameValue)) KeyObj->SetNumberField(TEXT("shear_x"), *Value);
				if (const float* Value = ShearY.Find(FrameValue)) KeyObj->SetNumberField(TEXT("shear_y"), *Value);
				if (const float* Value = Angle.Find(FrameValue)) KeyObj->SetNumberField(TEXT("angle"), *Value);
				KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
			}

			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("widget_name"), WidgetName);
			TrackObj->SetStringField(TEXT("track_kind"), TEXT("transform"));
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			return TrackObj;
		}

		if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
		{
			const FString PropertyName = FloatTrack->GetPropertyName().ToString();
			const FString PropertyPath = FloatTrack->GetPropertyPath().ToString();
			const TArray<UMovieSceneSection*>& Sections = FloatTrack->GetAllSections();
			UMovieSceneFloatSection* FloatSection = Sections.Num() > 0 ? Cast<UMovieSceneFloatSection>(Sections[0]) : nullptr;
			if (!FloatSection)
			{
				return nullptr;
			}

			TMap<int32, float> Values;
			BuildFloatKeyMap(FloatSection->GetChannel(), Values);
			TArray<int32> SortedFrames;
			Values.GetKeys(SortedFrames);
			SortedFrames.Sort();

			TArray<TSharedPtr<FJsonValue>> KeysArray;
			for (const int32 FrameValue : SortedFrames)
			{
				TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
				KeyObj->SetNumberField(TEXT("frame"), FrameValue);
				KeyObj->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(FrameValue))));
				KeyObj->SetNumberField(TEXT("opacity"), Values[FrameValue]);
				KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
			}

			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("widget_name"), WidgetName);
			TrackObj->SetStringField(TEXT("track_kind"), PropertyPath.Equals(TEXT("RenderOpacity"), ESearchCase::IgnoreCase) ? TEXT("opacity") : TEXT("float_property"));
			TrackObj->SetStringField(TEXT("property_name"), PropertyName);
			TrackObj->SetStringField(TEXT("property_path"), PropertyPath);
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			return TrackObj;
		}

		if (UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track))
		{
			const FString PropertyName = ColorTrack->GetPropertyName().ToString();
			const FString PropertyPath = ColorTrack->GetPropertyPath().ToString();
			const TArray<UMovieSceneSection*>& Sections = ColorTrack->GetAllSections();
			UMovieSceneColorSection* ColorSection = Sections.Num() > 0 ? Cast<UMovieSceneColorSection>(Sections[0]) : nullptr;
			if (!ColorSection)
			{
				return nullptr;
			}

			TMap<int32, float> RedValues;
			TMap<int32, float> GreenValues;
			TMap<int32, float> BlueValues;
			TMap<int32, float> AlphaValues;
			BuildFloatKeyMap(ColorSection->GetRedChannel(), RedValues);
			BuildFloatKeyMap(ColorSection->GetGreenChannel(), GreenValues);
			BuildFloatKeyMap(ColorSection->GetBlueChannel(), BlueValues);
			BuildFloatKeyMap(ColorSection->GetAlphaChannel(), AlphaValues);

			TSet<int32> Frames;
			for (const auto& Pair : RedValues) Frames.Add(Pair.Key);
			for (const auto& Pair : GreenValues) Frames.Add(Pair.Key);
			for (const auto& Pair : BlueValues) Frames.Add(Pair.Key);
			for (const auto& Pair : AlphaValues) Frames.Add(Pair.Key);
			TArray<int32> SortedFrames = Frames.Array();
			SortedFrames.Sort();

			TArray<TSharedPtr<FJsonValue>> KeysArray;
			for (const int32 FrameValue : SortedFrames)
			{
				TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
				KeyObj->SetNumberField(TEXT("frame"), FrameValue);
				KeyObj->SetNumberField(TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(FrameValue))));
				if (const float* Value = RedValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("red"), *Value);
				if (const float* Value = GreenValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("green"), *Value);
				if (const float* Value = BlueValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("blue"), *Value);
				if (const float* Value = AlphaValues.Find(FrameValue)) KeyObj->SetNumberField(TEXT("alpha"), *Value);
				KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
			}

			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("widget_name"), WidgetName);
			const FString TrackKind =
				PropertyPath.Equals(TEXT("ColorAndOpacity"), ESearchCase::IgnoreCase) ? TEXT("color") :
				(PropertyPath.Equals(TEXT("BackgroundColor"), ESearchCase::IgnoreCase) ? TEXT("background_color") : TEXT("color_property"));
			TrackObj->SetStringField(TEXT("track_kind"), TrackKind);
			TrackObj->SetStringField(TEXT("property_name"), PropertyName);
			TrackObj->SetStringField(TEXT("property_path"), PropertyPath);
			TrackObj->SetArrayField(TEXT("keys"), KeysArray);
			return TrackObj;
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> ExportAnimationsJson(UWidgetBlueprint* WidgetBlueprint)
	{
		TArray<TSharedPtr<FJsonValue>> AnimationsArray;
		if (WidgetBlueprint)
		{
			TArray<UWidgetAnimation*> SortedAnimations = WidgetBlueprint->Animations;
			SortedAnimations.Sort([](const UWidgetAnimation& A, const UWidgetAnimation& B)
			{
				return A.GetName() < B.GetName();
			});

			for (UWidgetAnimation* Animation : SortedAnimations)
			{
				if (!Animation)
				{
					continue;
				}

				UMovieScene* MovieScene = Animation->GetMovieScene();
				if (!MovieScene)
				{
					continue;
				}

				TSharedPtr<FJsonObject> AnimationObj = MakeShared<FJsonObject>();
				AnimationObj->SetStringField(TEXT("name"), Animation->GetName());
#if WITH_EDITOR
				AnimationObj->SetStringField(TEXT("display_label"), Animation->GetDisplayLabel());
#else
				AnimationObj->SetStringField(TEXT("display_label"), Animation->GetName());
#endif
				AnimationObj->SetNumberField(TEXT("start_seconds"), Animation->GetStartTime());
				AnimationObj->SetNumberField(TEXT("duration_seconds"), Animation->GetEndTime() - Animation->GetStartTime());
				AnimationObj->SetNumberField(TEXT("display_rate_num"), MovieScene->GetDisplayRate().Numerator);
				AnimationObj->SetNumberField(TEXT("display_rate_den"), MovieScene->GetDisplayRate().Denominator);

				TArray<TSharedPtr<FJsonValue>> TracksArray;
				for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
				{
					if (const FMovieSceneBinding* MovieSceneBinding = MovieScene->FindBinding(Binding.AnimationGuid))
					{
						for (UMovieSceneTrack* Track : MovieSceneBinding->GetTracks())
						{
							if (TSharedPtr<FJsonObject> TrackObj = BuildAnimationTrackJson(MovieScene->GetTickResolution(), Binding.WidgetName.ToString(), Track))
							{
								TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
							}
						}
					}
				}

				AnimationObj->SetArrayField(TEXT("tracks"), TracksArray);
				AnimationsArray.Add(MakeShared<FJsonValueObject>(AnimationObj));
			}
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("animations"), AnimationsArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}

	static bool CopyFileIfExists(const FString& SourcePath, const FString& DestinationPath)
	{
		if (!IFileManager::Get().FileExists(*SourcePath))
		{
			return true;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true);
		return IFileManager::Get().Copy(*DestinationPath, *SourcePath, true, true) == COPY_OK;
	}

	static bool SaveBlueprintProxyFolder(
		const FString& AssetPath,
		const FString& WidgetFolderPath,
		const TSharedPtr<FJsonObject>& SettingsJson,
		FString& OutProxyFolderPath,
		FString& OutError)
	{
		const FString RelativeFolder = NormalizeAssetRelativeFolder(AssetPath);
		OutProxyFolderPath = FPaths::Combine(GetBlueprintProxyRootAbsolute(), RelativeFolder);
		IFileManager::Get().DeleteDirectory(*OutProxyFolderPath, false, true);

		const FString MembersDir = FPaths::Combine(OutProxyFolderPath, TEXT("members"));
		const FString ComponentsDir = FPaths::Combine(OutProxyFolderPath, TEXT("components"));
		const FString GraphsDir = FPaths::Combine(OutProxyFolderPath, TEXT("graphs"));
		IFileManager::Get().MakeDirectory(*MembersDir, true);
		IFileManager::Get().MakeDirectory(*ComponentsDir, true);
		IFileManager::Get().MakeDirectory(*GraphsDir, true);

		FString ParentClass = TEXT("/Script/UMG.UserWidget");
		TryGetString(SettingsJson, TEXT("parent_class"), ParentClass);

		TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
		AssetJson->SetNumberField(TEXT("format_version"), 1);
		AssetJson->SetStringField(TEXT("asset_kind"), TEXT("actor_blueprint"));
		AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
		AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		AssetJson->SetStringField(TEXT("parent_class"), ParentClass);
		AssetJson->SetStringField(TEXT("generated_class_name"), TEXT(""));
		if (!SaveJsonFile(FPaths::Combine(OutProxyFolderPath, TEXT("asset.json")), AssetJson))
		{
			OutError = TEXT("save_proxy_asset_json_failed");
			return false;
		}

		const FString WidgetMembersDir = FPaths::Combine(WidgetFolderPath, TEXT("members"));
		if (!CopyFileIfExists(FPaths::Combine(WidgetMembersDir, TEXT("variables.json")), FPaths::Combine(MembersDir, TEXT("variables.json"))))
		{
			OutError = TEXT("copy_proxy_variables_failed");
			return false;
		}

		if (IFileManager::Get().FileExists(*(FPaths::Combine(WidgetMembersDir, TEXT("delegates.json")))))
		{
			if (!CopyFileIfExists(FPaths::Combine(WidgetMembersDir, TEXT("delegates.json")), FPaths::Combine(MembersDir, TEXT("delegates.json"))))
			{
				OutError = TEXT("copy_proxy_delegates_failed");
				return false;
			}
		}
		else
		{
			TSharedPtr<FJsonObject> EmptyDelegates = MakeShared<FJsonObject>();
			EmptyDelegates->SetArrayField(TEXT("delegates"), {});
			if (!SaveJsonFile(FPaths::Combine(MembersDir, TEXT("delegates.json")), EmptyDelegates))
			{
				OutError = TEXT("save_proxy_delegates_failed");
				return false;
			}
		}

		TSharedPtr<FJsonObject> EmptyDefaults = MakeShared<FJsonObject>();
		EmptyDefaults->SetArrayField(TEXT("defaults"), {});
		if (!SaveJsonFile(FPaths::Combine(MembersDir, TEXT("defaults.json")), EmptyDefaults))
		{
			OutError = TEXT("save_proxy_defaults_failed");
			return false;
		}

		TSharedPtr<FJsonObject> EmptyComponents = MakeShared<FJsonObject>();
		EmptyComponents->SetArrayField(TEXT("components"), {});
		if (!SaveJsonFile(FPaths::Combine(ComponentsDir, TEXT("tree.json")), EmptyComponents))
		{
			OutError = TEXT("save_proxy_components_failed");
			return false;
		}

		const FString WidgetGraphsDir = FPaths::Combine(WidgetFolderPath, TEXT("graphs"));
		TArray<FString> GraphFiles;
		IFileManager::Get().FindFiles(GraphFiles, *(FPaths::Combine(WidgetGraphsDir, TEXT("*.json"))), true, false);
		for (const FString& GraphFile : GraphFiles)
		{
			if (!CopyFileIfExists(FPaths::Combine(WidgetGraphsDir, GraphFile), FPaths::Combine(GraphsDir, GraphFile)))
			{
				OutError = TEXT("copy_proxy_graph_failed");
				return false;
			}
		}

		return true;
	}
}

bool FUeAgentHttpServer::CmdUmgExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	const FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentWidgetBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString SettingsDir = FPaths::Combine(FolderPath, TEXT("settings"));
	const FString MembersDir = FPaths::Combine(FolderPath, TEXT("members"));
	const FString WidgetTreeDir = FPaths::Combine(FolderPath, TEXT("widget_tree"));
	const FString BindingsDir = FPaths::Combine(FolderPath, TEXT("bindings"));
	const FString AnimationsDir = FPaths::Combine(FolderPath, TEXT("animations"));
	const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*SettingsDir, true);
	IFileManager::Get().MakeDirectory(*MembersDir, true);
	IFileManager::Get().MakeDirectory(*WidgetTreeDir, true);
	IFileManager::Get().MakeDirectory(*BindingsDir, true);
	IFileManager::Get().MakeDirectory(*AnimationsDir, true);
	IFileManager::Get().MakeDirectory(*GraphsDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentWidgetBlueprintFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = TEXT("save_export_file_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetNumberField(TEXT("format_version"), 1);
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	AssetJson->SetNumberField(TEXT("profile_version"), 1);
	AssetJson->SetStringField(TEXT("widget_subkind"), TEXT("widget_blueprint"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson)) return false;
	if (!SaveRequiredJson(FPaths::Combine(SettingsDir, TEXT("widget_blueprint.json")), UeAgentWidgetBlueprintFolderOps::ExportSettingsJson(WidgetBlueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(MembersDir, TEXT("variables.json")), UeAgentWidgetBlueprintFolderOps::ExportVariablesJson(WidgetBlueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(WidgetTreeDir, TEXT("tree.json")), UeAgentWidgetBlueprintFolderOps::ExportWidgetTreeJson(WidgetBlueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(BindingsDir, TEXT("property_bindings.json")), UeAgentWidgetBlueprintFolderOps::ExportBindingsJson(WidgetBlueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(AnimationsDir, TEXT("animations.json")), UeAgentWidgetBlueprintFolderOps::ExportAnimationsJson(WidgetBlueprint))) return false;

	TSharedPtr<FJsonObject> ProxyData;
	FString ProxyError;
	auto InvokeBlueprintSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("umg_folder_export");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	TSharedPtr<FJsonObject> ProxyParams = MakeShared<FJsonObject>();
	ProxyParams->SetStringField(TEXT("asset_path"), AssetPath);
	ProxyParams->SetBoolField(TEXT("clean_output_dir"), true);
	ProxyParams->SetBoolField(TEXT("include_validation"), false);
	if (!InvokeBlueprintSubCommand(TEXT("blueprint_export_folder"), ProxyParams, ProxyData, ProxyError))
	{
		OutError = ProxyError;
		return false;
	}

	const FString ProxyFolderPath = ProxyData->GetStringField(TEXT("folder_path"));
	const FString ProxyMembersDir = FPaths::Combine(ProxyFolderPath, TEXT("members"));
	const FString ProxyGraphsDir = FPaths::Combine(ProxyFolderPath, TEXT("graphs"));

	if (IFileManager::Get().FileExists(*(FPaths::Combine(ProxyMembersDir, TEXT("delegates.json")))))
	{
		if (!UeAgentWidgetBlueprintFolderOps::CopyFileIfExists(FPaths::Combine(ProxyMembersDir, TEXT("delegates.json")), FPaths::Combine(MembersDir, TEXT("delegates.json"))))
		{
			OutError = TEXT("copy_delegates_failed");
			return false;
		}
		++FileCount;
	}

	TArray<FString> GraphFiles;
	IFileManager::Get().FindFiles(GraphFiles, *(FPaths::Combine(ProxyGraphsDir, TEXT("*.json"))), true, false);
	for (const FString& GraphFile : GraphFiles)
	{
		if (!UeAgentWidgetBlueprintFolderOps::CopyFileIfExists(FPaths::Combine(ProxyGraphsDir, GraphFile), FPaths::Combine(GraphsDir, GraphFile)))
		{
			OutError = TEXT("copy_graph_failed");
			return false;
		}
		++FileCount;
	}

	IFileManager::Get().DeleteDirectory(*ProxyFolderPath, false, true);

	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentWidgetBlueprintFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentWidgetBlueprintFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdUmgApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentUmgOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentWidgetBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentWidgetBlueprintFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentWidgetBlueprintFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("widget_blueprint"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported_asset_kind");
		return false;
	}

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bSaveAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	int32 VariablesApplied = 0;
	int32 WidgetsRemoved = 0;
	int32 WidgetsAdded = 0;
	int32 WidgetPropertiesApplied = 0;
	int32 SlotPropertiesApplied = 0;
	int32 WidgetPropertiesSkipped = 0;
	int32 SlotPropertiesSkipped = 0;
	int32 BindingsApplied = 0;
	int32 AnimationsApplied = 0;
	int32 AnimationKeysApplied = 0;
	int32 GraphsApplied = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	auto InvokeUmgSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("umg_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteUmgCommand(Command, SubCtx, SubData, SubError);
	};

	auto InvokeSequenceSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("umg_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteSequenceCommand(Command, SubCtx, SubData, SubError);
	};

	auto InvokeBlueprintSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("umg_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	TSharedPtr<FJsonObject> SettingsJson;
	FString SettingsError;
	if (!UeAgentWidgetBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("widget_blueprint.json")), SettingsJson, SettingsError))
	{
		OutError = SettingsError;
		return false;
	}
	if (!SettingsJson.IsValid())
	{
		SettingsJson = MakeShared<FJsonObject>();
	}

	UWidgetBlueprint* WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("widget_blueprint_not_found");
			return false;
		}

		FString ParentClassPath = TEXT("/Script/UMG.UserWidget");
		UeAgentWidgetBlueprintFolderOps::TryGetString(SettingsJson, TEXT("parent_class"), ParentClassPath);
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		CreateParams->SetStringField(TEXT("parent_class"), ParentClassPath);
		CreateParams->SetBoolField(TEXT("create_default_root"), false);
		CreateParams->SetBoolField(TEXT("compile_after_create"), false);
		CreateParams->SetBoolField(TEXT("open_editor"), false);
		CreateParams->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!InvokeUmgSubCommand(TEXT("umg_create_widget_blueprint"), CreateParams, CreateData, OutError))
		{
			return false;
		}
		WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
	}

	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_load_after_create_failed");
		return false;
	}

	{
		FString ProxyFolderPath;
		if (!UeAgentWidgetBlueprintFolderOps::SaveBlueprintProxyFolder(AssetPath, FolderPath, SettingsJson, ProxyFolderPath, OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ProxyApplyParams = MakeShared<FJsonObject>();
		ProxyApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
		ProxyApplyParams->SetBoolField(TEXT("create_if_missing"), false);
		ProxyApplyParams->SetBoolField(TEXT("compile_after_apply"), false);
		ProxyApplyParams->SetBoolField(TEXT("save_after_apply"), false);
		TSharedPtr<FJsonObject> ProxyApplyData;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_apply_folder"), ProxyApplyParams, ProxyApplyData, OutError))
		{
			OutError = FString::Printf(TEXT("blueprint_proxy_apply_failed:%s"), *OutError);
			return false;
		}

		VariablesApplied = static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("variables_added")) + ProxyApplyData->GetIntegerField(TEXT("variable_defaults_updated")));
		GraphsApplied = static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("graphs_rebuilt")));

		IFileManager::Get().DeleteDirectory(*ProxyFolderPath, false, true);
	}

	WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_blueprint_reload_failed");
		return false;
	}

	bool bIsFocusable = false;
	if (UeAgentWidgetBlueprintFolderOps::TryGetBool(SettingsJson, TEXT("is_focusable"), bIsFocusable))
	{
		TSharedPtr<FJsonObject> SetFocusableParams = MakeShared<FJsonObject>();
		SetFocusableParams->SetStringField(TEXT("asset_path"), AssetPath);
		SetFocusableParams->SetStringField(TEXT("property_name"), TEXT("bIsFocusable"));
		SetFocusableParams->SetStringField(TEXT("value_text"), bIsFocusable ? TEXT("true") : TEXT("false"));
		TSharedPtr<FJsonObject> SetFocusableData;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_set_cdo_property"), SetFocusableParams, SetFocusableData, OutError))
		{
			OutError = FString::Printf(TEXT("widget_blueprint_settings_apply_failed:%s"), *OutError);
			return false;
		}
	}

	TSharedPtr<FJsonObject> TreeJson;
	if (!UeAgentWidgetBlueprintFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("widget_tree"), TEXT("tree.json")), TreeJson, OutError))
	{
		return false;
	}

	FString RootWidgetId;
	UeAgentWidgetBlueprintFolderOps::TryGetString(TreeJson, TEXT("root_widget"), RootWidgetId);
	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	if (!TreeJson->TryGetArrayField(TEXT("widgets"), WidgetsArray) || !WidgetsArray)
	{
		OutError = TEXT("missing_widgets_array");
		return false;
	}

	TMap<FString, TSharedPtr<FJsonObject>> DesiredWidgets;
	for (const TSharedPtr<FJsonValue>& WidgetValue : *WidgetsArray)
	{
		const TSharedPtr<FJsonObject>* WidgetObjPtr = nullptr;
		if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObjPtr) || !WidgetObjPtr || !WidgetObjPtr->IsValid())
		{
			AddWarning(TEXT("widget_invalid_item"), TEXT("Skipped widget tree entry because it is not a JSON object."));
			continue;
		}
		FString WidgetId;
		if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*WidgetObjPtr, TEXT("id"), WidgetId) || WidgetId.IsEmpty())
		{
			AddWarning(TEXT("widget_missing_id"), TEXT("Skipped widget tree entry because id is missing."));
			continue;
		}
		DesiredWidgets.Add(WidgetId, *WidgetObjPtr);
	}

	bool bPreserveRootWidget = false;
	if (!RootWidgetId.IsEmpty() && WidgetBlueprint->WidgetTree->RootWidget)
	{
		if (WidgetBlueprint->WidgetTree->RootWidget->GetName().Equals(RootWidgetId, ESearchCase::CaseSensitive))
		{
			const TSharedPtr<FJsonObject>* RootWidgetObjPtr = DesiredWidgets.Find(RootWidgetId);
			if (RootWidgetObjPtr && RootWidgetObjPtr->IsValid())
			{
				FString DesiredRootClassPath;
				if (UeAgentWidgetBlueprintFolderOps::TryGetString(*RootWidgetObjPtr, TEXT("widget_class"), DesiredRootClassPath))
				{
					const FString ExistingRootClassPath = WidgetBlueprint->WidgetTree->RootWidget->GetClass()->GetPathName();
					bPreserveRootWidget = ExistingRootClassPath.Equals(DesiredRootClassPath, ESearchCase::IgnoreCase);
				}
			}
		}
	}

	TMap<FString, int32> DepthByName;
	UeAgentWidgetBlueprintFolderOps::BuildWidgetDepthMap(WidgetBlueprint->WidgetTree->RootWidget, 0, DepthByName);
	TArray<FString> ExistingWidgetNames;
	DepthByName.GetKeys(ExistingWidgetNames);
	ExistingWidgetNames.Sort([&DepthByName](const FString& A, const FString& B)
	{
		return DepthByName.FindRef(A) > DepthByName.FindRef(B);
	});

	for (int32 Index = ExistingWidgetNames.Num() - 1; Index >= 0; --Index)
	{
		if (bPreserveRootWidget && ExistingWidgetNames[Index].Equals(RootWidgetId, ESearchCase::CaseSensitive))
		{
			ExistingWidgetNames.RemoveAt(Index);
		}
	}

	for (const FString& ExistingWidgetName : ExistingWidgetNames)
	{
		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetStringField(TEXT("widget_name"), ExistingWidgetName);
		RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
		RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveData;
		if (!InvokeUmgSubCommand(TEXT("umg_remove_widget"), RemoveParams, RemoveData, OutError))
		{
			return false;
		}
		++WidgetsRemoved;
	}

	TFunction<bool(const FString&, const FString&, int32)> AddWidgetRecursive;
	AddWidgetRecursive = [&](const FString& WidgetId, const FString& ParentWidgetName, int32 InsertIndex) -> bool
	{
		const TSharedPtr<FJsonObject>* WidgetObjPtr = DesiredWidgets.Find(WidgetId);
		if (!WidgetObjPtr || !WidgetObjPtr->IsValid())
		{
			OutError = TEXT("widget_spec_not_found");
			return false;
		}
		const TSharedPtr<FJsonObject>& WidgetObj = *WidgetObjPtr;

		FString WidgetClassPath;
		if (!UeAgentWidgetBlueprintFolderOps::TryGetString(WidgetObj, TEXT("widget_class"), WidgetClassPath) || WidgetClassPath.IsEmpty())
		{
			OutError = TEXT("missing_widget_class");
			return false;
		}

		bool bMakeVariable = true;
		UeAgentWidgetBlueprintFolderOps::TryGetBool(WidgetObj, TEXT("make_variable"), bMakeVariable);

		const bool bIsPreservedRoot =
			bPreserveRootWidget &&
			ParentWidgetName.IsEmpty() &&
			WidgetId.Equals(RootWidgetId, ESearchCase::CaseSensitive);

		if (bIsPreservedRoot)
		{
			const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
			if (WidgetObj->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
			{
				for (int32 ChildIndex = 0; ChildIndex < ChildrenArray->Num(); ++ChildIndex)
				{
					FString ChildId;
					if ((*ChildrenArray)[ChildIndex].IsValid() && (*ChildrenArray)[ChildIndex]->TryGetString(ChildId) && !ChildId.IsEmpty())
					{
						if (!AddWidgetRecursive(ChildId, WidgetId, ChildIndex))
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("widget_class"), WidgetClassPath);
		AddParams->SetStringField(TEXT("widget_name"), WidgetId);
		AddParams->SetBoolField(TEXT("make_variable"), bMakeVariable);
		AddParams->SetBoolField(TEXT("compile_after_add"), false);
		AddParams->SetBoolField(TEXT("save_after_add"), false);
		if (!ParentWidgetName.IsEmpty())
		{
			AddParams->SetStringField(TEXT("parent_widget"), ParentWidgetName);
			AddParams->SetNumberField(TEXT("insert_index"), InsertIndex);
		}
		TSharedPtr<FJsonObject> AddData;
		if (!InvokeUmgSubCommand(TEXT("umg_add_widget"), AddParams, AddData, OutError))
		{
			return false;
		}
		++WidgetsAdded;

		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
		if (WidgetObj->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
		{
			for (int32 ChildIndex = 0; ChildIndex < ChildrenArray->Num(); ++ChildIndex)
			{
				FString ChildId;
				if ((*ChildrenArray)[ChildIndex].IsValid() && (*ChildrenArray)[ChildIndex]->TryGetString(ChildId) && !ChildId.IsEmpty())
				{
					if (!AddWidgetRecursive(ChildId, WidgetId, ChildIndex))
					{
						return false;
					}
				}
			}
		}
		return true;
	};

	if (!RootWidgetId.IsEmpty() && !AddWidgetRecursive(RootWidgetId, FString(), INDEX_NONE))
	{
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : DesiredWidgets)
	{
		const FString& WidgetId = Pair.Key;
		const TSharedPtr<FJsonObject>& WidgetObj = Pair.Value;

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (WidgetObj->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray)
		{
			for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
			{
				const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
				if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
				{
					continue;
				}
				FString PropertyName;
				FString ValueText;
				if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
				{
					continue;
				}
				if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
				{
					continue;
				}

				TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
				SetParams->SetStringField(TEXT("asset_path"), AssetPath);
				SetParams->SetStringField(TEXT("widget_name"), WidgetId);
				SetParams->SetStringField(TEXT("property_name"), PropertyName);
				SetParams->SetStringField(TEXT("value_text"), ValueText);
				SetParams->SetBoolField(TEXT("compile_after_set"), false);
				SetParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> SetData;
				if (!InvokeUmgSubCommand(TEXT("umg_set_widget_property"), SetParams, SetData, OutError))
				{
					if (OutError == TEXT("property_not_found"))
					{
						TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
						WarningObj->SetStringField(TEXT("kind"), TEXT("widget_property_skipped"));
						WarningObj->SetStringField(TEXT("widget_name"), WidgetId);
						WarningObj->SetStringField(TEXT("property_name"), PropertyName);
						Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
						++WidgetPropertiesSkipped;
						OutError.Reset();
						continue;
					}
					OutError = FString::Printf(TEXT("widget_property_apply_failed:%s:%s:%s"), *WidgetId, *PropertyName, *OutError);
					return false;
				}
				++WidgetPropertiesApplied;
			}
		}

		TSharedPtr<FJsonObject> SlotObj;
		if (UeAgentWidgetBlueprintFolderOps::TryGetObjectField(WidgetObj, TEXT("slot"), SlotObj))
		{
			const TArray<TSharedPtr<FJsonValue>>* SlotPropertiesArray = nullptr;
			if (SlotObj->TryGetArrayField(TEXT("properties"), SlotPropertiesArray) && SlotPropertiesArray)
			{
				for (const TSharedPtr<FJsonValue>& PropertyValue : *SlotPropertiesArray)
				{
					const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
					if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
					{
						continue;
					}
					FString PropertyName;
					FString ValueText;
					if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
					{
						continue;
					}
					if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
					{
						continue;
					}

					TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
					SetParams->SetStringField(TEXT("asset_path"), AssetPath);
					SetParams->SetStringField(TEXT("widget_name"), WidgetId);
					SetParams->SetStringField(TEXT("property_name"), PropertyName);
					SetParams->SetStringField(TEXT("value_text"), ValueText);
					SetParams->SetBoolField(TEXT("compile_after_set"), false);
					SetParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> SetData;
					if (!InvokeUmgSubCommand(TEXT("umg_set_slot_property"), SetParams, SetData, OutError))
					{
						if (OutError == TEXT("property_not_found"))
						{
							TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
							WarningObj->SetStringField(TEXT("kind"), TEXT("slot_property_skipped"));
							WarningObj->SetStringField(TEXT("widget_name"), WidgetId);
							WarningObj->SetStringField(TEXT("property_name"), PropertyName);
							Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
							++SlotPropertiesSkipped;
							OutError.Reset();
							continue;
						}
						OutError = FString::Printf(TEXT("slot_property_apply_failed:%s:%s:%s"), *WidgetId, *PropertyName, *OutError);
						return false;
					}
					++SlotPropertiesApplied;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BindingsJson;
	FString BindingsError;
	if (!UeAgentWidgetBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("property_bindings.json")), BindingsJson, BindingsError))
	{
		OutError = BindingsError;
		return false;
	}
	if (BindingsJson.IsValid())
	{
		WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
		if (!WidgetBlueprint)
		{
			OutError = TEXT("widget_blueprint_reload_failed");
			return false;
		}
		if (!WidgetBlueprint->SkeletonGeneratedClass)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->Bindings.Reset();

		const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
		if (BindingsJson->TryGetArrayField(TEXT("bindings"), BindingsArray) && BindingsArray)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *BindingsArray)
			{
				const TSharedPtr<FJsonObject>* BindingObjPtr = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObjPtr) || !BindingObjPtr || !BindingObjPtr->IsValid())
				{
					AddWarning(TEXT("binding_invalid_item"), TEXT("Skipped binding entry because it is not a JSON object."));
					continue;
				}

				FString WidgetName;
				FString PropertyName;
				FString BindingKind;
				if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*BindingObjPtr, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
				{
					AddWarning(TEXT("binding_missing_widget_name"), TEXT("Skipped binding entry because widget_name is missing."));
					continue;
				}
				if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*BindingObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
				{
					AddWarning(TEXT("binding_missing_property_name"), FString::Printf(TEXT("Skipped binding for widget '%s' because property_name is missing."), *WidgetName));
					continue;
				}
				UeAgentWidgetBlueprintFolderOps::TryGetString(*BindingObjPtr, TEXT("binding_kind"), BindingKind);

				FDelegateEditorBinding NewBinding;
				NewBinding.ObjectName = WidgetName;
				NewBinding.PropertyName = FName(*PropertyName);
				if (BindingKind.Equals(TEXT("property_variable"), ESearchCase::IgnoreCase))
				{
					FString SourceVariableName;
					if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*BindingObjPtr, TEXT("source_variable_name"), SourceVariableName) || SourceVariableName.IsEmpty())
					{
						OutError = FString::Printf(TEXT("binding_apply_failed:%s:%s:missing_source_variable_name"), *WidgetName, *PropertyName);
						return false;
					}
					if (!WidgetBlueprint->SkeletonGeneratedClass)
					{
						OutError = FString::Printf(TEXT("binding_apply_failed:%s:%s:widget_blueprint_skeleton_not_found"), *WidgetName, *PropertyName);
						return false;
					}
					FProperty* SourceProperty = FindFProperty<FProperty>(WidgetBlueprint->SkeletonGeneratedClass, FName(*SourceVariableName));
					if (!SourceProperty)
					{
						OutError = FString::Printf(TEXT("binding_apply_failed:%s:%s:source_variable_not_found:%s"), *WidgetName, *PropertyName, *SourceVariableName);
						return false;
					}
					NewBinding.FunctionName = FName(*SourceVariableName);
					NewBinding.SourceProperty = FName(*SourceVariableName);
					NewBinding.SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{ FFieldVariant(SourceProperty) });
					NewBinding.Kind = EBindingKind::Property;
				}
				else
				{
					FString FunctionName;
					if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*BindingObjPtr, TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
					{
						OutError = FString::Printf(TEXT("binding_apply_failed:%s:%s:missing_binding_function_name"), *WidgetName, *PropertyName);
						return false;
					}
					NewBinding.FunctionName = FName(*FunctionName);
					NewBinding.SourceProperty = NAME_None;
					NewBinding.SourcePath = FEditorPropertyPath();
					NewBinding.Kind = EBindingKind::Function;
				}
				WidgetBlueprint->Bindings.Add(NewBinding);
				++BindingsApplied;
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	}

	TSharedPtr<FJsonObject> AnimationsJson;
	FString AnimationsError;
	if (!UeAgentWidgetBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("animations"), TEXT("animations.json")), AnimationsJson, AnimationsError))
	{
		OutError = AnimationsError;
		return false;
	}
	if (AnimationsJson.IsValid())
	{
		WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
		if (!WidgetBlueprint)
		{
			OutError = TEXT("widget_blueprint_reload_failed");
			return false;
		}

		TArray<FString> ExistingAnimationNames;
		for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (Animation)
			{
				ExistingAnimationNames.Add(Animation->GetName());
			}
		}
		for (const FString& ExistingAnimationName : ExistingAnimationNames)
		{
			TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
			RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
			RemoveParams->SetStringField(TEXT("animation_name"), ExistingAnimationName);
			RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
			RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
			TSharedPtr<FJsonObject> RemoveData;
			if (!InvokeSequenceSubCommand(TEXT("sequence_remove_umg_animation"), RemoveParams, RemoveData, OutError))
			{
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* AnimationsArray = nullptr;
		if (AnimationsJson->TryGetArrayField(TEXT("animations"), AnimationsArray) && AnimationsArray)
		{
			for (const TSharedPtr<FJsonValue>& AnimationValue : *AnimationsArray)
			{
				const TSharedPtr<FJsonObject>* AnimationObjPtr = nullptr;
				if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObjPtr) || !AnimationObjPtr || !AnimationObjPtr->IsValid())
				{
					AddWarning(TEXT("animation_invalid_item"), TEXT("Skipped animation entry because it is not a JSON object."));
					continue;
				}
				const TSharedPtr<FJsonObject>& AnimationObj = *AnimationObjPtr;

				FString AnimationName;
				if (!UeAgentWidgetBlueprintFolderOps::TryGetString(AnimationObj, TEXT("name"), AnimationName) || AnimationName.IsEmpty())
				{
					AddWarning(TEXT("animation_missing_name"), TEXT("Skipped animation entry because name is missing."));
					continue;
				}

				double StartSeconds = 0.0;
				double DurationSeconds = 1.0;
				double DisplayRateNum = 20.0;
				double DisplayRateDen = 1.0;
				UeAgentWidgetBlueprintFolderOps::TryGetNumber(AnimationObj, TEXT("start_seconds"), StartSeconds);
				UeAgentWidgetBlueprintFolderOps::TryGetNumber(AnimationObj, TEXT("duration_seconds"), DurationSeconds);
				UeAgentWidgetBlueprintFolderOps::TryGetNumber(AnimationObj, TEXT("display_rate_num"), DisplayRateNum);
				UeAgentWidgetBlueprintFolderOps::TryGetNumber(AnimationObj, TEXT("display_rate_den"), DisplayRateDen);

				TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
				CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
				CreateParams->SetStringField(TEXT("animation_name"), AnimationName);
				CreateParams->SetNumberField(TEXT("start_seconds"), StartSeconds);
				CreateParams->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
				CreateParams->SetBoolField(TEXT("compile_after_create"), false);
				CreateParams->SetBoolField(TEXT("save_after_create"), false);
				TSharedPtr<FJsonObject> CreateData;
				if (!InvokeSequenceSubCommand(TEXT("sequence_create_umg_animation"), CreateParams, CreateData, OutError))
				{
					OutError = FString::Printf(TEXT("animation_apply_failed:%s:create:%s"), *AnimationName, *OutError);
					return false;
				}
				FString EffectiveAnimationName = AnimationName;
				if (CreateData.IsValid() && CreateData->HasField(TEXT("animation_name")))
				{
					EffectiveAnimationName = CreateData->GetStringField(TEXT("animation_name"));
				}
				if (!EffectiveAnimationName.Equals(AnimationName, ESearchCase::CaseSensitive))
				{
					TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
					RenameParams->SetStringField(TEXT("asset_path"), AssetPath);
					RenameParams->SetStringField(TEXT("animation_name"), EffectiveAnimationName);
					RenameParams->SetStringField(TEXT("new_animation_name"), AnimationName);
					RenameParams->SetBoolField(TEXT("compile_after_rename"), false);
					RenameParams->SetBoolField(TEXT("save_after_rename"), false);
					TSharedPtr<FJsonObject> RenameData;
					if (!InvokeSequenceSubCommand(TEXT("sequence_rename_umg_animation"), RenameParams, RenameData, OutError))
					{
						OutError = FString::Printf(TEXT("animation_apply_failed:%s:rename:%s"), *AnimationName, *OutError);
						return false;
					}
					EffectiveAnimationName = AnimationName;
				}

				TSharedPtr<FJsonObject> RateParams = MakeShared<FJsonObject>();
				RateParams->SetStringField(TEXT("asset_path"), AssetPath);
				RateParams->SetStringField(TEXT("animation_name"), EffectiveAnimationName);
				RateParams->SetNumberField(TEXT("display_rate_num"), DisplayRateNum);
				RateParams->SetNumberField(TEXT("display_rate_den"), DisplayRateDen);
				RateParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> RateData;
				if (!InvokeSequenceSubCommand(TEXT("sequence_set_umg_animation_display_rate"), RateParams, RateData, OutError))
				{
					OutError = FString::Printf(TEXT("animation_apply_failed:%s:display_rate:%s"), *EffectiveAnimationName, *OutError);
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
				if (AnimationObj->TryGetArrayField(TEXT("tracks"), TracksArray) && TracksArray)
				{
					for (const TSharedPtr<FJsonValue>& TrackValue : *TracksArray)
					{
						const TSharedPtr<FJsonObject>* TrackObjPtr = nullptr;
						if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObjPtr) || !TrackObjPtr || !TrackObjPtr->IsValid())
						{
							continue;
						}

						FString WidgetName;
						FString TrackKind;
						if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
						{
							continue;
						}
						if (!UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("track_kind"), TrackKind) || TrackKind.IsEmpty())
						{
							continue;
						}

						const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
						if (!(*TrackObjPtr)->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
						{
							continue;
						}

						for (const TSharedPtr<FJsonValue>& KeyValue : *KeysArray)
						{
							const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
							if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObjPtr) || !KeyObjPtr || !KeyObjPtr->IsValid())
							{
								continue;
							}

							double TimeSeconds = 0.0;
							if (!UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("time_seconds"), TimeSeconds))
							{
								continue;
							}

							if (TrackKind.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
							{
								TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
								KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
								KeyParams->SetStringField(TEXT("animation_name"), EffectiveAnimationName);
								KeyParams->SetStringField(TEXT("widget_name"), WidgetName);
								KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);

								double Value = 0.0;
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("translation_x"), Value)) KeyParams->SetNumberField(TEXT("translation_x"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("translation_y"), Value)) KeyParams->SetNumberField(TEXT("translation_y"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("scale_x"), Value)) KeyParams->SetNumberField(TEXT("scale_x"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("scale_y"), Value)) KeyParams->SetNumberField(TEXT("scale_y"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("shear_x"), Value)) KeyParams->SetNumberField(TEXT("shear_x"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("shear_y"), Value)) KeyParams->SetNumberField(TEXT("shear_y"), Value);
								if (UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("angle"), Value)) KeyParams->SetNumberField(TEXT("angle"), Value);
								KeyParams->SetBoolField(TEXT("compile_after_set"), false);
								KeyParams->SetBoolField(TEXT("save_after_set"), false);
								TSharedPtr<FJsonObject> KeyData;
								if (!InvokeSequenceSubCommand(TEXT("sequence_add_umg_widget_transform_key"), KeyParams, KeyData, OutError))
								{
									OutError = FString::Printf(TEXT("animation_key_apply_failed:%s:%s:transform:%s"), *EffectiveAnimationName, *WidgetName, *OutError);
									return false;
								}
								++AnimationKeysApplied;
							}
							else if (TrackKind.Equals(TEXT("opacity"), ESearchCase::IgnoreCase) || TrackKind.Equals(TEXT("float_property"), ESearchCase::IgnoreCase))
							{
								double FloatValue = 0.0;
								if (TrackKind.Equals(TEXT("opacity"), ESearchCase::IgnoreCase))
								{
									if (!UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("opacity"), FloatValue))
									{
										continue;
									}
								}
								else if (!UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("value"), FloatValue))
								{
									continue;
								}

								TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
								KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
								KeyParams->SetStringField(TEXT("animation_name"), EffectiveAnimationName);
								KeyParams->SetStringField(TEXT("widget_name"), WidgetName);
								KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
								if (TrackKind.Equals(TEXT("opacity"), ESearchCase::IgnoreCase))
								{
									KeyParams->SetNumberField(TEXT("opacity"), FloatValue);
									KeyParams->SetBoolField(TEXT("compile_after_set"), false);
									KeyParams->SetBoolField(TEXT("save_after_set"), false);
									TSharedPtr<FJsonObject> KeyData;
									if (!InvokeSequenceSubCommand(TEXT("sequence_add_umg_widget_opacity_key"), KeyParams, KeyData, OutError))
									{
										OutError = FString::Printf(TEXT("animation_key_apply_failed:%s:%s:opacity:%s"), *EffectiveAnimationName, *WidgetName, *OutError);
										return false;
									}
								}
								else
								{
									FString PropertyPath = TEXT("RenderOpacity");
									FString PropertyName = TEXT("RenderOpacity");
									UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("property_path"), PropertyPath);
									UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("property_name"), PropertyName);
									KeyParams->SetStringField(TEXT("property_path"), PropertyPath);
									KeyParams->SetStringField(TEXT("property_name"), PropertyName);
									KeyParams->SetNumberField(TEXT("value"), FloatValue);
									KeyParams->SetBoolField(TEXT("compile_after_set"), false);
									KeyParams->SetBoolField(TEXT("save_after_set"), false);
									TSharedPtr<FJsonObject> KeyData;
									if (!InvokeSequenceSubCommand(TEXT("sequence_add_umg_widget_float_key"), KeyParams, KeyData, OutError))
									{
										OutError = FString::Printf(TEXT("animation_key_apply_failed:%s:%s:float_property:%s"), *EffectiveAnimationName, *WidgetName, *OutError);
										return false;
									}
								}
								++AnimationKeysApplied;
							}
							else if (TrackKind.Equals(TEXT("color"), ESearchCase::IgnoreCase)
								|| TrackKind.Equals(TEXT("background_color"), ESearchCase::IgnoreCase)
								|| TrackKind.Equals(TEXT("color_property"), ESearchCase::IgnoreCase))
							{
								double Red = 0.0;
								double Green = 0.0;
								double Blue = 0.0;
								double Alpha = 1.0;
								if (!UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("red"), Red)
									|| !UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("green"), Green)
									|| !UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("blue"), Blue))
								{
									continue;
								}
								UeAgentWidgetBlueprintFolderOps::TryGetNumber(*KeyObjPtr, TEXT("alpha"), Alpha);
								FString PropertyPath = TrackKind.Equals(TEXT("background_color"), ESearchCase::IgnoreCase) ? TEXT("BackgroundColor") : TEXT("ColorAndOpacity");
								FString PropertyName = PropertyPath;
								UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("property_path"), PropertyPath);
								UeAgentWidgetBlueprintFolderOps::TryGetString(*TrackObjPtr, TEXT("property_name"), PropertyName);

								TSharedPtr<FJsonObject> KeyParams = MakeShared<FJsonObject>();
								KeyParams->SetStringField(TEXT("asset_path"), AssetPath);
								KeyParams->SetStringField(TEXT("animation_name"), EffectiveAnimationName);
								KeyParams->SetStringField(TEXT("widget_name"), WidgetName);
								KeyParams->SetNumberField(TEXT("time_seconds"), TimeSeconds);
								KeyParams->SetNumberField(TEXT("red"), Red);
								KeyParams->SetNumberField(TEXT("green"), Green);
								KeyParams->SetNumberField(TEXT("blue"), Blue);
								KeyParams->SetNumberField(TEXT("alpha"), Alpha);
								KeyParams->SetStringField(TEXT("property_name"), PropertyName);
								KeyParams->SetStringField(TEXT("property_path"), PropertyPath);
								KeyParams->SetBoolField(TEXT("compile_after_set"), false);
								KeyParams->SetBoolField(TEXT("save_after_set"), false);
								TSharedPtr<FJsonObject> KeyData;
								if (!InvokeSequenceSubCommand(TEXT("sequence_add_umg_widget_color_key"), KeyParams, KeyData, OutError))
								{
									OutError = FString::Printf(TEXT("animation_key_apply_failed:%s:%s:color:%s"), *EffectiveAnimationName, *WidgetName, *OutError);
									return false;
								}
								++AnimationKeysApplied;
							}
						}
					}
				}
				++AnimationsApplied;
			}
		}
	}

	if (bCompileAfterApply)
	{
		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
		CompileParams->SetBoolField(TEXT("include_messages"), true);
		CompileParams->SetStringField(TEXT("severity_filter"), TEXT("all"));
		CompileParams->SetNumberField(TEXT("max_messages"), 64);
		CompileParams->SetBoolField(TEXT("save_after_compile"), bSaveAfterApply);
		TSharedPtr<FJsonObject> CompileData;
		if (!InvokeUmgSubCommand(TEXT("umg_compile"), CompileParams, CompileData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("compile"), CompileData);
	}
	else if (bSaveAfterApply)
	{
		WidgetBlueprint = UeAgentUmgOps::LoadWidgetBlueprint(AssetPath);
		if (!WidgetBlueprint || !UeAgentUmgOps::SaveWidgetBlueprintPackage(WidgetBlueprint, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("variables_applied"), VariablesApplied);
	OutData->SetNumberField(TEXT("widgets_removed"), WidgetsRemoved);
	OutData->SetNumberField(TEXT("widgets_added"), WidgetsAdded);
	OutData->SetNumberField(TEXT("widget_properties_applied"), WidgetPropertiesApplied);
	OutData->SetNumberField(TEXT("slot_properties_applied"), SlotPropertiesApplied);
	OutData->SetNumberField(TEXT("widget_properties_skipped"), WidgetPropertiesSkipped);
	OutData->SetNumberField(TEXT("slot_properties_skipped"), SlotPropertiesSkipped);
	OutData->SetNumberField(TEXT("bindings_applied"), BindingsApplied);
	OutData->SetNumberField(TEXT("animations_applied"), AnimationsApplied);
	OutData->SetNumberField(TEXT("animation_keys_applied"), AnimationKeysApplied);
	OutData->SetNumberField(TEXT("graphs_applied"), GraphsApplied);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterApply);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}
