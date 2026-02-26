//
// RT64
//

#include "rt64_interpreter.h"

#include <cassert>
#include <cinttypes>

#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"

//#define DUMP_DISPLAY_LISTS

namespace RT64 {
    static FILE *displayListFp = nullptr;

    // Interpreter

    Interpreter::Interpreter() {
        state = nullptr;
        hleGBI = nullptr;
        extendedFunction = gbiManager.getExtendedFunction();
    }

    void Interpreter::setup(State *state) {
        this->state = state;
    }

    void Interpreter::loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask) {
        if (!resetFromTask) {
            state->flush();
        }

        const uint32_t AddressMask = 0xFFFFF8;
        const uint32_t maskedTextAddress = textAddress & AddressMask;
        const uint32_t maskedDataAddress = dataAddress & AddressMask;
        if ((UCode.textAddress != maskedTextAddress) || (UCode.dataAddress != maskedDataAddress)) {
            hleGBI = gbiManager.getGBIForUCode(state->RDRAM, maskedTextAddress, maskedDataAddress);
            if (hleGBI != nullptr) {
                state->rsp->setGBI(hleGBI);
            }

            UCode.textAddress = maskedTextAddress;
            UCode.dataAddress = maskedDataAddress;
        }

        if (hleGBI != nullptr) {
            GBIReset resetFunction = resetFromTask ? hleGBI->resetFromTask : hleGBI->resetFromLoad;
            if (resetFunction != nullptr) {
                resetFunction(state);
            }
        }
    }

    void Interpreter::forceGBI(GBIUCode ucode, GBIFlags flags) {
        GBI &gbi = gbiManager.gbiCache[uint32_t(ucode)];
        if (gbi.ucode == GBIUCode::Unknown) {
            gbi.ucode = ucode;
            GBI_RDP::setup(&gbi, true);
            switch (ucode) {
            case GBIUCode::F3D:
                GBI_F3D::setup(&gbi);
                break;
            case GBIUCode::F3DEX:
                GBI_F3DEX::setup(&gbi);
                break;
            case GBIUCode::F3DEX2:
                GBI_F3DEX2::setup(&gbi);
                break;
            default:
                assert(false && "forceGBI: unsupported UCode");
                break;
            }
        }

        gbi.flags = flags;
        hleGBI = &gbi;
        if (state != nullptr) {
            state->rsp->setGBI(hleGBI);
        }
    }

    void Interpreter::processRDPLists(DisplayList *dlStart, DisplayList *dlEnd) {
        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = (uint32_t)(uintptr_t)dlStart;
        state->displayListCounter++;

        // Check RDRAM if required.
        state->checkRDRAM();

        GBI *rdpGBI = state->rdp->gbi;
        constexpr unsigned int opCodeMask = 0x3F;

        // Run the command interpreter.
        assert(rdpGBI != nullptr);
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        uint32_t cmdLength;
        size_t pendingCommandRemainingBytes = state->rdp->pendingCommandRemainingBytes;

        if (dlStart >= dlEnd) {
            state->dlCpuProfiler.end();
            return;
        }

        if (pendingCommandRemainingBytes != 0) {
            // Copy the remaining command bytes from the current displaylist
            uint32_t toCopy = (uint32_t)std::min(pendingCommandRemainingBytes, (uintptr_t)dlEnd - (uintptr_t)dl);
            memcpy(state->rdp->pendingCommandBuffer.data() + state->rdp->pendingCommandCurrentBytes, dl, toCopy);

            // Modify start to skip the copied bytes
            dl = (DisplayList *)(toCopy + (uintptr_t)dl);

            // Check if we've copied all of the bytes of the command into the buffer
            if (pendingCommandRemainingBytes == toCopy) {
                // All bytes have been copied, so run the completed command
                DisplayList *pendingCommand = (DisplayList *)state->rdp->pendingCommandBuffer.data();
                opCode = (pendingCommand->w0 >> 24) & opCodeMask;
                func = rdpGBI->map[opCode];

                if (func != nullptr) {
                    func(state, &pendingCommand);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }

                state->rdp->pendingCommandCurrentBytes = 0;
                state->rdp->pendingCommandRemainingBytes = 0;
            }
            // Not all of the bytes were copied, so adjust RDP state accordingly and exit.
            else {
                state->rdp->pendingCommandCurrentBytes += toCopy;
                state->rdp->pendingCommandRemainingBytes -= toCopy;
                state->dlCpuProfiler.end();
                return;
            }
        }

        // Create a dummy pointer and pass that, since displaylist pointer incrementing is handled differently in LLE.
        DisplayList *dummy;
        while ((dl != nullptr) && ((dlEnd == nullptr) || (dl < dlEnd))) {
            opCode = (dl->w0 >> 24) & opCodeMask;

            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                dummy = dl;
                extendedFunction(state, &dl);
                cmdLength = 1;
            }
            else {
                func = rdpGBI->map[opCode];
                cmdLength = state->rdp->commandWordLengths[opCode];

#       ifdef DUMP_DISPLAY_LISTS
                RT64_LOG_PRINTF("0x%08X 0x%08X", dl->w0, dl->w1);
#       endif

                // Check if this command is unfinished and store the partial contents if so.
                if (dl + cmdLength > dlEnd) {
                    uint32_t toCopy = (uint32_t)((uintptr_t)dlEnd - (uintptr_t)dl);
                    memcpy(state->rdp->pendingCommandBuffer.data(), dl, toCopy);
                    state->rdp->pendingCommandCurrentBytes = toCopy;
                    state->rdp->pendingCommandRemainingBytes = cmdLength * sizeof(DisplayList) - toCopy;
                    break;
                }

                if (func != nullptr) {
                    dummy = dl;
                    func(state, &dummy);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown RDP opCode: %u / 0x%X", opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl += cmdLength;
            }
        }

        state->dlCpuProfiler.end();
    }

    void Interpreter::processDisplayLists(DisplayList *dlStart) {
        assert(hleGBI != nullptr);

        state->dlCpuProfiler.start();

        // Update the state with the current display list address.
        state->displayListAddress = (uint32_t)(uintptr_t)dlStart;
        state->displayListCounter++;

        // Check RDRAM if required.
        state->checkRDRAM();

        // Run the command interpreter.
        DisplayList *dl = dlStart;
        uint8_t opCode;
        GBIFunction func;
        while (dl != nullptr) {
            opCode = (dl->w0 >> 24);

            if ((extendedOpCode != 0) && (opCode == extendedOpCode)) {
                extendedFunction(state, &dl);
            }
            else {
#       ifdef DUMP_DISPLAY_LISTS
                const char* opName = "Unknown";
                if (hleGBI->ucode == GBIUCode::F3DEX2) {
                    switch (opCode) {
                        case 0x00: opName = "G_SPNOOP"; break;
                        case 0x01: opName = "G_VTX"; break;
                        case 0x02: opName = "G_MODIFYVTX"; break;
                        case 0x03: opName = "G_CULLDL"; break;
                        case 0x04: opName = "G_BRANCH_Z"; break;
                        case 0x05: opName = "G_TRI1"; break;
                        case 0x06: opName = "G_TRI2"; break;
                        case 0x07: opName = "G_QUAD"; break;
                        case 0x08: opName = "G_LINE3D"; break;
                        case 0xD6: opName = "G_DMA_IO"; break;
                        case 0xD7: opName = "G_TEXTURE"; break;
                        case 0xD8: opName = "G_POPMTX"; break;
                        case 0xD9: opName = "G_GEOMETRYMODE"; break;
                        case 0xDA: opName = "G_MTX"; break;
                        case 0xDB: opName = "G_MOVEWORD"; break;
                        case 0xDC: opName = "G_MOVEMEM"; break;
                        case 0xDD: opName = "G_LOAD_UCODE"; break;
                        case 0xDE: opName = "G_DL"; break;
                        case 0xDF: opName = "G_ENDDL"; break;
                        case 0xE0: opName = "G_SPNOOP"; break;
                        case 0xE1: opName = "G_RDPHALF_1"; break;
                        case 0xE2: opName = "G_SETOTHERMODE_L"; break;
                        case 0xE3: opName = "G_SETOTHERMODE_H"; break;
                        case 0xE4: opName = "G_TEXRECT"; break;
                        case 0xE5: opName = "G_TEXRECTFLIP"; break;
                        case 0xE6: opName = "G_RDPLOADSYNC"; break;
                        case 0xE7: opName = "G_RDPPIPESYNC"; break;
                        case 0xE8: opName = "G_RDPTILESYNC"; break;
                        case 0xE9: opName = "G_RDPFULLSYNC"; break;
                        case 0xEA: opName = "G_SETKEYGB"; break;
                        case 0xEB: opName = "G_SETKEYR"; break;
                        case 0xEC: opName = "G_SETCONVERT"; break;
                        case 0xED: opName = "G_SETSCISSOR"; break;
                        case 0xEE: opName = "G_SETPRIMDEPTH"; break;
                        case 0xEF: opName = "G_RDPSETOTHERMODE"; break;
                        case 0xF0: opName = "G_LOADTLUT"; break;
                        case 0xF1: opName = "G_RDPHALF_2"; break;
                        case 0xF2: opName = "G_SETTILESIZE"; break;
                        case 0xF3: opName = "G_LOADBLOCK"; break;
                        case 0xF4: opName = "G_LOADTILE"; break;
                        case 0xF5: opName = "G_SETTILE"; break;
                        case 0xF6: opName = "G_FILLRECT"; break;
                        case 0xF7: opName = "G_SETFILLCOLOR"; break;
                        case 0xF8: opName = "G_SETFOGCOLOR"; break;
                        case 0xF9: opName = "G_SETBLENDCOLOR"; break;
                        case 0xFA: opName = "G_SETPRIMCOLOR"; break;
                        case 0xFB: opName = "G_SETENVCOLOR"; break;
                        case 0xFC: opName = "G_SETCOMBINE"; break;
                        case 0xFD: opName = "G_SETTIMG"; break;
                        case 0xFE: opName = "G_SETZIMG"; break;
                        case 0xFF: opName = "G_SETCIMG"; break;
                    }
                } else if (hleGBI->ucode == GBIUCode::F3DEX) {
                    switch (opCode) {
                        case 0x00: opName = "G_SPNOOP"; break;
                        case 0x01: opName = "G_MTX"; break;
                        case 0x03: opName = "G_MOVEMEM"; break;
                        case 0x04: opName = "G_VTX"; break;
                        case 0x06: opName = "G_DL"; break;
                        case 0xB0: opName = "G_BRANCH_Z"; break;
                        case 0xB1: opName = "G_TRI2"; break;
                        case 0xB2: opName = "G_MODIFYVTX"; break;
                        case 0xB3: opName = "G_RDPHALF_2"; break;
                        case 0xB4: opName = "G_RDPHALF_1"; break;
                        case 0xBF: opName = "G_TRI1"; break;
                        case 0xC0: opName = "G_NOOP"; break;
                        case 0xE4: opName = "G_TEXRECT"; break;
                        case 0xE5: opName = "G_TEXRECTFLIP"; break;
                        case 0xE6: opName = "G_RDPLOADSYNC"; break;
                        case 0xE7: opName = "G_RDPPIPESYNC"; break;
                        case 0xE8: opName = "G_RDPTILESYNC"; break;
                        case 0xE9: opName = "G_RDPFULLSYNC"; break;
                        case 0xEA: opName = "G_SETKEYGB"; break;
                        case 0xEB: opName = "G_SETKEYR"; break;
                        case 0xEC: opName = "G_SETCONVERT"; break;
                        case 0xED: opName = "G_SETSCISSOR"; break;
                        case 0xEE: opName = "G_SETPRIMDEPTH"; break;
                        case 0xEF: opName = "G_RDPSETOTHERMODE"; break;
                        case 0xF0: opName = "G_LOADTLUT"; break;
                        case 0xF2: opName = "G_SETTILESIZE"; break;
                        case 0xF3: opName = "G_LOADBLOCK"; break;
                        case 0xF4: opName = "G_LOADTILE"; break;
                        case 0xF5: opName = "G_SETTILE"; break;
                        case 0xF6: opName = "G_FILLRECT"; break;
                        case 0xF7: opName = "G_SETFILLCOLOR"; break;
                        case 0xF8: opName = "G_SETFOGCOLOR"; break;
                        case 0xF9: opName = "G_SETBLENDCOLOR"; break;
                        case 0xFA: opName = "G_SETPRIMCOLOR"; break;
                        case 0xFB: opName = "G_SETENVCOLOR"; break;
                        case 0xFC: opName = "G_SETCOMBINE"; break;
                        case 0xFD: opName = "G_SETTIMG"; break;
                        case 0xFE: opName = "G_SETZIMG"; break;
                        case 0xFF: opName = "G_SETCIMG"; break;
                    }
                } else if (hleGBI->ucode == GBIUCode::F3D) {
                    switch (opCode) {
                        case 0x00: opName = "G_SPNOOP"; break;
                        case 0x01: opName = "G_MTX"; break;
                        case 0x03: opName = "G_MOVEMEM"; break;
                        case 0x04: opName = "G_VTX"; break;
                        case 0x06: opName = "G_DL"; break;
                        case 0xBF: opName = "G_TRI1"; break;
                        case 0xC0: opName = "G_NOOP"; break;
                        case 0xE4: opName = "G_TEXRECT"; break;
                        case 0xE5: opName = "G_TEXRECTFLIP"; break;
                        case 0xE6: opName = "G_RDPLOADSYNC"; break;
                        case 0xE7: opName = "G_RDPPIPESYNC"; break;
                        case 0xE8: opName = "G_RDPTILESYNC"; break;
                        case 0xE9: opName = "G_RDPFULLSYNC"; break;
                        case 0xEA: opName = "G_SETKEYGB"; break;
                        case 0xEB: opName = "G_SETKEYR"; break;
                        case 0xEC: opName = "G_SETCONVERT"; break;
                        case 0xED: opName = "G_SETSCISSOR"; break;
                        case 0xEE: opName = "G_SETPRIMDEPTH"; break;
                        case 0xEF: opName = "G_RDPSETOTHERMODE"; break;
                        case 0xF0: opName = "G_LOADTLUT"; break;
                        case 0xF2: opName = "G_SETTILESIZE"; break;
                        case 0xF3: opName = "G_LOADBLOCK"; break;
                        case 0xF4: opName = "G_LOADTILE"; break;
                        case 0xF5: opName = "G_SETTILE"; break;
                        case 0xF6: opName = "G_FILLRECT"; break;
                        case 0xF7: opName = "G_SETFILLCOLOR"; break;
                        case 0xF8: opName = "G_SETFOGCOLOR"; break;
                        case 0xF9: opName = "G_SETBLENDCOLOR"; break;
                        case 0xFA: opName = "G_SETPRIMCOLOR"; break;
                        case 0xFB: opName = "G_SETENVCOLOR"; break;
                        case 0xFC: opName = "G_SETCOMBINE"; break;
                        case 0xFD: opName = "G_SETTIMG"; break;
                        case 0xFE: opName = "G_SETZIMG"; break;
                        case 0xFF: opName = "G_SETCIMG"; break;
                    }
                }

                if (dl->w1 > 0xFFFFFFFF) {
                    RT64_LOG_PRINTF("Command: %-17s (0x%02X): 0x%08X : 0x%016" PRIX64, opName, opCode, dl->w0, (uint64_t)dl->w1);
                } else {
                    RT64_LOG_PRINTF("Command: %-17s (0x%02X): 0x%08X : 0x%08X", opName, opCode, dl->w0, (uint32_t)dl->w1);
                }
#       endif
                func = hleGBI->map[opCode];

                if (func != nullptr) {
                    func(state, &dl);
                }
                else {
                    RT64_LOG_PRINTF("DL Parser ran into an unknown opCode (GBI %u): %u / 0x%X", uint32_t(hleGBI->ucode), opCode, opCode);
                }
            }

            if (dl != nullptr) {
                dl++;
            }
        }

        state->dlCpuProfiler.end();
    }
};
