#include <xbyak/xbyak.h>
#include "ChargeItemFix.h"

namespace YASTM {
	/**
	 * Check if memory has the expected bytes for patching.
	 */
	bool isChargeItemPatchable(std::uintptr_t baseAddress, std::uintptr_t offset) {
		namespace logger = SKSE::log;
		
		// Reuseable soul gem handling branch code.
		const std::uint8_t expected[] = {
			// .text:000000014088EB35
			// loc_14088EB35:
			0x48, 0x85, 0xc0,             // rax is probably ExtraDataList
			                              // test    rax, rax            ; TEST performs an implied AND operation that does not modify the destination but sets CPU flags as if it did.
										  //                             ; ANDing anything against itself produces itself, so this followed by JZ (Jump If Zero) is equivalent to the code:
										  //                             ; if (rax) { ...<jump destination>... }
			0x74, 0x05,                   // jz      short loc_14055EB3F
			0x48, 0x8b, 0x08,             // mov     rcx, [rax]          ; Dereferences rcx and assigns it to itself.
			0xeb, 0x03,                   // jmp     short loc_14088EB42
			// loc_14088EB3F:
			0x49, 0x8b, 0xcc,             // mov     rcx, r12            ; r12 has been set to 0 for the scope of the function
			// loc_14088EB42:
			0x33, 0xd2,                   // xor     edx, edx            ; equivalent to mov edx, 0
			
			// Commenting this test out because E8 is an x86 near, relative call, which can change when the game is
			// recompiled.
			//0xe8, 0x17, 0xfe, 0x87, 0xff, // call    sub_14010E960       ; BSExtraDataList::SetSoul(rcx, edx)
		};

		if (std::memcmp(reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(baseAddress + offset)), expected, sizeof expected) != 0) {
			logger::critical("[CHARGE] Expected bytes for reusable soul gem handling not found.");

			return false;
		}

		return true;
	}

	bool InstallChargeItemFix() {
		namespace logger = SKSE::log;

		// CraftingSubMenus::EnchantMenu::EnchantItem
		const REL::ID chargeItem_id{50980}; // SkyrimSE.exe + 0x88e890 (v1.5.97)
		const REL::ID player_id{517014};    // SkyrimSE.exe + 0x2f26ef8 (v1.5.97)
		// This probably isn't updateInventory and may actually be part of the update loop, 
		// but updating inventory is what we use it for here.
		const REL::ID updateInventory_id{51911};   // SkyrimSE.exe + 0x8d5710 (v1.5.97)

		constexpr std::uintptr_t patchOffset = 0x2a5;

		if (!isChargeItemPatchable(chargeItem_id.address(), patchOffset)) {
			return false;
		}

		struct Patch : Xbyak::CodeGenerator {
			/**
			 * @param[in] player_id             The REL::ID of the player.
			 * @param[in] chargeItem_id         The REL::ID of the function to patch.
			 */
			explicit Patch(
				const REL::ID& player_id, 
				const REL::ID& chargeItem_id,
				const REL::ID& updateInventory_id
			) {
				namespace logger = SKSE::log;
				constexpr std::uintptr_t stackSize = 0xc8;
				constexpr std::uintptr_t returnOffset = 0x2b9;
				constexpr std::uintptr_t branchReturnOffset = 0x2b2;

				// Pseudocode:
				// if (soulGem->NAM0 == null) {
				//     <go to original code>
				// } else {
				//     player->AddObjectToContainer(
				//         item      = soulGem->NAM0, 
				//         extraList = null, 
				//         count     = 1, 
				//         fromRefr  = null
				//     );
				//     updateInventory(player, soulGem->NAM0)
				//     player->RemoveItem(
				//         ???, 
				//         item      = soulGem, 
				//         count     = 1, 
				//         reason    = 0, 
				//         extraList = soulGemExtraDataList, 
				//         moveToRef = null,
				//         dropLoc   = null,
				//         rotate    = null
				//     );
				// }

				// rax = ExtraDataList* (probably)
				// rbx = TESSoulGem*
				// r12 = 0 (constant for this procedure)
				Xbyak::Label ifExtraDataListIsNullLabel;
				Xbyak::Label ifNAM0IsNullLabel;
				Xbyak::Label removeItemLabel;
				Xbyak::Label returnContinueLabel;

				// These labels duplicate the original branch's code.
				Xbyak::Label ifExtraDataListIsNullLabel2;
				Xbyak::Label setSoulLabel;

				// These labels reference external functions.
				Xbyak::Label updateInventoryLabel;

				// Check the NAM0 entry for the soul gem.
				mov(rcx, ptr[rbx + 0x100]); // rbx = soulGem, [rbx + 100h] = soulGem.NAM0
				test(rcx, rcx);             // ZF = 1 if rax is 0.
				jz(ifNAM0IsNullLabel, T_NEAR);

				// assign r10 to player
				mov(r10, player_id.address());
				mov(r10, ptr[r10]);

				// For details on how arguments are passed, see the x64 calling convention documentation
				// from Microsoft (specifically __fastcall).
				// 
				// - https://docs.microsoft.com/en-us/cpp/cpp/fastcall?view=msvc-160
				// - https://docs.microsoft.com/en-us/cpp/build/x64-software-conventions?view=msvc-160#register-usage

				// Case: TESSoulGem has NAM0 defined.
				//
				// PlayerCharacter::AddObjectToContainer(
				//     TESBoundObject * a_object,
				//     ExtraDataList * a_extraList,
				//     std::int32_t a_count,
				//     TESObjectREFR * a_fromRefr
				// )
				mov(rax, ptr[r10]);                    // rax <= player
				mov(ptr[rsp + stackSize - 0xa8], r12); // a_fromRefr = 0
				mov(r9d, 1);                           // a_count = 1
				mov(r8, r12);                          // a_extraList = 0
				mov(rdx, ptr[rbx + 0x100]);            // a_object = soulGem.NAM0
				mov(rcx, r10);                         // this = player
				call(qword[rax + 0x2d0]);              // PlayerCharacter::AddObjectToContainer

				// Updates the inventory UI. If we don't call this, the added soul gem won't show up until the user reopens 
				// the inventory menu.
				// 
				// Also, it seems we need to call this before removing the item, otherwise this seems to do nothing.
				//
				// 1st argument of the function seems to be the actor in question, and the 2nd the item to add.
				// When removing items, the 2nd argument should be NULL. 
				// 
				// This function is called for the item remove case already, but not for the added item (since it 
				// doesn't originally call it at all).
				mov(rdx, ptr[rbx + 0x100]);
				mov(rcx, player_id.address());
				mov(rcx, ptr[rcx]);
				call(ptr[rip + updateInventoryLabel]);

				// assign r10 to player (again, since r10 may not be preserved).
				mov(r10, player_id.address());
				mov(r10, ptr[r10]);

				// re-fetch the original value for rax since we overwrote it earlier.
				mov(rax, ptr[r15 + 0x8]);  // rax is probably extraDataList

				test(rax, rax);
				jz(ifExtraDataListIsNullLabel);
				mov(rcx, ptr[rax]); // dereference
				jmp(removeItemLabel);

				L(ifExtraDataListIsNullLabel);
				mov(rcx, r12);

				L(removeItemLabel);
				// PlayerCharacter::RemoveItem(
				//     char* ???,                      <= ???
				//     TESBoundObject * a_item,        <= soulGem
				//     std::int32_t a_count,           <= 1
				//     ITEM_REMOVE_REASON a_reason,    <= 0
				//     ExtraDataList * a_extraList,    <= soulGem's extraDataList (if it exists)
				//     TESObjectREFR * a_moveToRef,    <= 0
				//     const NiPoint3 * a_dropLoc = 0, <= 0
				//     const NiPoint3 * a_rotate = 0   <= 0
				// )
				mov(rax, ptr[r10]);                       // rax <= player
				mov(ptr[rsp + stackSize - 0x88], r12);    // a_rotate = 0
				mov(ptr[rsp + stackSize - 0x90], r12);    // a_dropLoc = 0
				mov(ptr[rsp + stackSize - 0x98], r12);    // a_moveToRef = 0
				mov(ptr[rsp + stackSize - 0xa0], rcx);    // a_extraList = soulGem's extraDataList (if it exists)
				mov(dword[rsp + stackSize - 0xa8], r12d); // a_reason = 0
				mov(r9d, 1);                              // a_count = 1
				mov(r8, rbx);                             // a_item = soulGem
				lea(rdx, ptr[rsp + stackSize + 0x8]);     // ???
				mov(rcx, r10);                            // this = player
				call(qword[rax + 0x2b0]);                 // PlayerCharacter::RemoveItem
				jmp(ptr[rip + returnContinueLabel]);

				L(returnContinueLabel);
				dq(chargeItem_id.address() + returnOffset);

				L(ifNAM0IsNullLabel);
				// Original branch code since we've overwritten some of it when performing the jump.
				test(rax, rax);
				jz(ifExtraDataListIsNullLabel2);
				mov(rcx, ptr[rax]);
				jmp(ptr[rip + setSoulLabel]);

				L(ifExtraDataListIsNullLabel2);
				mov(rcx, r12);
				jmp(ptr[rip + setSoulLabel]);

				L(setSoulLabel);
				dq(chargeItem_id.address() + branchReturnOffset);

				jmp(ptr[rip + returnContinueLabel]);

				L(updateInventoryLabel);
				dq(updateInventory_id.address());
			}
		};

		Patch patch{player_id, chargeItem_id, updateInventory_id};
		patch.ready();

		logger::info("[CHARGE] Patch size: {}", patch.getSize());

		auto& trampoline = SKSE::GetTrampoline();
		// Patch code is significantly larger than the default trampoline size, so we need to allocate more.
		SKSE::AllocTrampoline(1 << 8);
		trampoline.write_branch<6>(
			chargeItem_id.address() + patchOffset,
			trampoline.allocate(patch)
		);

		return true;
	}
} // namespace YASTM
