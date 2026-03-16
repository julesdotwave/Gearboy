/*
 * Gearboy - Nintendo Game Boy Emulator
 * Copyright (C) 2012  Ignacio Sanchez

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#include <algorithm>
#include <ctype.h>
#include "Processor.h"
#include "opcode_timing.h"
#include "opcode_names.h"

Processor::Processor(Memory* pMemory)
{
    m_pMemory = pMemory;
    m_pMemory->SetProcessor(this);
    InitOPCodeFunctors();
    m_bIME = false;
    m_bHalt = false;
    m_bCGBSpeed = false;
    m_iSpeedMultiplier = 0;
    m_bBranchTaken = false;
    m_bSkipPCBug = false;
    m_iCurrentClockCycles = 0;
    m_iDIVCycles = 0;
    m_iTIMACycles = 0;
    m_iIMECycles = 0;
    m_iSerialBit = 0;
    m_iSerialCycles = 0;
    m_bCGB = false;
    m_iUnhaltCycles = 0;
    m_iInterruptDelayCycles = 0;
    m_iAccurateOPCodeState = 0;
    m_iReadCache = 0;
    m_bBreakpointHit = false;
    m_bRequestMemBreakpoint = false;

    m_ProcessorState.AF = &AF;
    m_ProcessorState.BC = &BC;
    m_ProcessorState.DE = &DE;
    m_ProcessorState.HL = &HL;
    m_ProcessorState.SP = &SP;
    m_ProcessorState.PC = &PC;
    m_ProcessorState.IME = &m_bIME;
    m_ProcessorState.Halt = &m_bHalt;
}

Processor::~Processor()
{
}

void Processor::Init()
{
    Reset(false, false);
}

void Processor::Reset(bool bCGB, bool bGBA)
{
    m_bCGB = bCGB;
    m_bIME = false;
    m_bHalt = false;
    m_bCGBSpeed = false;
    m_iSpeedMultiplier = 0;
    m_bBranchTaken = false;
    m_bSkipPCBug = false;
    m_iCurrentClockCycles = 0;
    m_iDIVCycles = 0;
    m_iTIMACycles = 0;
    m_iIMECycles = 0;
    m_iSerialBit = 0;
    m_iSerialCycles = 0;
    m_iUnhaltCycles = 0;

    if(m_pMemory->IsBootromEnabled())
    {
        PC.SetValue(0);
        SP.SetValue(0);
        AF.SetValue(0);
        BC.SetValue(0);
        DE.SetValue(0);
        HL.SetValue(0);
    }
    else
    {
        m_pMemory->DisableBootromRegistry();
        PC.SetValue(0x100);
        SP.SetValue(0xFFFE);

        if (m_bCGB)
        {
            if (bGBA)
            {
                AF.SetValue(0x1100);
                BC.SetValue(0x0100);
            }
            else
            {
                AF.SetValue(0x1180);
                BC.SetValue(0x0000);
            }
            DE.SetValue(0xFF56);
            HL.SetValue(0x000D);
        }
        else
        {
            AF.SetValue(0x01B0);
            BC.SetValue(0x0013);
            DE.SetValue(0x00D8);
            HL.SetValue(0x014D);
        }
    }

    m_iInterruptDelayCycles = 0;
    m_iAccurateOPCodeState = 0;
    m_iReadCache = 0;
    m_GameSharkList.clear();
    m_bBreakpointHit = false;
    m_bRequestMemBreakpoint = false;
}

u8 Processor::RunFor(u8 ticks)
{
    u8 executed = 0;

    while (executed < ticks)
    {
        m_iCurrentClockCycles = 0;
        m_bBreakpointHit = false;
        m_bRequestMemBreakpoint = false;

        if (m_iAccurateOPCodeState == 0 && m_bHalt)
        {
            m_iCurrentClockCycles += AdjustedCycles(4);

            if (m_iUnhaltCycles > 0)
            {
                m_iUnhaltCycles -= m_iCurrentClockCycles;

                if (m_iUnhaltCycles <= 0)
                {
                    m_iUnhaltCycles = 0;
                    m_bHalt = false;
                }
            }

            if (m_bHalt && (InterruptPending() != None_Interrupt) && (m_iUnhaltCycles == 0))
            {
                m_iUnhaltCycles = AdjustedCycles(12);
            }
        }

        bool interrupt_served = false;

        if (!m_bHalt)
        {
            Interrupts interrupt = InterruptPending();

            if (m_bIME && (interrupt != None_Interrupt) && (m_iAccurateOPCodeState == 0))
            {
                ServeInterrupt(interrupt);
                interrupt_served = true;
            }
            else
            {
                u8 opcode = m_pMemory->Read(PC.GetValue());
                PC.Increment();

                if (m_bSkipPCBug)
                {
                    m_bSkipPCBug = false;
                    PC.Decrement();
                }

                const u8* accurateOPcodes;
                const u8* machineCycles;
                bool isCB = (opcode == 0xCB);

                if (isCB)
                {
                    accurateOPcodes = kOPCodeCBAccurate;
                    machineCycles = kOPCodeCBMachineCycles;

                    opcode = m_pMemory->Read(PC.GetValue());
                    PC.Increment();

                    if (m_bSkipPCBug)
                    {
                        m_bSkipPCBug = false;
                        PC.Decrement();
                    }
                }
                else
                {
                    accurateOPcodes = kOPCodeAccurate;
                    machineCycles = kOPCodeMachineCycles;
                }

                if ((accurateOPcodes[opcode] != 0) && (m_iAccurateOPCodeState == 0))
                {
                    int left_cycles = (accurateOPcodes[opcode] < 3 ? 2 : 3);
                    m_iCurrentClockCycles += (machineCycles[opcode] - left_cycles) * AdjustedCycles(4);
                    m_iAccurateOPCodeState = 1;
                    PC.Decrement();
                    if (isCB)
                        PC.Decrement();
                }
                else
                {
                    if (isCB)
                        ExecuteOPCodeCB(opcode);
                    else
                        ExecuteOPCode(opcode);

                    if (m_bBranchTaken)
                    {
                        m_bBranchTaken = false;
                        m_iCurrentClockCycles += kOPCodeBranchMachineCycles[opcode] * AdjustedCycles(4);
                    }
                    else
                    {
                        switch (m_iAccurateOPCodeState)
                        {
                        case 0:
                            m_iCurrentClockCycles += machineCycles[opcode] * AdjustedCycles(4);
                            break;
                        case 1:
                            if (accurateOPcodes[opcode] == 3)
                            {
                                m_iCurrentClockCycles += 1 * AdjustedCycles(4);
                                m_iAccurateOPCodeState = 2;
                                PC.Decrement();
                                if (isCB)
                                    PC.Decrement();
                            }
                            else
                            {
                                m_iCurrentClockCycles += 2 * AdjustedCycles(4);
                                m_iAccurateOPCodeState = 0;
                            }
                            break;
                        case 2:
                            m_iCurrentClockCycles += 2 * AdjustedCycles(4);
                            m_iAccurateOPCodeState = 0;
                            break;
                        }
                    }
                }
            }

            #ifndef GEARBOY_DISABLE_DISASSEMBLER
            if (Disassemble(PC.GetValue()) || m_bRequestMemBreakpoint)
                m_bBreakpointHit = true;
            #endif
        }

        if (!interrupt_served && (m_iInterruptDelayCycles > 0))
        {
            m_iInterruptDelayCycles -= m_iCurrentClockCycles;
        }

        if (!interrupt_served && (m_iAccurateOPCodeState == 0) && (m_iIMECycles > 0))
        {
            m_iIMECycles -= m_iCurrentClockCycles;

            if (m_iIMECycles <= 0)
            {
                m_iIMECycles = 0;
                m_bIME = true;
            }
        }

        executed += m_iCurrentClockCycles;
    }

    return executed;
}

void Processor::ServeInterrupt(Interrupts interrupt)
{
    u8 if_reg = m_pMemory->Retrieve(0xFF0F);
    m_bIME = false;
    StackPush(&PC);
    m_iCurrentClockCycles += AdjustedCycles(20);
    
    switch (interrupt)
    {
        case VBlank_Interrupt:
            m_iInterruptDelayCycles= 0;
            m_pMemory->Load(0xFF0F, if_reg & 0xFE);
            PC.SetValue(0x0040);
            UpdateGameShark();
            break;
        case LCDSTAT_Interrupt:
            m_pMemory->Load(0xFF0F, if_reg & 0xFD);
            PC.SetValue(0x0048);
            break;
        case Timer_Interrupt:
            m_pMemory->Load(0xFF0F, if_reg & 0xFB);
            PC.SetValue(0x0050);
            break;
        case Serial_Interrupt:
            m_pMemory->Load(0xFF0F, if_reg & 0xF7);
            PC.SetValue(0x0058);
            break;
        case Joypad_Interrupt:
            m_pMemory->Load(0xFF0F, if_reg & 0xEF);
            PC.SetValue(0x0060);
            break;
        case None_Interrupt:
            break;
    }
}

void Processor::UpdateTimers(u8 ticks)
{
    m_iDIVCycles += ticks;

    unsigned int div_cycles = AdjustedCycles(256);

    while (m_iDIVCycles >= div_cycles)
    {
        m_iDIVCycles -= div_cycles;
        u8 div = m_pMemory->Retrieve(0xFF04);
        div++;
        m_pMemory->Load(0xFF04, div);
    }

    u8 tac = m_pMemory->Retrieve(0xFF07);

    // if tima is running
    if (tac & 0x04)
    {
        m_iTIMACycles += ticks;

        unsigned int freq = 0;

        switch (tac & 0x03)
        {
            case 0:
                freq = AdjustedCycles(1024);
                break;
            case 1:
                freq = AdjustedCycles(16);
                break;
            case 2:
                freq = AdjustedCycles(64);
                break;
            case 3:
                freq = AdjustedCycles(256);
                break;
        }

        while (m_iTIMACycles >= freq)
        {
            m_iTIMACycles -= freq;
            u8 tima = m_pMemory->Retrieve(0xFF05);

            if (tima == 0xFF)
            {
                tima = m_pMemory->Retrieve(0xFF06);
                RequestInterrupt(Timer_Interrupt);
            }
            else
                tima++;

            m_pMemory->Load(0xFF05, tima);
        }
    }
}

void Processor::UpdateSerial(u8 ticks)
{
    u8 sc = m_pMemory->Retrieve(0xFF02);

    if (IsSetBit(sc, 7) && IsSetBit(sc, 0))
    {
        m_iSerialCycles += ticks;

        if (m_iSerialBit < 0)
        {
            m_iSerialBit = 0;
            m_iSerialCycles = 0;
            return;
        }

        int serial_cycles = AdjustedCycles(512);

        if (m_iSerialCycles >= serial_cycles)
        {
            if (m_iSerialBit > 7)
            {
                m_pMemory->Load(0xFF02, sc & 0x7F);
                RequestInterrupt(Serial_Interrupt);
                m_iSerialBit = -1;

                return;
            }

            u8 sb = m_pMemory->Retrieve(0xFF01);
            sb <<= 1;
            sb |= 0x01;
            m_pMemory->Load(0xFF01, sb);

            m_iSerialCycles -= serial_cycles;
            m_iSerialBit++;
        }
    }
}

void Processor::UpdateGameShark()
{
    std::list<GameSharkCode>::iterator it;

    for (it = m_GameSharkList.begin(); it != m_GameSharkList.end(); it++)
    {
        if (it->type == 0x01)
        {
            m_pMemory->Write(it->address, it->value);
        }
    }
}

bool Processor::Disassemble(u16 address)
{
    Memory::stDisassembleRecord** memoryMap = m_pMemory->GetDisassembledMemoryMap();
    Memory::stDisassembleRecord** romMap = m_pMemory->GetDisassembledROMMemoryMap();

    Memory::stDisassembleRecord** map = NULL;

    int offset = address;
    int bank = 0;
    bool rom = false;

    if ((address & 0xC000) == 0x0000)
    {
        bank = m_pMemory->GetCurrentRule()->GetCurrentRomBank0Index();
        offset = (0x4000 * bank) + address;
        map = romMap;
        rom = true;
    }
    else if ((address & 0xC000) == 0x4000)
    {
        bank = m_pMemory->GetCurrentRule()->GetCurrentRomBank1Index();
        offset = (0x4000 * bank) + (address & 0x3FFF);
        map = romMap;
        rom = true;
    }
    else
    {
        map = memoryMap;
        rom = false;
    }

    if (!IsValidPointer(map[offset]))
    {
        map[offset] = new Memory::stDisassembleRecord;

        if (rom)
        {
            map[offset]->address = offset & 0x3FFF;
            map[offset]->bank = offset >> 14;
        }
        else
        {
            map[offset]->address = 0;
            map[offset]->bank = 0;
        }

        map[offset]->name[0] = 0;
        map[offset]->bytes[0] = 0;
        map[offset]->size = 0;
        map[offset]->jump = false;
        map[offset]->jump_address = 0;
        map[offset]->disabled = false;
        for (int i = 0; i < 4; i++)
            map[offset]->opcodes[i] = 0;
    }

    u8 opcodes[4];
    bool changed = false;

    for (int i = 0; i < map[offset]->size; i++)
    {
        opcodes[i] = m_pMemory->Read(address + i);

        if (opcodes[i] != map[offset]->opcodes[i])
            changed = true;
    }

    if ((map[offset]->size == 0) || changed)
    {
        map[offset]->bank = bank;
        map[offset]->address = address;

        for (int i = 0; i < 4; i++)
            map[offset]->opcodes[i] = m_pMemory->Read(address + i);

        u8 opcode = map[offset]->opcodes[0];
        bool cb = false;

        if (opcode == 0xCB)
        {
            cb = true;
            opcode = map[offset]->opcodes[1];
        }

        stOPCodeInfo info = cb ? kOPCodeCBNames[opcode] : kOPCodeNames[opcode];

        map[offset]->size = info.size;

        map[offset]->bytes[0] = 0;

        for (int i = 0; i < 4; i++)
        {
            if (i < info.size)
            {
                char value[8];
                snprintf(value, sizeof(value), "%02X", map[offset]->opcodes[i]);
                strcat(map[offset]->bytes, value);
            }
            else
            {
                strcat(map[offset]->bytes, "  ");
            }

            if (i < 3)
                strcat(map[offset]->bytes, " ");
        }

        switch (info.type)
        {
            case 0:
                strcpy(map[offset]->name, info.name);
                break;
            case 1:
                snprintf(map[offset]->name, sizeof(map[offset]->name), info.name, map[offset]->opcodes[1]);
                break;
            case 2:
                map[offset]->jump = true;
                map[offset]->jump_address = (map[offset]->opcodes[2] << 8) | map[offset]->opcodes[1];
                snprintf(map[offset]->name, sizeof(map[offset]->name), info.name, map[offset]->jump_address);
                break;
            case 3:
                snprintf(map[offset]->name, sizeof(map[offset]->name), info.name, (s8)map[offset]->opcodes[1]);
                break;
            case 4:
                map[offset]->jump = true;
                map[offset]->jump_address = address + info.size + (s8)map[offset]->opcodes[1];
                snprintf(map[offset]->name, sizeof(map[offset]->name), info.name, map[offset]->jump_address, (s8)map[offset]->opcodes[1]);
                break;
            case 5:
                snprintf(map[offset]->name, sizeof(map[offset]->name), info.name, map[offset]->opcodes[1], kRegisterNames[map[offset]->opcodes[1]]);
                break;
            default:
                strcpy(map[offset]->name, "PARSE ERROR");
        }
    }

    Memory::stDisassembleRecord* runtobreakpoint = m_pMemory->GetRunToBreakpoint();
    std::vector<Memory::stDisassembleRecord*>* breakpoints = m_pMemory->GetBreakpointsCPU();

    if (IsValidPointer(runtobreakpoint))
    {
        if (runtobreakpoint == map[offset])
        {
            m_pMemory->SetRunToBreakpoint(NULL);
            return true;
        }
        else
            return false;
    }
    else
    {
        std::size_t size = breakpoints->size();

        for (std::size_t b = 0; b < size; b++)
        {
            if (((*breakpoints)[b] == map[offset]) && !(*breakpoints)[b]->disabled)
            {
                return true;
            }
        }
    }

    return false;
}

bool Processor::BreakpointHit()
{
    return m_bBreakpointHit;
}

void Processor::RequestMemoryBreakpoint()
{
    m_bRequestMemBreakpoint = true;
}

void Processor::SaveState(std::ostream& stream)
{
    using namespace std;

    u16 af = AF.GetValue();
    u16 bc = BC.GetValue();
    u16 de = DE.GetValue();
    u16 hl = HL.GetValue();
    u16 sp = SP.GetValue();
    u16 pc = PC.GetValue();

    stream.write(reinterpret_cast<const char*> (&af), sizeof(af));
    stream.write(reinterpret_cast<const char*> (&bc), sizeof(bc));
    stream.write(reinterpret_cast<const char*> (&de), sizeof(de));
    stream.write(reinterpret_cast<const char*> (&hl), sizeof(hl));
    stream.write(reinterpret_cast<const char*> (&sp), sizeof(sp));
    stream.write(reinterpret_cast<const char*> (&pc), sizeof(pc));

    stream.write(reinterpret_cast<const char*> (&m_bIME), sizeof(m_bIME));
    stream.write(reinterpret_cast<const char*> (&m_bHalt), sizeof(m_bHalt));
    stream.write(reinterpret_cast<const char*> (&m_bBranchTaken), sizeof(m_bBranchTaken));
    stream.write(reinterpret_cast<const char*> (&m_bSkipPCBug), sizeof(m_bSkipPCBug));
    stream.write(reinterpret_cast<const char*> (&m_iCurrentClockCycles), sizeof(m_iCurrentClockCycles));
    stream.write(reinterpret_cast<const char*> (&m_iDIVCycles), sizeof(m_iDIVCycles));
    stream.write(reinterpret_cast<const char*> (&m_iTIMACycles), sizeof(m_iTIMACycles));
    stream.write(reinterpret_cast<const char*> (&m_iSerialBit), sizeof(m_iSerialBit));
    stream.write(reinterpret_cast<const char*> (&m_iSerialCycles), sizeof(m_iSerialCycles));
    stream.write(reinterpret_cast<const char*> (&m_iIMECycles), sizeof(m_iIMECycles));
    stream.write(reinterpret_cast<const char*> (&m_iUnhaltCycles), sizeof(m_iUnhaltCycles));
    stream.write(reinterpret_cast<const char*> (&m_iInterruptDelayCycles), sizeof(m_iInterruptDelayCycles));
    stream.write(reinterpret_cast<const char*> (&m_bCGBSpeed), sizeof(m_bCGBSpeed));
    stream.write(reinterpret_cast<const char*> (&m_iSpeedMultiplier), sizeof(m_iSpeedMultiplier));
    stream.write(reinterpret_cast<const char*> (&m_iAccurateOPCodeState), sizeof(m_iAccurateOPCodeState));
    stream.write(reinterpret_cast<const char*> (&m_iReadCache), sizeof(m_iReadCache));
}

void Processor::LoadState(std::istream& stream)
{
    using namespace std;

    u16 af;
    u16 bc;
    u16 de;
    u16 hl;
    u16 sp;
    u16 pc;

    stream.read(reinterpret_cast<char*> (&af), sizeof(af));
    stream.read(reinterpret_cast<char*> (&bc), sizeof(bc));
    stream.read(reinterpret_cast<char*> (&de), sizeof(de));
    stream.read(reinterpret_cast<char*> (&hl), sizeof(hl));
    stream.read(reinterpret_cast<char*> (&sp), sizeof(sp));
    stream.read(reinterpret_cast<char*> (&pc), sizeof(pc));

    AF.SetValue(af);
    BC.SetValue(bc);
    DE.SetValue(de);
    HL.SetValue(hl);
    SP.SetValue(sp);
    PC.SetValue(pc);

    stream.read(reinterpret_cast<char*> (&m_bIME), sizeof(m_bIME));
    stream.read(reinterpret_cast<char*> (&m_bHalt), sizeof(m_bHalt));
    stream.read(reinterpret_cast<char*> (&m_bBranchTaken), sizeof(m_bBranchTaken));
    stream.read(reinterpret_cast<char*> (&m_bSkipPCBug), sizeof(m_bSkipPCBug));
    stream.read(reinterpret_cast<char*> (&m_iCurrentClockCycles), sizeof(m_iCurrentClockCycles));
    stream.read(reinterpret_cast<char*> (&m_iDIVCycles), sizeof(m_iDIVCycles));
    stream.read(reinterpret_cast<char*> (&m_iTIMACycles), sizeof(m_iTIMACycles));
    stream.read(reinterpret_cast<char*> (&m_iSerialBit), sizeof(m_iSerialBit));
    stream.read(reinterpret_cast<char*> (&m_iSerialCycles), sizeof(m_iSerialCycles));
    stream.read(reinterpret_cast<char*> (&m_iIMECycles), sizeof(m_iIMECycles));
    stream.read(reinterpret_cast<char*> (&m_iUnhaltCycles), sizeof(m_iUnhaltCycles));
    stream.read(reinterpret_cast<char*> (&m_iInterruptDelayCycles), sizeof(m_iInterruptDelayCycles));
    stream.read(reinterpret_cast<char*> (&m_bCGBSpeed), sizeof(m_bCGBSpeed));
    stream.read(reinterpret_cast<char*> (&m_iSpeedMultiplier), sizeof(m_iSpeedMultiplier));
    stream.read(reinterpret_cast<char*> (&m_iAccurateOPCodeState), sizeof(m_iAccurateOPCodeState));
    stream.read(reinterpret_cast<char*> (&m_iReadCache), sizeof(m_iReadCache));
}

void Processor::SetGameSharkCheat(const char* szCheat)
{
    std::string code(szCheat);
    for (std::string::iterator p = code.begin(); code.end() != p; ++p)
        *p = toupper(*p);

    if (code.length() == 8)
    {
        GameSharkCode gsc;

        gsc.type = AsHex(code[0]) << 4 | AsHex(code[1]);
        gsc.value = (AsHex(code[2]) << 4 | AsHex(code[3])) & 0xFF;
        gsc.address = (AsHex(code[4]) << 4 | AsHex(code[5]) | AsHex(code[6]) << 12 | AsHex(code[7]) << 8) & 0xFFFF;

        m_GameSharkList.push_back(gsc);
    }
}

void Processor::ClearGameSharkCheats()
{
    m_GameSharkList.clear();
}

Processor::ProcessorState* Processor::GetState()
{
    return &m_ProcessorState;
}

void Processor::ExecuteOPCode(u8 opcode)
{
    switch (opcode)
    {
        case 0x00: OPCode0x00(); break; case 0x01: OPCode0x01(); break;
        case 0x02: OPCode0x02(); break; case 0x03: OPCode0x03(); break;
        case 0x04: OPCode0x04(); break; case 0x05: OPCode0x05(); break;
        case 0x06: OPCode0x06(); break; case 0x07: OPCode0x07(); break;
        case 0x08: OPCode0x08(); break; case 0x09: OPCode0x09(); break;
        case 0x0A: OPCode0x0A(); break; case 0x0B: OPCode0x0B(); break;
        case 0x0C: OPCode0x0C(); break; case 0x0D: OPCode0x0D(); break;
        case 0x0E: OPCode0x0E(); break; case 0x0F: OPCode0x0F(); break;
        case 0x10: OPCode0x10(); break; case 0x11: OPCode0x11(); break;
        case 0x12: OPCode0x12(); break; case 0x13: OPCode0x13(); break;
        case 0x14: OPCode0x14(); break; case 0x15: OPCode0x15(); break;
        case 0x16: OPCode0x16(); break; case 0x17: OPCode0x17(); break;
        case 0x18: OPCode0x18(); break; case 0x19: OPCode0x19(); break;
        case 0x1A: OPCode0x1A(); break; case 0x1B: OPCode0x1B(); break;
        case 0x1C: OPCode0x1C(); break; case 0x1D: OPCode0x1D(); break;
        case 0x1E: OPCode0x1E(); break; case 0x1F: OPCode0x1F(); break;
        case 0x20: OPCode0x20(); break; case 0x21: OPCode0x21(); break;
        case 0x22: OPCode0x22(); break; case 0x23: OPCode0x23(); break;
        case 0x24: OPCode0x24(); break; case 0x25: OPCode0x25(); break;
        case 0x26: OPCode0x26(); break; case 0x27: OPCode0x27(); break;
        case 0x28: OPCode0x28(); break; case 0x29: OPCode0x29(); break;
        case 0x2A: OPCode0x2A(); break; case 0x2B: OPCode0x2B(); break;
        case 0x2C: OPCode0x2C(); break; case 0x2D: OPCode0x2D(); break;
        case 0x2E: OPCode0x2E(); break; case 0x2F: OPCode0x2F(); break;
        case 0x30: OPCode0x30(); break; case 0x31: OPCode0x31(); break;
        case 0x32: OPCode0x32(); break; case 0x33: OPCode0x33(); break;
        case 0x34: OPCode0x34(); break; case 0x35: OPCode0x35(); break;
        case 0x36: OPCode0x36(); break; case 0x37: OPCode0x37(); break;
        case 0x38: OPCode0x38(); break; case 0x39: OPCode0x39(); break;
        case 0x3A: OPCode0x3A(); break; case 0x3B: OPCode0x3B(); break;
        case 0x3C: OPCode0x3C(); break; case 0x3D: OPCode0x3D(); break;
        case 0x3E: OPCode0x3E(); break; case 0x3F: OPCode0x3F(); break;
        case 0x40: OPCode0x40(); break; case 0x41: OPCode0x41(); break;
        case 0x42: OPCode0x42(); break; case 0x43: OPCode0x43(); break;
        case 0x44: OPCode0x44(); break; case 0x45: OPCode0x45(); break;
        case 0x46: OPCode0x46(); break; case 0x47: OPCode0x47(); break;
        case 0x48: OPCode0x48(); break; case 0x49: OPCode0x49(); break;
        case 0x4A: OPCode0x4A(); break; case 0x4B: OPCode0x4B(); break;
        case 0x4C: OPCode0x4C(); break; case 0x4D: OPCode0x4D(); break;
        case 0x4E: OPCode0x4E(); break; case 0x4F: OPCode0x4F(); break;
        case 0x50: OPCode0x50(); break; case 0x51: OPCode0x51(); break;
        case 0x52: OPCode0x52(); break; case 0x53: OPCode0x53(); break;
        case 0x54: OPCode0x54(); break; case 0x55: OPCode0x55(); break;
        case 0x56: OPCode0x56(); break; case 0x57: OPCode0x57(); break;
        case 0x58: OPCode0x58(); break; case 0x59: OPCode0x59(); break;
        case 0x5A: OPCode0x5A(); break; case 0x5B: OPCode0x5B(); break;
        case 0x5C: OPCode0x5C(); break; case 0x5D: OPCode0x5D(); break;
        case 0x5E: OPCode0x5E(); break; case 0x5F: OPCode0x5F(); break;
        case 0x60: OPCode0x60(); break; case 0x61: OPCode0x61(); break;
        case 0x62: OPCode0x62(); break; case 0x63: OPCode0x63(); break;
        case 0x64: OPCode0x64(); break; case 0x65: OPCode0x65(); break;
        case 0x66: OPCode0x66(); break; case 0x67: OPCode0x67(); break;
        case 0x68: OPCode0x68(); break; case 0x69: OPCode0x69(); break;
        case 0x6A: OPCode0x6A(); break; case 0x6B: OPCode0x6B(); break;
        case 0x6C: OPCode0x6C(); break; case 0x6D: OPCode0x6D(); break;
        case 0x6E: OPCode0x6E(); break; case 0x6F: OPCode0x6F(); break;
        case 0x70: OPCode0x70(); break; case 0x71: OPCode0x71(); break;
        case 0x72: OPCode0x72(); break; case 0x73: OPCode0x73(); break;
        case 0x74: OPCode0x74(); break; case 0x75: OPCode0x75(); break;
        case 0x76: OPCode0x76(); break; case 0x77: OPCode0x77(); break;
        case 0x78: OPCode0x78(); break; case 0x79: OPCode0x79(); break;
        case 0x7A: OPCode0x7A(); break; case 0x7B: OPCode0x7B(); break;
        case 0x7C: OPCode0x7C(); break; case 0x7D: OPCode0x7D(); break;
        case 0x7E: OPCode0x7E(); break; case 0x7F: OPCode0x7F(); break;
        case 0x80: OPCode0x80(); break; case 0x81: OPCode0x81(); break;
        case 0x82: OPCode0x82(); break; case 0x83: OPCode0x83(); break;
        case 0x84: OPCode0x84(); break; case 0x85: OPCode0x85(); break;
        case 0x86: OPCode0x86(); break; case 0x87: OPCode0x87(); break;
        case 0x88: OPCode0x88(); break; case 0x89: OPCode0x89(); break;
        case 0x8A: OPCode0x8A(); break; case 0x8B: OPCode0x8B(); break;
        case 0x8C: OPCode0x8C(); break; case 0x8D: OPCode0x8D(); break;
        case 0x8E: OPCode0x8E(); break; case 0x8F: OPCode0x8F(); break;
        case 0x90: OPCode0x90(); break; case 0x91: OPCode0x91(); break;
        case 0x92: OPCode0x92(); break; case 0x93: OPCode0x93(); break;
        case 0x94: OPCode0x94(); break; case 0x95: OPCode0x95(); break;
        case 0x96: OPCode0x96(); break; case 0x97: OPCode0x97(); break;
        case 0x98: OPCode0x98(); break; case 0x99: OPCode0x99(); break;
        case 0x9A: OPCode0x9A(); break; case 0x9B: OPCode0x9B(); break;
        case 0x9C: OPCode0x9C(); break; case 0x9D: OPCode0x9D(); break;
        case 0x9E: OPCode0x9E(); break; case 0x9F: OPCode0x9F(); break;
        case 0xA0: OPCode0xA0(); break; case 0xA1: OPCode0xA1(); break;
        case 0xA2: OPCode0xA2(); break; case 0xA3: OPCode0xA3(); break;
        case 0xA4: OPCode0xA4(); break; case 0xA5: OPCode0xA5(); break;
        case 0xA6: OPCode0xA6(); break; case 0xA7: OPCode0xA7(); break;
        case 0xA8: OPCode0xA8(); break; case 0xA9: OPCode0xA9(); break;
        case 0xAA: OPCode0xAA(); break; case 0xAB: OPCode0xAB(); break;
        case 0xAC: OPCode0xAC(); break; case 0xAD: OPCode0xAD(); break;
        case 0xAE: OPCode0xAE(); break; case 0xAF: OPCode0xAF(); break;
        case 0xB0: OPCode0xB0(); break; case 0xB1: OPCode0xB1(); break;
        case 0xB2: OPCode0xB2(); break; case 0xB3: OPCode0xB3(); break;
        case 0xB4: OPCode0xB4(); break; case 0xB5: OPCode0xB5(); break;
        case 0xB6: OPCode0xB6(); break; case 0xB7: OPCode0xB7(); break;
        case 0xB8: OPCode0xB8(); break; case 0xB9: OPCode0xB9(); break;
        case 0xBA: OPCode0xBA(); break; case 0xBB: OPCode0xBB(); break;
        case 0xBC: OPCode0xBC(); break; case 0xBD: OPCode0xBD(); break;
        case 0xBE: OPCode0xBE(); break; case 0xBF: OPCode0xBF(); break;
        case 0xC0: OPCode0xC0(); break; case 0xC1: OPCode0xC1(); break;
        case 0xC2: OPCode0xC2(); break; case 0xC3: OPCode0xC3(); break;
        case 0xC4: OPCode0xC4(); break; case 0xC5: OPCode0xC5(); break;
        case 0xC6: OPCode0xC6(); break; case 0xC7: OPCode0xC7(); break;
        case 0xC8: OPCode0xC8(); break; case 0xC9: OPCode0xC9(); break;
        case 0xCA: OPCode0xCA(); break; case 0xCB: OPCode0xCB(); break;
        case 0xCC: OPCode0xCC(); break; case 0xCD: OPCode0xCD(); break;
        case 0xCE: OPCode0xCE(); break; case 0xCF: OPCode0xCF(); break;
        case 0xD0: OPCode0xD0(); break; case 0xD1: OPCode0xD1(); break;
        case 0xD2: OPCode0xD2(); break; case 0xD3: OPCode0xD3(); break;
        case 0xD4: OPCode0xD4(); break; case 0xD5: OPCode0xD5(); break;
        case 0xD6: OPCode0xD6(); break; case 0xD7: OPCode0xD7(); break;
        case 0xD8: OPCode0xD8(); break; case 0xD9: OPCode0xD9(); break;
        case 0xDA: OPCode0xDA(); break; case 0xDB: OPCode0xDB(); break;
        case 0xDC: OPCode0xDC(); break; case 0xDD: OPCode0xDD(); break;
        case 0xDE: OPCode0xDE(); break; case 0xDF: OPCode0xDF(); break;
        case 0xE0: OPCode0xE0(); break; case 0xE1: OPCode0xE1(); break;
        case 0xE2: OPCode0xE2(); break; case 0xE3: OPCode0xE3(); break;
        case 0xE4: OPCode0xE4(); break; case 0xE5: OPCode0xE5(); break;
        case 0xE6: OPCode0xE6(); break; case 0xE7: OPCode0xE7(); break;
        case 0xE8: OPCode0xE8(); break; case 0xE9: OPCode0xE9(); break;
        case 0xEA: OPCode0xEA(); break; case 0xEB: OPCode0xEB(); break;
        case 0xEC: OPCode0xEC(); break; case 0xED: OPCode0xED(); break;
        case 0xEE: OPCode0xEE(); break; case 0xEF: OPCode0xEF(); break;
        case 0xF0: OPCode0xF0(); break; case 0xF1: OPCode0xF1(); break;
        case 0xF2: OPCode0xF2(); break; case 0xF3: OPCode0xF3(); break;
        case 0xF4: OPCode0xF4(); break; case 0xF5: OPCode0xF5(); break;
        case 0xF6: OPCode0xF6(); break; case 0xF7: OPCode0xF7(); break;
        case 0xF8: OPCode0xF8(); break; case 0xF9: OPCode0xF9(); break;
        case 0xFA: OPCode0xFA(); break; case 0xFB: OPCode0xFB(); break;
        case 0xFC: OPCode0xFC(); break; case 0xFD: OPCode0xFD(); break;
        case 0xFE: OPCode0xFE(); break; case 0xFF: OPCode0xFF(); break;
    }
}

void Processor::ExecuteOPCodeCB(u8 opcode)
{
    switch (opcode)
    {
        case 0x00: OPCodeCB0x00(); break; case 0x01: OPCodeCB0x01(); break;
        case 0x02: OPCodeCB0x02(); break; case 0x03: OPCodeCB0x03(); break;
        case 0x04: OPCodeCB0x04(); break; case 0x05: OPCodeCB0x05(); break;
        case 0x06: OPCodeCB0x06(); break; case 0x07: OPCodeCB0x07(); break;
        case 0x08: OPCodeCB0x08(); break; case 0x09: OPCodeCB0x09(); break;
        case 0x0A: OPCodeCB0x0A(); break; case 0x0B: OPCodeCB0x0B(); break;
        case 0x0C: OPCodeCB0x0C(); break; case 0x0D: OPCodeCB0x0D(); break;
        case 0x0E: OPCodeCB0x0E(); break; case 0x0F: OPCodeCB0x0F(); break;
        case 0x10: OPCodeCB0x10(); break; case 0x11: OPCodeCB0x11(); break;
        case 0x12: OPCodeCB0x12(); break; case 0x13: OPCodeCB0x13(); break;
        case 0x14: OPCodeCB0x14(); break; case 0x15: OPCodeCB0x15(); break;
        case 0x16: OPCodeCB0x16(); break; case 0x17: OPCodeCB0x17(); break;
        case 0x18: OPCodeCB0x18(); break; case 0x19: OPCodeCB0x19(); break;
        case 0x1A: OPCodeCB0x1A(); break; case 0x1B: OPCodeCB0x1B(); break;
        case 0x1C: OPCodeCB0x1C(); break; case 0x1D: OPCodeCB0x1D(); break;
        case 0x1E: OPCodeCB0x1E(); break; case 0x1F: OPCodeCB0x1F(); break;
        case 0x20: OPCodeCB0x20(); break; case 0x21: OPCodeCB0x21(); break;
        case 0x22: OPCodeCB0x22(); break; case 0x23: OPCodeCB0x23(); break;
        case 0x24: OPCodeCB0x24(); break; case 0x25: OPCodeCB0x25(); break;
        case 0x26: OPCodeCB0x26(); break; case 0x27: OPCodeCB0x27(); break;
        case 0x28: OPCodeCB0x28(); break; case 0x29: OPCodeCB0x29(); break;
        case 0x2A: OPCodeCB0x2A(); break; case 0x2B: OPCodeCB0x2B(); break;
        case 0x2C: OPCodeCB0x2C(); break; case 0x2D: OPCodeCB0x2D(); break;
        case 0x2E: OPCodeCB0x2E(); break; case 0x2F: OPCodeCB0x2F(); break;
        case 0x30: OPCodeCB0x30(); break; case 0x31: OPCodeCB0x31(); break;
        case 0x32: OPCodeCB0x32(); break; case 0x33: OPCodeCB0x33(); break;
        case 0x34: OPCodeCB0x34(); break; case 0x35: OPCodeCB0x35(); break;
        case 0x36: OPCodeCB0x36(); break; case 0x37: OPCodeCB0x37(); break;
        case 0x38: OPCodeCB0x38(); break; case 0x39: OPCodeCB0x39(); break;
        case 0x3A: OPCodeCB0x3A(); break; case 0x3B: OPCodeCB0x3B(); break;
        case 0x3C: OPCodeCB0x3C(); break; case 0x3D: OPCodeCB0x3D(); break;
        case 0x3E: OPCodeCB0x3E(); break; case 0x3F: OPCodeCB0x3F(); break;
        case 0x40: OPCodeCB0x40(); break; case 0x41: OPCodeCB0x41(); break;
        case 0x42: OPCodeCB0x42(); break; case 0x43: OPCodeCB0x43(); break;
        case 0x44: OPCodeCB0x44(); break; case 0x45: OPCodeCB0x45(); break;
        case 0x46: OPCodeCB0x46(); break; case 0x47: OPCodeCB0x47(); break;
        case 0x48: OPCodeCB0x48(); break; case 0x49: OPCodeCB0x49(); break;
        case 0x4A: OPCodeCB0x4A(); break; case 0x4B: OPCodeCB0x4B(); break;
        case 0x4C: OPCodeCB0x4C(); break; case 0x4D: OPCodeCB0x4D(); break;
        case 0x4E: OPCodeCB0x4E(); break; case 0x4F: OPCodeCB0x4F(); break;
        case 0x50: OPCodeCB0x50(); break; case 0x51: OPCodeCB0x51(); break;
        case 0x52: OPCodeCB0x52(); break; case 0x53: OPCodeCB0x53(); break;
        case 0x54: OPCodeCB0x54(); break; case 0x55: OPCodeCB0x55(); break;
        case 0x56: OPCodeCB0x56(); break; case 0x57: OPCodeCB0x57(); break;
        case 0x58: OPCodeCB0x58(); break; case 0x59: OPCodeCB0x59(); break;
        case 0x5A: OPCodeCB0x5A(); break; case 0x5B: OPCodeCB0x5B(); break;
        case 0x5C: OPCodeCB0x5C(); break; case 0x5D: OPCodeCB0x5D(); break;
        case 0x5E: OPCodeCB0x5E(); break; case 0x5F: OPCodeCB0x5F(); break;
        case 0x60: OPCodeCB0x60(); break; case 0x61: OPCodeCB0x61(); break;
        case 0x62: OPCodeCB0x62(); break; case 0x63: OPCodeCB0x63(); break;
        case 0x64: OPCodeCB0x64(); break; case 0x65: OPCodeCB0x65(); break;
        case 0x66: OPCodeCB0x66(); break; case 0x67: OPCodeCB0x67(); break;
        case 0x68: OPCodeCB0x68(); break; case 0x69: OPCodeCB0x69(); break;
        case 0x6A: OPCodeCB0x6A(); break; case 0x6B: OPCodeCB0x6B(); break;
        case 0x6C: OPCodeCB0x6C(); break; case 0x6D: OPCodeCB0x6D(); break;
        case 0x6E: OPCodeCB0x6E(); break; case 0x6F: OPCodeCB0x6F(); break;
        case 0x70: OPCodeCB0x70(); break; case 0x71: OPCodeCB0x71(); break;
        case 0x72: OPCodeCB0x72(); break; case 0x73: OPCodeCB0x73(); break;
        case 0x74: OPCodeCB0x74(); break; case 0x75: OPCodeCB0x75(); break;
        case 0x76: OPCodeCB0x76(); break; case 0x77: OPCodeCB0x77(); break;
        case 0x78: OPCodeCB0x78(); break; case 0x79: OPCodeCB0x79(); break;
        case 0x7A: OPCodeCB0x7A(); break; case 0x7B: OPCodeCB0x7B(); break;
        case 0x7C: OPCodeCB0x7C(); break; case 0x7D: OPCodeCB0x7D(); break;
        case 0x7E: OPCodeCB0x7E(); break; case 0x7F: OPCodeCB0x7F(); break;
        case 0x80: OPCodeCB0x80(); break; case 0x81: OPCodeCB0x81(); break;
        case 0x82: OPCodeCB0x82(); break; case 0x83: OPCodeCB0x83(); break;
        case 0x84: OPCodeCB0x84(); break; case 0x85: OPCodeCB0x85(); break;
        case 0x86: OPCodeCB0x86(); break; case 0x87: OPCodeCB0x87(); break;
        case 0x88: OPCodeCB0x88(); break; case 0x89: OPCodeCB0x89(); break;
        case 0x8A: OPCodeCB0x8A(); break; case 0x8B: OPCodeCB0x8B(); break;
        case 0x8C: OPCodeCB0x8C(); break; case 0x8D: OPCodeCB0x8D(); break;
        case 0x8E: OPCodeCB0x8E(); break; case 0x8F: OPCodeCB0x8F(); break;
        case 0x90: OPCodeCB0x90(); break; case 0x91: OPCodeCB0x91(); break;
        case 0x92: OPCodeCB0x92(); break; case 0x93: OPCodeCB0x93(); break;
        case 0x94: OPCodeCB0x94(); break; case 0x95: OPCodeCB0x95(); break;
        case 0x96: OPCodeCB0x96(); break; case 0x97: OPCodeCB0x97(); break;
        case 0x98: OPCodeCB0x98(); break; case 0x99: OPCodeCB0x99(); break;
        case 0x9A: OPCodeCB0x9A(); break; case 0x9B: OPCodeCB0x9B(); break;
        case 0x9C: OPCodeCB0x9C(); break; case 0x9D: OPCodeCB0x9D(); break;
        case 0x9E: OPCodeCB0x9E(); break; case 0x9F: OPCodeCB0x9F(); break;
        case 0xA0: OPCodeCB0xA0(); break; case 0xA1: OPCodeCB0xA1(); break;
        case 0xA2: OPCodeCB0xA2(); break; case 0xA3: OPCodeCB0xA3(); break;
        case 0xA4: OPCodeCB0xA4(); break; case 0xA5: OPCodeCB0xA5(); break;
        case 0xA6: OPCodeCB0xA6(); break; case 0xA7: OPCodeCB0xA7(); break;
        case 0xA8: OPCodeCB0xA8(); break; case 0xA9: OPCodeCB0xA9(); break;
        case 0xAA: OPCodeCB0xAA(); break; case 0xAB: OPCodeCB0xAB(); break;
        case 0xAC: OPCodeCB0xAC(); break; case 0xAD: OPCodeCB0xAD(); break;
        case 0xAE: OPCodeCB0xAE(); break; case 0xAF: OPCodeCB0xAF(); break;
        case 0xB0: OPCodeCB0xB0(); break; case 0xB1: OPCodeCB0xB1(); break;
        case 0xB2: OPCodeCB0xB2(); break; case 0xB3: OPCodeCB0xB3(); break;
        case 0xB4: OPCodeCB0xB4(); break; case 0xB5: OPCodeCB0xB5(); break;
        case 0xB6: OPCodeCB0xB6(); break; case 0xB7: OPCodeCB0xB7(); break;
        case 0xB8: OPCodeCB0xB8(); break; case 0xB9: OPCodeCB0xB9(); break;
        case 0xBA: OPCodeCB0xBA(); break; case 0xBB: OPCodeCB0xBB(); break;
        case 0xBC: OPCodeCB0xBC(); break; case 0xBD: OPCodeCB0xBD(); break;
        case 0xBE: OPCodeCB0xBE(); break; case 0xBF: OPCodeCB0xBF(); break;
        case 0xC0: OPCodeCB0xC0(); break; case 0xC1: OPCodeCB0xC1(); break;
        case 0xC2: OPCodeCB0xC2(); break; case 0xC3: OPCodeCB0xC3(); break;
        case 0xC4: OPCodeCB0xC4(); break; case 0xC5: OPCodeCB0xC5(); break;
        case 0xC6: OPCodeCB0xC6(); break; case 0xC7: OPCodeCB0xC7(); break;
        case 0xC8: OPCodeCB0xC8(); break; case 0xC9: OPCodeCB0xC9(); break;
        case 0xCA: OPCodeCB0xCA(); break; case 0xCB: OPCodeCB0xCB(); break;
        case 0xCC: OPCodeCB0xCC(); break; case 0xCD: OPCodeCB0xCD(); break;
        case 0xCE: OPCodeCB0xCE(); break; case 0xCF: OPCodeCB0xCF(); break;
        case 0xD0: OPCodeCB0xD0(); break; case 0xD1: OPCodeCB0xD1(); break;
        case 0xD2: OPCodeCB0xD2(); break; case 0xD3: OPCodeCB0xD3(); break;
        case 0xD4: OPCodeCB0xD4(); break; case 0xD5: OPCodeCB0xD5(); break;
        case 0xD6: OPCodeCB0xD6(); break; case 0xD7: OPCodeCB0xD7(); break;
        case 0xD8: OPCodeCB0xD8(); break; case 0xD9: OPCodeCB0xD9(); break;
        case 0xDA: OPCodeCB0xDA(); break; case 0xDB: OPCodeCB0xDB(); break;
        case 0xDC: OPCodeCB0xDC(); break; case 0xDD: OPCodeCB0xDD(); break;
        case 0xDE: OPCodeCB0xDE(); break; case 0xDF: OPCodeCB0xDF(); break;
        case 0xE0: OPCodeCB0xE0(); break; case 0xE1: OPCodeCB0xE1(); break;
        case 0xE2: OPCodeCB0xE2(); break; case 0xE3: OPCodeCB0xE3(); break;
        case 0xE4: OPCodeCB0xE4(); break; case 0xE5: OPCodeCB0xE5(); break;
        case 0xE6: OPCodeCB0xE6(); break; case 0xE7: OPCodeCB0xE7(); break;
        case 0xE8: OPCodeCB0xE8(); break; case 0xE9: OPCodeCB0xE9(); break;
        case 0xEA: OPCodeCB0xEA(); break; case 0xEB: OPCodeCB0xEB(); break;
        case 0xEC: OPCodeCB0xEC(); break; case 0xED: OPCodeCB0xED(); break;
        case 0xEE: OPCodeCB0xEE(); break; case 0xEF: OPCodeCB0xEF(); break;
        case 0xF0: OPCodeCB0xF0(); break; case 0xF1: OPCodeCB0xF1(); break;
        case 0xF2: OPCodeCB0xF2(); break; case 0xF3: OPCodeCB0xF3(); break;
        case 0xF4: OPCodeCB0xF4(); break; case 0xF5: OPCodeCB0xF5(); break;
        case 0xF6: OPCodeCB0xF6(); break; case 0xF7: OPCodeCB0xF7(); break;
        case 0xF8: OPCodeCB0xF8(); break; case 0xF9: OPCodeCB0xF9(); break;
        case 0xFA: OPCodeCB0xFA(); break; case 0xFB: OPCodeCB0xFB(); break;
        case 0xFC: OPCodeCB0xFC(); break; case 0xFD: OPCodeCB0xFD(); break;
        case 0xFE: OPCodeCB0xFE(); break; case 0xFF: OPCodeCB0xFF(); break;
    }
}

void Processor::InitOPCodeFunctors()
{
    m_OPCodes[0x00] = &Processor::OPCode0x00;
    m_OPCodes[0x01] = &Processor::OPCode0x01;
    m_OPCodes[0x02] = &Processor::OPCode0x02;
    m_OPCodes[0x03] = &Processor::OPCode0x03;
    m_OPCodes[0x04] = &Processor::OPCode0x04;
    m_OPCodes[0x05] = &Processor::OPCode0x05;
    m_OPCodes[0x06] = &Processor::OPCode0x06;
    m_OPCodes[0x07] = &Processor::OPCode0x07;
    m_OPCodes[0x08] = &Processor::OPCode0x08;
    m_OPCodes[0x09] = &Processor::OPCode0x09;
    m_OPCodes[0x0A] = &Processor::OPCode0x0A;
    m_OPCodes[0x0B] = &Processor::OPCode0x0B;
    m_OPCodes[0x0C] = &Processor::OPCode0x0C;
    m_OPCodes[0x0D] = &Processor::OPCode0x0D;
    m_OPCodes[0x0E] = &Processor::OPCode0x0E;
    m_OPCodes[0x0F] = &Processor::OPCode0x0F;

    m_OPCodes[0x10] = &Processor::OPCode0x10;
    m_OPCodes[0x11] = &Processor::OPCode0x11;
    m_OPCodes[0x12] = &Processor::OPCode0x12;
    m_OPCodes[0x13] = &Processor::OPCode0x13;
    m_OPCodes[0x14] = &Processor::OPCode0x14;
    m_OPCodes[0x15] = &Processor::OPCode0x15;
    m_OPCodes[0x16] = &Processor::OPCode0x16;
    m_OPCodes[0x17] = &Processor::OPCode0x17;
    m_OPCodes[0x18] = &Processor::OPCode0x18;
    m_OPCodes[0x19] = &Processor::OPCode0x19;
    m_OPCodes[0x1A] = &Processor::OPCode0x1A;
    m_OPCodes[0x1B] = &Processor::OPCode0x1B;
    m_OPCodes[0x1C] = &Processor::OPCode0x1C;
    m_OPCodes[0x1D] = &Processor::OPCode0x1D;
    m_OPCodes[0x1E] = &Processor::OPCode0x1E;
    m_OPCodes[0x1F] = &Processor::OPCode0x1F;

    m_OPCodes[0x20] = &Processor::OPCode0x20;
    m_OPCodes[0x21] = &Processor::OPCode0x21;
    m_OPCodes[0x22] = &Processor::OPCode0x22;
    m_OPCodes[0x23] = &Processor::OPCode0x23;
    m_OPCodes[0x24] = &Processor::OPCode0x24;
    m_OPCodes[0x25] = &Processor::OPCode0x25;
    m_OPCodes[0x26] = &Processor::OPCode0x26;
    m_OPCodes[0x27] = &Processor::OPCode0x27;
    m_OPCodes[0x28] = &Processor::OPCode0x28;
    m_OPCodes[0x29] = &Processor::OPCode0x29;
    m_OPCodes[0x2A] = &Processor::OPCode0x2A;
    m_OPCodes[0x2B] = &Processor::OPCode0x2B;
    m_OPCodes[0x2C] = &Processor::OPCode0x2C;
    m_OPCodes[0x2D] = &Processor::OPCode0x2D;
    m_OPCodes[0x2E] = &Processor::OPCode0x2E;
    m_OPCodes[0x2F] = &Processor::OPCode0x2F;

    m_OPCodes[0x30] = &Processor::OPCode0x30;
    m_OPCodes[0x31] = &Processor::OPCode0x31;
    m_OPCodes[0x32] = &Processor::OPCode0x32;
    m_OPCodes[0x33] = &Processor::OPCode0x33;
    m_OPCodes[0x34] = &Processor::OPCode0x34;
    m_OPCodes[0x35] = &Processor::OPCode0x35;
    m_OPCodes[0x36] = &Processor::OPCode0x36;
    m_OPCodes[0x37] = &Processor::OPCode0x37;
    m_OPCodes[0x38] = &Processor::OPCode0x38;
    m_OPCodes[0x39] = &Processor::OPCode0x39;
    m_OPCodes[0x3A] = &Processor::OPCode0x3A;
    m_OPCodes[0x3B] = &Processor::OPCode0x3B;
    m_OPCodes[0x3C] = &Processor::OPCode0x3C;
    m_OPCodes[0x3D] = &Processor::OPCode0x3D;
    m_OPCodes[0x3E] = &Processor::OPCode0x3E;
    m_OPCodes[0x3F] = &Processor::OPCode0x3F;

    m_OPCodes[0x40] = &Processor::OPCode0x40;
    m_OPCodes[0x41] = &Processor::OPCode0x41;
    m_OPCodes[0x42] = &Processor::OPCode0x42;
    m_OPCodes[0x43] = &Processor::OPCode0x43;
    m_OPCodes[0x44] = &Processor::OPCode0x44;
    m_OPCodes[0x45] = &Processor::OPCode0x45;
    m_OPCodes[0x46] = &Processor::OPCode0x46;
    m_OPCodes[0x47] = &Processor::OPCode0x47;
    m_OPCodes[0x48] = &Processor::OPCode0x48;
    m_OPCodes[0x49] = &Processor::OPCode0x49;
    m_OPCodes[0x4A] = &Processor::OPCode0x4A;
    m_OPCodes[0x4B] = &Processor::OPCode0x4B;
    m_OPCodes[0x4C] = &Processor::OPCode0x4C;
    m_OPCodes[0x4D] = &Processor::OPCode0x4D;
    m_OPCodes[0x4E] = &Processor::OPCode0x4E;
    m_OPCodes[0x4F] = &Processor::OPCode0x4F;

    m_OPCodes[0x50] = &Processor::OPCode0x50;
    m_OPCodes[0x51] = &Processor::OPCode0x51;
    m_OPCodes[0x52] = &Processor::OPCode0x52;
    m_OPCodes[0x53] = &Processor::OPCode0x53;
    m_OPCodes[0x54] = &Processor::OPCode0x54;
    m_OPCodes[0x55] = &Processor::OPCode0x55;
    m_OPCodes[0x56] = &Processor::OPCode0x56;
    m_OPCodes[0x57] = &Processor::OPCode0x57;
    m_OPCodes[0x58] = &Processor::OPCode0x58;
    m_OPCodes[0x59] = &Processor::OPCode0x59;
    m_OPCodes[0x5A] = &Processor::OPCode0x5A;
    m_OPCodes[0x5B] = &Processor::OPCode0x5B;
    m_OPCodes[0x5C] = &Processor::OPCode0x5C;
    m_OPCodes[0x5D] = &Processor::OPCode0x5D;
    m_OPCodes[0x5E] = &Processor::OPCode0x5E;
    m_OPCodes[0x5F] = &Processor::OPCode0x5F;

    m_OPCodes[0x60] = &Processor::OPCode0x60;
    m_OPCodes[0x61] = &Processor::OPCode0x61;
    m_OPCodes[0x62] = &Processor::OPCode0x62;
    m_OPCodes[0x63] = &Processor::OPCode0x63;
    m_OPCodes[0x64] = &Processor::OPCode0x64;
    m_OPCodes[0x65] = &Processor::OPCode0x65;
    m_OPCodes[0x66] = &Processor::OPCode0x66;
    m_OPCodes[0x67] = &Processor::OPCode0x67;
    m_OPCodes[0x68] = &Processor::OPCode0x68;
    m_OPCodes[0x69] = &Processor::OPCode0x69;
    m_OPCodes[0x6A] = &Processor::OPCode0x6A;
    m_OPCodes[0x6B] = &Processor::OPCode0x6B;
    m_OPCodes[0x6C] = &Processor::OPCode0x6C;
    m_OPCodes[0x6D] = &Processor::OPCode0x6D;
    m_OPCodes[0x6E] = &Processor::OPCode0x6E;
    m_OPCodes[0x6F] = &Processor::OPCode0x6F;

    m_OPCodes[0x70] = &Processor::OPCode0x70;
    m_OPCodes[0x71] = &Processor::OPCode0x71;
    m_OPCodes[0x72] = &Processor::OPCode0x72;
    m_OPCodes[0x73] = &Processor::OPCode0x73;
    m_OPCodes[0x74] = &Processor::OPCode0x74;
    m_OPCodes[0x75] = &Processor::OPCode0x75;
    m_OPCodes[0x76] = &Processor::OPCode0x76;
    m_OPCodes[0x77] = &Processor::OPCode0x77;
    m_OPCodes[0x78] = &Processor::OPCode0x78;
    m_OPCodes[0x79] = &Processor::OPCode0x79;
    m_OPCodes[0x7A] = &Processor::OPCode0x7A;
    m_OPCodes[0x7B] = &Processor::OPCode0x7B;
    m_OPCodes[0x7C] = &Processor::OPCode0x7C;
    m_OPCodes[0x7D] = &Processor::OPCode0x7D;
    m_OPCodes[0x7E] = &Processor::OPCode0x7E;
    m_OPCodes[0x7F] = &Processor::OPCode0x7F;

    m_OPCodes[0x80] = &Processor::OPCode0x80;
    m_OPCodes[0x81] = &Processor::OPCode0x81;
    m_OPCodes[0x82] = &Processor::OPCode0x82;
    m_OPCodes[0x83] = &Processor::OPCode0x83;
    m_OPCodes[0x84] = &Processor::OPCode0x84;
    m_OPCodes[0x85] = &Processor::OPCode0x85;
    m_OPCodes[0x86] = &Processor::OPCode0x86;
    m_OPCodes[0x87] = &Processor::OPCode0x87;
    m_OPCodes[0x88] = &Processor::OPCode0x88;
    m_OPCodes[0x89] = &Processor::OPCode0x89;
    m_OPCodes[0x8A] = &Processor::OPCode0x8A;
    m_OPCodes[0x8B] = &Processor::OPCode0x8B;
    m_OPCodes[0x8C] = &Processor::OPCode0x8C;
    m_OPCodes[0x8D] = &Processor::OPCode0x8D;
    m_OPCodes[0x8E] = &Processor::OPCode0x8E;
    m_OPCodes[0x8F] = &Processor::OPCode0x8F;

    m_OPCodes[0x90] = &Processor::OPCode0x90;
    m_OPCodes[0x91] = &Processor::OPCode0x91;
    m_OPCodes[0x92] = &Processor::OPCode0x92;
    m_OPCodes[0x93] = &Processor::OPCode0x93;
    m_OPCodes[0x94] = &Processor::OPCode0x94;
    m_OPCodes[0x95] = &Processor::OPCode0x95;
    m_OPCodes[0x96] = &Processor::OPCode0x96;
    m_OPCodes[0x97] = &Processor::OPCode0x97;
    m_OPCodes[0x98] = &Processor::OPCode0x98;
    m_OPCodes[0x99] = &Processor::OPCode0x99;
    m_OPCodes[0x9A] = &Processor::OPCode0x9A;
    m_OPCodes[0x9B] = &Processor::OPCode0x9B;
    m_OPCodes[0x9C] = &Processor::OPCode0x9C;
    m_OPCodes[0x9D] = &Processor::OPCode0x9D;
    m_OPCodes[0x9E] = &Processor::OPCode0x9E;
    m_OPCodes[0x9F] = &Processor::OPCode0x9F;

    m_OPCodes[0xA0] = &Processor::OPCode0xA0;
    m_OPCodes[0xA1] = &Processor::OPCode0xA1;
    m_OPCodes[0xA2] = &Processor::OPCode0xA2;
    m_OPCodes[0xA3] = &Processor::OPCode0xA3;
    m_OPCodes[0xA4] = &Processor::OPCode0xA4;
    m_OPCodes[0xA5] = &Processor::OPCode0xA5;
    m_OPCodes[0xA6] = &Processor::OPCode0xA6;
    m_OPCodes[0xA7] = &Processor::OPCode0xA7;
    m_OPCodes[0xA8] = &Processor::OPCode0xA8;
    m_OPCodes[0xA9] = &Processor::OPCode0xA9;
    m_OPCodes[0xAA] = &Processor::OPCode0xAA;
    m_OPCodes[0xAB] = &Processor::OPCode0xAB;
    m_OPCodes[0xAC] = &Processor::OPCode0xAC;
    m_OPCodes[0xAD] = &Processor::OPCode0xAD;
    m_OPCodes[0xAE] = &Processor::OPCode0xAE;
    m_OPCodes[0xAF] = &Processor::OPCode0xAF;

    m_OPCodes[0xB0] = &Processor::OPCode0xB0;
    m_OPCodes[0xB1] = &Processor::OPCode0xB1;
    m_OPCodes[0xB2] = &Processor::OPCode0xB2;
    m_OPCodes[0xB3] = &Processor::OPCode0xB3;
    m_OPCodes[0xB4] = &Processor::OPCode0xB4;
    m_OPCodes[0xB5] = &Processor::OPCode0xB5;
    m_OPCodes[0xB6] = &Processor::OPCode0xB6;
    m_OPCodes[0xB7] = &Processor::OPCode0xB7;
    m_OPCodes[0xB8] = &Processor::OPCode0xB8;
    m_OPCodes[0xB9] = &Processor::OPCode0xB9;
    m_OPCodes[0xBA] = &Processor::OPCode0xBA;
    m_OPCodes[0xBB] = &Processor::OPCode0xBB;
    m_OPCodes[0xBC] = &Processor::OPCode0xBC;
    m_OPCodes[0xBD] = &Processor::OPCode0xBD;
    m_OPCodes[0xBE] = &Processor::OPCode0xBE;
    m_OPCodes[0xBF] = &Processor::OPCode0xBF;

    m_OPCodes[0xC0] = &Processor::OPCode0xC0;
    m_OPCodes[0xC1] = &Processor::OPCode0xC1;
    m_OPCodes[0xC2] = &Processor::OPCode0xC2;
    m_OPCodes[0xC3] = &Processor::OPCode0xC3;
    m_OPCodes[0xC4] = &Processor::OPCode0xC4;
    m_OPCodes[0xC5] = &Processor::OPCode0xC5;
    m_OPCodes[0xC6] = &Processor::OPCode0xC6;
    m_OPCodes[0xC7] = &Processor::OPCode0xC7;
    m_OPCodes[0xC8] = &Processor::OPCode0xC8;
    m_OPCodes[0xC9] = &Processor::OPCode0xC9;
    m_OPCodes[0xCA] = &Processor::OPCode0xCA;
    m_OPCodes[0xCB] = &Processor::OPCode0xCB;
    m_OPCodes[0xCC] = &Processor::OPCode0xCC;
    m_OPCodes[0xCD] = &Processor::OPCode0xCD;
    m_OPCodes[0xCE] = &Processor::OPCode0xCE;
    m_OPCodes[0xCF] = &Processor::OPCode0xCF;

    m_OPCodes[0xD0] = &Processor::OPCode0xD0;
    m_OPCodes[0xD1] = &Processor::OPCode0xD1;
    m_OPCodes[0xD2] = &Processor::OPCode0xD2;
    m_OPCodes[0xD3] = &Processor::OPCode0xD3;
    m_OPCodes[0xD4] = &Processor::OPCode0xD4;
    m_OPCodes[0xD5] = &Processor::OPCode0xD5;
    m_OPCodes[0xD6] = &Processor::OPCode0xD6;
    m_OPCodes[0xD7] = &Processor::OPCode0xD7;
    m_OPCodes[0xD8] = &Processor::OPCode0xD8;
    m_OPCodes[0xD9] = &Processor::OPCode0xD9;
    m_OPCodes[0xDA] = &Processor::OPCode0xDA;
    m_OPCodes[0xDB] = &Processor::OPCode0xDB;
    m_OPCodes[0xDC] = &Processor::OPCode0xDC;
    m_OPCodes[0xDD] = &Processor::OPCode0xDD;
    m_OPCodes[0xDE] = &Processor::OPCode0xDE;
    m_OPCodes[0xDF] = &Processor::OPCode0xDF;

    m_OPCodes[0xE0] = &Processor::OPCode0xE0;
    m_OPCodes[0xE1] = &Processor::OPCode0xE1;
    m_OPCodes[0xE2] = &Processor::OPCode0xE2;
    m_OPCodes[0xE3] = &Processor::OPCode0xE3;
    m_OPCodes[0xE4] = &Processor::OPCode0xE4;
    m_OPCodes[0xE5] = &Processor::OPCode0xE5;
    m_OPCodes[0xE6] = &Processor::OPCode0xE6;
    m_OPCodes[0xE7] = &Processor::OPCode0xE7;
    m_OPCodes[0xE8] = &Processor::OPCode0xE8;
    m_OPCodes[0xE9] = &Processor::OPCode0xE9;
    m_OPCodes[0xEA] = &Processor::OPCode0xEA;
    m_OPCodes[0xEB] = &Processor::OPCode0xEB;
    m_OPCodes[0xEC] = &Processor::OPCode0xEC;
    m_OPCodes[0xED] = &Processor::OPCode0xED;
    m_OPCodes[0xEE] = &Processor::OPCode0xEE;
    m_OPCodes[0xEF] = &Processor::OPCode0xEF;

    m_OPCodes[0xF0] = &Processor::OPCode0xF0;
    m_OPCodes[0xF1] = &Processor::OPCode0xF1;
    m_OPCodes[0xF2] = &Processor::OPCode0xF2;
    m_OPCodes[0xF3] = &Processor::OPCode0xF3;
    m_OPCodes[0xF4] = &Processor::OPCode0xF4;
    m_OPCodes[0xF5] = &Processor::OPCode0xF5;
    m_OPCodes[0xF6] = &Processor::OPCode0xF6;
    m_OPCodes[0xF7] = &Processor::OPCode0xF7;
    m_OPCodes[0xF8] = &Processor::OPCode0xF8;
    m_OPCodes[0xF9] = &Processor::OPCode0xF9;
    m_OPCodes[0xFA] = &Processor::OPCode0xFA;
    m_OPCodes[0xFB] = &Processor::OPCode0xFB;
    m_OPCodes[0xFC] = &Processor::OPCode0xFC;
    m_OPCodes[0xFD] = &Processor::OPCode0xFD;
    m_OPCodes[0xFE] = &Processor::OPCode0xFE;
    m_OPCodes[0xFF] = &Processor::OPCode0xFF;


    m_OPCodesCB[0x00] = &Processor::OPCodeCB0x00;
    m_OPCodesCB[0x01] = &Processor::OPCodeCB0x01;
    m_OPCodesCB[0x02] = &Processor::OPCodeCB0x02;
    m_OPCodesCB[0x03] = &Processor::OPCodeCB0x03;
    m_OPCodesCB[0x04] = &Processor::OPCodeCB0x04;
    m_OPCodesCB[0x05] = &Processor::OPCodeCB0x05;
    m_OPCodesCB[0x06] = &Processor::OPCodeCB0x06;
    m_OPCodesCB[0x07] = &Processor::OPCodeCB0x07;
    m_OPCodesCB[0x08] = &Processor::OPCodeCB0x08;
    m_OPCodesCB[0x09] = &Processor::OPCodeCB0x09;
    m_OPCodesCB[0x0A] = &Processor::OPCodeCB0x0A;
    m_OPCodesCB[0x0B] = &Processor::OPCodeCB0x0B;
    m_OPCodesCB[0x0C] = &Processor::OPCodeCB0x0C;
    m_OPCodesCB[0x0D] = &Processor::OPCodeCB0x0D;
    m_OPCodesCB[0x0E] = &Processor::OPCodeCB0x0E;
    m_OPCodesCB[0x0F] = &Processor::OPCodeCB0x0F;

    m_OPCodesCB[0x10] = &Processor::OPCodeCB0x10;
    m_OPCodesCB[0x11] = &Processor::OPCodeCB0x11;
    m_OPCodesCB[0x12] = &Processor::OPCodeCB0x12;
    m_OPCodesCB[0x13] = &Processor::OPCodeCB0x13;
    m_OPCodesCB[0x14] = &Processor::OPCodeCB0x14;
    m_OPCodesCB[0x15] = &Processor::OPCodeCB0x15;
    m_OPCodesCB[0x16] = &Processor::OPCodeCB0x16;
    m_OPCodesCB[0x17] = &Processor::OPCodeCB0x17;
    m_OPCodesCB[0x18] = &Processor::OPCodeCB0x18;
    m_OPCodesCB[0x19] = &Processor::OPCodeCB0x19;
    m_OPCodesCB[0x1A] = &Processor::OPCodeCB0x1A;
    m_OPCodesCB[0x1B] = &Processor::OPCodeCB0x1B;
    m_OPCodesCB[0x1C] = &Processor::OPCodeCB0x1C;
    m_OPCodesCB[0x1D] = &Processor::OPCodeCB0x1D;
    m_OPCodesCB[0x1E] = &Processor::OPCodeCB0x1E;
    m_OPCodesCB[0x1F] = &Processor::OPCodeCB0x1F;

    m_OPCodesCB[0x20] = &Processor::OPCodeCB0x20;
    m_OPCodesCB[0x21] = &Processor::OPCodeCB0x21;
    m_OPCodesCB[0x22] = &Processor::OPCodeCB0x22;
    m_OPCodesCB[0x23] = &Processor::OPCodeCB0x23;
    m_OPCodesCB[0x24] = &Processor::OPCodeCB0x24;
    m_OPCodesCB[0x25] = &Processor::OPCodeCB0x25;
    m_OPCodesCB[0x26] = &Processor::OPCodeCB0x26;
    m_OPCodesCB[0x27] = &Processor::OPCodeCB0x27;
    m_OPCodesCB[0x28] = &Processor::OPCodeCB0x28;
    m_OPCodesCB[0x29] = &Processor::OPCodeCB0x29;
    m_OPCodesCB[0x2A] = &Processor::OPCodeCB0x2A;
    m_OPCodesCB[0x2B] = &Processor::OPCodeCB0x2B;
    m_OPCodesCB[0x2C] = &Processor::OPCodeCB0x2C;
    m_OPCodesCB[0x2D] = &Processor::OPCodeCB0x2D;
    m_OPCodesCB[0x2E] = &Processor::OPCodeCB0x2E;
    m_OPCodesCB[0x2F] = &Processor::OPCodeCB0x2F;

    m_OPCodesCB[0x30] = &Processor::OPCodeCB0x30;
    m_OPCodesCB[0x31] = &Processor::OPCodeCB0x31;
    m_OPCodesCB[0x32] = &Processor::OPCodeCB0x32;
    m_OPCodesCB[0x33] = &Processor::OPCodeCB0x33;
    m_OPCodesCB[0x34] = &Processor::OPCodeCB0x34;
    m_OPCodesCB[0x35] = &Processor::OPCodeCB0x35;
    m_OPCodesCB[0x36] = &Processor::OPCodeCB0x36;
    m_OPCodesCB[0x37] = &Processor::OPCodeCB0x37;
    m_OPCodesCB[0x38] = &Processor::OPCodeCB0x38;
    m_OPCodesCB[0x39] = &Processor::OPCodeCB0x39;
    m_OPCodesCB[0x3A] = &Processor::OPCodeCB0x3A;
    m_OPCodesCB[0x3B] = &Processor::OPCodeCB0x3B;
    m_OPCodesCB[0x3C] = &Processor::OPCodeCB0x3C;
    m_OPCodesCB[0x3D] = &Processor::OPCodeCB0x3D;
    m_OPCodesCB[0x3E] = &Processor::OPCodeCB0x3E;
    m_OPCodesCB[0x3F] = &Processor::OPCodeCB0x3F;

    m_OPCodesCB[0x40] = &Processor::OPCodeCB0x40;
    m_OPCodesCB[0x41] = &Processor::OPCodeCB0x41;
    m_OPCodesCB[0x42] = &Processor::OPCodeCB0x42;
    m_OPCodesCB[0x43] = &Processor::OPCodeCB0x43;
    m_OPCodesCB[0x44] = &Processor::OPCodeCB0x44;
    m_OPCodesCB[0x45] = &Processor::OPCodeCB0x45;
    m_OPCodesCB[0x46] = &Processor::OPCodeCB0x46;
    m_OPCodesCB[0x47] = &Processor::OPCodeCB0x47;
    m_OPCodesCB[0x48] = &Processor::OPCodeCB0x48;
    m_OPCodesCB[0x49] = &Processor::OPCodeCB0x49;
    m_OPCodesCB[0x4A] = &Processor::OPCodeCB0x4A;
    m_OPCodesCB[0x4B] = &Processor::OPCodeCB0x4B;
    m_OPCodesCB[0x4C] = &Processor::OPCodeCB0x4C;
    m_OPCodesCB[0x4D] = &Processor::OPCodeCB0x4D;
    m_OPCodesCB[0x4E] = &Processor::OPCodeCB0x4E;
    m_OPCodesCB[0x4F] = &Processor::OPCodeCB0x4F;

    m_OPCodesCB[0x50] = &Processor::OPCodeCB0x50;
    m_OPCodesCB[0x51] = &Processor::OPCodeCB0x51;
    m_OPCodesCB[0x52] = &Processor::OPCodeCB0x52;
    m_OPCodesCB[0x53] = &Processor::OPCodeCB0x53;
    m_OPCodesCB[0x54] = &Processor::OPCodeCB0x54;
    m_OPCodesCB[0x55] = &Processor::OPCodeCB0x55;
    m_OPCodesCB[0x56] = &Processor::OPCodeCB0x56;
    m_OPCodesCB[0x57] = &Processor::OPCodeCB0x57;
    m_OPCodesCB[0x58] = &Processor::OPCodeCB0x58;
    m_OPCodesCB[0x59] = &Processor::OPCodeCB0x59;
    m_OPCodesCB[0x5A] = &Processor::OPCodeCB0x5A;
    m_OPCodesCB[0x5B] = &Processor::OPCodeCB0x5B;
    m_OPCodesCB[0x5C] = &Processor::OPCodeCB0x5C;
    m_OPCodesCB[0x5D] = &Processor::OPCodeCB0x5D;
    m_OPCodesCB[0x5E] = &Processor::OPCodeCB0x5E;
    m_OPCodesCB[0x5F] = &Processor::OPCodeCB0x5F;

    m_OPCodesCB[0x60] = &Processor::OPCodeCB0x60;
    m_OPCodesCB[0x61] = &Processor::OPCodeCB0x61;
    m_OPCodesCB[0x62] = &Processor::OPCodeCB0x62;
    m_OPCodesCB[0x63] = &Processor::OPCodeCB0x63;
    m_OPCodesCB[0x64] = &Processor::OPCodeCB0x64;
    m_OPCodesCB[0x65] = &Processor::OPCodeCB0x65;
    m_OPCodesCB[0x66] = &Processor::OPCodeCB0x66;
    m_OPCodesCB[0x67] = &Processor::OPCodeCB0x67;
    m_OPCodesCB[0x68] = &Processor::OPCodeCB0x68;
    m_OPCodesCB[0x69] = &Processor::OPCodeCB0x69;
    m_OPCodesCB[0x6A] = &Processor::OPCodeCB0x6A;
    m_OPCodesCB[0x6B] = &Processor::OPCodeCB0x6B;
    m_OPCodesCB[0x6C] = &Processor::OPCodeCB0x6C;
    m_OPCodesCB[0x6D] = &Processor::OPCodeCB0x6D;
    m_OPCodesCB[0x6E] = &Processor::OPCodeCB0x6E;
    m_OPCodesCB[0x6F] = &Processor::OPCodeCB0x6F;

    m_OPCodesCB[0x70] = &Processor::OPCodeCB0x70;
    m_OPCodesCB[0x71] = &Processor::OPCodeCB0x71;
    m_OPCodesCB[0x72] = &Processor::OPCodeCB0x72;
    m_OPCodesCB[0x73] = &Processor::OPCodeCB0x73;
    m_OPCodesCB[0x74] = &Processor::OPCodeCB0x74;
    m_OPCodesCB[0x75] = &Processor::OPCodeCB0x75;
    m_OPCodesCB[0x76] = &Processor::OPCodeCB0x76;
    m_OPCodesCB[0x77] = &Processor::OPCodeCB0x77;
    m_OPCodesCB[0x78] = &Processor::OPCodeCB0x78;
    m_OPCodesCB[0x79] = &Processor::OPCodeCB0x79;
    m_OPCodesCB[0x7A] = &Processor::OPCodeCB0x7A;
    m_OPCodesCB[0x7B] = &Processor::OPCodeCB0x7B;
    m_OPCodesCB[0x7C] = &Processor::OPCodeCB0x7C;
    m_OPCodesCB[0x7D] = &Processor::OPCodeCB0x7D;
    m_OPCodesCB[0x7E] = &Processor::OPCodeCB0x7E;
    m_OPCodesCB[0x7F] = &Processor::OPCodeCB0x7F;

    m_OPCodesCB[0x80] = &Processor::OPCodeCB0x80;
    m_OPCodesCB[0x81] = &Processor::OPCodeCB0x81;
    m_OPCodesCB[0x82] = &Processor::OPCodeCB0x82;
    m_OPCodesCB[0x83] = &Processor::OPCodeCB0x83;
    m_OPCodesCB[0x84] = &Processor::OPCodeCB0x84;
    m_OPCodesCB[0x85] = &Processor::OPCodeCB0x85;
    m_OPCodesCB[0x86] = &Processor::OPCodeCB0x86;
    m_OPCodesCB[0x87] = &Processor::OPCodeCB0x87;
    m_OPCodesCB[0x88] = &Processor::OPCodeCB0x88;
    m_OPCodesCB[0x89] = &Processor::OPCodeCB0x89;
    m_OPCodesCB[0x8A] = &Processor::OPCodeCB0x8A;
    m_OPCodesCB[0x8B] = &Processor::OPCodeCB0x8B;
    m_OPCodesCB[0x8C] = &Processor::OPCodeCB0x8C;
    m_OPCodesCB[0x8D] = &Processor::OPCodeCB0x8D;
    m_OPCodesCB[0x8E] = &Processor::OPCodeCB0x8E;
    m_OPCodesCB[0x8F] = &Processor::OPCodeCB0x8F;

    m_OPCodesCB[0x90] = &Processor::OPCodeCB0x90;
    m_OPCodesCB[0x91] = &Processor::OPCodeCB0x91;
    m_OPCodesCB[0x92] = &Processor::OPCodeCB0x92;
    m_OPCodesCB[0x93] = &Processor::OPCodeCB0x93;
    m_OPCodesCB[0x94] = &Processor::OPCodeCB0x94;
    m_OPCodesCB[0x95] = &Processor::OPCodeCB0x95;
    m_OPCodesCB[0x96] = &Processor::OPCodeCB0x96;
    m_OPCodesCB[0x97] = &Processor::OPCodeCB0x97;
    m_OPCodesCB[0x98] = &Processor::OPCodeCB0x98;
    m_OPCodesCB[0x99] = &Processor::OPCodeCB0x99;
    m_OPCodesCB[0x9A] = &Processor::OPCodeCB0x9A;
    m_OPCodesCB[0x9B] = &Processor::OPCodeCB0x9B;
    m_OPCodesCB[0x9C] = &Processor::OPCodeCB0x9C;
    m_OPCodesCB[0x9D] = &Processor::OPCodeCB0x9D;
    m_OPCodesCB[0x9E] = &Processor::OPCodeCB0x9E;
    m_OPCodesCB[0x9F] = &Processor::OPCodeCB0x9F;

    m_OPCodesCB[0xA0] = &Processor::OPCodeCB0xA0;
    m_OPCodesCB[0xA1] = &Processor::OPCodeCB0xA1;
    m_OPCodesCB[0xA2] = &Processor::OPCodeCB0xA2;
    m_OPCodesCB[0xA3] = &Processor::OPCodeCB0xA3;
    m_OPCodesCB[0xA4] = &Processor::OPCodeCB0xA4;
    m_OPCodesCB[0xA5] = &Processor::OPCodeCB0xA5;
    m_OPCodesCB[0xA6] = &Processor::OPCodeCB0xA6;
    m_OPCodesCB[0xA7] = &Processor::OPCodeCB0xA7;
    m_OPCodesCB[0xA8] = &Processor::OPCodeCB0xA8;
    m_OPCodesCB[0xA9] = &Processor::OPCodeCB0xA9;
    m_OPCodesCB[0xAA] = &Processor::OPCodeCB0xAA;
    m_OPCodesCB[0xAB] = &Processor::OPCodeCB0xAB;
    m_OPCodesCB[0xAC] = &Processor::OPCodeCB0xAC;
    m_OPCodesCB[0xAD] = &Processor::OPCodeCB0xAD;
    m_OPCodesCB[0xAE] = &Processor::OPCodeCB0xAE;
    m_OPCodesCB[0xAF] = &Processor::OPCodeCB0xAF;

    m_OPCodesCB[0xB0] = &Processor::OPCodeCB0xB0;
    m_OPCodesCB[0xB1] = &Processor::OPCodeCB0xB1;
    m_OPCodesCB[0xB2] = &Processor::OPCodeCB0xB2;
    m_OPCodesCB[0xB3] = &Processor::OPCodeCB0xB3;
    m_OPCodesCB[0xB4] = &Processor::OPCodeCB0xB4;
    m_OPCodesCB[0xB5] = &Processor::OPCodeCB0xB5;
    m_OPCodesCB[0xB6] = &Processor::OPCodeCB0xB6;
    m_OPCodesCB[0xB7] = &Processor::OPCodeCB0xB7;
    m_OPCodesCB[0xB8] = &Processor::OPCodeCB0xB8;
    m_OPCodesCB[0xB9] = &Processor::OPCodeCB0xB9;
    m_OPCodesCB[0xBA] = &Processor::OPCodeCB0xBA;
    m_OPCodesCB[0xBB] = &Processor::OPCodeCB0xBB;
    m_OPCodesCB[0xBC] = &Processor::OPCodeCB0xBC;
    m_OPCodesCB[0xBD] = &Processor::OPCodeCB0xBD;
    m_OPCodesCB[0xBE] = &Processor::OPCodeCB0xBE;
    m_OPCodesCB[0xBF] = &Processor::OPCodeCB0xBF;

    m_OPCodesCB[0xC0] = &Processor::OPCodeCB0xC0;
    m_OPCodesCB[0xC1] = &Processor::OPCodeCB0xC1;
    m_OPCodesCB[0xC2] = &Processor::OPCodeCB0xC2;
    m_OPCodesCB[0xC3] = &Processor::OPCodeCB0xC3;
    m_OPCodesCB[0xC4] = &Processor::OPCodeCB0xC4;
    m_OPCodesCB[0xC5] = &Processor::OPCodeCB0xC5;
    m_OPCodesCB[0xC6] = &Processor::OPCodeCB0xC6;
    m_OPCodesCB[0xC7] = &Processor::OPCodeCB0xC7;
    m_OPCodesCB[0xC8] = &Processor::OPCodeCB0xC8;
    m_OPCodesCB[0xC9] = &Processor::OPCodeCB0xC9;
    m_OPCodesCB[0xCA] = &Processor::OPCodeCB0xCA;
    m_OPCodesCB[0xCB] = &Processor::OPCodeCB0xCB;
    m_OPCodesCB[0xCC] = &Processor::OPCodeCB0xCC;
    m_OPCodesCB[0xCD] = &Processor::OPCodeCB0xCD;
    m_OPCodesCB[0xCE] = &Processor::OPCodeCB0xCE;
    m_OPCodesCB[0xCF] = &Processor::OPCodeCB0xCF;

    m_OPCodesCB[0xD0] = &Processor::OPCodeCB0xD0;
    m_OPCodesCB[0xD1] = &Processor::OPCodeCB0xD1;
    m_OPCodesCB[0xD2] = &Processor::OPCodeCB0xD2;
    m_OPCodesCB[0xD3] = &Processor::OPCodeCB0xD3;
    m_OPCodesCB[0xD4] = &Processor::OPCodeCB0xD4;
    m_OPCodesCB[0xD5] = &Processor::OPCodeCB0xD5;
    m_OPCodesCB[0xD6] = &Processor::OPCodeCB0xD6;
    m_OPCodesCB[0xD7] = &Processor::OPCodeCB0xD7;
    m_OPCodesCB[0xD8] = &Processor::OPCodeCB0xD8;
    m_OPCodesCB[0xD9] = &Processor::OPCodeCB0xD9;
    m_OPCodesCB[0xDA] = &Processor::OPCodeCB0xDA;
    m_OPCodesCB[0xDB] = &Processor::OPCodeCB0xDB;
    m_OPCodesCB[0xDC] = &Processor::OPCodeCB0xDC;
    m_OPCodesCB[0xDD] = &Processor::OPCodeCB0xDD;
    m_OPCodesCB[0xDE] = &Processor::OPCodeCB0xDE;
    m_OPCodesCB[0xDF] = &Processor::OPCodeCB0xDF;

    m_OPCodesCB[0xE0] = &Processor::OPCodeCB0xE0;
    m_OPCodesCB[0xE1] = &Processor::OPCodeCB0xE1;
    m_OPCodesCB[0xE2] = &Processor::OPCodeCB0xE2;
    m_OPCodesCB[0xE3] = &Processor::OPCodeCB0xE3;
    m_OPCodesCB[0xE4] = &Processor::OPCodeCB0xE4;
    m_OPCodesCB[0xE5] = &Processor::OPCodeCB0xE5;
    m_OPCodesCB[0xE6] = &Processor::OPCodeCB0xE6;
    m_OPCodesCB[0xE7] = &Processor::OPCodeCB0xE7;
    m_OPCodesCB[0xE8] = &Processor::OPCodeCB0xE8;
    m_OPCodesCB[0xE9] = &Processor::OPCodeCB0xE9;
    m_OPCodesCB[0xEA] = &Processor::OPCodeCB0xEA;
    m_OPCodesCB[0xEB] = &Processor::OPCodeCB0xEB;
    m_OPCodesCB[0xEC] = &Processor::OPCodeCB0xEC;
    m_OPCodesCB[0xED] = &Processor::OPCodeCB0xED;
    m_OPCodesCB[0xEE] = &Processor::OPCodeCB0xEE;
    m_OPCodesCB[0xEF] = &Processor::OPCodeCB0xEF;

    m_OPCodesCB[0xF0] = &Processor::OPCodeCB0xF0;
    m_OPCodesCB[0xF1] = &Processor::OPCodeCB0xF1;
    m_OPCodesCB[0xF2] = &Processor::OPCodeCB0xF2;
    m_OPCodesCB[0xF3] = &Processor::OPCodeCB0xF3;
    m_OPCodesCB[0xF4] = &Processor::OPCodeCB0xF4;
    m_OPCodesCB[0xF5] = &Processor::OPCodeCB0xF5;
    m_OPCodesCB[0xF6] = &Processor::OPCodeCB0xF6;
    m_OPCodesCB[0xF7] = &Processor::OPCodeCB0xF7;
    m_OPCodesCB[0xF8] = &Processor::OPCodeCB0xF8;
    m_OPCodesCB[0xF9] = &Processor::OPCodeCB0xF9;
    m_OPCodesCB[0xFA] = &Processor::OPCodeCB0xFA;
    m_OPCodesCB[0xFB] = &Processor::OPCodeCB0xFB;
    m_OPCodesCB[0xFC] = &Processor::OPCodeCB0xFC;
    m_OPCodesCB[0xFD] = &Processor::OPCodeCB0xFD;
    m_OPCodesCB[0xFE] = &Processor::OPCodeCB0xFE;
    m_OPCodesCB[0xFF] = &Processor::OPCodeCB0xFF;
}
