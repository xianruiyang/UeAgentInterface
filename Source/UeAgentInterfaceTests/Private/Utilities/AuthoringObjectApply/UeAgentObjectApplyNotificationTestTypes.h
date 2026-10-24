#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "StructUtils/InstancedStruct.h"
#include "Curves/CurveFloat.h"
#include "StructUtils/PropertyBag.h"
#include "UeAgentObjectApplyNotificationTestTypes.generated.h"

USTRUCT()
struct FUeAgentNotificationSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Count = 3;
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<int32> Values = {1};
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 ReadOnly = 41;
};

USTRUCT()
struct FUeAgentAlternateNotificationSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Value = 8;
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 ReadOnly = 62;
};

UCLASS()
class UUeAgentMapCompletionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FName, FUeAgentNotificationSettings> Records;
};

USTRUCT()
struct FUeAgentReadOnlySettings
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 Kept = 73;
};

USTRUCT()
struct FUeAgentEmptyNestedSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	FUeAgentReadOnlySettings Nested;
};

USTRUCT()
struct FUeAgentMixedActionableSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	FUeAgentEmptyNestedSettings Empty;
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Count = 5;
};

UCLASS()
class UUeAgentMapActionableProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FName, FUeAgentMixedActionableSettings> Records;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FName, FUeAgentEmptyNestedSettings> EmptyRecords;
};

UCLASS()
class UUeAgentArrayOptionalProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FUeAgentMixedActionableSettings> Records;
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FUeAgentEmptyNestedSettings> EmptyRecords;
	UPROPERTY(EditAnywhere, Category=Test)
	TOptional<FUeAgentMixedActionableSettings> Current;
	UPROPERTY(EditAnywhere, Category=Test)
	TOptional<FUeAgentEmptyNestedSettings> EmptyCurrent;
};

UCLASS()
class UUeAgentPropertyBagProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedPropertyBag Bag;
};

UCLASS()
class UUeAgentInstancedStructProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedStruct Current;
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedStruct Empty;
};

UCLASS()
class UUeAgentActionableProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Empty)
	FUeAgentReadOnlySettings ReadOnly;
	UPROPERTY(EditAnywhere, Category=Empty)
	FUeAgentEmptyNestedSettings Nested;
	UPROPERTY(EditAnywhere, Category=Empty)
	FUeAgentEmptyNestedSettings FixedEmpty[2];
	UPROPERTY(EditAnywhere, Category=Mixed)
	FUeAgentMixedActionableSettings Mixed;
	UPROPERTY(EditAnywhere, Category=Mixed)
	FUeAgentMixedActionableSettings FixedMixed[2];
	UPROPERTY(EditAnywhere, Category=Containers)
	TArray<FUeAgentEmptyNestedSettings> EmptyArray;
	UPROPERTY(EditAnywhere, Category=Containers)
	TSet<int32> EmptySet;
	UPROPERTY(EditAnywhere, Category=Containers)
	TMap<FName, int32> EmptyMap;
	UPROPERTY(EditAnywhere, Category=Containers)
	TOptional<int32> Optional;
	UPROPERTY(EditAnywhere, Category=Containers)
	FInstancedStruct Dynamic;
	UPROPERTY(EditAnywhere, Category=Containers)
	FInstancedPropertyBag Bag;
	UPROPERTY(EditAnywhere, Category=Semantic)
	FColor Color = FColor::Black;
};

UCLASS()
class UUeAgentOwnedActionableProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Instanced, Category=Owned)
	TObjectPtr<UUeAgentActionableProjectionTarget> Child;
};

DECLARE_DYNAMIC_DELEGATE(FUeAgentUnsupportedDynamicDelegate);
USTRUCT()
struct FUeAgentUnsupportedDynamicSettings
{
	GENERATED_BODY()
	UPROPERTY()
	FUeAgentUnsupportedDynamicDelegate Callback;
};

USTRUCT()
struct FUeAgentNestedDynamicSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Value = 8;
	UPROPERTY(VisibleAnywhere, Category=Test)
	FInstancedStruct Nested;
};

USTRUCT()
struct FUeAgentDynamicCurveSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	FRuntimeFloatCurve Low;
	UPROPERTY(EditAnywhere, Category=Test)
	FRuntimeFloatCurve High;
};

USTRUCT()
struct FUeAgentReadOnlyHashIdentity
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 Identity = 7;
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Payload = 5;
	bool operator==(const FUeAgentReadOnlyHashIdentity& Other) const { return Identity == Other.Identity; }
	friend uint32 GetTypeHash(const FUeAgentReadOnlyHashIdentity& Value) { return ::GetTypeHash(Value.Identity); }
};

template<> struct TStructOpsTypeTraits<FUeAgentReadOnlyHashIdentity> : TStructOpsTypeTraitsBase2<FUeAgentReadOnlyHashIdentity>
{
	enum { WithIdenticalViaEquality = true };
};

UCLASS()
class UUeAgentReadOnlyHashIdentityTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentReadOnlyHashIdentity> Records;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FUeAgentReadOnlyHashIdentity, int32> Lookup;
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FName> Names;
};

USTRUCT()
struct FUeAgentMixedSetProjectionElement : public FUeAgentMixedActionableSettings
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 Identity = 7;
	bool operator==(const FUeAgentMixedSetProjectionElement& Other) const { return Identity == Other.Identity; }
	friend uint32 GetTypeHash(const FUeAgentMixedSetProjectionElement& Value) { return ::GetTypeHash(Value.Identity); }
};
template<> struct TStructOpsTypeTraits<FUeAgentMixedSetProjectionElement> : TStructOpsTypeTraitsBase2<FUeAgentMixedSetProjectionElement>
{
	enum { WithIdenticalViaEquality = true };
};

USTRUCT()
struct FUeAgentEmptySetProjectionElement : public FUeAgentEmptyNestedSettings
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 Identity = 7;
	bool operator==(const FUeAgentEmptySetProjectionElement& Other) const { return Identity == Other.Identity; }
	friend uint32 GetTypeHash(const FUeAgentEmptySetProjectionElement& Value) { return ::GetTypeHash(Value.Identity); }
};
template<> struct TStructOpsTypeTraits<FUeAgentEmptySetProjectionElement> : TStructOpsTypeTraitsBase2<FUeAgentEmptySetProjectionElement>
{
	enum { WithIdenticalViaEquality = true };
};

UCLASS()
class UUeAgentSetActionableProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentMixedSetProjectionElement> Records;
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentEmptySetProjectionElement> EmptyRecords;
};

USTRUCT()
struct FUeAgentKeyedDynamicSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	FName Key;
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedStruct Payload;
	bool operator==(const FUeAgentKeyedDynamicSettings& Other) const { return Key == Other.Key; }
	friend uint32 GetTypeHash(const FUeAgentKeyedDynamicSettings& Value) { return GetTypeHash(Value.Key); }
};
template<> struct TStructOpsTypeTraits<FUeAgentKeyedDynamicSettings> : TStructOpsTypeTraitsBase2<FUeAgentKeyedDynamicSettings>
{
	enum { WithIdenticalViaEquality = true };
};

UCLASS()
class UUeAgentSetCompletionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentKeyedDynamicSettings> Records;
};

UCLASS()
class UUeAgentMapKeyProjectionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FUeAgentMixedSetProjectionElement, int32> Records;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FUeAgentEmptySetProjectionElement, int32> EmptyRecords;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FUeAgentKeyedDynamicSettings, int32> DynamicRecords;
};

USTRUCT()
struct FUeAgentPairedContainers
{
	GENERATED_BODY()
	UPROPERTY(VisibleAnywhere, Category=Test)
	int32 ReadOnly = 73;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FName, FInstancedStruct> Map;
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentKeyedDynamicSettings> Set;
};

UCLASS()
class UUeAgentPairedDefinitionTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	FUeAgentPairedContainers Nested;
	UPROPERTY(EditAnywhere, Category=Test)
	FUeAgentPairedContainers Fixed[2];
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FUeAgentPairedContainers> Array;
	UPROPERTY(EditAnywhere, Category=Test)
	TOptional<FUeAgentPairedContainers> Optional;
};

USTRUCT()
struct FUeAgentKeyedCurve
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Test)
	FName Key;
	UPROPERTY(EditAnywhere, Category=Test)
	FRuntimeFloatCurve Curve;
	bool operator==(const FUeAgentKeyedCurve& Other) const { return Key == Other.Key; }
	friend uint32 GetTypeHash(const FUeAgentKeyedCurve& Value) { return GetTypeHash(Value.Key); }
};
template<> struct TStructOpsTypeTraits<FUeAgentKeyedCurve> : TStructOpsTypeTraitsBase2<FUeAgentKeyedCurve>
{
	enum { WithIdenticalViaEquality = true };
};

struct FUeAgentObservedPropertyEvent
{
	FName Member;
	FName Leaf;
	EPropertyChangeType::Type Change = EPropertyChangeType::Unspecified;
	int32 ArrayIndex = 0;
	int32 ValueAtEvent = 0;
	int32 CountAtEvent = 0;
	TArray<int32> ArrayAtEvent;
};

UCLASS(EditInlineNew)
class UUeAgentObjectApplyNotificationTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Value = 2;
	UPROPERTY(EditAnywhere, Category=Test)
	FText Caption;
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FText> Captions;
	UPROPERTY(EditAnywhere, Category=Test)
	FText FixedCaptions[2];
	UPROPERTY(EditAnywhere, Category=Test)
	FUeAgentNotificationSettings Settings;
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FUeAgentNotificationSettings> Records;
	UPROPERTY(EditAnywhere, EditFixedSize, Category=Test)
	TArray<int32> FixedSizeValues;
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedStruct Dynamic;
	UPROPERTY(EditAnywhere, Category=Test)
	TArray<FInstancedStruct> DynamicRecords;
	UPROPERTY(EditAnywhere, Category=Test)
	TMap<FName, FRuntimeFloatCurve> CurveMap;
	UPROPERTY(EditAnywhere, Category=Test)
	FRuntimeFloatCurve FixedCurves[2];
	UPROPERTY(EditAnywhere, Category=Test)
	TSet<FUeAgentKeyedCurve> CurveSet;
	UPROPERTY(EditAnywhere, Category=Test)
	TOptional<FRuntimeFloatCurve> OptionalCurve;
	UPROPERTY(EditAnywhere, Category=Test)
	FInstancedPropertyBag CurveBag;
	UPROPERTY(EditAnywhere, Category=Test)
	int32 Fixed[2] = {4, 6};
	UPROPERTY(EditAnywhere, Instanced, Category=Test)
	TObjectPtr<UUeAgentObjectApplyNotificationTarget> Child;

	TArray<FName> PreLeaves;
	TArray<FUeAgentObservedPropertyEvent> Events;
	void PreEditChange(FProperty* Property) override
	{
		PreLeaves.Add(Property ? Property->GetFName() : NAME_None);
		Super::PreEditChange(Property);
	}
	void PostEditChangeProperty(FPropertyChangedEvent& Event) override
	{
		FUeAgentObservedPropertyEvent Observed;
		Observed.Member = Event.GetMemberPropertyName();
		Observed.Leaf = Event.GetPropertyName();
		Observed.Change = Event.ChangeType;
		Observed.ArrayIndex = Event.GetArrayIndex(Observed.Leaf.ToString());
		Observed.ValueAtEvent = Value;
		Observed.CountAtEvent = Settings.Count;
		Observed.ArrayAtEvent = Settings.Values;
		Events.Add(MoveTemp(Observed));
		Super::PostEditChangeProperty(Event);
	}
};
