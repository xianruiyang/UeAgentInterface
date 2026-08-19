#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"

#include "Animation/Skeleton.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		enum class EStructuredAssetKind : uint8
		{
			StaticMesh,
			DataTable,
			SkeletalMesh,
		};

		const EStructuredAssetKind* FindStructuredAssetKind(
			const FString& NativeMechanicId)
		{
			static const TMap<FString, EStructuredAssetKind> Kinds{
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_static_mesh:v1"),
					EStructuredAssetKind::StaticMesh,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"replace_data_table:v1"),
					EStructuredAssetKind::DataTable,
				},
				{
					TEXT(
						"uai.asset_data.mechanic:"
						"update_skeletal_mesh:v1"),
					EStructuredAssetKind::SkeletalMesh,
				},
			};
			return Kinds.Find(NativeMechanicId);
		}

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		const FStructuredAssetChangeView* FindChange(
			const TArray<FStructuredAssetChangeView>& Changes,
			const FString& FieldId)
		{
			for (const FStructuredAssetChangeView& Change : Changes)
			{
				if (Change.FieldId == FieldId)
				{
					return &Change;
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValueRef* FindObjectFieldRef(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}

		bool HasExactObjectFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Names)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object
				|| Object.GetObjectItems().Num()
					!= static_cast<int32>(Names.size()))
			{
				return false;
			}
			for (const TCHAR* Name : Names)
			{
				if (!FindObjectField(Object, Name))
				{
					return false;
				}
			}
			return true;
		}

		bool TryReadString(
			const FUeAgentCanonicalValue& Value,
			FString& Out)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			Out = Value.GetText();
			return true;
		}

		bool TryReadBool(
			const FUeAgentCanonicalValue& Value,
			bool& Out)
		{
			if (Value.GetKind() != EUeAgentValueKind::Boolean)
			{
				return false;
			}
			Out = Value.GetBoolean();
			return true;
		}

		bool TryReadNumber(
			const FUeAgentCanonicalValue& Value,
			double& Out)
		{
			if (Value.GetKind() != EUeAgentValueKind::Integer
				&& Value.GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			return LexTryParseString(Out, *Value.GetLexeme())
				&& FMath::IsFinite(Out);
		}

		bool TryReadInt32(
			const FUeAgentCanonicalValue& Value,
			int32& Out)
		{
			double Number = 0.0;
			if (!TryReadNumber(Value, Number)
				|| Number != FMath::FloorToDouble(Number)
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			Out = static_cast<int32>(Number);
			return true;
		}

		bool TryReadVector(
			const FUeAgentCanonicalValue& Value,
			FVector& Out)
		{
			if (!HasExactObjectFields(
					Value,
					{TEXT("x"), TEXT("y"), TEXT("z")}))
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!TryReadNumber(
					*FindObjectField(Value, TEXT("x")),
					X)
				|| !TryReadNumber(
					*FindObjectField(Value, TEXT("y")),
					Y)
				|| !TryReadNumber(
					*FindObjectField(Value, TEXT("z")),
					Z))
			{
				return false;
			}
			Out = FVector(X, Y, Z);
			return !Out.ContainsNaN();
		}

		bool TryReadRotator(
			const FUeAgentCanonicalValue& Value,
			FRotator& Out)
		{
			if (!HasExactObjectFields(
					Value,
					{
						TEXT("pitch"),
						TEXT("yaw"),
						TEXT("roll"),
					}))
			{
				return false;
			}
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if (!TryReadNumber(
					*FindObjectField(Value, TEXT("pitch")),
					Pitch)
				|| !TryReadNumber(
					*FindObjectField(Value, TEXT("yaw")),
					Yaw)
				|| !TryReadNumber(
					*FindObjectField(Value, TEXT("roll")),
					Roll))
			{
				return false;
			}
			Out = FRotator(Pitch, Yaw, Roll);
			return !Out.ContainsNaN();
		}

		struct FMaterialSlotValue final
		{
			FName SlotName;
			TWeakObjectPtr<UMaterialInterface> Material;
		};

		bool TryReadMaterialSlots(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FMaterialSlotValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_material_slots_invalid"));
			}
			Out.Reset();
			TSet<FName> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{
							TEXT("slot_name"),
							TEXT("material_ref"),
						}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_material_slot_invalid"));
				}
				FString SlotName;
				FString MaterialRef;
				if (!TryReadString(
						*FindObjectField(
							*Item,
							TEXT("slot_name")),
						SlotName)
					|| !TryReadString(
						*FindObjectField(
							*Item,
							TEXT("material_ref")),
						MaterialRef)
					|| SlotName.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("asset_data_material_slot_invalid"));
				}
				FMaterialSlotValue Slot;
				Slot.SlotName = FName(*SlotName);
				if (Seen.Contains(Slot.SlotName))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_material_slot_duplicate"),
						SlotName);
				}
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						MaterialRef,
						Resolved,
						OutError))
				{
					return false;
				}
				UMaterialInterface* Material =
					Cast<UMaterialInterface>(Resolved);
				if (!Material)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_material_ref_invalid"),
						MaterialRef);
				}
				Slot.Material = Material;
				Seen.Add(Slot.SlotName);
				Out.Add(MoveTemp(Slot));
			}
			return true;
		}

		TArray<FStaticMaterial> MakeStaticMaterials(
			const TArray<FMaterialSlotValue>& Slots)
		{
			TArray<FStaticMaterial> Out;
			Out.Reserve(Slots.Num());
			for (const FMaterialSlotValue& Slot : Slots)
			{
				Out.Emplace(
					Slot.Material.Get(),
					Slot.SlotName,
					Slot.SlotName);
			}
			return Out;
		}

		TArray<FSkeletalMaterial> MakeSkeletalMaterials(
			const TArray<FMaterialSlotValue>& Slots)
		{
			TArray<FSkeletalMaterial> Out;
			Out.Reserve(Slots.Num());
			for (const FMaterialSlotValue& Slot : Slots)
			{
				Out.Emplace(
					Slot.Material.Get(),
					Slot.SlotName,
					Slot.SlotName);
			}
			return Out;
		}

		TArray<FMaterialSlotValue> CaptureStaticMaterialSlots(
			const UStaticMesh& Mesh)
		{
			TArray<FMaterialSlotValue> Out;
			for (const FStaticMaterial& Material
				: Mesh.GetStaticMaterials())
			{
				FMaterialSlotValue Slot;
				Slot.SlotName = Material.MaterialSlotName;
				Slot.Material = Material.MaterialInterface;
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

		bool TryReadRemovalNames(
			const FUeAgentCanonicalValue& Value,
			TSet<FName>& Out,
			FExecutionMutationError& OutError,
			const FString& ErrorCode)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, ErrorCode);
			}
			Out.Reset();
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				FString Name;
				if (!TryReadString(*Item, Name)
					|| Name.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, ErrorCode);
				}
				Out.Add(FName(*Name));
			}
			return true;
		}

		bool TryApplyMaterialPresence(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FStructuredAssetChangeView& Change,
			const TArray<FMaterialSlotValue>& Current,
			TArray<FMaterialSlotValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Change.Presence == TEXT("clear"))
			{
				Out.Reset();
				return true;
			}
			if (!Change.CanonicalValue)
			{
				return Fail(
					OutError,
					TEXT("asset_data_material_slots_invalid"));
			}
			if (Change.Presence == TEXT("replace"))
			{
				return TryReadMaterialSlots(
					Resolver,
					*Change.CanonicalValue,
					Out,
					OutError);
			}
			if (Change.Presence == TEXT("set"))
			{
				TArray<FMaterialSlotValue> Updates;
				if (!TryReadMaterialSlots(
						Resolver,
						*Change.CanonicalValue,
						Updates,
						OutError))
				{
					return false;
				}
				Out = Current;
				for (FMaterialSlotValue& Update : Updates)
				{
					const int32 Existing =
						Out.IndexOfByPredicate(
							[&Update](
								const FMaterialSlotValue& Item)
							{
								return Item.SlotName
									== Update.SlotName;
							});
					if (Existing == INDEX_NONE)
					{
						Out.Add(MoveTemp(Update));
					}
					else
					{
						Out[Existing] = MoveTemp(Update);
					}
				}
				return true;
			}
			if (Change.Presence == TEXT("remove"))
			{
				TSet<FName> Names;
				if (!TryReadRemovalNames(
						*Change.CanonicalValue,
						Names,
						OutError,
						TEXT(
							"asset_data_material_remove_invalid")))
				{
					return false;
				}
				Out = Current;
				Out.RemoveAll(
					[&Names](const FMaterialSlotValue& Item)
					{
						return Names.Contains(Item.SlotName);
					});
				return true;
			}
			return Fail(
				OutError,
				TEXT("asset_data_material_presence_invalid"),
				Change.Presence);
		}

		bool TryReadCollisionTraceMode(
			const FString& Value,
			ECollisionTraceFlag& Out)
		{
			const FString Name = Value.TrimStartAndEnd().ToLower();
			if (Name == TEXT("default"))
			{
				Out = CTF_UseDefault;
			}
			else if (Name == TEXT("use_simple_and_complex"))
			{
				Out = CTF_UseSimpleAndComplex;
			}
			else if (Name == TEXT("use_simple_as_complex"))
			{
				Out = CTF_UseSimpleAsComplex;
			}
			else if (Name == TEXT("use_complex_as_simple"))
			{
				Out = CTF_UseComplexAsSimple;
			}
			else
			{
				return false;
			}
			return true;
		}

		bool TryReadCollision(
			const FUeAgentCanonicalValue& Value,
			FKAggregateGeom& OutGeometry,
			ECollisionTraceFlag& OutTrace,
			FExecutionMutationError& OutError)
		{
			if (!HasExactObjectFields(
					Value,
					{
						TEXT("trace_mode"),
						TEXT("boxes"),
						TEXT("spheres"),
						TEXT("capsules"),
					}))
			{
				return Fail(
					OutError,
					TEXT("asset_data_collision_invalid"));
			}
			FString TraceMode;
			const FUeAgentCanonicalValue* Boxes =
				FindObjectField(Value, TEXT("boxes"));
			const FUeAgentCanonicalValue* Spheres =
				FindObjectField(Value, TEXT("spheres"));
			const FUeAgentCanonicalValue* Capsules =
				FindObjectField(Value, TEXT("capsules"));
			if (!TryReadString(
					*FindObjectField(Value, TEXT("trace_mode")),
					TraceMode)
				|| !TryReadCollisionTraceMode(TraceMode, OutTrace)
				|| !Boxes
				|| Boxes->GetKind() != EUeAgentValueKind::Array
				|| !Spheres
				|| Spheres->GetKind() != EUeAgentValueKind::Array
				|| !Capsules
				|| Capsules->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_collision_invalid"));
			}
			OutGeometry = FKAggregateGeom();
			for (const FUeAgentCanonicalValueRef& Item
				: Boxes->GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{
							TEXT("center"),
							TEXT("extent"),
							TEXT("rotation"),
						}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_box_invalid"));
				}
				FVector Center = FVector::ZeroVector;
				FVector Extent = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				if (!TryReadVector(
						*FindObjectField(*Item, TEXT("center")),
						Center)
					|| !TryReadVector(
						*FindObjectField(*Item, TEXT("extent")),
						Extent)
					|| !TryReadRotator(
						*FindObjectField(*Item, TEXT("rotation")),
						Rotation)
					|| Extent.GetMin() <= 0.0)
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_box_invalid"));
				}
				FKBoxElem Box;
				Box.Center = Center;
				Box.Rotation = Rotation;
				Box.X = Extent.X * 2.0;
				Box.Y = Extent.Y * 2.0;
				Box.Z = Extent.Z * 2.0;
				OutGeometry.BoxElems.Add(Box);
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Spheres->GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{TEXT("center"), TEXT("radius")}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_sphere_invalid"));
				}
				FVector Center = FVector::ZeroVector;
				double Radius = 0.0;
				if (!TryReadVector(
						*FindObjectField(*Item, TEXT("center")),
						Center)
					|| !TryReadNumber(
						*FindObjectField(*Item, TEXT("radius")),
						Radius)
					|| Radius <= 0.0)
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_sphere_invalid"));
				}
				FKSphereElem Sphere;
				Sphere.Center = Center;
				Sphere.Radius = Radius;
				OutGeometry.SphereElems.Add(Sphere);
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Capsules->GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{
							TEXT("center"),
							TEXT("rotation"),
							TEXT("radius"),
							TEXT("length"),
						}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_capsule_invalid"));
				}
				FVector Center = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				double Radius = 0.0;
				double Length = 0.0;
				if (!TryReadVector(
						*FindObjectField(*Item, TEXT("center")),
						Center)
					|| !TryReadRotator(
						*FindObjectField(*Item, TEXT("rotation")),
						Rotation)
					|| !TryReadNumber(
						*FindObjectField(*Item, TEXT("radius")),
						Radius)
					|| !TryReadNumber(
						*FindObjectField(*Item, TEXT("length")),
						Length)
					|| Radius <= 0.0
					|| Length <= 0.0)
				{
					return Fail(
						OutError,
						TEXT("asset_data_collision_capsule_invalid"));
				}
				FKSphylElem Capsule;
				Capsule.Center = Center;
				Capsule.Rotation = Rotation;
				Capsule.Radius = Radius;
				Capsule.Length = Length;
				OutGeometry.SphylElems.Add(Capsule);
			}
			return true;
		}

		struct FSocketValue final
		{
			FName Name;
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale = FVector::OneVector;
		};

		TArray<FSocketValue> CaptureSockets(
			const UStaticMesh& Mesh)
		{
			TArray<FSocketValue> Out;
			for (const UStaticMeshSocket* Socket : Mesh.Sockets)
			{
				if (!Socket)
				{
					continue;
				}
				FSocketValue Value;
				Value.Name = Socket->SocketName;
				Value.Location = Socket->RelativeLocation;
				Value.Rotation = Socket->RelativeRotation;
				Value.Scale = Socket->RelativeScale;
				Out.Add(MoveTemp(Value));
			}
			return Out;
		}

		bool TryReadSockets(
			const FUeAgentCanonicalValue& Value,
			TArray<FSocketValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sockets_invalid"));
			}
			Out.Reset();
			TSet<FName> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{
							TEXT("name"),
							TEXT("location"),
							TEXT("rotation"),
							TEXT("scale"),
						}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_socket_invalid"));
				}
				FString Name;
				FSocketValue Socket;
				if (!TryReadString(
						*FindObjectField(*Item, TEXT("name")),
						Name)
					|| Name.TrimStartAndEnd().IsEmpty()
					|| !TryReadVector(
						*FindObjectField(*Item, TEXT("location")),
						Socket.Location)
					|| !TryReadRotator(
						*FindObjectField(*Item, TEXT("rotation")),
						Socket.Rotation)
					|| !TryReadVector(
						*FindObjectField(*Item, TEXT("scale")),
						Socket.Scale)
					|| Socket.Scale.GetAbsMin() <= 0.0)
				{
					return Fail(
						OutError,
						TEXT("asset_data_socket_invalid"));
				}
				Socket.Name = FName(*Name);
				if (Seen.Contains(Socket.Name))
				{
					return Fail(
						OutError,
						TEXT("asset_data_socket_duplicate"),
						Name);
				}
				Seen.Add(Socket.Name);
				Out.Add(MoveTemp(Socket));
			}
			return true;
		}

		bool TryApplySocketPresence(
			const FStructuredAssetChangeView& Change,
			const TArray<FSocketValue>& Current,
			TArray<FSocketValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (Change.Presence == TEXT("clear"))
			{
				Out.Reset();
				return true;
			}
			if (!Change.CanonicalValue)
			{
				return Fail(
					OutError,
					TEXT("asset_data_sockets_invalid"));
			}
			if (Change.Presence == TEXT("replace"))
			{
				return TryReadSockets(
					*Change.CanonicalValue,
					Out,
					OutError);
			}
			if (Change.Presence == TEXT("set"))
			{
				TArray<FSocketValue> Updates;
				if (!TryReadSockets(
						*Change.CanonicalValue,
						Updates,
						OutError))
				{
					return false;
				}
				Out = Current;
				for (FSocketValue& Update : Updates)
				{
					const int32 Existing =
						Out.IndexOfByPredicate(
							[&Update](const FSocketValue& Item)
							{
								return Item.Name == Update.Name;
							});
					if (Existing == INDEX_NONE)
					{
						Out.Add(MoveTemp(Update));
					}
					else
					{
						Out[Existing] = MoveTemp(Update);
					}
				}
				return true;
			}
			if (Change.Presence == TEXT("remove"))
			{
				TSet<FName> Names;
				if (!TryReadRemovalNames(
						*Change.CanonicalValue,
						Names,
						OutError,
						TEXT("asset_data_socket_remove_invalid")))
				{
					return false;
				}
				Out = Current;
				Out.RemoveAll(
					[&Names](const FSocketValue& Item)
					{
						return Names.Contains(Item.Name);
					});
				return true;
			}
			return Fail(
				OutError,
				TEXT("asset_data_socket_presence_invalid"),
				Change.Presence);
		}

		bool TryReadBuildSettings(
			const FUeAgentCanonicalValue& Value,
			int32 SourceModelCount,
			const TArray<FMeshBuildSettings>& Current,
			TArray<FMeshBuildSettings>& Out,
			FExecutionMutationError& OutError)
		{
			if (!HasExactObjectFields(Value, {TEXT("lods")}))
			{
				return Fail(
					OutError,
					TEXT("asset_data_build_settings_invalid"));
			}
			const FUeAgentCanonicalValue* Lods =
				FindObjectField(Value, TEXT("lods"));
			if (!Lods || Lods->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_build_settings_invalid"));
			}
			Out = Current;
			TSet<int32> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Lods->GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{
							TEXT("lod_index"),
							TEXT("recompute_normals"),
							TEXT("recompute_tangents"),
							TEXT("compute_weighted_normals"),
							TEXT("remove_degenerates"),
							TEXT("use_mikk_tspace"),
							TEXT("generate_lightmap_uvs"),
						}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_build_settings_lod_invalid"));
				}
				int32 Index = INDEX_NONE;
				bool bRecomputeNormals = false;
				bool bRecomputeTangents = false;
				bool bComputeWeightedNormals = false;
				bool bRemoveDegenerates = false;
				bool bUseMikk = false;
				bool bGenerateLightmapUvs = false;
				if (!TryReadInt32(
						*FindObjectField(*Item, TEXT("lod_index")),
						Index)
					|| Index < 0
					|| Index >= SourceModelCount
					|| Seen.Contains(Index)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("recompute_normals")),
						bRecomputeNormals)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("recompute_tangents")),
						bRecomputeTangents)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("compute_weighted_normals")),
						bComputeWeightedNormals)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("remove_degenerates")),
						bRemoveDegenerates)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("use_mikk_tspace")),
						bUseMikk)
					|| !TryReadBool(
						*FindObjectField(
							*Item,
							TEXT("generate_lightmap_uvs")),
						bGenerateLightmapUvs))
				{
					return Fail(
						OutError,
						TEXT("asset_data_build_settings_lod_invalid"));
				}
				FMeshBuildSettings& Settings = Out[Index];
				Settings.bRecomputeNormals = bRecomputeNormals;
				Settings.bRecomputeTangents = bRecomputeTangents;
				Settings.bComputeWeightedNormals =
					bComputeWeightedNormals;
				Settings.bRemoveDegenerates = bRemoveDegenerates;
				Settings.bUseMikkTSpace = bUseMikk;
				Settings.bGenerateLightmapUVs =
					bGenerateLightmapUvs;
				Seen.Add(Index);
			}
			return !Seen.IsEmpty();
		}

		struct FOwnedStructBuffer final
		{
			explicit FOwnedStructBuffer(
				UScriptStruct* InType)
				: Type(InType)
			{
				if (Type)
				{
					Data = static_cast<uint8*>(
						FMemory::Malloc(
							Type->GetStructureSize(),
							Type->GetMinAlignment()));
					Type->InitializeStruct(Data);
				}
			}

			~FOwnedStructBuffer()
			{
				if (Type && Data)
				{
					Type->DestroyStruct(Data);
					FMemory::Free(Data);
				}
			}

			FOwnedStructBuffer(const FOwnedStructBuffer&) = delete;
			FOwnedStructBuffer& operator=(
				const FOwnedStructBuffer&) = delete;

			void CopyFrom(const void* Source)
			{
				check(Type && Data && Source);
				Type->CopyScriptStruct(Data, Source);
			}

			UScriptStruct* Type = nullptr;
			uint8* Data = nullptr;
		};

		struct FTableRowValue final
		{
			FName Name;
			TSharedPtr<FOwnedStructBuffer, ESPMode::ThreadSafe>
				Data;
		};

		bool TryWriteCanonicalProperty(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FProperty& Property,
			void* ValuePtr,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError);

		bool TryWriteCanonicalStruct(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UStruct& Struct,
			void* StructData,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_struct_invalid"),
					Struct.GetPathName());
			}
			TSet<const FProperty*> Written;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value.GetObjectItems())
			{
				FProperty* Property =
					FindFProperty<FProperty>(
						&Struct,
						FName(*Item.Key));
				if (!Property
					|| Written.Contains(Property)
					|| Property->HasAnyPropertyFlags(
						CPF_Transient | CPF_Deprecated))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_field_invalid"),
						Item.Key);
				}
				void* PropertyValue =
					Property->ContainerPtrToValuePtr<void>(
						StructData);
				if (!TryWriteCanonicalProperty(
						Resolver,
						*Property,
						PropertyValue,
						Item.Value.Get(),
						OutError))
				{
					return false;
				}
				Written.Add(Property);
			}
			return true;
		}

		bool TryWriteCanonicalProperty(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FProperty& Property,
			void* ValuePtr,
			const FUeAgentCanonicalValue& Value,
			FExecutionMutationError& OutError)
		{
			if (const FBoolProperty* Bool =
				CastField<FBoolProperty>(&Property))
			{
				bool Parsed = false;
				if (!TryReadBool(Value, Parsed))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Bool->SetPropertyValue(ValuePtr, Parsed);
				return true;
			}
			if (const FByteProperty* Byte =
				CastField<FByteProperty>(&Property);
				Byte && Byte->Enum)
			{
				int64 EnumValue = INDEX_NONE;
				if (Value.GetKind() == EUeAgentValueKind::String)
				{
					EnumValue =
						Byte->Enum->GetValueByNameString(
							Value.GetText());
				}
				else
				{
					int32 Parsed = INDEX_NONE;
					if (TryReadInt32(Value, Parsed))
					{
						EnumValue = Parsed;
					}
				}
				if (EnumValue == INDEX_NONE
					|| !Byte->Enum->IsValidEnumValue(EnumValue))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Byte->SetPropertyValue(
					ValuePtr,
					static_cast<uint8>(EnumValue));
				return true;
			}
			if (const FNumericProperty* Numeric =
				CastField<FNumericProperty>(&Property))
			{
				if (Numeric->IsInteger())
				{
					if (Value.GetKind() != EUeAgentValueKind::Integer)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_data_table_value_invalid"),
							Property.GetName());
					}
					const FString& Lexeme = Value.GetLexeme();
					if (Lexeme.StartsWith(TEXT("-")))
					{
						int64 Parsed = 0;
						if (!LexTryParseString(Parsed, *Lexeme)
							|| !Numeric->CanHoldValue(Parsed))
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_data_table_value_invalid"),
								Property.GetName());
						}
						Numeric->SetIntPropertyValue(ValuePtr, Parsed);
					}
					else
					{
						uint64 Parsed = 0;
						if (!LexTryParseString(Parsed, *Lexeme)
							|| !Numeric->CanHoldValue(Parsed))
						{
							return Fail(
								OutError,
								TEXT(
									"asset_data_data_table_value_invalid"),
								Property.GetName());
						}
						Numeric->SetIntPropertyValue(ValuePtr, Parsed);
					}
				}
				else
				{
					double Parsed = 0.0;
					if (!TryReadNumber(Value, Parsed)
						|| !Numeric->CanHoldValue(Parsed))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_data_table_value_invalid"),
							Property.GetName());
					}
					Numeric->SetFloatingPointPropertyValue(
						ValuePtr,
						Parsed);
				}
				return true;
			}
			if (const FStrProperty* String =
				CastField<FStrProperty>(&Property))
			{
				FString Parsed;
				if (!TryReadString(Value, Parsed))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				String->SetPropertyValue(ValuePtr, Parsed);
				return true;
			}
			if (const FNameProperty* Name =
				CastField<FNameProperty>(&Property))
			{
				FString Parsed;
				if (!TryReadString(Value, Parsed))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Name->SetPropertyValue(ValuePtr, FName(*Parsed));
				return true;
			}
			if (const FTextProperty* Text =
				CastField<FTextProperty>(&Property))
			{
				FString Parsed;
				if (!TryReadString(Value, Parsed))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Text->SetPropertyValue(
					ValuePtr,
					FText::FromString(Parsed));
				return true;
			}
			if (const FEnumProperty* Enum =
				CastField<FEnumProperty>(&Property))
			{
				int64 EnumValue = INDEX_NONE;
				if (Value.GetKind() == EUeAgentValueKind::String)
				{
					EnumValue =
						Enum->GetEnum()->GetValueByNameString(
							Value.GetText());
				}
				else
				{
					int32 Parsed = INDEX_NONE;
					if (TryReadInt32(Value, Parsed))
					{
						EnumValue = Parsed;
					}
				}
				if (EnumValue == INDEX_NONE
					|| !Enum->GetEnum()->IsValidEnumValue(
						EnumValue))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Enum->GetUnderlyingProperty()
					->SetIntPropertyValue(ValuePtr, EnumValue);
				return true;
			}
			if (const FSoftObjectProperty* Soft =
				CastField<FSoftObjectProperty>(&Property))
			{
				if (Value.GetKind() == EUeAgentValueKind::Null)
				{
					Soft->SetPropertyValue(
						ValuePtr,
						FSoftObjectPtr());
					return true;
				}
				FString Path;
				if (!TryReadString(Value, Path))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				Soft->SetPropertyValue(
					ValuePtr,
					FSoftObjectPtr(FSoftObjectPath(Path)));
				return true;
			}
			if (const FObjectPropertyBase* Object =
				CastField<FObjectPropertyBase>(&Property))
			{
				if (Value.GetKind() == EUeAgentValueKind::Null)
				{
					Object->SetObjectPropertyValue(ValuePtr, nullptr);
					return true;
				}
				FString Ref;
				if (!TryReadString(Value, Ref))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				UObject* Resolved = nullptr;
				if (!Resolver.TryResolveExisting(
						Ref,
						Resolved,
						OutError))
				{
					return false;
				}
				if (!Resolved
					|| !Resolved->IsA(Object->PropertyClass))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_reference_type_invalid"),
						Property.GetName());
				}
				Object->SetObjectPropertyValue(
					ValuePtr,
					Resolved);
				return true;
			}
			if (const FStructProperty* Struct =
				CastField<FStructProperty>(&Property))
			{
				return TryWriteCanonicalStruct(
					Resolver,
					*Struct->Struct,
					ValuePtr,
					Value,
					OutError);
			}
			if (const FArrayProperty* Array =
				CastField<FArrayProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				FScriptArrayHelper Helper(Array, ValuePtr);
				Helper.EmptyValues();
				for (const FUeAgentCanonicalValueRef& Item
					: Value.GetArrayItems())
				{
					const int32 Index = Helper.AddValue();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Array->Inner,
							Helper.GetRawPtr(Index),
							Item.Get(),
							OutError))
					{
						return false;
					}
				}
				return true;
			}
			if (const FSetProperty* Set =
				CastField<FSetProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				FScriptSetHelper Helper(Set, ValuePtr);
				Helper.EmptyElements();
				for (const FUeAgentCanonicalValueRef& Item
					: Value.GetArrayItems())
				{
					const int32 Index =
						Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Set->ElementProp,
							Helper.GetElementPtr(Index),
							Item.Get(),
							OutError))
					{
						return false;
					}
				}
				Helper.Rehash();
				return true;
			}
			if (const FMapProperty* Map =
				CastField<FMapProperty>(&Property))
			{
				if (Value.GetKind() != EUeAgentValueKind::Array)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_value_invalid"),
						Property.GetName());
				}
				FScriptMapHelper Helper(Map, ValuePtr);
				Helper.EmptyValues();
				for (const FUeAgentCanonicalValueRef& Item
					: Value.GetArrayItems())
				{
					if (!HasExactObjectFields(
							*Item,
							{TEXT("key"), TEXT("value")}))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_data_table_map_entry_invalid"),
							Property.GetName());
					}
					const int32 Index =
						Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!TryWriteCanonicalProperty(
							Resolver,
							*Map->KeyProp,
							Helper.GetKeyPtr(Index),
							*FindObjectField(*Item, TEXT("key")),
							OutError)
						|| !TryWriteCanonicalProperty(
							Resolver,
							*Map->ValueProp,
							Helper.GetValuePtr(Index),
							*FindObjectField(*Item, TEXT("value")),
							OutError))
					{
						return false;
					}
				}
				Helper.Rehash();
				return true;
			}
			return Fail(
				OutError,
				TEXT(
					"asset_data_data_table_property_type_unsupported"),
				Property.GetName());
		}

		TArray<FTableRowValue> CaptureRows(
			UDataTable& Table,
			UScriptStruct* RowStruct)
		{
			TArray<FTableRowValue> Out;
			if (!RowStruct)
			{
				return Out;
			}
			for (const TPair<FName, uint8*>& Row
				: Table.GetRowMap())
			{
				FTableRowValue Value;
				Value.Name = Row.Key;
				Value.Data =
					MakeShared<
						FOwnedStructBuffer,
						ESPMode::ThreadSafe>(RowStruct);
				Value.Data->CopyFrom(Row.Value);
				Out.Add(MoveTemp(Value));
			}
			return Out;
		}

		bool TryReadRows(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			UScriptStruct* RowStruct,
			const FUeAgentCanonicalValue& Value,
			TArray<FTableRowValue>& Out,
			FExecutionMutationError& OutError)
		{
			if (!RowStruct
				|| !RowStruct->IsChildOf(
					FTableRowBase::StaticStruct())
				|| Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_rows_invalid"));
			}
			Out.Reset();
			TSet<FName> Names;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				if (!HasExactObjectFields(
						*Item,
						{TEXT("name"), TEXT("values")}))
				{
					return Fail(
						OutError,
						TEXT("asset_data_data_table_row_invalid"));
				}
				FString Name;
				if (!TryReadString(
						*FindObjectField(*Item, TEXT("name")),
						Name)
					|| Name.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("asset_data_data_table_row_invalid"));
				}
				FTableRowValue Row;
				Row.Name = FName(*Name);
				if (Names.Contains(Row.Name))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_data_table_row_duplicate"),
						Name);
				}
				Row.Data =
					MakeShared<
						FOwnedStructBuffer,
						ESPMode::ThreadSafe>(RowStruct);
				if (!TryWriteCanonicalStruct(
						Resolver,
						*RowStruct,
						Row.Data->Data,
						*FindObjectField(*Item, TEXT("values")),
						OutError))
				{
					return false;
				}
				Names.Add(Row.Name);
				Out.Add(MoveTemp(Row));
			}
			return true;
		}

		bool WriteRows(
			UDataTable& Table,
			UScriptStruct* RowStruct,
			const TArray<FTableRowValue>& Rows)
		{
			if (!RowStruct)
			{
				return false;
			}
			for (const FTableRowValue& Row : Rows)
			{
				if (!Row.Data.IsValid()
					|| Row.Data->Type != RowStruct
					|| !Row.Data->Data)
				{
					return false;
				}
			}
			Table.EmptyTable();
			Table.RowStruct = RowStruct;
			for (const FTableRowValue& Row : Rows)
			{
				Table.AddRow(
					Row.Name,
					Row.Data->Data,
					RowStruct);
			}
			return true;
		}

		bool TryReadBounds(
			const FUeAgentCanonicalValue& Value,
			FBoxSphereBounds& Out)
		{
			if (!HasExactObjectFields(
					Value,
					{TEXT("min"), TEXT("max")}))
			{
				return false;
			}
			FVector Min;
			FVector Max;
			if (!TryReadVector(
					*FindObjectField(Value, TEXT("min")),
					Min)
				|| !TryReadVector(
					*FindObjectField(Value, TEXT("max")),
					Max)
				|| Min.X > Max.X
				|| Min.Y > Max.Y
				|| Min.Z > Max.Z)
			{
				return false;
			}
			Out = FBoxSphereBounds(FBox(Min, Max));
			return true;
		}

		bool TryBuildProjectionString(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_string_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionNull(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryNull(
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_null_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionBoolean(
			const bool Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryBoolean(
					Value,
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_boolean_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionInteger(
			const FString& Lexeme,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryInteger(
					Lexeme,
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_integer_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionDecimal(
			const double Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FMath::IsFinite(Value)
				&& FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Value),
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_decimal_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionArray(
			TArray<FUeAgentCanonicalValueRef> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_array_invalid")
						: Error.Code,
					Error.Detail);
		}

		bool TryBuildProjectionObject(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					Out,
					Error)
				? true
				: Fail(
					OutError,
					Error.Code.IsEmpty()
						? TEXT("asset_data_structured_asset_projection_object_invalid")
						: Error.Code,
					Error.Detail);
		}

		FString ProjectionFingerprint(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return TEXT("n");
			case EUeAgentValueKind::Boolean:
				return Value.GetBoolean() ? TEXT("b1") : TEXT("b0");
			case EUeAgentValueKind::String:
				return FString::Printf(
					TEXT("s%d:%s"),
					Value.GetText().Len(),
					*Value.GetText());
			case EUeAgentValueKind::Integer:
				return TEXT("i") + Value.GetLexeme();
			case EUeAgentValueKind::Decimal:
				return TEXT("d") + Value.GetLexeme();
			case EUeAgentValueKind::Array:
				{
					FString Out = TEXT("a[");
					for (const FUeAgentCanonicalValueRef& Item
						: Value.GetArrayItems())
					{
						Out += ProjectionFingerprint(Item.Get()) + TEXT(";");
					}
					return Out + TEXT("]");
				}
			case EUeAgentValueKind::Object:
				{
					FString Out = TEXT("o{");
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value.GetObjectItems())
					{
						Out += FString::Printf(
							TEXT("%d:%s="),
							Item.Key.Len(),
							*Item.Key);
						Out += ProjectionFingerprint(Item.Value.Get()) + TEXT(";");
					}
					return Out + TEXT("}");
				}
			default:
				return TEXT("unsupported");
			}
		}

		bool IsUnsignedProjectionNumber(
			const FNumericProperty& Property)
		{
			return CastField<FByteProperty>(&Property)
				|| CastField<FUInt16Property>(&Property)
				|| CastField<FUInt32Property>(&Property)
				|| CastField<FUInt64Property>(&Property);
		}

		bool TryProjectCanonicalProperty(
			const FProperty& Property,
			const void* ValuePtr,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError);

		bool TryProjectCanonicalStruct(
			UStruct& Struct,
			const void* StructData,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property
					|| Property->HasAnyPropertyFlags(
						CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				FUeAgentCanonicalValuePtr Value;
				if (!TryProjectCanonicalProperty(
						*Property,
						Property->ContainerPtrToValuePtr<void>(StructData),
						Resources,
						Value,
						OutError))
				{
					return false;
				}
				Items.Emplace(Property->GetName(), Value.ToSharedRef());
			}
			return TryBuildProjectionObject(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryProjectCanonicalProperty(
			const FProperty& Property,
			const void* ValuePtr,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			if (const FBoolProperty* Bool =
				CastField<FBoolProperty>(&Property))
			{
				return TryBuildProjectionBoolean(
					Bool->GetPropertyValue(ValuePtr),
					Resources,
					Out,
					OutError);
			}
			if (const FByteProperty* Byte =
				CastField<FByteProperty>(&Property);
				Byte && Byte->Enum)
			{
				return TryBuildProjectionString(
					Byte->Enum->GetNameStringByValue(
						Byte->GetPropertyValue(ValuePtr)),
					Resources,
					Out,
					OutError);
			}
			if (const FEnumProperty* Enum =
				CastField<FEnumProperty>(&Property))
			{
				const int64 Raw = Enum->GetUnderlyingProperty()
					->GetSignedIntPropertyValue(ValuePtr);
				return TryBuildProjectionString(
					Enum->GetEnum()->GetNameStringByValue(Raw),
					Resources,
					Out,
					OutError);
			}
			if (const FNumericProperty* Numeric =
				CastField<FNumericProperty>(&Property))
			{
				if (Numeric->IsFloatingPoint())
				{
					return TryBuildProjectionDecimal(
						Numeric->GetFloatingPointPropertyValue(ValuePtr),
						Resources,
						Out,
						OutError);
				}
				return TryBuildProjectionInteger(
					IsUnsignedProjectionNumber(*Numeric)
						? FString::Printf(
							TEXT("%llu"),
							static_cast<unsigned long long>(
								Numeric->GetUnsignedIntPropertyValue(ValuePtr)))
						: FString::Printf(
							TEXT("%lld"),
							static_cast<long long>(
								Numeric->GetSignedIntPropertyValue(ValuePtr))),
					Resources,
					Out,
					OutError);
			}
			if (const FStrProperty* String =
				CastField<FStrProperty>(&Property))
			{
				return TryBuildProjectionString(
					String->GetPropertyValue(ValuePtr),
					Resources,
					Out,
					OutError);
			}
			if (const FNameProperty* Name =
				CastField<FNameProperty>(&Property))
			{
				return TryBuildProjectionString(
					Name->GetPropertyValue(ValuePtr).ToString(),
					Resources,
					Out,
					OutError);
			}
			if (const FTextProperty* Text =
				CastField<FTextProperty>(&Property))
			{
				return TryBuildProjectionString(
					Text->GetPropertyValue(ValuePtr).ToString(),
					Resources,
					Out,
					OutError);
			}
			if (const FSoftObjectProperty* Soft =
				CastField<FSoftObjectProperty>(&Property))
			{
				const FSoftObjectPtr Value = Soft->GetPropertyValue(ValuePtr);
				return Value.IsNull()
					? TryBuildProjectionNull(Resources, Out, OutError)
					: TryBuildProjectionString(
						Value.ToSoftObjectPath().ToString(),
						Resources,
						Out,
						OutError);
			}
			if (const FObjectPropertyBase* Object =
				CastField<FObjectPropertyBase>(&Property))
			{
				const UObject* Value = Object->GetObjectPropertyValue(ValuePtr);
				return Value
					? TryBuildProjectionString(
						Value->GetPathName(),
						Resources,
						Out,
						OutError)
					: TryBuildProjectionNull(Resources, Out, OutError);
			}
			if (const FStructProperty* Struct =
				CastField<FStructProperty>(&Property))
			{
				return TryProjectCanonicalStruct(
					*Struct->Struct,
					ValuePtr,
					Resources,
					Out,
					OutError);
			}
			if (const FArrayProperty* Array =
				CastField<FArrayProperty>(&Property))
			{
				FScriptArrayHelper Helper(
					Array,
					const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				Items.Reserve(Helper.Num());
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					FUeAgentCanonicalValuePtr Item;
					if (!TryProjectCanonicalProperty(
							*Array->Inner,
							Helper.GetRawPtr(Index),
							Resources,
							Item,
							OutError))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
			if (const FSetProperty* Set =
				CastField<FSetProperty>(&Property))
			{
				FScriptSetHelper Helper(Set, const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Item;
					if (!TryProjectCanonicalProperty(
							*Set->ElementProp,
							Helper.GetElementPtr(Index),
							Resources,
							Item,
							OutError))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				Items.Sort(
					[](const FUeAgentCanonicalValueRef& Left,
						const FUeAgentCanonicalValueRef& Right)
					{
						return ProjectionFingerprint(Left.Get())
							< ProjectionFingerprint(Right.Get());
					});
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
			if (const FMapProperty* Map =
				CastField<FMapProperty>(&Property))
			{
				FScriptMapHelper Helper(Map, const_cast<void*>(ValuePtr));
				TArray<FUeAgentCanonicalValueRef> Items;
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Key;
					FUeAgentCanonicalValuePtr Value;
					FUeAgentCanonicalValuePtr Entry;
					if (!TryProjectCanonicalProperty(
							*Map->KeyProp,
							Helper.GetKeyPtr(Index),
							Resources,
							Key,
							OutError)
						|| !TryProjectCanonicalProperty(
							*Map->ValueProp,
							Helper.GetValuePtr(Index),
							Resources,
							Value,
							OutError)
						|| !TryBuildProjectionObject(
							{
								{TEXT("key"), Key.ToSharedRef()},
								{TEXT("value"), Value.ToSharedRef()},
							},
							Resources,
							Entry,
							OutError))
					{
						return false;
					}
					Items.Add(Entry.ToSharedRef());
				}
				Items.Sort(
					[](const FUeAgentCanonicalValueRef& Left,
						const FUeAgentCanonicalValueRef& Right)
					{
						return ProjectionFingerprint(Left.Get())
							< ProjectionFingerprint(Right.Get());
					});
				return TryBuildProjectionArray(
					MoveTemp(Items),
					Resources,
					Out,
					OutError);
			}
			return Fail(
				OutError,
				TEXT("asset_data_data_table_property_type_unsupported"),
				Property.GetName());
		}

		bool TryBuildProjectedRows(
			UScriptStruct* RowStruct,
			const TArray<FTableRowValue>& Rows,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			if (!RowStruct)
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_row_struct_invalid"));
			}
			TArray<const FTableRowValue*> Ordered;
			Ordered.Reserve(Rows.Num());
			for (const FTableRowValue& Row : Rows)
			{
				Ordered.Add(&Row);
			}
			Ordered.Sort(
				[](const FTableRowValue& Left, const FTableRowValue& Right)
				{
					return Left.Name.LexicalLess(Right.Name);
				});
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FTableRowValue* Row : Ordered)
			{
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Values;
				FUeAgentCanonicalValuePtr Item;
				if (!Row
					|| !Row->Data.IsValid()
					|| Row->Data->Type != RowStruct
					|| !TryBuildProjectionString(
						Row->Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !TryProjectCanonicalStruct(
						*RowStruct,
						Row->Data->Data,
						Resources,
						Values,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("name"), Name.ToSharedRef()},
							{TEXT("values"), Values.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryBuildProjectedVector(
			const FVector& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			FUeAgentCanonicalValuePtr Z;
			return TryBuildProjectionDecimal(Value.X, Resources, X, OutError)
				&& TryBuildProjectionDecimal(Value.Y, Resources, Y, OutError)
				&& TryBuildProjectionDecimal(Value.Z, Resources, Z, OutError)
				&& TryBuildProjectionObject(
					{
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
						{TEXT("z"), Z.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedRotator(
			const FRotator& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Pitch;
			FUeAgentCanonicalValuePtr Yaw;
			FUeAgentCanonicalValuePtr Roll;
			return TryBuildProjectionDecimal(
					Value.Pitch,
					Resources,
					Pitch,
					OutError)
				&& TryBuildProjectionDecimal(
					Value.Yaw,
					Resources,
					Yaw,
					OutError)
				&& TryBuildProjectionDecimal(
					Value.Roll,
					Resources,
					Roll,
					OutError)
				&& TryBuildProjectionObject(
					{
						{TEXT("pitch"), Pitch.ToSharedRef()},
						{TEXT("yaw"), Yaw.ToSharedRef()},
						{TEXT("roll"), Roll.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		struct FProjectedMaterialSlot final
		{
			FName Name;
			FString MaterialPath;
			bool bNullMaterial = true;
		};

		bool TryBuildProjectedMaterials(
			const TArray<FProjectedMaterialSlot>& Slots,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FProjectedMaterialSlot& Slot : Slots)
			{
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Material;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionString(
						Slot.Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !(Slot.bNullMaterial
						? TryBuildProjectionNull(
							Resources,
							Material,
							OutError)
						: TryBuildProjectionString(
							Slot.MaterialPath,
							Resources,
							Material,
							OutError))
					|| !TryBuildProjectionObject(
						{
							{TEXT("slot_name"), Name.ToSharedRef()},
							{TEXT("material_ref"), Material.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		TArray<FProjectedMaterialSlot> CaptureProjectedMaterials(
			const UStaticMesh& Mesh)
		{
			TArray<FProjectedMaterialSlot> Out;
			for (const FStaticMaterial& Material : Mesh.GetStaticMaterials())
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Material.MaterialSlotName;
				Slot.bNullMaterial = Material.MaterialInterface == nullptr;
				if (Material.MaterialInterface)
				{
					Slot.MaterialPath =
						Material.MaterialInterface->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

		TArray<FProjectedMaterialSlot> CaptureProjectedMaterials(
			const USkeletalMesh& Mesh)
		{
			TArray<FProjectedMaterialSlot> Out;
			for (const FSkeletalMaterial& Material : Mesh.GetMaterials())
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Material.MaterialSlotName;
				Slot.bNullMaterial = Material.MaterialInterface == nullptr;
				if (Material.MaterialInterface)
				{
					Slot.MaterialPath =
						Material.MaterialInterface->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return Out;
		}

		bool TryReadProjectedMaterials(
			const FUeAgentCanonicalValue& Value,
			TArray<FProjectedMaterialSlot>& Out,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, TEXT("asset_data_material_slots_invalid"));
			}
			Out.Reset();
			TSet<FName> Seen;
			for (const FUeAgentCanonicalValueRef& Item : Value.GetArrayItems())
			{
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Item.Get(), TEXT("slot_name"));
				const FUeAgentCanonicalValue* MaterialValue =
					FindObjectField(Item.Get(), TEXT("material_ref"));
				FString Name;
				FProjectedMaterialSlot Slot;
				if (!HasExactObjectFields(
						Item.Get(),
						{TEXT("slot_name"), TEXT("material_ref")})
					|| !NameValue
					|| !MaterialValue
					|| !TryReadString(*NameValue, Name)
					|| Name.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, TEXT("asset_data_material_slot_invalid"));
				}
				Slot.Name = FName(*Name);
				if (Seen.Contains(Slot.Name))
				{
					return Fail(
						OutError,
						TEXT("asset_data_material_slot_duplicate"),
						Name);
				}
				if (MaterialValue->GetKind() == EUeAgentValueKind::Null)
				{
					Slot.bNullMaterial = true;
				}
				else if (TryReadString(*MaterialValue, Slot.MaterialPath))
				{
					Slot.bNullMaterial = false;
				}
				else
				{
					return Fail(OutError, TEXT("asset_data_material_slot_invalid"));
				}
				Seen.Add(Slot.Name);
				Out.Add(MoveTemp(Slot));
			}
			return true;
		}

		bool TryNormalizeRequestedMaterials(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			TArray<FProjectedMaterialSlot>& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FMaterialSlotValue> Parsed;
			if (!TryReadMaterialSlots(Resolver, Value, Parsed, OutError))
			{
				return false;
			}
			Out.Reset();
			for (const FMaterialSlotValue& Item : Parsed)
			{
				FProjectedMaterialSlot Slot;
				Slot.Name = Item.SlotName;
				Slot.bNullMaterial = !Item.Material.IsValid();
				if (Item.Material.IsValid())
				{
					Slot.MaterialPath = Item.Material->GetPathName();
				}
				Out.Add(MoveTemp(Slot));
			}
			return true;
		}

		const TCHAR* CollisionTraceName(const ECollisionTraceFlag Value)
		{
			switch (Value)
			{
			case CTF_UseDefault:
				return TEXT("default");
			case CTF_UseSimpleAndComplex:
				return TEXT("use_simple_and_complex");
			case CTF_UseSimpleAsComplex:
				return TEXT("use_simple_as_complex");
			case CTF_UseComplexAsSimple:
				return TEXT("use_complex_as_simple");
			default:
				return nullptr;
			}
		}

		bool TryBuildProjectedCollision(
			const FKAggregateGeom& Geometry,
			const ECollisionTraceFlag Trace,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			const TCHAR* TraceName = CollisionTraceName(Trace);
			if (!TraceName)
			{
				return Fail(OutError, TEXT("asset_data_collision_invalid"));
			}
			FUeAgentCanonicalValuePtr TraceValue;
			TArray<FUeAgentCanonicalValueRef> Boxes;
			TArray<FUeAgentCanonicalValueRef> Spheres;
			TArray<FUeAgentCanonicalValueRef> Capsules;
			if (!TryBuildProjectionString(
					TraceName,
					Resources,
					TraceValue,
					OutError))
			{
				return false;
			}
			for (const FKBoxElem& Box : Geometry.BoxElems)
			{
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Extent;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(Box.Center, Resources, Center, OutError)
					|| !TryBuildProjectedVector(
						FVector(Box.X, Box.Y, Box.Z) * 0.5,
						Resources,
						Extent,
						OutError)
					|| !TryBuildProjectedRotator(
						Box.Rotation,
						Resources,
						Rotation,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("extent"), Extent.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Boxes.Add(Item.ToSharedRef());
			}
			for (const FKSphereElem& Sphere : Geometry.SphereElems)
			{
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Radius;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(
						Sphere.Center,
						Resources,
						Center,
						OutError)
					|| !TryBuildProjectionDecimal(
						Sphere.Radius,
						Resources,
						Radius,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("radius"), Radius.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Spheres.Add(Item.ToSharedRef());
			}
			for (const FKSphylElem& Capsule : Geometry.SphylElems)
			{
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Radius;
				FUeAgentCanonicalValuePtr Length;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectedVector(
						Capsule.Center,
						Resources,
						Center,
						OutError)
					|| !TryBuildProjectedRotator(
						Capsule.Rotation,
						Resources,
						Rotation,
						OutError)
					|| !TryBuildProjectionDecimal(
						Capsule.Radius,
						Resources,
						Radius,
						OutError)
					|| !TryBuildProjectionDecimal(
						Capsule.Length,
						Resources,
						Length,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("center"), Center.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
							{TEXT("radius"), Radius.ToSharedRef()},
							{TEXT("length"), Length.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Capsules.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr BoxArray;
			FUeAgentCanonicalValuePtr SphereArray;
			FUeAgentCanonicalValuePtr CapsuleArray;
			return TryBuildProjectionArray(
					MoveTemp(Boxes),
					Resources,
					BoxArray,
					OutError)
				&& TryBuildProjectionArray(
					MoveTemp(Spheres),
					Resources,
					SphereArray,
					OutError)
				&& TryBuildProjectionArray(
					MoveTemp(Capsules),
					Resources,
					CapsuleArray,
					OutError)
				&& TryBuildProjectionObject(
					{
						{TEXT("trace_mode"), TraceValue.ToSharedRef()},
						{TEXT("boxes"), BoxArray.ToSharedRef()},
						{TEXT("spheres"), SphereArray.ToSharedRef()},
						{TEXT("capsules"), CapsuleArray.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedSockets(
			const TArray<FSocketValue>& Sockets,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FSocketValue& Socket : Sockets)
			{
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Location;
				FUeAgentCanonicalValuePtr Rotation;
				FUeAgentCanonicalValuePtr Scale;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionString(
						Socket.Name.ToString(),
						Resources,
						Name,
						OutError)
					|| !TryBuildProjectedVector(
						Socket.Location,
						Resources,
						Location,
						OutError)
					|| !TryBuildProjectedRotator(
						Socket.Rotation,
						Resources,
						Rotation,
						OutError)
					|| !TryBuildProjectedVector(
						Socket.Scale,
						Resources,
						Scale,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("name"), Name.ToSharedRef()},
							{TEXT("location"), Location.ToSharedRef()},
							{TEXT("rotation"), Rotation.ToSharedRef()},
							{TEXT("scale"), Scale.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildProjectionArray(
				MoveTemp(Items),
				Resources,
				Out,
				OutError);
		}

		bool TryBuildProjectedBuildSettings(
			const TArray<FMeshBuildSettings>& Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Lods;
			for (int32 Index = 0; Index < Settings.Num(); ++Index)
			{
				const FMeshBuildSettings& Value = Settings[Index];
				FUeAgentCanonicalValuePtr LodIndex;
				FUeAgentCanonicalValuePtr RecomputeNormals;
				FUeAgentCanonicalValuePtr RecomputeTangents;
				FUeAgentCanonicalValuePtr WeightedNormals;
				FUeAgentCanonicalValuePtr RemoveDegenerates;
				FUeAgentCanonicalValuePtr UseMikk;
				FUeAgentCanonicalValuePtr GenerateLightmapUvs;
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildProjectionInteger(
						FString::FromInt(Index),
						Resources,
						LodIndex,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRecomputeNormals,
						Resources,
						RecomputeNormals,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRecomputeTangents,
						Resources,
						RecomputeTangents,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bComputeWeightedNormals,
						Resources,
						WeightedNormals,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bRemoveDegenerates,
						Resources,
						RemoveDegenerates,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bUseMikkTSpace,
						Resources,
						UseMikk,
						OutError)
					|| !TryBuildProjectionBoolean(
						Value.bGenerateLightmapUVs,
						Resources,
						GenerateLightmapUvs,
						OutError)
					|| !TryBuildProjectionObject(
						{
							{TEXT("lod_index"), LodIndex.ToSharedRef()},
							{TEXT("recompute_normals"), RecomputeNormals.ToSharedRef()},
							{TEXT("recompute_tangents"), RecomputeTangents.ToSharedRef()},
							{TEXT("compute_weighted_normals"), WeightedNormals.ToSharedRef()},
							{TEXT("remove_degenerates"), RemoveDegenerates.ToSharedRef()},
							{TEXT("use_mikk_tspace"), UseMikk.ToSharedRef()},
							{TEXT("generate_lightmap_uvs"), GenerateLightmapUvs.ToSharedRef()},
						},
						Resources,
						Item,
						OutError))
				{
					return false;
				}
				Lods.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr LodArray;
			return TryBuildProjectionArray(
					MoveTemp(Lods),
					Resources,
					LodArray,
					OutError)
				&& TryBuildProjectionObject(
					{{TEXT("lods"), LodArray.ToSharedRef()}},
					Resources,
					Out,
					OutError);
		}

		bool TryReadProjectedBuildSettings(
			const FUeAgentCanonicalValue& Value,
			TArray<FMeshBuildSettings>& Out,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValue* Lods =
				FindObjectField(Value, TEXT("lods"));
			if (!Lods || Lods->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, TEXT("asset_data_build_settings_invalid"));
			}
			Out.SetNum(Lods->GetArrayItems().Num());
			TArray<FMeshBuildSettings> Defaults;
			Defaults.SetNum(Out.Num());
			return Out.IsEmpty()
				? true
				: TryReadBuildSettings(
					Value,
					Out.Num(),
					Defaults,
					Out,
					OutError);
		}

		bool TryBuildProjectedBounds(
			const FBoxSphereBounds& Bounds,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			const FBox Box = Bounds.GetBox();
			FUeAgentCanonicalValuePtr Min;
			FUeAgentCanonicalValuePtr Max;
			return TryBuildProjectedVector(Box.Min, Resources, Min, OutError)
				&& TryBuildProjectedVector(Box.Max, Resources, Max, OutError)
				&& TryBuildProjectionObject(
					{
						{TEXT("min"), Min.ToSharedRef()},
						{TEXT("max"), Max.ToSharedRef()},
					},
					Resources,
					Out,
					OutError);
		}

		bool TryBuildProjectedReference(
			const UObject* Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			return Object
				? TryBuildProjectionString(
					Object->GetPathName(),
					Resources,
					Out,
					OutError)
				: TryBuildProjectionNull(Resources, Out, OutError);
		}

		bool TryBuildProjectedLodSettings(
			const USkeletalMeshLODSettings* Settings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& Out,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Ref;
			return TryBuildProjectedReference(
					Settings,
					Resources,
					Ref,
					OutError)
				&& TryBuildProjectionObject(
					{{TEXT("lod_settings_ref"), Ref.ToSharedRef()}},
					Resources,
					Out,
					OutError);
		}

		const TCHAR* StructuredProjectionTag(
			const EStructuredAssetKind Kind,
			const FString& FieldId)
		{
			if (Kind == EStructuredAssetKind::StaticMesh)
			{
				if (FieldId == TEXT("materials")) return TEXT("static_mesh_material_slots");
				if (FieldId == TEXT("collision")) return TEXT("static_mesh_collision_asset_local");
				if (FieldId == TEXT("sockets")) return TEXT("static_mesh_sockets_relative");
				if (FieldId == TEXT("build_settings")) return TEXT("static_mesh_build_settings");
			}
			else if (Kind == EStructuredAssetKind::DataTable)
			{
				if (FieldId == TEXT("row_struct_ref")) return TEXT("data_table_row_struct");
				if (FieldId == TEXT("rows")) return TEXT("data_table_typed_rows");
			}
			else if (Kind == EStructuredAssetKind::SkeletalMesh)
			{
				if (FieldId == TEXT("materials")) return TEXT("skeletal_mesh_material_slots");
				if (FieldId == TEXT("lod_settings")) return TEXT("skeletal_mesh_lod_settings");
				if (FieldId == TEXT("bounds")) return TEXT("skeletal_mesh_imported_bounds");
				if (FieldId == TEXT("skeleton_ref")) return TEXT("skeletal_mesh_skeleton");
			}
			return nullptr;
		}

		bool TryBuildStructuredProjectionMaps(
			const EStructuredAssetKind Kind,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutCurrent,
			TMap<FString, FUeAgentCanonicalValueRef>& OutDefaults,
			FExecutionMutationError& OutError)
		{
			OutCurrent.Reset();
			OutDefaults.Reset();
			if (Kind == EStructuredAssetKind::StaticMesh)
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup = Mesh ? Mesh->GetBodySetup() : nullptr;
				if (!Mesh || !BodySetup || Mesh->GetNumSourceModels() <= 0)
				{
					return Fail(OutError, TEXT("asset_data_static_mesh_invalid"));
				}
				TArray<FMeshBuildSettings> CurrentBuild;
				TArray<FMeshBuildSettings> DefaultBuild;
				for (int32 Index = 0; Index < Mesh->GetNumSourceModels(); ++Index)
				{
					CurrentBuild.Add(Mesh->GetSourceModel(Index).BuildSettings);
					DefaultBuild.Add(FMeshBuildSettings());
				}
				FUeAgentCanonicalValuePtr CurrentMaterials;
				FUeAgentCanonicalValuePtr DefaultMaterials;
				FUeAgentCanonicalValuePtr CurrentCollision;
				FUeAgentCanonicalValuePtr DefaultCollision;
				FUeAgentCanonicalValuePtr CurrentSockets;
				FUeAgentCanonicalValuePtr DefaultSockets;
				FUeAgentCanonicalValuePtr CurrentBuildValue;
				FUeAgentCanonicalValuePtr DefaultBuildValue;
				if (!TryBuildProjectedMaterials(
						CaptureProjectedMaterials(*Mesh),
						Resources,
						CurrentMaterials,
						OutError)
					|| !TryBuildProjectedMaterials(
						{},
						Resources,
						DefaultMaterials,
						OutError)
					|| !TryBuildProjectedCollision(
						BodySetup->AggGeom,
						BodySetup->CollisionTraceFlag,
						Resources,
						CurrentCollision,
						OutError)
					|| !TryBuildProjectedCollision(
						FKAggregateGeom(),
						CTF_UseDefault,
						Resources,
						DefaultCollision,
						OutError)
					|| !TryBuildProjectedSockets(
						CaptureSockets(*Mesh),
						Resources,
						CurrentSockets,
						OutError)
					|| !TryBuildProjectedSockets(
						{},
						Resources,
						DefaultSockets,
						OutError)
					|| !TryBuildProjectedBuildSettings(
						CurrentBuild,
						Resources,
						CurrentBuildValue,
						OutError)
					|| !TryBuildProjectedBuildSettings(
						DefaultBuild,
						Resources,
						DefaultBuildValue,
						OutError))
				{
					return false;
				}
				OutCurrent.Add(TEXT("materials"), CurrentMaterials.ToSharedRef());
				OutCurrent.Add(TEXT("collision"), CurrentCollision.ToSharedRef());
				OutCurrent.Add(TEXT("sockets"), CurrentSockets.ToSharedRef());
				OutCurrent.Add(TEXT("build_settings"), CurrentBuildValue.ToSharedRef());
				OutDefaults.Add(TEXT("materials"), DefaultMaterials.ToSharedRef());
				OutDefaults.Add(TEXT("collision"), DefaultCollision.ToSharedRef());
				OutDefaults.Add(TEXT("sockets"), DefaultSockets.ToSharedRef());
				OutDefaults.Add(TEXT("build_settings"), DefaultBuildValue.ToSharedRef());
				return true;
			}
			if (Kind == EStructuredAssetKind::DataTable)
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				UScriptStruct* RowStruct = Table
					? const_cast<UScriptStruct*>(Table->GetRowStruct())
					: nullptr;
				if (!Table
					|| !RowStruct
					|| !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
				{
					return Fail(OutError, TEXT("asset_data_data_table_row_struct_invalid"));
				}
				FUeAgentCanonicalValuePtr RowStructValue;
				FUeAgentCanonicalValuePtr RowsValue;
				if (!TryBuildProjectionString(
						RowStruct->GetPathName(),
						Resources,
						RowStructValue,
						OutError)
					|| !TryBuildProjectedRows(
						RowStruct,
						CaptureRows(*Table, RowStruct),
						Resources,
						RowsValue,
						OutError))
				{
					return false;
				}
				OutCurrent.Add(TEXT("row_struct_ref"), RowStructValue.ToSharedRef());
				OutCurrent.Add(TEXT("rows"), RowsValue.ToSharedRef());
				OutDefaults = OutCurrent;
				return true;
			}

			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
			if (!Mesh)
			{
				return Fail(OutError, TEXT("asset_data_skeletal_mesh_invalid"));
			}
			FUeAgentCanonicalValuePtr Materials;
			FUeAgentCanonicalValuePtr LodSettings;
			FUeAgentCanonicalValuePtr Bounds;
			FUeAgentCanonicalValuePtr Skeleton;
			if (!TryBuildProjectedMaterials(
					CaptureProjectedMaterials(*Mesh),
					Resources,
					Materials,
					OutError)
				|| !TryBuildProjectedLodSettings(
					Mesh->GetLODSettings(),
					Resources,
					LodSettings,
					OutError)
				|| !TryBuildProjectedBounds(
					Mesh->GetImportedBounds(),
					Resources,
					Bounds,
					OutError)
				|| !TryBuildProjectedReference(
					Mesh->GetSkeleton(),
					Resources,
					Skeleton,
					OutError))
			{
				return false;
			}
			OutCurrent.Add(TEXT("materials"), Materials.ToSharedRef());
			OutCurrent.Add(TEXT("lod_settings"), LodSettings.ToSharedRef());
			OutCurrent.Add(TEXT("bounds"), Bounds.ToSharedRef());
			OutCurrent.Add(TEXT("skeleton_ref"), Skeleton.ToSharedRef());
			OutDefaults = OutCurrent;
			return true;
		}

		bool TrySelectStructuredProjectionFields(
			const EStructuredAssetKind Kind,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& Current,
			const TMap<FString, FUeAgentCanonicalValueRef>& Defaults,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrent,
			FUeAgentCanonicalValuePtr& OutDefaults,
			FUeAgentCanonicalValuePtr& OutContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_structured_asset_selected_fields_missing"));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* CurrentValue =
					Current.Find(FieldId);
				const FUeAgentCanonicalValueRef* DefaultValue =
					Defaults.Find(FieldId);
				const TCHAR* Tag = StructuredProjectionTag(Kind, FieldId);
				FUeAgentCanonicalValuePtr ContextValue;
				if (Seen.Contains(FieldId)
					|| !CurrentValue
					|| !DefaultValue
					|| !Tag
					|| !TryBuildProjectionString(
						Tag,
						Resources,
						ContextValue,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_selected_field_invalid"),
						FieldId);
				}
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *CurrentValue);
				DefaultItems.Emplace(FieldId, *DefaultValue);
				ContextItems.Emplace(FieldId, ContextValue.ToSharedRef());
			}
			return TryBuildProjectionObject(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrent,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaults,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(ContextItems),
					Resources,
					OutContext,
					OutError);
		}

		struct FProjectedChange final
		{
			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
		};

		bool TryReadStructuredChanges(
			const EStructuredAssetKind Kind,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			TMap<FString, FProjectedChange>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(ProposedAfter, TEXT("typed_changes"));
			if (CurrentFields.GetKind() != EUeAgentValueKind::Object
				|| DefaultFields.GetKind() != EUeAgentValueKind::Object
				|| ProjectionContext.GetKind() != EUeAgentValueKind::Object
				|| CurrentFields.GetObjectItems().Num()
					!= DefaultFields.GetObjectItems().Num()
				|| CurrentFields.GetObjectItems().Num()
					!= ProjectionContext.GetObjectItems().Num()
				|| !TypedChanges
				|| TypedChanges->GetKind() != EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT("asset_data_structured_asset_expected_state_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& ChangeValue
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue* Field =
					FindObjectField(ChangeValue.Get(), TEXT("field_id"));
				const FUeAgentCanonicalValue* Presence =
					FindObjectField(ChangeValue.Get(), TEXT("presence"));
				FString FieldId;
				FProjectedChange Change;
				if (!Field
					|| !Presence
					|| !TryReadString(*Field, FieldId)
					|| !TryReadString(*Presence, Change.Presence)
					|| Out.Contains(FieldId)
					|| !FindObjectField(CurrentFields, FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_expected_change_invalid"),
						FieldId);
				}
				const FUeAgentCanonicalValue* Context =
					FindObjectField(ProjectionContext, FieldId);
				FString ContextText;
				const TCHAR* ExpectedTag =
					StructuredProjectionTag(Kind, FieldId);
				if (!Context
					|| !ExpectedTag
					|| !TryReadString(*Context, ContextText)
					|| ContextText != ExpectedTag)
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_projection_context_invalid"),
						FieldId);
				}
				const FUeAgentCanonicalValue* TypedValue =
					FindObjectField(ChangeValue.Get(), TEXT("value"));
				Change.Requested = TypedValue
					? FindObjectField(*TypedValue, TEXT("canonical_value"))
					: nullptr;
				if (Change.Presence != TEXT("clear") && !Change.Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_expected_value_missing"),
						FieldId);
				}
				Out.Add(FieldId, MoveTemp(Change));
			}
			return true;
		}

		bool TryBuildExpectedStaticFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
				: CurrentFields.GetObjectItems())
			{
				const FProjectedChange* Change = Changes.Find(Current.Key);
				const FUeAgentCanonicalValueRef* Default =
					FindObjectFieldRef(DefaultFields, Current.Key);
				if (!Change || !Default)
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_expected_change_invalid"),
						Current.Key);
				}
				if (Change->Presence == TEXT("clear"))
				{
					Out.Add(Current.Key, *Default);
					continue;
				}
				FUeAgentCanonicalValuePtr Expected;
				if (Current.Key == TEXT("materials"))
				{
					TArray<FProjectedMaterialSlot> Slots;
					if (Change->Presence == TEXT("replace"))
					{
						if (!TryNormalizeRequestedMaterials(
								Resolver,
								*Change->Requested,
								Slots,
								OutError))
						{
							return false;
						}
					}
					else if (Change->Presence == TEXT("set"))
					{
						TArray<FProjectedMaterialSlot> Updates;
						if (!TryReadProjectedMaterials(Current.Value.Get(), Slots, OutError)
							|| !TryNormalizeRequestedMaterials(
								Resolver,
								*Change->Requested,
								Updates,
								OutError))
						{
							return false;
						}
						for (FProjectedMaterialSlot& Update : Updates)
						{
							const int32 Index = Slots.IndexOfByPredicate(
								[&Update](const FProjectedMaterialSlot& Item)
								{
									return Item.Name == Update.Name;
								});
							if (Index == INDEX_NONE)
							{
								Slots.Add(MoveTemp(Update));
							}
							else
							{
								Slots[Index] = MoveTemp(Update);
							}
						}
					}
					else if (Change->Presence == TEXT("remove"))
					{
						TSet<FName> Names;
						if (!TryReadProjectedMaterials(Current.Value.Get(), Slots, OutError)
							|| !TryReadRemovalNames(
								*Change->Requested,
								Names,
								OutError,
								TEXT("asset_data_material_remove_invalid")))
						{
							return false;
						}
						Slots.RemoveAll(
							[&Names](const FProjectedMaterialSlot& Item)
							{
								return Names.Contains(Item.Name);
							});
					}
					else
					{
						return Fail(
							OutError,
							TEXT("asset_data_material_presence_invalid"),
							Change->Presence);
					}
					if (!TryBuildProjectedMaterials(
							Slots,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("collision"))
				{
					FKAggregateGeom Geometry;
					ECollisionTraceFlag Trace = CTF_UseDefault;
					if (Change->Presence != TEXT("replace")
						|| !TryReadCollision(
							*Change->Requested,
							Geometry,
							Trace,
							OutError)
						|| !TryBuildProjectedCollision(
							Geometry,
							Trace,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("sockets"))
				{
					TArray<FSocketValue> CurrentSockets;
					TArray<FSocketValue> ExpectedSockets;
					FStructuredAssetChangeView View{
						Current.Key,
						Change->Presence,
						Change->Requested,
					};
					if (!TryReadSockets(Current.Value.Get(), CurrentSockets, OutError)
						|| !TryApplySocketPresence(
							View,
							CurrentSockets,
							ExpectedSockets,
							OutError)
						|| !TryBuildProjectedSockets(
							ExpectedSockets,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("build_settings"))
				{
					TArray<FMeshBuildSettings> CurrentSettings;
					TArray<FMeshBuildSettings> ExpectedSettings;
					if (Change->Presence != TEXT("set")
						|| !TryReadProjectedBuildSettings(
							Current.Value.Get(),
							CurrentSettings,
							OutError)
						|| !TryReadBuildSettings(
							*Change->Requested,
							CurrentSettings.Num(),
							CurrentSettings,
							ExpectedSettings,
							OutError)
						|| !TryBuildProjectedBuildSettings(
							ExpectedSettings,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_structured_asset_expected_field_invalid"),
						Current.Key);
				}
				Out.Add(Current.Key, Expected.ToSharedRef());
			}
			return true;
		}

		bool TryBuildExpectedDataTableFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			const FProjectedChange* RowStructChange =
				Changes.Find(TEXT("row_struct_ref"));
			const FProjectedChange* RowsChange = Changes.Find(TEXT("rows"));
			FString RowStructPath;
			if (Changes.Num() != 2
				|| !RowStructChange
				|| RowStructChange->Presence != TEXT("replace")
				|| !RowStructChange->Requested
				|| !TryReadString(*RowStructChange->Requested, RowStructPath)
				|| !RowsChange
				|| RowsChange->Presence != TEXT("replace")
				|| !RowsChange->Requested)
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_changes_invalid"));
			}
			UScriptStruct* RowStruct = LoadObject<UScriptStruct>(
				nullptr,
				*RowStructPath);
			if (!RowStruct
				|| !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_row_struct_invalid"),
					RowStructPath);
			}
			TArray<FTableRowValue> Rows;
			FUeAgentCanonicalValuePtr ProjectedRowStruct;
			FUeAgentCanonicalValuePtr ProjectedRows;
			if (!TryReadRows(
					Resolver,
					RowStruct,
					*RowsChange->Requested,
					Rows,
					OutError)
				|| !TryBuildProjectionString(
					RowStruct->GetPathName(),
					Resources,
					ProjectedRowStruct,
					OutError)
				|| !TryBuildProjectedRows(
					RowStruct,
					Rows,
					Resources,
					ProjectedRows,
					OutError))
			{
				return false;
			}
			Out.Add(TEXT("row_struct_ref"), ProjectedRowStruct.ToSharedRef());
			Out.Add(TEXT("rows"), ProjectedRows.ToSharedRef());
			return true;
		}

		bool TryBuildExpectedSkeletalFields(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const TMap<FString, FProjectedChange>& Changes,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& Out,
			FExecutionMutationError& OutError)
		{
			Out.Reset();
			const FProjectedChange* Materials = Changes.Find(TEXT("materials"));
			const FProjectedChange* LodSettings = Changes.Find(TEXT("lod_settings"));
			const FProjectedChange* Bounds = Changes.Find(TEXT("bounds"));
			const FProjectedChange* Skeleton = Changes.Find(TEXT("skeleton_ref"));
			if (Changes.Num() != 4
				|| !Materials
				|| Materials->Presence != TEXT("replace")
				|| !Materials->Requested
				|| !LodSettings
				|| LodSettings->Presence != TEXT("replace")
				|| !LodSettings->Requested
				|| !Bounds
				|| Bounds->Presence != TEXT("replace")
				|| !Bounds->Requested
				|| !Skeleton
				|| Skeleton->Presence != TEXT("replace")
				|| !Skeleton->Requested)
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_changes_invalid"));
			}
			TArray<FProjectedMaterialSlot> Slots;
			if (!TryNormalizeRequestedMaterials(
					Resolver,
					*Materials->Requested,
					Slots,
					OutError)
				|| !HasExactObjectFields(
					*LodSettings->Requested,
					{TEXT("lod_settings_ref")}))
			{
				return false;
			}
			FString LodSettingsRef;
			FString SkeletonRef;
			if (!TryReadString(
					*FindObjectField(
						*LodSettings->Requested,
						TEXT("lod_settings_ref")),
					LodSettingsRef)
				|| !TryReadString(*Skeleton->Requested, SkeletonRef))
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_reference_invalid"));
			}
			UObject* LodSettingsObject = nullptr;
			UObject* SkeletonObject = nullptr;
			FBoxSphereBounds ParsedBounds;
			if (!Resolver.TryResolveExisting(
					LodSettingsRef,
					LodSettingsObject,
					OutError)
				|| !Resolver.TryResolveExisting(
					SkeletonRef,
					SkeletonObject,
					OutError)
				|| !Cast<USkeletalMeshLODSettings>(LodSettingsObject)
				|| !Cast<USkeleton>(SkeletonObject)
				|| !TryReadBounds(*Bounds->Requested, ParsedBounds))
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_value_invalid"));
			}
			FUeAgentCanonicalValuePtr ProjectedMaterials;
			FUeAgentCanonicalValuePtr ProjectedLodSettings;
			FUeAgentCanonicalValuePtr ProjectedBounds;
			FUeAgentCanonicalValuePtr ProjectedSkeleton;
			if (!TryBuildProjectedMaterials(
					Slots,
					Resources,
					ProjectedMaterials,
					OutError)
				|| !TryBuildProjectedLodSettings(
					CastChecked<USkeletalMeshLODSettings>(LodSettingsObject),
					Resources,
					ProjectedLodSettings,
					OutError)
				|| !TryBuildProjectedBounds(
					ParsedBounds,
					Resources,
					ProjectedBounds,
					OutError)
				|| !TryBuildProjectedReference(
					CastChecked<USkeleton>(SkeletonObject),
					Resources,
					ProjectedSkeleton,
					OutError))
			{
				return false;
			}
			Out.Add(TEXT("materials"), ProjectedMaterials.ToSharedRef());
			Out.Add(TEXT("lod_settings"), ProjectedLodSettings.ToSharedRef());
			Out.Add(TEXT("bounds"), ProjectedBounds.ToSharedRef());
			Out.Add(TEXT("skeleton_ref"), ProjectedSkeleton.ToSharedRef());
			return true;
		}
	}

	bool IsStructuredAssetMechanicId(
		const FString& NativeMechanicId)
	{
		return FindStructuredAssetKind(NativeMechanicId) != nullptr;
	}

	bool TryProjectStructuredAssetCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		(void)Resolver;
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
		if (!Kind || !Target)
		{
			return Fail(
				OutError,
				TEXT("asset_data_structured_asset_projection_unsupported"),
				NativeMechanicId);
		}
		TMap<FString, FUeAgentCanonicalValueRef> Current;
		TMap<FString, FUeAgentCanonicalValueRef> Defaults;
		return TryBuildStructuredProjectionMaps(
				*Kind,
				Target,
				Resources,
				Current,
				Defaults,
				OutError)
			&& TrySelectStructuredProjectionFields(
				*Kind,
				SelectedFieldIds,
				Current,
				Defaults,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
	}

	bool TryProjectStructuredAssetExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		OutExpectedFields.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
		if (!Kind)
		{
			return Fail(
				OutError,
				TEXT("asset_data_structured_asset_projection_unsupported"),
				NativeMechanicId);
		}
		TMap<FString, FProjectedChange> Changes;
		if (!TryReadStructuredChanges(
				*Kind,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Changes,
				OutError))
		{
			return false;
		}
		TMap<FString, FUeAgentCanonicalValueRef> Expected;
		const bool bBuilt =
			*Kind == EStructuredAssetKind::StaticMesh
				? TryBuildExpectedStaticFields(
					Resolver,
					CurrentFields,
					DefaultFields,
					Changes,
					Resources,
					Expected,
					OutError)
				: *Kind == EStructuredAssetKind::DataTable
					? TryBuildExpectedDataTableFields(
						Resolver,
						Changes,
						Resources,
						Expected,
						OutError)
					: TryBuildExpectedSkeletalFields(
						Resolver,
						Changes,
						Resources,
						Expected,
						OutError);
		if (!bBuilt
			|| Expected.Num() != CurrentFields.GetObjectItems().Num())
		{
			return bBuilt
				? Fail(
					OutError,
					TEXT("asset_data_structured_asset_expected_field_count_invalid"))
				: false;
		}
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
			: CurrentFields.GetObjectItems())
		{
			const FUeAgentCanonicalValueRef* Value = Expected.Find(Current.Key);
			if (!Value)
			{
				return Fail(
					OutError,
					TEXT("asset_data_structured_asset_expected_field_invalid"),
					Current.Key);
			}
			Items.Emplace(Current.Key, *Value);
		}
		return TryBuildProjectionObject(
			MoveTemp(Items),
			Resources,
			OutExpectedFields,
			OutError);
	}

	struct FStructuredAssetPreparedData
	{
		EStructuredAssetKind Kind =
			EStructuredAssetKind::StaticMesh;

		bool bChangeStaticMaterials = false;
		bool bChangeStaticCollision = false;
		bool bChangeStaticSockets = false;
		bool bChangeStaticBuildSettings = false;
		TArray<FStaticMaterial> OriginalStaticMaterials;
		TArray<FStaticMaterial> NewStaticMaterials;
		FKAggregateGeom OriginalStaticGeometry;
		FKAggregateGeom NewStaticGeometry;
		ECollisionTraceFlag OriginalStaticTrace = CTF_UseDefault;
		ECollisionTraceFlag NewStaticTrace = CTF_UseDefault;
		TArray<TObjectPtr<UStaticMeshSocket>>
			OriginalStaticSockets;
		TArray<FSocketValue> NewStaticSockets;
		TArray<FMeshBuildSettings>
			OriginalStaticBuildSettings;
		TArray<FMeshBuildSettings> NewStaticBuildSettings;

		TStrongObjectPtr<UScriptStruct> OriginalRowStruct;
		TStrongObjectPtr<UScriptStruct> NewRowStruct;
		TArray<FTableRowValue> OriginalRows;
		TArray<FTableRowValue> NewRows;

		TArray<FSkeletalMaterial> OriginalSkeletalMaterials;
		TArray<FSkeletalMaterial> NewSkeletalMaterials;
		TStrongObjectPtr<USkeletalMeshLODSettings>
			OriginalLodSettings;
		TStrongObjectPtr<USkeletalMeshLODSettings>
			NewLodSettings;
		FBoxSphereBounds OriginalSkeletalBounds;
		FBoxSphereBounds NewSkeletalBounds;
		TStrongObjectPtr<USkeleton> OriginalSkeleton;
		TStrongObjectPtr<USkeleton> NewSkeleton;
	};

	bool TryPrepareStructuredAssetUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FStructuredAssetChangeView>& Changes,
		FStructuredAssetPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		const EStructuredAssetKind* Kind =
			FindStructuredAssetKind(NativeMechanicId);
		if (!Kind)
		{
			return Fail(
				OutError,
				TEXT("asset_data_native_profile_unsupported"),
				NativeMechanicId);
		}
		if (!Target || Changes.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("asset_data_structured_asset_changes_missing"),
				NativeMechanicId);
		}
		const TSharedRef<
			FStructuredAssetPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FStructuredAssetPreparedData,
					ESPMode::ThreadSafe>();
		Data->Kind = *Kind;

		if (Data->Kind == EStructuredAssetKind::StaticMesh)
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
			if (!Mesh || !Mesh->GetBodySetup() || Changes.Num() > 4)
			{
				return Fail(
					OutError,
					TEXT("asset_data_static_mesh_invalid"));
			}
			Data->OriginalStaticMaterials =
				Mesh->GetStaticMaterials();
			Data->NewStaticMaterials =
				Data->OriginalStaticMaterials;
			Data->OriginalStaticGeometry =
				Mesh->GetBodySetup()->AggGeom;
			Data->NewStaticGeometry =
				Data->OriginalStaticGeometry;
			Data->OriginalStaticTrace =
				Mesh->GetBodySetup()->CollisionTraceFlag;
			Data->NewStaticTrace =
				Data->OriginalStaticTrace;
			Data->OriginalStaticSockets = Mesh->Sockets;
			Data->NewStaticSockets = CaptureSockets(*Mesh);
			for (int32 Index = 0;
				Index < Mesh->GetNumSourceModels();
				++Index)
			{
				Data->OriginalStaticBuildSettings.Add(
					Mesh->GetSourceModel(Index).BuildSettings);
			}
			Data->NewStaticBuildSettings =
				Data->OriginalStaticBuildSettings;

			if (const FStructuredAssetChangeView* Materials =
				FindChange(Changes, TEXT("materials")))
			{
				Data->bChangeStaticMaterials = true;
				TArray<FMaterialSlotValue> NewSlots;
				if (!TryApplyMaterialPresence(
						Resolver,
						*Materials,
						CaptureStaticMaterialSlots(*Mesh),
						NewSlots,
						OutError))
				{
					return false;
				}
				Data->NewStaticMaterials =
					MakeStaticMaterials(NewSlots);
			}
			if (const FStructuredAssetChangeView* Collision =
				FindChange(Changes, TEXT("collision")))
			{
				Data->bChangeStaticCollision = true;
				if (Collision->Presence == TEXT("clear"))
				{
					Data->NewStaticGeometry = FKAggregateGeom();
					Data->NewStaticTrace = CTF_UseDefault;
				}
				else if (
					Collision->Presence != TEXT("replace")
					|| !Collision->CanonicalValue
					|| !TryReadCollision(
						*Collision->CanonicalValue,
						Data->NewStaticGeometry,
						Data->NewStaticTrace,
						OutError))
				{
					return false;
				}
			}
			if (const FStructuredAssetChangeView* Sockets =
				FindChange(Changes, TEXT("sockets")))
			{
				Data->bChangeStaticSockets = true;
				if (!TryApplySocketPresence(
						*Sockets,
						CaptureSockets(*Mesh),
						Data->NewStaticSockets,
						OutError))
				{
					return false;
				}
			}
			if (const FStructuredAssetChangeView* BuildSettings =
				FindChange(Changes, TEXT("build_settings")))
			{
				Data->bChangeStaticBuildSettings = true;
				if (BuildSettings->Presence == TEXT("clear"))
				{
					Data->NewStaticBuildSettings.Reset();
					Data->NewStaticBuildSettings.SetNum(
						Mesh->GetNumSourceModels());
				}
				else if (
					BuildSettings->Presence != TEXT("set")
					|| !BuildSettings->CanonicalValue
					|| !TryReadBuildSettings(
						*BuildSettings->CanonicalValue,
						Mesh->GetNumSourceModels(),
						Data->OriginalStaticBuildSettings,
						Data->NewStaticBuildSettings,
						OutError))
				{
					return false;
				}
			}
			if (!Data->bChangeStaticMaterials
				&& !Data->bChangeStaticCollision
				&& !Data->bChangeStaticSockets
				&& !Data->bChangeStaticBuildSettings)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_structured_asset_changes_missing"));
			}
		}
		else if (Data->Kind == EStructuredAssetKind::DataTable)
		{
			UDataTable* Table = Cast<UDataTable>(Target);
			const FStructuredAssetChangeView* RowStruct =
				FindChange(Changes, TEXT("row_struct_ref"));
			const FStructuredAssetChangeView* Rows =
				FindChange(Changes, TEXT("rows"));
			if (!Table
				|| Changes.Num() != 2
				|| !RowStruct
				|| RowStruct->Presence != TEXT("replace")
				|| !RowStruct->CanonicalValue
				|| !Rows
				|| Rows->Presence != TEXT("replace")
				|| !Rows->CanonicalValue)
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_changes_invalid"));
			}
			FString RowStructPath;
			if (!TryReadString(
					*RowStruct->CanonicalValue,
					RowStructPath))
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_row_struct_invalid"));
			}
			UScriptStruct* NewRowStruct =
				LoadObject<UScriptStruct>(
					nullptr,
					*RowStructPath);
			if (!NewRowStruct
				|| !NewRowStruct->IsChildOf(
					FTableRowBase::StaticStruct()))
			{
				return Fail(
					OutError,
					TEXT("asset_data_data_table_row_struct_invalid"),
					RowStructPath);
			}
			UScriptStruct* OriginalRowStruct =
				const_cast<UScriptStruct*>(Table->GetRowStruct());
			Data->OriginalRowStruct.Reset(OriginalRowStruct);
			Data->NewRowStruct.Reset(NewRowStruct);
			Data->OriginalRows =
				CaptureRows(*Table, OriginalRowStruct);
			if (!TryReadRows(
					Resolver,
					NewRowStruct,
					*Rows->CanonicalValue,
					Data->NewRows,
					OutError))
			{
				return false;
			}
		}
		else if (Data->Kind == EStructuredAssetKind::SkeletalMesh)
		{
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
			const FStructuredAssetChangeView* Materials =
				FindChange(Changes, TEXT("materials"));
			const FStructuredAssetChangeView* LodSettings =
				FindChange(Changes, TEXT("lod_settings"));
			const FStructuredAssetChangeView* Bounds =
				FindChange(Changes, TEXT("bounds"));
			const FStructuredAssetChangeView* Skeleton =
				FindChange(Changes, TEXT("skeleton_ref"));
			if (!Mesh
				|| Changes.Num() != 4
				|| !Materials
				|| Materials->Presence != TEXT("replace")
				|| !Materials->CanonicalValue
				|| !LodSettings
				|| LodSettings->Presence != TEXT("replace")
				|| !LodSettings->CanonicalValue
				|| !Bounds
				|| Bounds->Presence != TEXT("replace")
				|| !Bounds->CanonicalValue
				|| !Skeleton
				|| Skeleton->Presence != TEXT("replace")
				|| !Skeleton->CanonicalValue)
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_changes_invalid"));
			}
			TArray<FMaterialSlotValue> Slots;
			if (!TryReadMaterialSlots(
					Resolver,
					*Materials->CanonicalValue,
					Slots,
					OutError))
			{
				return false;
			}
			if (!HasExactObjectFields(
					*LodSettings->CanonicalValue,
					{TEXT("lod_settings_ref")}))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_skeletal_mesh_lod_settings_invalid"));
			}
			FString LodSettingsRef;
			FString SkeletonRef;
			if (!TryReadString(
					*FindObjectField(
						*LodSettings->CanonicalValue,
						TEXT("lod_settings_ref")),
					LodSettingsRef)
				|| !TryReadString(
					*Skeleton->CanonicalValue,
					SkeletonRef))
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_reference_invalid"));
			}
			UObject* LodSettingsObject = nullptr;
			UObject* SkeletonObject = nullptr;
			if (!Resolver.TryResolveExisting(
					LodSettingsRef,
					LodSettingsObject,
					OutError)
				|| !Resolver.TryResolveExisting(
					SkeletonRef,
					SkeletonObject,
					OutError))
			{
				return false;
			}
			USkeletalMeshLODSettings* NewLodSettings =
				Cast<USkeletalMeshLODSettings>(LodSettingsObject);
			USkeleton* NewSkeleton = Cast<USkeleton>(SkeletonObject);
			if (!NewLodSettings
				|| !NewSkeleton
				|| !TryReadBounds(
					*Bounds->CanonicalValue,
					Data->NewSkeletalBounds))
			{
				return Fail(
					OutError,
					TEXT("asset_data_skeletal_mesh_value_invalid"));
			}
			Data->OriginalSkeletalMaterials = Mesh->GetMaterials();
			Data->NewSkeletalMaterials =
				MakeSkeletalMaterials(Slots);
			Data->OriginalLodSettings.Reset(Mesh->GetLODSettings());
			Data->NewLodSettings.Reset(NewLodSettings);
			Data->OriginalSkeletalBounds =
				Mesh->GetImportedBounds();
			Data->OriginalSkeleton.Reset(Mesh->GetSkeleton());
			Data->NewSkeleton.Reset(NewSkeleton);
		}

		OutState.Data = Data;
		return true;
	}

	bool TryApplyStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State,
		FExecutionMutationError& OutError)
	{
		if (!Target || !State.Data.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_structured_asset_state_invalid"));
		}
		const FStructuredAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case EStructuredAssetKind::StaticMesh:
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup =
					Mesh ? Mesh->GetBodySetup() : nullptr;
				if (!Mesh || !BodySetup)
				{
					return Fail(
						OutError,
						TEXT("asset_data_static_mesh_invalid"));
				}
				if (Data.bChangeStaticBuildSettings
					&& Mesh->GetNumSourceModels()
						!= Data.NewStaticBuildSettings.Num())
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_static_mesh_source_model_changed"));
				}
				Mesh->Modify();
				if (Data.bChangeStaticMaterials)
				{
					Mesh->SetStaticMaterials(
						Data.NewStaticMaterials);
				}
				if (Data.bChangeStaticCollision)
				{
					BodySetup->Modify();
					BodySetup->AggGeom = Data.NewStaticGeometry;
					BodySetup->CollisionTraceFlag =
						Data.NewStaticTrace;
					BodySetup->InvalidatePhysicsData();
				}
				if (Data.bChangeStaticSockets)
				{
					Mesh->Sockets.Reset();
					for (const FSocketValue& Value
						: Data.NewStaticSockets)
					{
						UStaticMeshSocket* Socket =
							NewObject<UStaticMeshSocket>(
								Mesh,
								NAME_None,
								RF_Transactional);
						Socket->SocketName = Value.Name;
						Socket->RelativeLocation =
							Value.Location;
						Socket->RelativeRotation =
							Value.Rotation;
						Socket->RelativeScale = Value.Scale;
						Mesh->Sockets.Add(Socket);
					}
				}
				if (Data.bChangeStaticBuildSettings)
				{
					for (int32 Index = 0;
						Index < Data.NewStaticBuildSettings.Num();
						++Index)
					{
						Mesh->GetSourceModel(Index).BuildSettings =
							Data.NewStaticBuildSettings[Index];
					}
				}
				Mesh->PostEditChange();
				break;
			}
		case EStructuredAssetKind::DataTable:
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				if (!Table
					|| !State.Data->NewRowStruct.IsValid())
				{
					return Fail(
						OutError,
						TEXT("asset_data_data_table_apply_failed"));
				}
				Table->Modify();
				if (!WriteRows(
						*Table,
						State.Data->NewRowStruct.Get(),
						State.Data->NewRows))
				{
					return Fail(
						OutError,
						TEXT("asset_data_data_table_apply_failed"));
				}
				Table->PostEditChange();
				break;
			}
		case EStructuredAssetKind::SkeletalMesh:
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
				if (!Mesh
					|| !Data.NewLodSettings.IsValid()
					|| !Data.NewSkeleton.IsValid())
				{
					return Fail(
						OutError,
						TEXT("asset_data_skeletal_mesh_invalid"));
				}
				Mesh->Modify();
				Mesh->SetMaterials(Data.NewSkeletalMaterials);
				Mesh->SetLODSettings(Data.NewLodSettings.Get());
				Mesh->SetImportedBounds(Data.NewSkeletalBounds);
				Mesh->SetSkeleton(Data.NewSkeleton.Get());
				Mesh->PostEditChange();
				break;
			}
		}
		return true;
	}

	bool TryRollbackStructuredAssetUpdate(
		UObject* Target,
		const FStructuredAssetPreparedState& State)
	{
		if (!Target || !State.Data.IsValid())
		{
			return false;
		}
		const FStructuredAssetPreparedData& Data = *State.Data;
		switch (Data.Kind)
		{
		case EStructuredAssetKind::StaticMesh:
			{
				UStaticMesh* Mesh = Cast<UStaticMesh>(Target);
				UBodySetup* BodySetup =
					Mesh ? Mesh->GetBodySetup() : nullptr;
				if (!Mesh || !BodySetup)
				{
					return false;
				}
				if (Data.bChangeStaticBuildSettings
					&& Mesh->GetNumSourceModels()
						!= Data.OriginalStaticBuildSettings.Num())
				{
					return false;
				}
				Mesh->Modify();
				if (Data.bChangeStaticMaterials)
				{
					Mesh->SetStaticMaterials(
						Data.OriginalStaticMaterials);
				}
				if (Data.bChangeStaticCollision)
				{
					BodySetup->Modify();
					BodySetup->AggGeom =
						Data.OriginalStaticGeometry;
					BodySetup->CollisionTraceFlag =
						Data.OriginalStaticTrace;
					BodySetup->InvalidatePhysicsData();
				}
				if (Data.bChangeStaticSockets)
				{
					Mesh->Sockets = Data.OriginalStaticSockets;
				}
				if (Data.bChangeStaticBuildSettings)
				{
					for (int32 Index = 0;
						Index
							< Data.OriginalStaticBuildSettings.Num();
						++Index)
					{
						Mesh->GetSourceModel(Index).BuildSettings =
							Data.OriginalStaticBuildSettings[Index];
					}
				}
				Mesh->PostEditChange();
				break;
			}
		case EStructuredAssetKind::DataTable:
			{
				UDataTable* Table = Cast<UDataTable>(Target);
				if (!Table
					|| !State.Data->OriginalRowStruct.IsValid())
				{
					return false;
				}
				Table->Modify();
				if (!WriteRows(
						*Table,
						State.Data->OriginalRowStruct.Get(),
						State.Data->OriginalRows))
				{
					return false;
				}
				Table->PostEditChange();
				break;
			}
		case EStructuredAssetKind::SkeletalMesh:
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Target);
				if (!Mesh)
				{
					return false;
				}
				Mesh->Modify();
				Mesh->SetMaterials(Data.OriginalSkeletalMaterials);
				Mesh->SetLODSettings(
					Data.OriginalLodSettings.Get());
				Mesh->SetImportedBounds(
					Data.OriginalSkeletalBounds);
				Mesh->SetSkeleton(Data.OriginalSkeleton.Get());
				Mesh->PostEditChange();
				break;
			}
		}
		return true;
	}
}
