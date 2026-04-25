# JTAG Primer

A practical introduction to JTAG for someone who knows software and basic
electronics but hasn't worked with JTAG before.

---

## What JTAG Actually Is

JTAG (Joint Test Access Group, IEEE 1149.1) is a serial protocol that lets you
talk to a chip's internals through a small number of pins. It was originally
designed for testing solder joints on PCBs ("boundary scan" — you can wiggle
individual pins and read their state to verify they're connected correctly), but
it turned out to be incredibly useful for:

- **Debugging** — halt the CPU, inspect registers, set breakpoints, single-step
- **Flash programming** — read and write the flash memory chip through the CPU
- **Production testing** — verify board assembly without physical probes

For our purposes, we care about the second use: reading and writing flash to
recover a bricked device.

---

## The Physical Interface

JTAG uses **4 signal wires** plus ground:

| Signal | Direction | Purpose |
|--------|-----------|---------|
| **TCK** | Adapter → Target | Clock. The adapter drives this. Every rising edge clocks one bit in/out. |
| **TMS** | Adapter → Target | Test Mode Select. Controls the JTAG state machine. Think of it as "which register am I talking to." |
| **TDI** | Adapter → Target | Test Data In. Serial data going INTO the chip. |
| **TDO** | Target → Adapter | Test Data Out. Serial data coming OUT of the chip. |

Plus two optional signals:

| Signal | Direction | Purpose |
|--------|-----------|---------|
| **TRST*** | Adapter → Target | Test Reset. Active-low, resets the JTAG state machine. Optional because you can also reset via TMS. |
| **SRST/nRESET** | Adapter → Target | System Reset. Resets the entire chip (not just JTAG). Lets your debugger reset the target without power cycling. |

The asterisk (`*`) means active-low — the signal does its thing when pulled to
ground, and is inactive when high.

### Why So Few Wires?

Everything in JTAG is serial — one bit at a time. To read a 32-bit register,
you clock 32 bits out on TDO. To write one, you clock 32 bits in on TDI. This
is slow compared to a parallel bus, but the simplicity means you only need 4
signal pins on the chip — a tiny cost for the functionality you get.

### Voltage

JTAG signals run at the target's I/O voltage. The NS7520 is 3.3V. The J-Link
EDU Mini is voltage-adaptive — you connect VTref (pin 1) to the target's 3.3V
rail, and it adjusts its output drivers to match. Never connect a 5V JTAG
adapter to a 3.3V target without level shifting.

---

## The JTAG State Machine (TAP Controller)

The protocol is built around a 16-state finite state machine inside the chip
called the TAP (Test Access Port) controller. You don't need to memorize all 16
states, but understanding the high-level flow helps:

```
                    ┌──────────────┐
                    │  Test-Logic  │◄── Power-on / TRST*
                    │    Reset     │
                    └──────┬───────┘
                           │ TMS=0
                    ┌──────▼───────┐
              ┌────►│   Run-Test   │◄────┐
              │     │    /Idle     │     │
              │     └──────┬───────┘     │
              │            │ TMS=1       │
              │     ┌──────▼───────┐     │
              │     │  Select-DR   │     │
              │     │    Scan      │     │
              │     └──┬───────┬───┘     │
              │        │       │         │
              │   ┌────▼──┐ ┌──▼─────┐   │
              │   │ DATA  │ │ Select │   │
              │   │ path  │ │ IR-Scan│   │
              │   │       │ │        │   │
              │   │Capture│ └──┬─────┘   │
              │   │ Shift │    │         │
              │   │Update │ ┌──▼─────┐   │
              │   └───┬───┘ │INSTRUC │   │
              │       │     │ path   │   │
              │       │     │Capture │   │
              │       │     │ Shift  │   │
              │       │     │Update  │   │
              │       │     └──┬─────┘   │
              │       └────────┴─────────┘
```

In practice:

1. You first shift bits into the **Instruction Register (IR)** to select what
   you want to do (e.g., "I want to access the debug registers" or "I want to
   do boundary scan")
2. Then you shift bits through the **Data Register (DR)** to read/write the
   actual data

The TMS signal controls which path through the state machine you take. Your
JTAG adapter (J-Link) and software (OpenOCD) handle all of this automatically —
you never manually toggle TMS. But knowing it's a state machine explains why
JTAG operations are sequential and why wiring problems cause "scan chain" errors
(the state machine got out of sync).

---

## The Scan Chain

Multiple JTAG-capable chips on a board can be **daisy-chained**: TDO of chip 1
connects to TDI of chip 2. The adapter sees one long shift register. This is
called the "scan chain."

```
  J-Link                 Chip A              Chip B
  ┌────┐    TDI    ┌────────────┐    ┌────────────┐
  │    ├──────────►│TDI      TDO├───►│TDI      TDO├──┐
  │    │           └────────────┘    └────────────┘  │
  │    │◄────────────────────────────────────────────┘
  │    │    TDO
  │    │
  │    ├──────────► TCK (shared by all chips)
  │    ├──────────► TMS (shared by all chips)
  └────┘
```

On the SN110, there's likely only one chip in the JTAG chain (the NS7520), so
this is simple. The OpenOCD config line:

```tcl
jtag newtap ns7520 cpu -irlen 4
```

...tells OpenOCD: "There's one device in the chain with a 4-bit instruction
register." If this is wrong (wrong IR length, or there are unexpected devices
in the chain), the scan will fail. OpenOCD will tell you what it found vs. what
it expected.

---

## How Flash Programming Works Through JTAG

This is the part that confused me at first: the J-Link doesn't talk directly to
the flash chip. The data path is:

```
  J-Link ──JTAG──► NS7520 CPU ──memory bus──► Intel Flash Chip
```

Here's what happens when you run `flash write_image`:

1. OpenOCD halts the ARM7TDMI CPU inside the NS7520
2. OpenOCD uploads a small "flash loader" program into the NS7520's RAM
3. OpenOCD feeds your flash data to the RAM in chunks
4. The flash loader program runs on the ARM CPU and performs the Intel CFI
   flash write sequence (unlock → program → verify) through the normal memory
   bus
5. Repeat until all data is written

This is why `flash probe 0` is needed first — OpenOCD queries the flash chip
(through the CPU) using CFI (Common Flash Interface) commands to discover its
size, sector layout, and supported write algorithms.

Reading is simpler: OpenOCD halts the CPU and reads memory addresses directly
through JTAG debug access. Since flash is memory-mapped, reading address
0x00000000 gives you the first byte of flash.

---

## Speed

JTAG is serial, one bit per clock cycle. At 100 kHz TCK:

- Reading a 32-bit word takes ~50 clock cycles (32 data + overhead) = ~2ms
- Reading 4MB = ~1,048,576 words × 2ms = ~35 minutes

At 1 MHz TCK: ~3.5 minutes for 4MB. At 4 MHz: under a minute.

The J-Link EDU Mini can go up to several MHz, but the practical limit depends
on wire length, quality, and the target's JTAG implementation. Start at 100 kHz
and increase once you have a stable connection:

```
> adapter speed 100      # Safe starting point
> adapter speed 1000     # 1 MHz — usually fine with short wires
> adapter speed 4000     # 4 MHz — may need good wiring
```

If you get read errors or scan failures at higher speeds, drop back down.

---

## What Can Go Wrong

### Nothing — JTAG Is Read-Safe

The most important thing to know: **reading through JTAG is completely safe**.
You can read flash, RAM, and registers all day without changing anything on the
target. The only destructive operations are explicit writes. So the dump step
carries zero risk.

### Wiring Problems (Most Common)

Symptoms: "JTAG scan chain interrogation failed", "no device found"

- **No GND connection**: The most common mistake. JTAG signals are referenced
  to ground. Without a solid GND connection, nothing works.
- **TDI/TDO swapped**: Easy to mix up. TDI on the adapter goes to TDI on the
  chip (not crossed like TX/RX on UART).
- **Floating TRST***: If TRST* is floating and picks up noise, it can randomly
  reset the JTAG state machine. Tie it high (to VCC via 10k resistor) or
  connect it to the adapter's TRST output.
- **Bad solder joint**: Especially on QFP pins. Use a magnifying glass to
  check for bridges between adjacent pins.

### The CPU Won't Halt

Symptoms: "timed out while waiting for target halted"

On the SN110, the CPU is in a ~3-second boot loop (boot → crash → reset).
The `halt` command needs to catch the CPU while it's running. Strategies:

- Run `halt` repeatedly until it catches
- Use `reset halt` (requires nRESET connected) — this resets the CPU and
  immediately asserts a halt request, catching it before it executes any code
- Increase JTAG speed so the halt command arrives faster

### Flash Operations Fail

- **"not probed"**: Run `flash probe 0` first
- **Write-protected sectors**: Check with `flash protect_check 0`
- **Wrong base address**: Flash might be at 0x10000000 instead of 0x00000000
  depending on the NS7520's chip select configuration. If your dump is all
  0xFF, try different base addresses
- **Erase before write**: Flash bits can only go from 1→0. To write new data,
  you must erase first (sets all bits to 1). `flash write_image erase` does
  this automatically.

---

## OpenOCD Command Cheat Sheet

These are the commands you'll use in the OpenOCD telnet session (`telnet
localhost 4444`):

### Connection and Control

```
halt                              # Stop the CPU
resume                            # Continue execution
reset run                         # Reset and run
reset halt                        # Reset and immediately halt
reg                               # Show all CPU registers
adapter speed <kHz>               # Set JTAG clock speed
```

### Memory Access (Read)

```
mdw <addr> [count]                # Read 32-bit words (Memory Display Word)
mdh <addr> [count]                # Read 16-bit halfwords
mdb <addr> [count]                # Read bytes

# Examples:
mdw 0x00000000 4                  # First 4 words of flash
mdb 0xFFB00000 16                 # First 16 bytes of NS7520 internal RAM
```

### Memory Access (Write)

```
mww <addr> <value>                # Write 32-bit word (Memory Write Word)
mwh <addr> <value>                # Write 16-bit halfword
mwb <addr> <value>                # Write byte
```

**Note**: Writing to flash addresses with `mww` does NOT work — flash requires
special unlock/program sequences. Use the `flash` commands below instead.

### Flash Operations

```
flash probe 0                     # Detect and identify flash chip
flash info 0                      # Show flash geometry (sectors, sizes)
flash protect_check 0             # Check which sectors are write-protected

flash read_bank 0 <file> [offset] [length]
                                  # Read flash to file (alternative to dump_image)

flash write_image erase <file> [offset]
                                  # Erase affected sectors and write file to flash

flash erase_sector 0 <first> <last>
                                  # Erase specific sectors

flash verify_image <file> [offset]
                                  # Compare file against flash contents (non-destructive)
```

### Bulk Read/Write

```
dump_image <file> <addr> <size>   # Read arbitrary memory region to file
load_image <file> <addr>          # Write file to memory (RAM, not flash)
verify_image <file> <addr>        # Compare file against memory
```

---

## Mental Model

Think of JTAG as a **remote control for the CPU**. Through four wires, you can:

1. **Pause** the CPU at any point
2. **Peek** at anything the CPU can see — RAM, flash, I/O registers, its own
   internal registers
3. **Poke** anything the CPU can write to — RAM, I/O registers
4. **Run small programs** on the CPU (this is how flash writing works — OpenOCD
   loads a helper program into RAM and runs it)

You are effectively the CPU's operator. Anything the CPU could do by executing
code, you can do through JTAG — just much more slowly, since every operation
goes through a serial interface. But for recovery work, speed doesn't matter.
What matters is that JTAG gives you **complete access** regardless of what state
the software is in. The CPU's JTAG port is wired into the silicon itself — it
works whether the flash is empty, corrupt, or running code. That's what makes
it the ultimate recovery tool.
