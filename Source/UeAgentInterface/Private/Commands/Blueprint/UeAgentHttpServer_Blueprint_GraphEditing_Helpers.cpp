#include "UeAgentHttpServer_Blueprint.h"

#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"

#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentBlueprintOps

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



	static UObject* ResolvePinSubCategoryObjectOrAlias(const FString& InPathOrAlias)

	{

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



	static bool ParsePinContainerTypeStrict(const FString& InContainerType, EPinContainerType& OutContainerType, FString& OutError)

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

		OutError = TEXT("invalid_container_type");

		return false;

	}



	static bool BuildPinTypePrimaryFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FString& OutError)

	{

		if (!TypeObj.IsValid())

		{

			OutError = TEXT("missing_pin_type");

			return false;

		}

		FString PinCategoryText;

		if (!TypeObj->TryGetStringField(TEXT("pin_category"), PinCategoryText) || PinCategoryText.TrimStartAndEnd().IsEmpty())

		{

			OutError = TEXT("missing_pin_category");

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

			OutError = TEXT("invalid_pin_category");

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

			UObject* SubCategoryObject = ResolvePinSubCategoryObjectOrAlias(PinSubCategoryObjectPath);

			if (!SubCategoryObject)

			{

				OutError = TEXT("invalid_pin_subcategory_object");

				return false;

			}

			OutPinType.PinSubCategoryObject = SubCategoryObject;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)

		{

			if (OutPinType.PinSubCategory.IsNone())

			{

				OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

			}

			if (OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Float && OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Double)

			{

				OutError = TEXT("invalid_real_pin_subcategory");

				return false;

			}

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Struct)

		{

			UObject* StructObject = OutPinType.PinSubCategoryObject.Get();

			if (!StructObject)

			{

				OutError = TEXT("missing_struct_pin_subcategory_object");

				return false;

			}

			if (!Cast<UScriptStruct>(StructObject))

			{

				OutError = TEXT("invalid_struct_pin_subcategory_object");

				return false;

			}

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Enum)

		{

			UObject* EnumObject = OutPinType.PinSubCategoryObject.Get();

			if (!EnumObject)

			{

				OutError = TEXT("missing_enum_pin_subcategory_object");

				return false;

			}

			if (!Cast<UEnum>(EnumObject))

			{

				OutError = TEXT("invalid_enum_pin_subcategory_object");

				return false;

			}

			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Byte && OutPinType.PinSubCategoryObject.IsValid() && !Cast<UEnum>(OutPinType.PinSubCategoryObject.Get()))

		{

			OutError = TEXT("invalid_byte_enum_pin_subcategory_object");

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

			OutError = TEXT("missing_interface_pin_subcategory_object");

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

			OutError = TEXT("invalid_class_pin_subcategory_object");

			return false;

		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Interface)

		{

			const UClass* InterfaceClass = Cast<UClass>(OutPinType.PinSubCategoryObject.Get());

			if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))

			{

				OutError = TEXT("invalid_interface_pin_subcategory_object");

				return false;

			}

		}

		FString ContainerTypeText;

		TypeObj->TryGetStringField(TEXT("container_type"), ContainerTypeText);

		if (!ParsePinContainerTypeStrict(ContainerTypeText, OutPinType.ContainerType, OutError))

		{

			return false;

		}

		return true;

	}



	bool BuildPinTypeFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FString& OutError)

	{

		if (!BuildPinTypePrimaryFromJsonObject(TypeObj, OutPinType, OutError))

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

			OutError = TEXT("missing_map_value_type");

			return false;

		}

		FEdGraphPinType ValuePinType;

		if (!BuildPinTypePrimaryFromJsonObject(*ValueTypeObj, ValuePinType, OutError))

		{

			return false;

		}

		if (ValuePinType.ContainerType != EPinContainerType::None)

		{

			OutError = TEXT("nested_container_value_type_not_supported");

			return false;

		}

		OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;

		OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;

		OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;

		return true;

	}

}
