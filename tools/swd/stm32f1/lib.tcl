# gemifi helpers for OpenOCD (M-Audio Code 61, STM32F103RBT6). Loaded after the probe config with
# `-f lib.tcl`. Defines procs only; does nothing by itself. code61.sh calls them.
#
# Flash map: 0x08000000-0x080023FF bootloader (M-Audio's, or gemifi's from bootloader/stm32f1),
# 0x08002400-0x0801AFFF app, 0x0801B000-0x0801B3FF M-Audio's image checksum page, 0x0801B400-0x0801FFFF
# settings + update flag.

# Soft power (boards/m-audio-code61.h): the power button (PC13) powers the board only while held; PC15
# HIGH keeps it on. Halt the core and latch it by hand, so a halted or crashed chip stays powered.
proc code61_hold_power {} {
    halt
    mmw 0x40021018 0x10 0                ;# RCC_APB2ENR |= IOPCEN
    mww 0x40011010 0x8000                ;# GPIOC_BSRR: PC15 = 1
    mmw 0x40011004 0x10000000 0xE0000000 ;# GPIOC_CRH: PC15 = output push-pull, 10 MHz
    echo "GEMIFI POWER PC15 latched high"
}

# One 32-bit word, or "fail" (a read can fail with readout protection on, or on a flaky link).
proc code61_word {addr} {
    if {[catch {set v [lindex [read_memory $addr 32 1] 0]}]} { return fail }
    return [format 0x%08x $v]
}

proc code61_info {} {
    set obr [code61_word 0x4002201C]
    echo "GEMIFI IDCODE [code61_word 0xE0042000]  CPUID [code61_word 0xE000ED00]"
    echo "GEMIFI OBR $obr  WRPR [code61_word 0x40022020]  FLASH_SIZE [code61_word 0x1FFFF7E0]"
    echo "GEMIFI BOOTLOADER [code61_word 0x08000000] [code61_word 0x08000004]"
    echo "GEMIFI APP        [code61_word 0x08002400] [code61_word 0x08002404]"
    echo "GEMIFI UPDATE FLAG [code61_word 0x0801FC00] [code61_word 0x0801FC04]"
    echo "GEMIFI PC [lindex [reg pc] 2]  RCC_CSR [code61_word 0x40021024]"
    if {$obr ne "fail" && ($obr & 2)} {
        echo "GEMIFI RDP ON: readout protection is set; flash can't be read over SWD"
    }
}

# Whole 128 KB, bootloader included. Private: never commit the output.
proc code61_backup {file} {
    halt
    dump_image $file 0x08000000 0x20000
    echo "GEMIFI BACKUP $file"
}

# An app at 0x08002400: a .gfw (under the gemifi bootloader), or a full app area from
# `maudio_update.py flashbin` (0x08002400..0x0801B3FF, under M-Audio's bootloader). Erases and writes only
# the pages the file covers; the bootloader and the settings stay untouched.
proc code61_write_app {file} {
    set size [file size $file]
    if {$size == 0 || $size > 0x19000} {
        error [format "GEMIFI ABORT: %s is %d bytes; the app area is 102400" $file $size]
    }
    halt
    flash write_image erase $file 0x08002400 bin
    verify_image $file 0x08002400 bin
    echo "GEMIFI WROTE $file"
}

# The gemifi bootloader (bootloader/stm32f1, build/gemifi-boot.bin) at 0x08000000. Only on a chip
# without M-Audio's bootloader (readout protection off): it replaces whatever is there.
proc code61_write_boot {file} {
    set size [file size $file]
    if {$size == 0 || $size > 0x2400} {
        error [format "GEMIFI ABORT: %s is %d bytes; the bootloader area is 9216" $file $size]
    }
    halt
    flash write_image erase $file 0x08000000 bin
    verify_image $file 0x08000000 bin
    echo "GEMIFI WROTE $file"
}

# Read-only snapshot of core and peripheral state (works with readout protection on). Call halted.
proc code61_state {} {
    foreach r {pc sp lr r0 r1 r2 r3 r4 r5 r6 r7 xPSR} {
        if {![catch {set v [lindex [reg $r] 2]}]} { echo "GEMIFI REG $r $v" }
    }
    foreach {name addr} {
        VTOR 0xE000ED08  ICSR 0xE000ED04  SHCSR 0xE000ED24  CFSR 0xE000ED28  HFSR 0xE000ED2C
        SYST_CSR 0xE000E010  NVIC_ISER0 0xE000E100  NVIC_ISER1 0xE000E104
        RCC_CR 0x40021000  RCC_CFGR 0x40021004  RCC_APB2ENR 0x40021018  RCC_APB1ENR 0x4002101C
        GPIOA_CRH 0x40010804  GPIOA_IDR 0x40010808  GPIOA_ODR 0x4001080C
        GPIOB_CRL 0x40010C00  GPIOB_CRH 0x40010C04  GPIOB_IDR 0x40010C08  GPIOB_ODR 0x40010C0C
        GPIOC_CRH 0x40011004  GPIOC_IDR 0x40011008  GPIOC_ODR 0x4001100C
        USB_CNTR 0x40005C40  USB_ISTR 0x40005C44  USB_DADDR 0x40005C4C  FLASH_SR 0x4002200C  FLASH_CR 0x40022010
    } {
        echo "GEMIFI $name [code61_word $addr]"
    }
}

# Watch the key-matrix columns (PB8..PB15, active low) for `seconds`, printing each change. Only means
# "row 16 buttons" while PB3..PB7 are all driven low (the vendor bootloader leaves them so when it faults).
proc code61_watch {seconds} {
    set last "-"
    set end [expr {[clock milliseconds] + $seconds * 1000}]
    while {[clock milliseconds] < $end} {
        set w [code61_word 0x40010C08]
        if {$w ne "fail"} {
            set held [list]
            for {set c 0} {$c < 8} {incr c} {
                if {!(($w >> (8 + $c)) & 1)} { lappend held "col$c/PB[expr {8 + $c}]" }
            }
            set s [join $held " "]
            if {$s ne $last} {
                echo "GEMIFI HELD [expr {$s eq "" ? "none" : $s}]"
                set last $s
            }
        }
        sleep 30
    }
}

# Drive the row select ourselves (PB3..PB5 = row - 16, PB6 = PB7 = 0) and scan button rows 16..20 for
# `seconds`, printing each change as "row/column = slot". Only for a core that leaves GPIOB alone
# (the vendor bootloader stuck in its HardFault loop, or halted).
proc code61_scan_buttons {seconds} {
    mmw 0x40021018 0x09 0                   ;# AFIO + IOPB clocks
    mmw 0x40010004 0x02000000 0x05000000    ;# AFIO_MAPR SWJ_CFG = 010: JTAG off, SWD on (frees PB3/PB4)
    mww 0x40010C00 0x11111444               ;# PB3..PB7 push-pull out, PB0..PB2 untouched-ish (input)
    mww 0x40010C04 0x88888888               ;# PB8..PB15 input pull
    set last "-"
    set end [expr {[clock milliseconds] + $seconds * 1000}]
    while {[clock milliseconds] < $end} {
        set held [list]
        for {set r 16} {$r <= 20} {incr r} {
            mww 0x40010C0C [expr {0xFF00 | (($r - 16) << 3)}]   ;# ODR: pull-ups on, row address, groups low
            set w [code61_word 0x40010C08]
            if {$w eq "fail"} { continue }
            for {set c 0} {$c < 8} {incr c} {
                if {!(($w >> (8 + $c)) & 1)} {
                    lappend held "row$r/col$c=slot[expr {($r - 16) * 8 + $c}]"
                }
            }
        }
        set s [join $held " "]
        if {$s ne $last} {
            echo "GEMIFI HELD [expr {$s eq "" ? "none" : $s}]"
            set last $s
        }
    }
    mww 0x40010C0C 0xFF00                   ;# back to row 16
}

# Catch the core at its first instruction after a reset released by hand: keep DEMCR.VC_CORERESET armed
# (it doesn't survive a power-on reset, and the core may not answer while NRST is held) until DHCSR shows
# S_HALT, for up to `seconds`. Hold NRST to GND before starting, let go a few seconds in.
proc code61_catch_reset {seconds} {
    set end [expr {[clock milliseconds] + $seconds * 1000}]
    set lastd ""
    while {[clock milliseconds] < $end} {
        # Every step caught: OpenOCD's own reset handling (run from inside sleep/poll) can throw.
        # DEMCR only. Never write DHCSR here: a write with C_HALT = 0 resumes a core that the vector
        # catch just stopped (C_DEBUGEN is already set by OpenOCD's connection).
        catch {mww 0xE000EDFC 0x01000001}            ;# DEMCR: TRCENA | VC_CORERESET
        set d fail
        catch {set d [code61_word 0xE000EDF0]}
        if {$d ne $lastd} {
            set pcs fail
            catch {set pcs [code61_word 0xE000101C]}
            echo "GEMIFI DHCSR $d PCSR $pcs"
            set lastd $d
        }
        if {$d ne "fail" && ($d & 0x00020000)} {
            set pc "?"
            catch {poll}
            catch {set pc [lindex [reg pc] 2]}
            echo "GEMIFI CAUGHT DHCSR $d PC $pc"
            return 1
        }
        catch {sleep 20}
    }
    echo "GEMIFI CATCH TIMEOUT"
    return 0
}

# Log `n` single steps: pc and the low registers after each.
proc code61_trace {n} {
    for {set i 0} {$i < $n} {incr i} {
        if {[catch {step}]} { echo "GEMIFI STEP $i failed"; return }
        set s "GEMIFI STEP $i"
        foreach r {pc r0 r1 r2 r3 r4 r5 sp} {
            catch {append s " $r=[lindex [reg $r] 2]"}
        }
        echo $s
    }
}

# Sample the running PC through DWT_PCSR without halting, n times.
proc code61_pcsample {n} {
    mmw 0xE000EDFC 0x01000000 0     ;# DEMCR.TRCENA
    for {set i 0} {$i < $n} {incr i} {
        echo "GEMIFI PCSR [code61_word 0xE000101C]"
        sleep 50
    }
}
