// 人工标注 —— 126 个核心函数的返回类型
// 依据：函数名语义（主）+ 真机采样数据（辅）
// 采样验证过的：Actor_isOnGround{0,1}=bool, CameraAPI_getFov 70/76.8=float,
//               ItemStack_getMaxStackSize{64,255}=int, Block_isAir 恒0=false
#pragma once
#include <stdint.h>

#define ZR_UNKNOWN 0
#define ZR_VOID    1
#define ZR_BOOL    2
#define ZR_INT     3
#define ZR_FLOAT   4
#define ZR_PTR     5
#define ZR_STRUCT  6   // 按值返回结构体（ARM64 上通常用 x8 传返回槽指针）

typedef struct {
    const char* name;
    uint32_t    off;
    uint8_t     kind;
    const char* typeName;
    uint8_t     pad[3];
} zh_rettype;

static const zh_rettype g_rettype[] = {
    {"Abilities_setBool", 0xd23f254, 1, "void"},
    {"ActorDefinitionIdentifier_ctor", 0xcbff020, 1, "void"},
    {"Actor_addEffect", 0xceb372c, 1, "void"},
    {"Actor_getDimensionBlockSource", 0xce855b4, 5, "BlockSource*"},
    {"Actor_getEffect", 0xce8ef40, 5, "MobEffectInstance*"},
    {"Actor_getHealth", 0xce9a7f0, 4, "float"},
    {"Actor_getLevel", 0xce84d98, 5, "Level*"},
    {"Actor_getPos", 0xca6cd00, 6, "Vec3"},
    {"Actor_getRideRoot", 0xd8eeb48, 5, "Actor*"},
    {"Actor_getRotation", 0xceb77cc, 6, "Vec2"},
    {"Actor_getStatusFlag", 0xce8f198, 2, "bool"},
    {"Actor_getVehicle", 0xce8bf70, 5, "Actor*"},
    {"Actor_init", 0xce81094, 1, "void"},
    {"Actor_isInWater", 0xce8f618, 2, "bool"},
    {"Actor_isOnGround", 0xd1bcbfc, 2, "bool"},
    {"Actor_isSprinting", 0xcb3d328, 2, "bool"},
    {"Actor_jumpFromGround", 0xc489a48, 1, "void"},
    {"Actor_lerpMotion", 0xce90290, 1, "void"},
    {"Actor_setAABBDim", 0xce90354, 1, "void"},
    {"Actor_setHeadRotation", 0xceb7634, 1, "void"},
    {"Actor_setPersistent", 0xce8cfe4, 1, "void"},
    {"Actor_setPos", 0x9c031d4, 1, "void"},
    {"Actor_setStatusFlagInner", 0xceabed0, 1, "void"},
    {"Actor_statusFlagPostUpdate", 0xce95aa8, 1, "void"},
    {"BlockSource_getBlock", 0xd30434c, 5, "Block*"},
    {"BlockSource_getBlockEntity", 0xd3045b0, 5, "BlockActor*"},
    {"Block_getDestroyProgress", 0xd1db2b4, 4, "float"},
    {"Block_isAir", 0xd9d63a0, 2, "bool"},
    {"CameraAPI_getFov", 0x7d2d798, 4, "float"},
    {"CameraAvoidanceSystem_tick", 0x61d1ed8, 1, "void"},
    {"ChestScreenController_handleAutoPlace", 0x677ea74, 1, "void"},
    {"ChestScreenController_tryExit", 0x6f6e6e4, 1, "void"},
    {"ClientInputUpdateSystem_tick", 0x66eabf0, 1, "void"},
    {"ClientNetworkHandler_handle", 0x76afbc8, 1, "void"},
    {"CommandRegistry_loadRemoteCommands", 0xceaaf24, 1, "void"},
    {"CommandUtils_spawnEntityAt", 0xc42f474, 5, "Actor*"},
    {"CompoundTag_ctor", 0xa8133ec, 1, "void"},
    {"CompoundTag_putByte", 0xa3e76c4, 1, "void"},
    {"CompoundTag_putCompound", 0xa3e7cc4, 1, "void"},
    {"CompoundTag_putDouble", 0xa3e7a50, 1, "void"},
    {"CompoundTag_putFloat", 0xa3e7984, 1, "void"},
    {"CompoundTag_putInt", 0xa3e7824, 1, "void"},
    {"CompoundTag_putInt64", 0xa3e78d4, 1, "void"},
    {"CompoundTag_putList", 0xa3e70e0, 1, "void"},
    {"CompoundTag_putShort", 0xa3e7774, 1, "void"},
    {"CompoundTag_putString", 0xa3e7b1c, 1, "void"},
    {"CompoundTag_removeTag", 0xa3e9de4, 1, "void"},
    {"ContainerScreenController_getStackFromModel", 0x677d6ac, 5, "ItemStack*"},
    {"ContainerScreenController_handleCoalesceToIndex", 0x677fa6c, 1, "void"},
    {"ContainerScreenController_handleDropItem", 0x677f53c, 1, "void"},
    {"ContainerScreenController_handlePlaceAll", 0x677e57c, 1, "void"},
    {"ContainerScreenController_handleSwap", 0x6780260, 1, "void"},
    {"ContainerScreenController_handleTakeAll", 0x66a4d1c, 1, "void"},
    {"ContainerScreenController_tick", 0x677cbd4, 1, "void"},
    {"DroppedItem_applyRotateY", 0x71b71cc, 1, "void"},
    {"DroppedItem_applyTranslate", 0x71b71ac, 1, "void"},
    {"EnchantUtils_applyEnchant", 0xd375388, 1, "void"},
    {"EnchantValidate", 0xd3775d0, 1, "void"},
    {"GameMode_attack", 0xd06c8d8, 2, "bool"},
    {"GameMode_baseUseItem", 0xd0709b8, 2, "bool"},
    {"GameMode_buildBlock", 0xd06ea80, 2, "bool"},
    {"GameMode_destroyBlock", 0xd06d694, 2, "bool"},
    {"GameMode_getDestroyRate", 0xd06e5d4, 4, "float"},
    {"GameMode_releaseUsingItem", 0xd07163c, 1, "void"},
    {"GameMode_startDestroyBlock", 0xd06cd88, 2, "bool"},
    {"Global_runtimeIdGetter", 0xce8be64, 3, "uint64_t"},
    {"GuiData_GuiData", 0x6171644, 1, "void"},
    {"GuiData_setActionBar", 0x6386f08, 1, "void"},
    {"GuiData_setSubtitle", 0x617a6c0, 1, "void"},
    {"GuiData_setTitle", 0x617a4f8, 1, "void"},
    {"GuiData_setTitleTimes", 0x617a928, 1, "void"},
    {"ItemInHandRenderer_renderFirstPerson", 0x7cd0300, 1, "void"},
    {"ItemRenderer_render", 0x71d0a4c, 1, "void"},
    {"ItemStackBase_getDamageValue", 0xd77db9c, 3, "int"},
    {"ItemStackBase_load", 0xd78dbe0, 2, "bool"},
    {"ItemStackBase_setDamageValue", 0xd796c7c, 1, "void"},
    {"ItemStack_copy", 0xd790254, 6, "ItemStack"},
    {"ItemStack_getCount", 0xe9117c0, 3, "int"},
    {"ItemStack_getItem", 0xd7701c0, 5, "Item*"},
    {"ItemStack_getMaxStackSize", 0xd78204c, 3, "int"},
    {"ItemStack_isBlock", 0xd779334, 2, "bool"},
    {"ItemStack_isNull", 0xd795008, 2, "bool"},
    {"Item_getRawNameId", 0xd772308, 3, "short"},
    {"JumpHack_getJumpVel", 0xa479b00, 4, "float"},
    {"KillAura_forEachPlayer", 0xdbb29d8, 1, "void"},
    {"KillAura_getDimension", 0xce8eba0, 5, "Dimension*"},
    {"KillAura_isAlive", 0xce9f020, 2, "bool"},
    {"LevelData_setTime", 0xe32a9e4, 1, "void"},
    {"LevelRendererPlayer_setupFog", 0x7d0e090, 1, "void"},
    {"ListTag_addTag", 0xa3f0134, 1, "void"},
    {"ListTag_ctor", 0xa81b4c0, 1, "void"},
    {"LocalPlayer_displayClientMessage", 0x759fcd8, 1, "void"},
    {"LocalPlayer_normalTick", 0x759b6c8, 1, "void"},
    {"LocalPlayer_startRiding", 0x75a3668, 1, "void"},
    {"LocalPlayer_swing", 0x759e50c, 1, "void"},
    {"LocalPlayer_teleportTo", 0xd1cd49c, 1, "void"},
    {"MatrixStack_getTop", 0x8f95038, 5, "Matrix*"},
    {"MobEffectInstance_ctor", 0xd111794, 1, "void"},
    {"MobEffect_getById", 0xdb73b64, 5, "MobEffect*"},
    {"Mob_BodyRotationComponent", 0xcb3c4ac, 6, "Vec2"},
    {"Mob_Del", 0xce83d00, 1, "void"},
    {"PlayerActionPacketCtor", 0xb3bef18, 1, "void"},
    {"PlayerActionPayloadCtor", 0xadc0c08, 1, "void"},
    {"PlayerInventory_setItem", 0xdc4420c, 1, "void"},
    {"Player_getAbilities", 0xd1d9f30, 5, "Abilities*"},
    {"Player_getCarriedItem", 0xd1d701c, 5, "ItemStack*"},
    {"Player_setPlayerGameType", 0xd1eacdc, 1, "void"},
    {"Player_startGliding", 0xd1ed840, 1, "void"},
    {"Player_startUsingItem", 0xd1d5dbc, 1, "void"},
    {"Player_stopGliding", 0xd1d8648, 1, "void"},
    {"Player_stopUsingItem", 0xd1d4f8c, 1, "void"},
    {"RenderContext_Ctor", 0x7491dd8, 1, "void"},
    {"ServerNetworkHandler_handleInventoryTransaction", 0x993e148, 1, "void"},
    {"ServerPlayer_sendNetworkPacket", 0xc47cce4, 1, "void"},
    {"SoundPlayerPlayFn", 0xa2772fc, 1, "void"},
    {"Supplies_getItemStack", 0xd1dfa2c, 5, "ItemStack*"},
    {"Supplies_selectSlot", 0xd1dc8ac, 1, "void"},
    {"TextPacket", 0x6da6048, 1, "void"},
    {"Timer_advanceTime", 0xcf000d0, 1, "void"},
    {"Timer_getTimeScale", 0xd957c1c, 4, "float"},
    {"Timer_setTimeScale", 0xcf000b4, 1, "void"},
    {"Weather_setLightningLevel", 0xe32ad54, 1, "void"},
    {"Weather_setRainLevel", 0xe32ad84, 1, "void"},
    {"sendToServer", 0xa39e13c, 1, "void"},
};
#define N_RETTYPE 124

static inline const zh_rettype* zret_find(const char* n) {
    for (int i = 0; i < N_RETTYPE; ++i)
        if (!__builtin_strcmp(g_rettype[i].name, n)) return &g_rettype[i];
    return 0;
}
