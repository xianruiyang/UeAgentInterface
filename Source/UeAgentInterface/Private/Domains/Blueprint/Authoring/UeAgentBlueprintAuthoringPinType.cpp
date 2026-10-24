#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentBlueprintAuthoringOps

{

	static FName NormalizePinCategoryName(const FString& InCategory)

	{

		const FString Cat = InCategory.TrimStartAndEnd();

		if (Cat.IsEmpty())

		{

			return NAME_None;

		}

		if (Cat.Equals(TEXT("bool"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Boolean;

		}

		if (Cat.Equals(TEXT("int"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("integer"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Int;

		}

		if (Cat.Equals(TEXT("int64"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Int64;

		}

		if (Cat.Equals(TEXT("byte"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Byte;

		}

		if (Cat.Equals(TEXT("enum"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Enum;

		}

		if (Cat.Equals(TEXT("float"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("double"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Real;

		}

		if (Cat.Equals(TEXT("name"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Name;

		}

		if (Cat.Equals(TEXT("string"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_String;

		}

		if (Cat.Equals(TEXT("text"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Text;

		}

		if (Cat.Equals(TEXT("struct"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Struct;

		}

		if (Cat.Equals(TEXT("object"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Object;

		}

		if (Cat.Equals(TEXT("class"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Class;

		}

		if (Cat.Equals(TEXT("softclass"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("soft_class"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_SoftClass;

		}

		if (Cat.Equals(TEXT("interface"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Interface;

		}

		if (Cat.Equals(TEXT("softobject"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("soft_object"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_SoftObject;

		}

		if (Cat.Equals(TEXT("fieldpath"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("field_path"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_FieldPath;

		}

		if (Cat.Equals(TEXT("delegate"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Delegate;

		}

		if (Cat.Equals(TEXT("multicastdelegate"), ESearchCase::IgnoreCase) || Cat.Equals(TEXT("mcdelegate"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_MCDelegate;

		}

		return FName(*Cat);

	}



	static FName NormalizeRealPinSubCategoryName(const FString& InSubCategory)

	{

		const FString SubCategory = InSubCategory.TrimStartAndEnd();

		if (SubCategory.IsEmpty())

		{

			return NAME_None;

		}

		if (SubCategory.Equals(TEXT("float"), ESearchCase::IgnoreCase) || SubCategory.Equals(TEXT("single"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Float;

		}

		if (SubCategory.Equals(TEXT("double"), ESearchCase::IgnoreCase))

		{

			return UEdGraphSchema_K2::PC_Double;

		}

		return FName(*SubCategory);

	}



	static EPinContainerType ParsePinContainerType(const FString& InContainerType)

	{

		const FString ContainerType = InContainerType.TrimStartAndEnd();

		if (ContainerType.Equals(TEXT("array"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Array;

		}

		if (ContainerType.Equals(TEXT("set"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Set;

		}

		if (ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase))

		{

			return EPinContainerType::Map;

		}

		return EPinContainerType::None;

	}



	FString PinContainerTypeToString(const EPinContainerType InContainerType)

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

	TSharedRef<FJsonObject> BuildPinTypeJsonObject(const FEdGraphPinType& PinType,
		const UBlueprint* Blueprint, const FString& CanonicalAssetPath)
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
		if (!PinType.PinSubCategory.IsNone())
		{
			State->SetStringField(
				TEXT("pin_subcategory"), PinType.PinSubCategory.ToString());
		}
		if (const UObject* Subcategory = PinType.PinSubCategoryObject.Get())
		{
			State->SetStringField(
				TEXT("pin_subcategory_object"), UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
					Blueprint, *Subcategory, CanonicalAssetPath));
			if (const auto* Struct = Cast<UScriptStruct>(Subcategory))
				if (const auto Semantics = UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct).ValueSemantics)
					State->SetObjectField(TEXT("value_semantics"), Semantics);
		}
		const FString Container = PinContainerTypeToString(PinType.ContainerType);
		if (!Container.IsEmpty())
		{
			State->SetStringField(TEXT("container_type"), Container);
		}
		if (PinType.ContainerType == EPinContainerType::Map
			&& !PinType.PinValueType.TerminalCategory.IsNone())
		{
			TSharedRef<FJsonObject> ValueType = MakeShared<FJsonObject>();
			ValueType->SetStringField(
				TEXT("pin_category"),
				PinType.PinValueType.TerminalCategory.ToString());
			if (!PinType.PinValueType.TerminalSubCategory.IsNone())
			{
				ValueType->SetStringField(
					TEXT("pin_subcategory"),
					PinType.PinValueType.TerminalSubCategory.ToString());
			}
			if (const UObject* ValueObject =
				PinType.PinValueType.TerminalSubCategoryObject.Get())
			{
				ValueType->SetStringField(
					TEXT("pin_subcategory_object"), UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
						Blueprint, *ValueObject, CanonicalAssetPath));
				if (const auto* Struct = Cast<UScriptStruct>(ValueObject))
					if (const auto Semantics = UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct).ValueSemantics)
						ValueType->SetObjectField(TEXT("value_semantics"), Semantics);
			}
			State->SetObjectField(TEXT("value_type"), ValueType);
		}
		return State;
	}



	static UObject* ResolvePinSubCategoryObject(const FString& InPath)

	{

		if (InPath.TrimStartAndEnd().IsEmpty())

		{

			return nullptr;

		}

		if (UClass* AsClass = ResolveClassByPath(InPath))

		{

			return AsClass;

		}

		return LoadAssetObject(InPath);

	}



	static FString NormalizePinTypeToken(const FString& InToken)

	{

		FString Token = InToken.TrimStartAndEnd();

		Token.ReplaceInline(TEXT("/Script/CoreUObject."), TEXT(""), ESearchCase::IgnoreCase);

		Token.ReplaceInline(TEXT(" "), TEXT(""));

		Token.ReplaceInline(TEXT("_"), TEXT(""));

		Token.ReplaceInline(TEXT("-"), TEXT(""));

		Token.ToLowerInline();

		return Token;

	}



	static bool LooksLikeReflectedObjectPath(const FString& InText)

	{

		const FString Text = InText.TrimStartAndEnd();

		return Text.StartsWith(TEXT("/")) || Text.Contains(TEXT(".")) || Text.Contains(TEXT("'"));

	}



	static UScriptStruct* ResolveCoreStructByName(const FString& StructName)

	{

		if (StructName.IsEmpty())

		{

			return nullptr;

		}

		const FString StructPath = FString::Printf(TEXT("/Script/CoreUObject.%s"), *StructName);

		if (UScriptStruct* FoundStruct = FindObject<UScriptStruct>(nullptr, *StructPath))

		{

			return FoundStruct;

		}

		return LoadObject<UScriptStruct>(nullptr, *StructPath);

	}



	static UScriptStruct* ResolveStructByPath(const FString& InPath)

	{

		FString StructPath = InPath.TrimStartAndEnd();

		if (StructPath.IsEmpty())

		{

			return nullptr;

		}

		if (UScriptStruct* Existing = FindObject<UScriptStruct>(nullptr, *StructPath))

		{

			return Existing;

		}

		return LoadObject<UScriptStruct>(nullptr, *StructPath);

	}



	static UEnum* ResolveEnumByPath(const FString& InPath)

	{

		FString EnumPath = InPath.TrimStartAndEnd();

		if (EnumPath.IsEmpty())

		{

			return nullptr;

		}

		if (UEnum* Existing = FindObject<UEnum>(nullptr, *EnumPath))

		{

			return Existing;

		}

		return LoadObject<UEnum>(nullptr, *EnumPath);

	}



	static UEnum* ResolveCommonEnumObject(const FString& InTypeName)

	{

		if (LooksLikeReflectedObjectPath(InTypeName))

		{

			if (UEnum* EnumByPath = ResolveEnumByPath(InTypeName))

			{

				return EnumByPath;

			}

		}

		const FString Token = NormalizePinTypeToken(InTypeName);

		if (Token.IsEmpty())

		{

			return nullptr;

		}

		struct FEnumAlias

		{

			const TCHAR* Alias;

			const TCHAR* Path;

		};

		static const FEnumAlias EnumAliases[] = {

			{ TEXT("collisionchannel"), TEXT("/Script/Engine.ECollisionChannel") },
			{ TEXT("ecollisionchannel"), TEXT("/Script/Engine.ECollisionChannel") },
			{ TEXT("objecttypequery"), TEXT("/Script/Engine.EObjectTypeQuery") },
			{ TEXT("eobjecttypequery"), TEXT("/Script/Engine.EObjectTypeQuery") },
			{ TEXT("tracetypequery"), TEXT("/Script/Engine.ETraceTypeQuery") },
			{ TEXT("etracetypequery"), TEXT("/Script/Engine.ETraceTypeQuery") },
			{ TEXT("inputevent"), TEXT("/Script/Engine.EInputEvent") },
			{ TEXT("einputevent"), TEXT("/Script/Engine.EInputEvent") },
			{ TEXT("controllerhand"), TEXT("/Script/InputCore.EControllerHand") },
			{ TEXT("econtrollerhand"), TEXT("/Script/InputCore.EControllerHand") },
			{ TEXT("touchindex"), TEXT("/Script/InputCore.ETouchIndex") },
			{ TEXT("etouchindex"), TEXT("/Script/InputCore.ETouchIndex") },
			{ TEXT("physicalsurface"), TEXT("/Script/PhysicsCore.EPhysicalSurface") },
			{ TEXT("ephysicalsurface"), TEXT("/Script/PhysicsCore.EPhysicalSurface") }

		};

		for (const FEnumAlias& Alias : EnumAliases)

		{

			if (Token == Alias.Alias)

			{

				return ResolveEnumByPath(Alias.Path);

			}

		}

		return nullptr;

	}



	static UScriptStruct* ResolveCommonStructObject(const FString& InTypeName)

	{

		if (LooksLikeReflectedObjectPath(InTypeName))

		{

			if (UScriptStruct* StructByPath = ResolveStructByPath(InTypeName))

			{

				return StructByPath;

			}

		}

		const FString Token = NormalizePinTypeToken(InTypeName);

		if (Token.IsEmpty())

		{

			return nullptr;

		}

		FString StructName;

		if (Token == TEXT("vector") || Token == TEXT("fvector"))

		{

			StructName = TEXT("Vector");

		}

		else if (Token == TEXT("vector2d") || Token == TEXT("fvector2d"))

		{

			StructName = TEXT("Vector2D");

		}

		else if (Token == TEXT("vector4") || Token == TEXT("fvector4"))

		{

			StructName = TEXT("Vector4");

		}

		else if (Token == TEXT("rotator") || Token == TEXT("frotator"))

		{

			StructName = TEXT("Rotator");

		}

		else if (Token == TEXT("transform") || Token == TEXT("ftransform"))

		{

			StructName = TEXT("Transform");

		}

		else if (Token == TEXT("linearcolor") || Token == TEXT("flinearcolor"))

		{

			StructName = TEXT("LinearColor");

		}

		else if (Token == TEXT("color") || Token == TEXT("fcolor"))

		{

			StructName = TEXT("Color");

		}

		else if (Token == TEXT("quat") || Token == TEXT("fquat") || Token == TEXT("quaternion"))

		{

			StructName = TEXT("Quat");

		}

		else if (Token == TEXT("intpoint") || Token == TEXT("fintpoint"))

		{

			StructName = TEXT("IntPoint");

		}

		else if (Token == TEXT("intvector") || Token == TEXT("fintvector"))

		{

			StructName = TEXT("IntVector");

		}

		else if (Token == TEXT("int64vector2") || Token == TEXT("fint64vector2"))

		{

			StructName = TEXT("Int64Vector2");

		}

		else if (Token == TEXT("intvector4") || Token == TEXT("fintvector4"))

		{

			StructName = TEXT("IntVector4");

		}

		else if (Token == TEXT("guid") || Token == TEXT("fguid"))

		{

			StructName = TEXT("Guid");

		}

		else if (Token == TEXT("datetime") || Token == TEXT("fdatetime"))

		{

			StructName = TEXT("DateTime");

		}

		else if (Token == TEXT("timespan") || Token == TEXT("ftimespan"))

		{

			StructName = TEXT("Timespan");

		}

		else if (Token == TEXT("randomstream") || Token == TEXT("frandomstream"))

		{

			StructName = TEXT("RandomStream");

		}

		else if (Token == TEXT("framenumber") || Token == TEXT("fframenumber"))

		{

			StructName = TEXT("FrameNumber");

		}

		else if (Token == TEXT("frametime") || Token == TEXT("fframetime"))

		{

			StructName = TEXT("FrameTime");

		}

		else if (Token == TEXT("framerate") || Token == TEXT("fframerate"))

		{

			StructName = TEXT("FrameRate");

		}

		else if (Token == TEXT("softobjectpath") || Token == TEXT("fsoftobjectpath"))

		{

			StructName = TEXT("SoftObjectPath");

		}

		else if (Token == TEXT("softclasspath") || Token == TEXT("fsoftclasspath"))

		{

			StructName = TEXT("SoftClassPath");

		}

		else if (Token == TEXT("primaryassettype") || Token == TEXT("fprimaryassettype"))

		{

			StructName = TEXT("PrimaryAssetType");

		}

		else if (Token == TEXT("primaryassetid") || Token == TEXT("fprimaryassetid"))

		{

			StructName = TEXT("PrimaryAssetId");

		}

		else if (Token == TEXT("toplevelassetpath") || Token == TEXT("ftoplevelassetpath"))

		{

			StructName = TEXT("TopLevelAssetPath");

		}

		else if (Token == TEXT("plane") || Token == TEXT("fplane"))

		{

			StructName = TEXT("Plane");

		}

		else if (Token == TEXT("box2d") || Token == TEXT("fbox2d"))

		{

			StructName = TEXT("Box2D");

		}

		else if (Token == TEXT("twovectors") || Token == TEXT("ftwovectors"))

		{

			StructName = TEXT("TwoVectors");

		}

		else if (Token == TEXT("box") || Token == TEXT("fbox"))

		{

			StructName = TEXT("Box");

		}

		else if (Token == TEXT("boxspherebounds") || Token == TEXT("fboxspherebounds") || Token == TEXT("bounds"))

		{

			StructName = TEXT("BoxSphereBounds");

		}

		if (!StructName.IsEmpty())

		{

			if (UScriptStruct* CoreStruct = ResolveCoreStructByName(StructName))

			{

				return CoreStruct;

			}

		}

		struct FStructAlias

		{

			const TCHAR* Alias;

			const TCHAR* Path;

		};

		static const FStructAlias StructAliases[] = {

			{ TEXT("hitresult"), TEXT("/Script/Engine.HitResult") },
			{ TEXT("fhitresult"), TEXT("/Script/Engine.HitResult") },
			{ TEXT("timerhandle"), TEXT("/Script/Engine.TimerHandle") },
			{ TEXT("ftimerhandle"), TEXT("/Script/Engine.TimerHandle") },
			{ TEXT("datatablerowhandle"), TEXT("/Script/Engine.DataTableRowHandle") },
			{ TEXT("fdatatablerowhandle"), TEXT("/Script/Engine.DataTableRowHandle") },
			{ TEXT("curvetablerowhandle"), TEXT("/Script/Engine.CurveTableRowHandle") },
			{ TEXT("fcurvetablerowhandle"), TEXT("/Script/Engine.CurveTableRowHandle") },
			{ TEXT("tablerowbase"), TEXT("/Script/Engine.TableRowBase") },
			{ TEXT("ftablerowbase"), TEXT("/Script/Engine.TableRowBase") },
			{ TEXT("componentreference"), TEXT("/Script/Engine.ComponentReference") },
			{ TEXT("fcomponentreference"), TEXT("/Script/Engine.ComponentReference") },
			{ TEXT("softcomponentreference"), TEXT("/Script/Engine.SoftComponentReference") },
			{ TEXT("fsoftcomponentreference"), TEXT("/Script/Engine.SoftComponentReference") },
			{ TEXT("latentactioninfo"), TEXT("/Script/Engine.LatentActionInfo") },
			{ TEXT("flatentactioninfo"), TEXT("/Script/Engine.LatentActionInfo") },
			{ TEXT("vectornetquantize"), TEXT("/Script/Engine.Vector_NetQuantize") },
			{ TEXT("vectornetquantize10"), TEXT("/Script/Engine.Vector_NetQuantize10") },
			{ TEXT("vectornetquantize100"), TEXT("/Script/Engine.Vector_NetQuantize100") },
			{ TEXT("vectornetquantizenormal"), TEXT("/Script/Engine.Vector_NetQuantizeNormal") },
			{ TEXT("inputchord"), TEXT("/Script/Slate.InputChord") },
			{ TEXT("finputchord"), TEXT("/Script/Slate.InputChord") },
			{ TEXT("gameplaytag"), TEXT("/Script/GameplayTags.GameplayTag") },
			{ TEXT("fgameplaytag"), TEXT("/Script/GameplayTags.GameplayTag") },
			{ TEXT("gameplaytagcontainer"), TEXT("/Script/GameplayTags.GameplayTagContainer") },
			{ TEXT("fgameplaytagcontainer"), TEXT("/Script/GameplayTags.GameplayTagContainer") },
			{ TEXT("gameplaytagquery"), TEXT("/Script/GameplayTags.GameplayTagQuery") },
			{ TEXT("fgameplaytagquery"), TEXT("/Script/GameplayTags.GameplayTagQuery") },
			{ TEXT("key"), TEXT("/Script/InputCore.Key") },
			{ TEXT("fkey"), TEXT("/Script/InputCore.Key") },
			{ TEXT("margin"), TEXT("/Script/SlateCore.Margin") },
			{ TEXT("fmargin"), TEXT("/Script/SlateCore.Margin") },
			{ TEXT("slatecolor"), TEXT("/Script/SlateCore.SlateColor") },
			{ TEXT("fslatecolor"), TEXT("/Script/SlateCore.SlateColor") },
			{ TEXT("slatebrush"), TEXT("/Script/SlateCore.SlateBrush") },
			{ TEXT("fslatebrush"), TEXT("/Script/SlateCore.SlateBrush") },
			{ TEXT("slatefontinfo"), TEXT("/Script/SlateCore.SlateFontInfo") },
			{ TEXT("fslatefontinfo"), TEXT("/Script/SlateCore.SlateFontInfo") },
			{ TEXT("widgettransform"), TEXT("/Script/UMG.WidgetTransform") },
			{ TEXT("fwidgettransform"), TEXT("/Script/UMG.WidgetTransform") },
			{ TEXT("anchors"), TEXT("/Script/Slate.Anchors") },
			{ TEXT("fanchors"), TEXT("/Script/Slate.Anchors") },
			{ TEXT("anchordata"), TEXT("/Script/UMG.AnchorData") },
			{ TEXT("fanchordata"), TEXT("/Script/UMG.AnchorData") }

		};

		for (const FStructAlias& Alias : StructAliases)

		{

			if (Token == Alias.Alias)

			{

				return ResolveStructByPath(Alias.Path);

			}

		}

		return nullptr;

	}



	static UClass* ResolveCommonClassObject(const FString& InTypeName)

	{

		if (LooksLikeReflectedObjectPath(InTypeName))

		{

			if (UClass* ClassByPath = ResolveClassByPath(InTypeName))

			{

				return ClassByPath;

			}

		}

		const FString Token = NormalizePinTypeToken(InTypeName);

		if (Token.IsEmpty())

		{

			return nullptr;

		}

		struct FClassAlias

		{

			const TCHAR* Alias;

			const TCHAR* Path;

		};

		static const FClassAlias ClassAliases[] = {

			{ TEXT("object"), TEXT("/Script/CoreUObject.Object") },
			{ TEXT("uobject"), TEXT("/Script/CoreUObject.Object") },
			{ TEXT("actor"), TEXT("/Script/Engine.Actor") },
			{ TEXT("pawn"), TEXT("/Script/Engine.Pawn") },
			{ TEXT("character"), TEXT("/Script/Engine.Character") },
			{ TEXT("controller"), TEXT("/Script/Engine.Controller") },
			{ TEXT("playercontroller"), TEXT("/Script/Engine.PlayerController") },
			{ TEXT("gamemodebase"), TEXT("/Script/Engine.GameModeBase") },
			{ TEXT("gamestatebase"), TEXT("/Script/Engine.GameStateBase") },
			{ TEXT("playerstate"), TEXT("/Script/Engine.PlayerState") },
			{ TEXT("hud"), TEXT("/Script/Engine.HUD") },
			{ TEXT("playercameramanager"), TEXT("/Script/Engine.PlayerCameraManager") },
			{ TEXT("gameinstance"), TEXT("/Script/Engine.GameInstance") },
			{ TEXT("savegame"), TEXT("/Script/Engine.SaveGame") },
			{ TEXT("world"), TEXT("/Script/Engine.World") },
			{ TEXT("level"), TEXT("/Script/Engine.Level") },
			{ TEXT("worldsubsystem"), TEXT("/Script/Engine.WorldSubsystem") },
			{ TEXT("gameinstancesubsystem"), TEXT("/Script/Engine.GameInstanceSubsystem") },
			{ TEXT("enginesubsystem"), TEXT("/Script/Engine.EngineSubsystem") },
			{ TEXT("actorcomponent"), TEXT("/Script/Engine.ActorComponent") },
			{ TEXT("scenecomponent"), TEXT("/Script/Engine.SceneComponent") },
			{ TEXT("primitivecomponent"), TEXT("/Script/Engine.PrimitiveComponent") },
			{ TEXT("staticmeshcomponent"), TEXT("/Script/Engine.StaticMeshComponent") },
			{ TEXT("skeletalmeshcomponent"), TEXT("/Script/Engine.SkeletalMeshComponent") },
			{ TEXT("cameracomponent"), TEXT("/Script/Engine.CameraComponent") },
			{ TEXT("scenecapturecomponent2d"), TEXT("/Script/Engine.SceneCaptureComponent2D") },
			{ TEXT("springarmcomponent"), TEXT("/Script/Engine.SpringArmComponent") },
			{ TEXT("audiocomponent"), TEXT("/Script/Engine.AudioComponent") },
			{ TEXT("timelinecomponent"), TEXT("/Script/Engine.TimelineComponent") },
			{ TEXT("particlesystemcomponent"), TEXT("/Script/Engine.ParticleSystemComponent") },
			{ TEXT("widgetcomponent"), TEXT("/Script/UMG.WidgetComponent") },
			{ TEXT("niagaracomponent"), TEXT("/Script/Niagara.NiagaraComponent") },
			{ TEXT("staticmesh"), TEXT("/Script/Engine.StaticMesh") },
			{ TEXT("skeletalmesh"), TEXT("/Script/Engine.SkeletalMesh") },
			{ TEXT("materialinterface"), TEXT("/Script/Engine.MaterialInterface") },
			{ TEXT("material"), TEXT("/Script/Engine.Material") },
			{ TEXT("materialinstance"), TEXT("/Script/Engine.MaterialInstance") },
			{ TEXT("materialinstanceconstant"), TEXT("/Script/Engine.MaterialInstanceConstant") },
			{ TEXT("texture"), TEXT("/Script/Engine.Texture") },
			{ TEXT("texture2d"), TEXT("/Script/Engine.Texture2D") },
			{ TEXT("texturerendertarget2d"), TEXT("/Script/Engine.TextureRenderTarget2D") },
			{ TEXT("soundbase"), TEXT("/Script/Engine.SoundBase") },
			{ TEXT("soundwave"), TEXT("/Script/Engine.SoundWave") },
			{ TEXT("soundcue"), TEXT("/Script/Engine.SoundCue") },
			{ TEXT("particlesystem"), TEXT("/Script/Engine.ParticleSystem") },
			{ TEXT("animsequence"), TEXT("/Script/Engine.AnimSequence") },
			{ TEXT("animmontage"), TEXT("/Script/Engine.AnimMontage") },
			{ TEXT("blendspace"), TEXT("/Script/Engine.BlendSpace") },
			{ TEXT("animblueprint"), TEXT("/Script/Engine.AnimBlueprint") },
			{ TEXT("animinstance"), TEXT("/Script/Engine.AnimInstance") },
			{ TEXT("skeleton"), TEXT("/Script/Engine.Skeleton") },
			{ TEXT("physicsasset"), TEXT("/Script/Engine.PhysicsAsset") },
			{ TEXT("datatable"), TEXT("/Script/Engine.DataTable") },
			{ TEXT("dataasset"), TEXT("/Script/Engine.DataAsset") },
			{ TEXT("primarydataasset"), TEXT("/Script/Engine.PrimaryDataAsset") },
			{ TEXT("curvefloat"), TEXT("/Script/Engine.CurveFloat") },
			{ TEXT("curvevector"), TEXT("/Script/Engine.CurveVector") },
			{ TEXT("curvelinearcolor"), TEXT("/Script/Engine.CurveLinearColor") },
			{ TEXT("blueprint"), TEXT("/Script/Engine.Blueprint") },
			{ TEXT("font"), TEXT("/Script/Engine.Font") },
			{ TEXT("slatebrushasset"), TEXT("/Script/UMG.SlateBrushAsset") },
			{ TEXT("userwidget"), TEXT("/Script/UMG.UserWidget") },
			{ TEXT("widget"), TEXT("/Script/UMG.Widget") },
			{ TEXT("levelsequence"), TEXT("/Script/LevelSequence.LevelSequence") },
			{ TEXT("niagarasystem"), TEXT("/Script/Niagara.NiagaraSystem") },
			{ TEXT("niagaraemitter"), TEXT("/Script/Niagara.NiagaraEmitter") },
			{ TEXT("inputaction"), TEXT("/Script/EnhancedInput.InputAction") },
			{ TEXT("inputmappingcontext"), TEXT("/Script/EnhancedInput.InputMappingContext") }

		};

		for (const FClassAlias& Alias : ClassAliases)

		{

			if (Token == Alias.Alias)

			{

				return ResolveClassByPath(Alias.Path);

			}

		}

		return nullptr;

	}



	static UObject* ResolvePinSubCategoryObjectOrAlias(const FString& InPathOrAlias, UBlueprint* Blueprint)

	{

		UClass* SelfClass = nullptr;
		if (Blueprint && UeAgentBlueprintAssetLifecycle::TryResolveAuthoringSelfClass(*Blueprint, InPathOrAlias, SelfClass))
			return SelfClass;

		if (UScriptStruct* CommonStruct = ResolveCommonStructObject(InPathOrAlias))

		{

			return CommonStruct;

		}

		if (UClass* CommonClass = ResolveCommonClassObject(InPathOrAlias))

		{

			return CommonClass;

		}

		if (UEnum* CommonEnum = ResolveCommonEnumObject(InPathOrAlias))

		{

			return CommonEnum;

		}

		return LooksLikeReflectedObjectPath(InPathOrAlias) ? ResolvePinSubCategoryObject(InPathOrAlias) : nullptr;

	}



	static bool IsSupportedVariablePinCategory(const FName PinCategory)

	{

		return PinCategory == UEdGraphSchema_K2::PC_Boolean

			|| PinCategory == UEdGraphSchema_K2::PC_Int

			|| PinCategory == UEdGraphSchema_K2::PC_Int64

			|| PinCategory == UEdGraphSchema_K2::PC_Byte

			|| PinCategory == UEdGraphSchema_K2::PC_Enum

			|| PinCategory == UEdGraphSchema_K2::PC_Real

			|| PinCategory == UEdGraphSchema_K2::PC_Name

			|| PinCategory == UEdGraphSchema_K2::PC_String

			|| PinCategory == UEdGraphSchema_K2::PC_Text

			|| PinCategory == UEdGraphSchema_K2::PC_Struct

			|| PinCategory == UEdGraphSchema_K2::PC_Object

			|| PinCategory == UEdGraphSchema_K2::PC_Class

			|| PinCategory == UEdGraphSchema_K2::PC_SoftObject

			|| PinCategory == UEdGraphSchema_K2::PC_SoftClass

			|| PinCategory == UEdGraphSchema_K2::PC_Interface

			|| PinCategory == UEdGraphSchema_K2::PC_FieldPath;

	}



	static TSharedPtr<FJsonValue> PinTypeFieldValue(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const auto Value = Object.IsValid() ? Object->TryGetField(Field) : TSharedPtr<FJsonValue>();
		return Value.IsValid() ? Value : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	}

	static FDiagnostic PinTypeFailure(const FString& Code, const FString& Reason, const FString& Field,
		TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
		const TSharedPtr<FJsonObject>& TypeObj, const UBlueprint* Blueprint)
	{
		FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("pin_type_decode"));
		Error.SemanticPointer = Field.IsEmpty() ? TEXT("/") : TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		Error.ActualValue = MoveTemp(Actual);
		Error.ExpectedValue = MoveTemp(Expected);
		if (Blueprint) Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
		if (TypeObj.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *TypeObj, Field);
		Error.PropertyPath = Field;
		return Error;
	}

	static bool ParsePinContainerTypeStrict(const FString& InContainerType, EPinContainerType& OutContainerType, FDiagnostic& OutError,
		const TSharedPtr<FJsonObject>& TypeObj, const UBlueprint* Blueprint)

	{

		const FString ContainerType = InContainerType.TrimStartAndEnd();

		if (ContainerType.IsEmpty())

		{

			OutContainerType = EPinContainerType::None;

			return true;

		}

		if (ContainerType.Equals(TEXT("array"), ESearchCase::IgnoreCase))

		{

			OutContainerType = EPinContainerType::Array;

			return true;

		}

		if (ContainerType.Equals(TEXT("set"), ESearchCase::IgnoreCase))

		{

			OutContainerType = EPinContainerType::Set;

			return true;

		}

		if (ContainerType.Equals(TEXT("map"), ESearchCase::IgnoreCase))

		{

			OutContainerType = EPinContainerType::Map;

			return true;

		}

		OutError = PinTypeFailure(TEXT("invalid_container_type"), TEXT("The pin container type is not recognized."),
			TEXT("container_type"), MakeShared<FJsonValueString>(InContainerType), MakeShared<FJsonValueString>(TEXT("empty, array, set, or map")), TypeObj, Blueprint);

		return false;

	}



	static bool BuildPinTypePrimaryFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FDiagnostic& OutError, UBlueprint* Blueprint)

	{

		if (!TypeObj.IsValid())

		{

			OutError = PinTypeFailure(TEXT("missing_pin_type"), TEXT("The pin type must be a JSON object."),
				FString(), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("an object describing the pin type")), TypeObj, Blueprint);

			return false;

		}

		FString PinCategoryText;

		if (!TypeObj->TryGetStringField(TEXT("pin_category"), PinCategoryText) || PinCategoryText.TrimStartAndEnd().IsEmpty())

		{

			OutError = PinTypeFailure(TEXT("missing_pin_category"), TEXT("The pin category must be a non-empty string."),
				TEXT("pin_category"), PinTypeFieldValue(TypeObj, TEXT("pin_category")), MakeShared<FJsonValueString>(TEXT("a non-empty pin category")), TypeObj, Blueprint);

			return false;

		}

		OutPinType = FEdGraphPinType();

		if (UScriptStruct* CommonStruct = ResolveCommonStructObject(PinCategoryText))

		{

			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;

			OutPinType.PinSubCategoryObject = CommonStruct;

		}

		else if (UEnum* CommonEnum = ResolveCommonEnumObject(PinCategoryText))

		{

			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;

			OutPinType.PinSubCategoryObject = CommonEnum;

		}

		else

		{

			OutPinType.PinCategory = NormalizePinCategoryName(PinCategoryText);

		}

		if (OutPinType.PinCategory.IsNone() || !IsSupportedVariablePinCategory(OutPinType.PinCategory))

		{

			OutError = PinTypeFailure(TEXT("invalid_pin_category"), TEXT("The requested pin category is not supported for Blueprint variables."),
				TEXT("pin_category"), MakeShared<FJsonValueString>(PinCategoryText),
				MakeShared<FJsonValueString>(TEXT("bool, int, int64, byte, enum, real (float/double), name, string, text, struct, object, class, softobject, softclass, interface, or fieldpath")), TypeObj, Blueprint);

			return false;

		}

		FString PinSubCategoryText;

		TypeObj->TryGetStringField(TEXT("pin_subcategory"), PinSubCategoryText);

		PinSubCategoryText.TrimStartAndEndInline();

		if (!PinSubCategoryText.IsEmpty())

		{

			if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)

			{

				OutPinType.PinSubCategory = NormalizeRealPinSubCategoryName(PinSubCategoryText);

			}

			else if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Byte || OutPinType.PinCategory == UEdGraphSchema_K2::PC_Enum)

			{

				if (UEnum* CommonEnum = ResolveCommonEnumObject(PinSubCategoryText))

				{

					OutPinType.PinSubCategoryObject = CommonEnum;

				}

				else

				{

					OutPinType.PinSubCategory = FName(*PinSubCategoryText);

				}

			}

			else if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct)

			{

				if (UScriptStruct* CommonStruct = ResolveCommonStructObject(PinSubCategoryText))

				{

					OutPinType.PinSubCategoryObject = CommonStruct;

				}

				else

				{

					OutPinType.PinSubCategory = FName(*PinSubCategoryText);

				}

			}

			else

			{

				OutPinType.PinSubCategory = FName(*PinSubCategoryText);

			}

		}

		FString PinSubCategoryObjectPath;

		TypeObj->TryGetStringField(TEXT("pin_subcategory_object"), PinSubCategoryObjectPath);

		if (!PinSubCategoryObjectPath.TrimStartAndEnd().IsEmpty())

		{

			UObject* SubCategoryObject = ResolvePinSubCategoryObjectOrAlias(PinSubCategoryObjectPath, Blueprint);

			if (!SubCategoryObject)

			{

				OutError = PinTypeFailure(TEXT("invalid_pin_subcategory_object"), TEXT("The pin subcategory object path or alias could not be resolved."),
					TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(PinSubCategoryObjectPath),
					MakeShared<FJsonValueString>(TEXT("a resolvable object path or supported pin type alias")), TypeObj, Blueprint);

				return false;

			}

			OutPinType.PinSubCategoryObject = SubCategoryObject;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)

		{

			if (OutPinType.PinSubCategory.IsNone())

			{

				OutPinType.PinSubCategory = PinCategoryText.TrimStartAndEnd().Equals(TEXT("double"), ESearchCase::IgnoreCase)
					? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;

			}

			if (OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Float && OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Double)

			{

				OutError = PinTypeFailure(TEXT("invalid_real_pin_subcategory"), TEXT("A real pin subcategory must resolve to float or double."),
					TEXT("pin_subcategory"), MakeShared<FJsonValueString>(PinSubCategoryText),
					MakeShared<FJsonValueString>(TEXT("float or double")), TypeObj, Blueprint);

				return false;

			}

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct)

		{

			UObject* StructObject = OutPinType.PinSubCategoryObject.Get();

			if (!StructObject)

			{

				OutError = PinTypeFailure(TEXT("missing_struct_pin_subcategory_object"), TEXT("The struct pin has no resolved UScriptStruct; supply a struct object path or a supported struct alias."),
					TEXT("pin_subcategory_object"), PinTypeFieldValue(TypeObj, TEXT("pin_subcategory_object")), MakeShared<FJsonValueString>(TEXT("UScriptStruct")), TypeObj, Blueprint);

				return false;

			}

			if (!Cast<UScriptStruct>(StructObject))

			{

				OutError = PinTypeFailure(TEXT("invalid_struct_pin_subcategory_object"),
					FString::Printf(TEXT("The struct pin object resolves to '%s', which is not a UScriptStruct."), *StructObject->GetClass()->GetPathName()),
					TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(StructObject->GetPathName()), MakeShared<FJsonValueString>(TEXT("UScriptStruct")), TypeObj, Blueprint);

				return false;

			}

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Enum)

		{

			UObject* EnumObject = OutPinType.PinSubCategoryObject.Get();

			if (!EnumObject)

			{

				OutError = PinTypeFailure(TEXT("missing_enum_pin_subcategory_object"), TEXT("The enum pin has no resolved UEnum; supply an enum object path or a supported enum alias."),
					TEXT("pin_subcategory_object"), PinTypeFieldValue(TypeObj, TEXT("pin_subcategory_object")), MakeShared<FJsonValueString>(TEXT("UEnum")), TypeObj, Blueprint);

				return false;

			}

			if (!Cast<UEnum>(EnumObject))

			{

				OutError = PinTypeFailure(TEXT("invalid_enum_pin_subcategory_object"),
					FString::Printf(TEXT("The enum pin object resolves to '%s', which is not a UEnum."), *EnumObject->GetClass()->GetPathName()),
					TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(EnumObject->GetPathName()), MakeShared<FJsonValueString>(TEXT("UEnum")), TypeObj, Blueprint);

				return false;

			}

			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Byte && OutPinType.PinSubCategoryObject.IsValid() && !Cast<UEnum>(OutPinType.PinSubCategoryObject.Get()))

		{

			OutError = PinTypeFailure(TEXT("invalid_byte_enum_pin_subcategory_object"),
				FString::Printf(TEXT("The byte pin object resolves to '%s'; byte pin objects must be enums."), *OutPinType.PinSubCategoryObject->GetClass()->GetPathName()),
				TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(OutPinType.PinSubCategoryObject->GetPathName()), MakeShared<FJsonValueString>(TEXT("UEnum")), TypeObj, Blueprint);

			return false;

		}

		if ((OutPinType.PinCategory == UEdGraphSchema_K2::PC_Object

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_Class

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)

			&& !OutPinType.PinSubCategoryObject.IsValid())

		{

			OutPinType.PinSubCategoryObject = UObject::StaticClass();

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Interface && !OutPinType.PinSubCategoryObject.IsValid())

		{

			OutError = PinTypeFailure(TEXT("missing_interface_pin_subcategory_object"), TEXT("The interface pin has no resolved interface class."),
				TEXT("pin_subcategory_object"), PinTypeFieldValue(TypeObj, TEXT("pin_subcategory_object")), MakeShared<FJsonValueString>(TEXT("a UClass with CLASS_Interface")), TypeObj, Blueprint);

			return false;

		}

		if ((OutPinType.PinCategory == UEdGraphSchema_K2::PC_Object

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_Class

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass

				|| OutPinType.PinCategory == UEdGraphSchema_K2::PC_Interface)

			&& OutPinType.PinSubCategoryObject.IsValid()

			&& !Cast<UClass>(OutPinType.PinSubCategoryObject.Get()))

		{

			OutError = PinTypeFailure(TEXT("invalid_class_pin_subcategory_object"),
				FString::Printf(TEXT("The '%s' pin object resolves to '%s', which is not a UClass."),
					*OutPinType.PinCategory.ToString(), *OutPinType.PinSubCategoryObject->GetClass()->GetPathName()),
				TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(OutPinType.PinSubCategoryObject->GetPathName()), MakeShared<FJsonValueString>(TEXT("UClass")), TypeObj, Blueprint);

			return false;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Interface)

		{

			const UClass* InterfaceClass = Cast<UClass>(OutPinType.PinSubCategoryObject.Get());

			if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))

			{

				OutError = PinTypeFailure(TEXT("invalid_interface_pin_subcategory_object"), TEXT("The resolved pin class is not marked as an Unreal interface."),
					TEXT("pin_subcategory_object"), MakeShared<FJsonValueString>(InterfaceClass ? InterfaceClass->GetPathName() : FString()),
					MakeShared<FJsonValueString>(TEXT("a UClass with CLASS_Interface")), TypeObj, Blueprint);

				return false;

			}

		}

		FString ContainerTypeText;

		TypeObj->TryGetStringField(TEXT("container_type"), ContainerTypeText);

		if (!ParsePinContainerTypeStrict(ContainerTypeText, OutPinType.ContainerType, OutError, TypeObj, Blueprint))

		{

			return false;

		}

		return true;

	}



	bool BuildPinTypeFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FDiagnostic& OutError, UBlueprint* Blueprint)

	{

		if (!BuildPinTypePrimaryFromJsonObject(TypeObj, OutPinType, OutError, Blueprint))

		{

			return false;

		}

		if (OutPinType.ContainerType != EPinContainerType::Map)

		{

			OutPinType.PinValueType = FEdGraphTerminalType();

			return true;

		}

		const TSharedPtr<FJsonObject>* ValueTypeObj = nullptr;

		if (!TypeObj.IsValid() || !TypeObj->TryGetObjectField(TEXT("value_type"), ValueTypeObj) || !ValueTypeObj || !ValueTypeObj->IsValid())

		{

			OutError = PinTypeFailure(TEXT("missing_map_value_type"), TEXT("A map pin requires a value_type object."),
				TEXT("value_type"), PinTypeFieldValue(TypeObj, TEXT("value_type")), MakeShared<FJsonValueString>(TEXT("an object describing the map value type")), TypeObj, Blueprint);

			return false;

		}

		FEdGraphPinType ValuePinType;

if (!BuildPinTypePrimaryFromJsonObject(*ValueTypeObj, ValuePinType, OutError, Blueprint))

		{
			UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/value_type"));

			return false;

		}

		if (ValuePinType.ContainerType != EPinContainerType::None)

		{

			OutError = PinTypeFailure(TEXT("nested_container_value_type_not_supported"), TEXT("A map value pin cannot itself be an array, set, or map."),
				TEXT("container_type"), PinTypeFieldValue(*ValueTypeObj, TEXT("container_type")),
				MakeShared<FJsonValueString>(TEXT("empty (no container)")), *ValueTypeObj, Blueprint);
			UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/value_type"));

			return false;

		}

		OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;

		OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;

		OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;

		return true;

	}

	bool TryDescribePinDefault(
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		UeAgentAuthoringTypeDescriptors::FDescriptor& OutType,
		TSharedPtr<FJsonValue>& OutValue,
		FDiagnostic& OutError,
		TSharedPtr<FJsonObject>* OutValueDefinition)
	{
		OutType = {};
		OutValue.Reset();
		OutError.Reset();
		if (OutValueDefinition) OutValueDefinition->Reset();
		const auto FailDefault = [&](const TCHAR* Code, const FString& Expected, const TCHAR* Reason)
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("pin_type_decode"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("default_value");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			OutError.ActualValue = MakeShared<FJsonValueString>(DefaultValue);
			return false;
		};
		if (PinType.ContainerType != EPinContainerType::None)
		{
			OutType.Kind = TEXT("string");
			OutValue = MakeShared<FJsonValueString>(DefaultValue);
			return true;
		}
		const FName Category = PinType.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			OutType.Kind = TEXT("boolean");
			if (DefaultValue.IsEmpty()
				|| DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
				|| DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			{
				OutValue = MakeShared<FJsonValueBoolean>(
					DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase));
				return true;
			}
			return FailDefault(TEXT("pin_default_boolean_invalid"), TEXT("true or false"),
				TEXT("The pin default is not a boolean."));
		}
		if (Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| (Category == UEdGraphSchema_K2::PC_Byte
				&& !PinType.PinSubCategoryObject.IsValid()))
		{
			int64 Parsed = 0;
			if (!DefaultValue.IsEmpty() && !LexTryParseString(Parsed, *DefaultValue))
			{
				return FailDefault(TEXT("pin_default_integer_invalid"), Category.ToString(),
					TEXT("The pin default is not an integer."));
			}
			OutType.Kind = Category == UEdGraphSchema_K2::PC_Int64
				? TEXT("int64")
				: Category == UEdGraphSchema_K2::PC_Byte ? TEXT("uint8") : TEXT("int32");
			OutValue = MakeShared<FJsonValueNumberString>(DefaultValue.IsEmpty() ? TEXT("0") : DefaultValue);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Real)
		{
			double Parsed = 0.0;
			if (!DefaultValue.IsEmpty() && !LexTryParseString(Parsed, *DefaultValue))
			{
				return FailDefault(TEXT("pin_default_decimal_invalid"), PinType.PinSubCategory.ToString(),
					TEXT("The pin default is not a decimal number."));
			}
			OutType.Kind = PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double
				? TEXT("double") : TEXT("float");
			OutValue = MakeShared<FJsonValueNumberString>(DefaultValue.IsEmpty() ? TEXT("0.0") : DefaultValue);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_String
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_Text)
		{
			OutType.Kind = Category == UEdGraphSchema_K2::PC_Name
				? TEXT("name")
				: Category == UEdGraphSchema_K2::PC_Text ? TEXT("text") : TEXT("string");
			OutValue = MakeShared<FJsonValueString>(DefaultValue);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
			if (!Struct)
			{
				OutError = FDiagnostic::Failure(TEXT("pin_default_struct_type_missing"),
					TEXT("The struct pin has no resolved struct type."), TEXT("pin_type_decode"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("pin_subcategory_object");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("UScriptStruct"));
				OutError.ActualValuePresent = PinType.PinSubCategoryObject.IsValid();
				if (PinType.PinSubCategoryObject.IsValid())
					OutError.ActualValue = MakeShared<FJsonValueString>(PinType.PinSubCategoryObject->GetPathName());
				return false;
			}
			TUniquePtr<FStructProperty> Property(new FStructProperty(FFieldVariant(), NAME_None, RF_NoFlags));
			Property->Struct = Struct;
			Property->SetElementSize(Struct->GetStructureSize());
			FStructOnScope Storage(Struct);
			// Kismet owns pin-specific text forms (including vector shorthand).
			if (!DefaultValue.IsEmpty() && !FBlueprintEditorUtils::PropertyValueFromString_Direct(
					Property.Get(), DefaultValue, Storage.GetStructMemory()))
			{
				return FailDefault(TEXT("pin_default_struct_text_invalid"), Struct->GetPathName(),
					TEXT("The pin default could not be parsed as the required struct."));
			}
			UeAgentPropertyValueCodec::FOptions Options;
			Options.bCaptureStructFieldDescriptors = true;
			// This temporary bridge property has no reflected owner/metadata.
			// Describe the real struct directly rather than querying field metadata.
			OutType.Kind = UeAgentAuthoringTypeDescriptors::GetStructKind(Struct);
			// Unlike an FProperty edit, a pin projection has no native property at
			// decode time; retain the exact struct (including float/double variants).
			OutType.StructPath = Struct->GetPathName();
			OutValue = UeAgentPropertyValueCodec::EncodeStructValue(
				Struct, Storage.GetStructMemory(), nullptr, Options, 0, OutValueDefinition);
			return true;
		}
		OutType.Kind = TEXT("string");
		OutValue = MakeShared<FJsonValueString>(DefaultValue);
		return true;
	}

	bool TryEncodeStructPinDefaultText(const FString& StructPath,
		const TSharedPtr<FJsonValue>& Value, FString& OutText, FDiagnostic& OutError)
	{
		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
		if (!Struct)
		{
			OutError = FDiagnostic::Failure(TEXT("pin_default_struct_type_missing"),
				TEXT("The struct type required to encode this pin default could not be loaded."), TEXT("pin_type_decode"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("struct_path");
			OutError.ActualValue = MakeShared<FJsonValueString>(StructPath);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a loadable UScriptStruct"));
			return false;
		}
		TUniquePtr<FStructProperty> Property(new FStructProperty(FFieldVariant(), NAME_None, RF_NoFlags));
		Property->Struct = Struct;
		Property->SetElementSize(Struct->GetStructureSize());
		FStructOnScope Storage(Struct);
		if (!UeAgentPropertyValueCodec::DecodeStructValue(
				Struct, Value, Storage.GetStructMemory(), OutError)) return false;
		if (!FBlueprintEditorUtils::PropertyValueToString_Direct(
				Property.Get(), Storage.GetStructMemory(), OutText))
		{
			OutError = FDiagnostic::Failure(TEXT("pin_default_struct_text_encode_failed"),
				FString::Printf(TEXT("The native editor could not encode the decoded '%s' struct value as pin default text."), *Struct->GetPathName()),
				TEXT("pin_type_decode"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("default_value");
			OutError.ActualValue = Value;
			OutError.ActualValuePresent = Value.IsValid();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a struct value encodable as native pin default text"));
			return false;
		}
		return true;
	}

}
