// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
enum class Mode : u64 {
    Default,
    Patch,
    Prim,
    Attr,
};

enum class Shift : u64 {
    Default,
    U16,
    B32,
};

} // Anonymous namespace

void TranslatorVisitor::ISBERD(u64 insn) {
    LOG_DEBUG(Shader, "called with insn={:#x}", insn);

    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg;
        BitField<8, 8, u32> src_reg_num;
        BitField<24, 8, u32> imm;
        BitField<31, 1, u64> skew;
        BitField<32, 1, u64> o;
        BitField<33, 2, Mode> mode;
        BitField<47, 2, Shift> shift;
    } const isberd{insn};

    if (isberd.skew != 0) {
        throw NotImplementedException("ISBERD SKEW");
    }
    if (isberd.o != 0) {
        throw NotImplementedException("ISBERD O");
    }

    switch (isberd.mode.Value()) {
    case Mode::Default:
        X(isberd.dest_reg.Value(), X(isberd.src_reg.Value()));
        return;
    case Mode::Attr:
        LOG_DEBUG(Shader, "(STUBBED) ISBERD Mode Attr");
        X(isberd.dest_reg.Value(), X(isberd.src_reg.Value()));
        return;
    default:
        throw NotImplementedException("ISBERD Mode {}",
                                      static_cast<u64>(isberd.mode.Value()));
    }
}

// nx2 pseudo instruction for the ISBERD/ISBEWR of Switch 2 task and mesh programs (see
// frontend/ampere/lower_to_maxwell.h). Addresses are linear bytes in the stage buffer: reads come
// from the input attribute region, writes go to the output attribute or map region.
void TranslatorVisitor::ISBE_nx2(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> reg;
        BitField<8, 8, IR::Reg> address_reg;
        BitField<20, 24, u64> offset;
        BitField<44, 1, u64> is_32;
        BitField<45, 1, u64> attribute;
        BitField<48, 1, u64> write;
    } const isbe{insn};

    const Stage stage{env.ShaderStage()};
    if (stage != Stage::Task && stage != Stage::Mesh) {
        throw NotImplementedException("Stage buffer access outside task and mesh programs");
    }
    const IR::U32 address{ir.IAdd(X(isbe.address_reg), ir.Imm32(static_cast<u32>(isbe.offset)))};
    if (isbe.write == 0) {
        X(isbe.reg, ir.IsbeRead(address));
        return;
    }
    const IR::U32 value{X(isbe.reg)};
    if (isbe.attribute != 0) {
        if (isbe.is_32 == 0) {
            throw NotImplementedException("U8 stage buffer attribute write");
        }
        ir.IsbeWriteAttribute(address, value);
    } else if (isbe.is_32 != 0) {
        ir.IsbeWriteMapU32(address, value);
    } else {
        ir.IsbeWriteMapU8(address, value);
    }
}

} // namespace Shader::Maxwell
