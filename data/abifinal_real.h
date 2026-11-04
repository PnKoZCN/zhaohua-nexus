// 自动生成 by gen_abifinal.py —— 参数位置 + 类型（最终版）
//
// 位置：反汇编"寄存器首次出现是读"→ 入参（规则严格，抽样 6/7 正确）
// 类型：反汇编分析寄存器用途，但只采信强证据
//       作 load/store 基址 或 参与双寄存器 add → PTR
//       参与 fmul/fadd/scvtf 等浮点运算   → FLOAT
//       其余（整数/小整数分不清，也没必要分）→ UNKNOWN
#pragma once
#include <stdint.h>

#define ZA_UNKNOWN 0
#define ZA_PTR     1   // 指针：可安全解引用 / 传给接受指针的函数
#define ZA_FLOAT   2   // 浮点：在 s 寄存器里

typedef struct {
    const char* name;
    uint32_t    off;
    uint8_t     xMask;      // bit0-7：x0-x7 是入参
    uint8_t     sMask;      // bit0-7：s0-s7 是入参
    uint8_t     xKind[8];   // ZA_* 类型
    uint8_t     sKind[8];
    uint8_t     nArg;
    uint8_t     nFArg;
} zh_abifinal;

static const zh_abifinal g_abifinal[] = {
    {"Abilities_setBool", 0xd23f254, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"ActorDefinitionIdentifier_ctor", 0xcbff020, 0x03, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Actor_addEffect", 0xceb372c, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Actor_getDimensionBlockSource", 0xce855b4, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_getEffect", 0xce8ef40, 0x03, 0x00, {0,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Actor_getHealth", 0xce9a7f0, 0x01, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"Actor_getLevel", 0xce84d98, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_getPos", 0xca6cd00, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_getRideRoot", 0xd8eeb48, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Actor_getRotation", 0xceb77cc, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_getStatusFlag", 0xce8f198, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_getVehicle", 0xce8bf70, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_init", 0xce81094, 0x0f, 0x00, {1,0,0,1,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"Actor_isInWater", 0xce8f618, 0x01, 0x05, {1,0,0,0,0,0,0,0}, {2,0,2,0,0,0,0,0}, 1, 2},
    {"Actor_isOnGround", 0xd1bcbfc, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_isSprinting", 0xcb3d328, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Actor_jumpFromGround", 0xc489a48, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_lerpMotion", 0xce90290, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Actor_setAABBDim", 0xce90354, 0x03, 0x02, {1,1,0,0,0,0,0,0}, {0,2,0,0,0,0,0,0}, 2, 1},
    {"Actor_setHeadRotation", 0xceb7634, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_setPersistent", 0xce8cfe4, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_setPos", 0x9c031d4, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"Actor_setStatusFlagInner", 0xceabed0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Actor_statusFlagPostUpdate", 0xce95aa8, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"BlockSource_getBlock", 0xd30434c, 0x03, 0x02, {1,1,0,0,0,0,0,0}, {0,2,0,0,0,0,0,0}, 2, 1},
    {"BlockSource_getBlockEntity", 0xd3045b0, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Block_getDestroyProgress", 0xd1db2b4, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Block_isAir", 0xd9d63a0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"CameraAPI_getFov", 0x7d2d798, 0x03, 0x07, {1,0,0,0,0,0,0,0}, {2,2,2,0,0,0,0,0}, 2, 3},
    {"CameraAvoidanceSystem_tick", 0x61d1ed8, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"ChestScreenController_handleAutoPlace", 0x677ea74, 0x0f, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"ChestScreenController_tryExit", 0x6f6e6e4, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ClientInputUpdateSystem_tick", 0x66eabf0, 0x02, 0x00, {0,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ClientInstance_initGuiData", 0x6347a7c, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ClientNetworkHandler_handle", 0x76afbc8, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"CommandRegistry_loadRemoteCommands", 0xceaaf24, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"CommandUtils_spawnEntityAt", 0xc42f474, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"CompoundTag_ctor", 0xa8133ec, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"CompoundTag_putByte", 0xa3e76c4, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putCompound", 0xa3e7cc4, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putDouble", 0xa3e7a50, 0x03, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 2, 1},
    {"CompoundTag_putFloat", 0xa3e7984, 0x03, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 2, 1},
    {"CompoundTag_putInt", 0xa3e7824, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putInt64", 0xa3e78d4, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putList", 0xa3e70e0, 0x07, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putShort", 0xa3e7774, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_putString", 0xa3e7b1c, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"CompoundTag_removeTag", 0xa3e9de4, 0x07, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"ContainerScreenController_getStackFromModel", 0x677d6ac, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ContainerScreenController_handleCoalesceToIndex", 0x677fa6c, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"ContainerScreenController_handleDropItem", 0x677f53c, 0x1f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 5, 0},
    {"ContainerScreenController_handlePlaceAll", 0x677e57c, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"ContainerScreenController_handleSwap", 0x6780260, 0x1f, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 5, 0},
    {"ContainerScreenController_handleTakeAll", 0x66a4d1c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ContainerScreenController_tick", 0x677cbd4, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"DroppedItem_applyRotateY", 0x71b71cc, 0x01, 0x01, {0,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"DroppedItem_applyTranslate", 0x71b71ac, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"EnchantUtils_applyEnchant", 0xd375388, 0x0f, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"EnchantValidate", 0xd3775d0, 0x03, 0x00, {0,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"GameMode_attack", 0xd06c8d8, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"GameMode_baseUseItem", 0xd0709b8, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"GameMode_buildBlock", 0xd06ea80, 0x0f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"GameMode_destroyBlock", 0xd06d694, 0x1f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 5, 0},
    {"GameMode_getDestroyRate", 0xd06e5d4, 0x01, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"GameMode_releaseUsingItem", 0xd07163c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"GameMode_startDestroyBlock", 0xd06cd88, 0x0f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"Global_runtimeIdGetter", 0xce8be64, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"GuiData_GuiData", 0x6171644, 0x02, 0x00, {0,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"GuiData_setActionBar", 0x6386f08, 0x05, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"GuiData_setSubtitle", 0x617a6c0, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"GuiData_setTitle", 0x617a4f8, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"GuiData_setTitleTimes", 0x617a928, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemInHandRenderer_renderFirstPerson", 0x7cd0300, 0xff, 0x01, {1,0,0,1,0,0,0,0}, {2,0,0,0,0,0,0,0}, 8, 1},
    {"ItemRenderer_render", 0x71d0a4c, 0x06, 0x07, {0,0,0,0,0,0,0,0}, {2,2,2,0,0,0,0,0}, 2, 3},
    {"ItemStackBase_getDamageValue", 0xd77db9c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemStackBase_load", 0xd78dbe0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemStackBase_setDamageValue", 0xd796c7c, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"ItemStack_copy", 0xd790254, 0x03, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"ItemStack_getCount", 0xe9117c0, 0x01, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"ItemStack_getItem", 0xd7701c0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemStack_getMaxStackSize", 0xd78204c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemStack_isBlock", 0xd779334, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"ItemStack_isNull", 0xd795008, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Item_getRawNameId", 0xd772308, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"JumpHack_getJumpVel", 0xa479b00, 0x04, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"KillAura_forEachPlayer", 0xdbb29d8, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"KillAura_getDimension", 0xce8eba0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"KillAura_isAlive", 0xce9f020, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"LevelData_setTime", 0xe32a9e4, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"LevelRendererPlayer_setupFog", 0x7d0e090, 0x03, 0x03, {1,0,0,0,0,0,0,0}, {2,2,0,0,0,0,0,0}, 2, 2},
    {"ListTag_addTag", 0xa3f0134, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"ListTag_ctor", 0xa81b4c0, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"LocalPlayer_displayClientMessage", 0x759fcd8, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"LocalPlayer_normalTick", 0x759b6c8, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"LocalPlayer_startRiding", 0x75a3668, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"LocalPlayer_swing", 0x759e50c, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"LocalPlayer_teleportTo", 0xd1cd49c, 0x3f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 6, 0},
    {"MatrixStack_getTop", 0x8f95038, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"MobEffectInstance_ctor", 0xd111794, 0x0f, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"MobEffect_getById", 0xdb73b64, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Mob_BodyRotationComponent", 0xcb3c4ac, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Mob_Del", 0xce83d00, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"PlayerActionPacketCtor", 0xb3bef18, 0x01, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"PlayerActionPayloadCtor", 0xadc0c08, 0x1f, 0x00, {1,0,1,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 5, 0},
    {"PlayerInventory_setItem", 0xdc4420c, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Player_getAbilities", 0xd1d9f30, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Player_getCarriedItem", 0xd1d701c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Player_setPlayerGameType", 0xd1eacdc, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"Player_startGliding", 0xd1ed840, 0x01, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"Player_startUsingItem", 0xd1d5dbc, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"Player_stopGliding", 0xd1d8648, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Player_stopUsingItem", 0xd1d4f8c, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"RenderContext_Ctor", 0x7491dd8, 0x0f, 0x00, {1,0,1,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 4, 0},
    {"ServerNetworkHandler_handleInventoryTransaction", 0x993e148, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"ServerPlayer_sendNetworkPacket", 0xc47cce4, 0x03, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
    {"SoundPlayerPlayFn", 0xa2772fc, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"Suggest_ParamTypeEnum", 0x200000, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Supplies_getItemStack", 0xd1dfa2c, 0x04, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Supplies_selectSlot", 0xd1dc8ac, 0x07, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 3, 0},
    {"TextPacket", 0x6da6048, 0xff, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 8, 0},
    {"Timer_advanceTime", 0xcf000d0, 0x01, 0x01, {1,0,0,0,0,0,0,0}, {2,0,0,0,0,0,0,0}, 1, 1},
    {"Timer_getTimeScale", 0xd957c1c, 0x00, 0x00, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 0, 0},
    {"Timer_setTimeScale", 0xcf000b4, 0x01, 0x02, {1,0,0,0,0,0,0,0}, {0,2,0,0,0,0,0,0}, 1, 1},
    {"Weather_setLightningLevel", 0xe32ad54, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"Weather_setRainLevel", 0xe32ad84, 0x01, 0x00, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1, 0},
    {"sendToServer", 0xa39e13c, 0x03, 0x00, {1,1,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 2, 0},
};
#define N_ABIFINAL 126

static inline const zh_abifinal* zabi_f(const char* n) {
    for (int i = 0; i < N_ABIFINAL; ++i)
        if (!__builtin_strcmp(g_abifinal[i].name, n)) return &g_abifinal[i];
    return 0;
}
