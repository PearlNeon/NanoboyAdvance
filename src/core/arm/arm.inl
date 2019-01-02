/*
 * Copyright (C) 2018 Frederic Meyer. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

inline void ARM7::Reset() {
    for (int i = 0; i < 16; i++)
        state.reg[i] = 0;

    for (int i = 0; i < BANK_COUNT; i++) {
        for (int j = 0; j < 7; j++)
            state.bank[i][j] = 0;
        state.spsr[i].v = 0;
    }

    state.cpsr.v = 0;
    SwitchMode(MODE_SYS);
    pipe[0] = 0xF0000000;
    pipe[1] = 0xF0000000;
}

inline void ARM7::Run() {
    auto instruction = pipe[0];

#ifdef DEBUGGER
    if (debugger != nullptr) {
        std::uint32_t ip = state.r15;
        if (state.cpsr.f.thumb) {
            ip -= 4;
        } else {
            ip -= 8;
        }
        for (auto breakpoint : debugger->Get(Breakpoint::Type::Code)) {
            if (breakpoint->address == ip) {
                breakpoint->hitTimes++;
                debugger->OnHit(breakpoint);
                /* HACK: use hit counter to determine if execution should be continued. */
                if ((breakpoint->hitTimes % 2) == 1)
                    return;
            }
        }
    }
#endif

    if (state.cpsr.f.thumb) {
        state.r15 &= ~1;

        pipe[0] = pipe[1];
        pipe[1] = ReadHalf(state.r15, ACCESS_SEQ);
        (this->*thumb_lut[instruction >> 6])(instruction);
    } else {
        state.r15 &= ~3;

        pipe[0] = pipe[1];
        pipe[1] = ReadWord(state.r15, ACCESS_SEQ);
        if (CheckCondition(static_cast<Condition>(instruction >> 28))) {
            int hash = ((instruction >> 16) & 0xFF0) |
                       ((instruction >>  4) & 0x00F);
            (this->*arm_lut[hash])(instruction);
        } else {
            state.r15 += 4;
        }
    }
}

inline void ARM7::SignalIrq() {
    if (state.cpsr.f.mask_irq) {
        return;
    }

    if (state.cpsr.f.thumb) {
        /* Store return address in r14<irq>. */
        state.bank[BANK_IRQ][BANK_R14] = state.r15;

        /* Save program status and switch to IRQ mode. */
        state.spsr[BANK_IRQ].v = state.cpsr.v;
        SwitchMode(MODE_IRQ);
        state.cpsr.f.thumb = 0;
        state.cpsr.f.mask_irq = 1;
    } else {
        /* Store return address in r14<irq>. */
        state.bank[BANK_IRQ][BANK_R14] = state.r15 - 4;

        /* Save program status and switch to IRQ mode. */
        state.spsr[BANK_IRQ].v = state.cpsr.v;
        SwitchMode(MODE_IRQ);
        state.cpsr.f.mask_irq = 1;
    }

    /* Jump to exception vector. */
    state.r15 = 0x18;
    RefillA();
}

inline void ARM7::RefillA() {
    pipe[0] = interface->ReadWord(state.r15+0, ACCESS_NSEQ);
    pipe[1] = interface->ReadWord(state.r15+4, ACCESS_SEQ);
    state.r15 += 8;
}

inline void ARM7::RefillT() {
    pipe[0] = interface->ReadHalf(state.r15+0, ACCESS_NSEQ);
    pipe[1] = interface->ReadHalf(state.r15+2, ACCESS_SEQ);
    state.r15 += 4;
}

inline void ARM7::SetNZ(std::uint32_t value) {
    state.cpsr.f.n = value >> 31;
    state.cpsr.f.z = (value == 0);
}

inline bool ARM7::CheckCondition(Condition condition) {
    auto& cpsr = state.cpsr;

    if (condition == COND_AL) {
        return true;
    }

    switch (condition) {
        case COND_EQ: return  cpsr.f.z;
        case COND_NE: return !cpsr.f.z;
        case COND_CS: return  cpsr.f.c;
        case COND_CC: return !cpsr.f.c;
        case COND_MI: return  cpsr.f.n;
        case COND_PL: return !cpsr.f.n;
        case COND_VS: return  cpsr.f.v;
        case COND_VC: return !cpsr.f.v;
        case COND_HI: return  cpsr.f.c && !cpsr.f.z;
        case COND_LS: return !cpsr.f.c ||  cpsr.f.z;
        case COND_GE: return  cpsr.f.n == cpsr.f.v;
        case COND_LT: return  cpsr.f.n != cpsr.f.v;
        case COND_GT: return !(cpsr.f.z || (cpsr.f.n != cpsr.f.v));
        case COND_LE: return   cpsr.f.z || (cpsr.f.n != cpsr.f.v);
        case COND_AL: return true;
        case COND_NV: return false;
    }

    return false;
}

inline Bank ARM7::ModeToBank(Mode mode) {
    switch (mode) {
    case MODE_USR:
    case MODE_SYS:
        return BANK_NONE;
    case MODE_FIQ:
        return BANK_FIQ;
    case MODE_IRQ:
        return BANK_IRQ;
    case MODE_SVC:
        return BANK_SVC;
    case MODE_ABT:
        return BANK_ABT;
    case MODE_UND:
        return BANK_UND;
    default:
        return BANK_NONE;
    }
}

inline void ARM7::SwitchMode(Mode new_mode) {
    auto old_mode = state.cpsr.f.mode;

    if (new_mode == old_mode)
        return;

    auto new_bank = ModeToBank(new_mode);
    auto old_bank = ModeToBank(old_mode);

    auto bank = &state.bank[0];

    state.cpsr.f.mode = new_mode;

    if (new_bank == old_bank)
        return;

    if (new_bank == BANK_FIQ || old_bank == BANK_FIQ) {
        auto old_gpr_bank = (old_bank == BANK_FIQ) ? BANK_FIQ : BANK_NONE;
        auto new_gpr_bank = (new_bank == BANK_FIQ) ? BANK_FIQ : BANK_NONE;

        /* Save General Purpose Registers */
        for (int i = 0; i < 5; i++) {
            bank[old_gpr_bank][2+i] = state.reg[8+i];
        }

        /* Restore General Purpose Registers */
        for (int i = 0; i < 5; i++) {
            state.reg[8+i] = bank[new_gpr_bank][2+i];
        }
    }

    /* Save SP and LR to current bank. */
    state.bank[old_bank][BANK_R13] = state.r13;
    state.bank[old_bank][BANK_R14] = state.r14;

    /* Restore SP and LR from new bank. */
    state.r13 = state.bank[new_bank][BANK_R13];
    state.r14 = state.bank[new_bank][BANK_R14];

    p_spsr = &state.spsr[new_bank];
}