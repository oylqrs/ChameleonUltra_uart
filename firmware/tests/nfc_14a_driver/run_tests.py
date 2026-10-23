"""Execute the freestanding NFC 14A driver regression harness."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent.parent / "nfc_fido_bridge" / "build" / "deps"))

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
from unicorn.riscv_const import UC_RISCV_REG_A0, UC_RISCV_REG_PC, UC_RISCV_REG_RA, UC_RISCV_REG_SP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--instruction-budget", type=int, default=10_000_000)
    arguments = parser.parse_args()

    emulator = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    emulator.mem_map(0x200000, 0x10000)
    emulator.mem_map(0x300000, 0x1000)
    emulator.mem_map(0x40000000, 0x10000)
    case_address = None

    with arguments.elf.open("rb") as source:
        elf = ELFFile(source)
        segments = [segment for segment in elf.iter_segments()
                    if segment["p_type"] == "PT_LOAD" and segment["p_memsz"]]
        start = min(segment["p_vaddr"] for segment in segments) & ~0xFFF
        end = max(segment["p_vaddr"] + segment["p_memsz"] for segment in segments)
        end = (end + 0xFFF) & ~0xFFF
        emulator.mem_map(start, end - start)
        for segment in segments:
            if segment["p_filesz"]:
                emulator.mem_write(segment["p_vaddr"], segment.data())
        symbols = elf.get_section_by_name(".symtab")
        if symbols is not None:
            matches = symbols.get_symbol_by_name("nfc_14a_test_case")
            if matches:
                case_address = matches[0]["st_value"]
        entry = elf.header["e_entry"]

    emulator.reg_write(UC_RISCV_REG_SP, 0x210000 - 16)
    emulator.reg_write(UC_RISCV_REG_RA, 0x300000)
    try:
        emulator.emu_start(entry, 0x300000, timeout=15_000_000,
                           count=arguments.instruction_budget)
    except Exception as error:
        pc = emulator.reg_read(UC_RISCV_REG_PC)
        raise RuntimeError(f"Regression harness failed at PC {pc:#x}") from error

    if emulator.reg_read(UC_RISCV_REG_PC) != 0x300000:
        raise RuntimeError("Regression harness exceeded its execution budget")
    result = emulator.reg_read(UC_RISCV_REG_A0)
    if result:
        case = None if case_address is None else int.from_bytes(
            emulator.mem_read(case_address, 4), "little")
        suffix = "" if case is None else f" (case {case})"
        print(f"{arguments.elf.stem}: FAILED at C source line {result}{suffix}")
    else:
        print(f"{arguments.elf.stem}: all behavior checks passed")
    return bool(result)


if __name__ == "__main__":
    sys.exit(main())
