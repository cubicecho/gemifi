# gemifi helpers for OpenOCD (NUC1xx). Loaded by the scripts in this folder with `-f lib.tcl`
# after the probe config. Defines procs only; does nothing by itself.
#
# Lines starting with "GEMIFI " are parsed by the shell scripts. Errors raised here make OpenOCD
# stop processing the remaining -c commands and exit non-zero.

set GEMIFI_PDID 0x10012008   ;# NUC120LC1BN

# Abort unless the chip ID is right. Plain AHB read; works with the core running.
proc gemifi_check_pdid {} {
    global GEMIFI_PDID
    set v [lindex [read_memory 0x50000000 32 1] 0]
    if {$v eq ""} { error "GEMIFI ABORT: could not read PDID at 0x50000000; nothing was written" }
    echo [format "GEMIFI PDID 0x%08x" $v]
    if {$v != $GEMIFI_PDID} {
        error [format "GEMIFI ABORT: PDID 0x%08x is not 0x%08x (NUC120LC1BN). Wrong chip or bad SWD link; nothing was written." $v $GEMIFI_PDID]
    }
}

# First 4 words of APROM. All 0xFFFFFFFF = locked (or erased); a vector table = unlocked.
proc gemifi_show_aprom_head {} {
    set w [read_memory 0x00000000 32 4]
    set s ""
    foreach x $w { append s [format " %08x" $x] }
    echo "GEMIFI APROM0$s"
    if {[string trim $s] eq "ffffffff ffffffff ffffffff ffffffff"} {
        echo "GEMIFI LOCK locked-or-erased"
    } else {
        echo "GEMIFI LOCK unlocked"
    }
}

# One word through the flash controller's ISP READ command (ISPCMD 0x00). Needs the CPU halted
# and ISP enabled (gemifi_isp_enable). Reads LDROM, data flash and CONFIG, which plain mdw can't.
proc gemifi_isp_read {addr} {
    for {set try 0} {$try < 3} {incr try} {
        if {[catch {
            mww 0x5000C00C 0x00
            mww 0x5000C004 $addr
            mww 0x5000C010 1
            set v [lindex [read_memory 0x5000C008 32 1] 0]
        }] == 0 && $v ne ""} { return $v }
    }
    error [format "GEMIFI ABORT: ISP read failed at 0x%08x" $addr]
}

# numicro read_isp sets ISPEN/LDUEN/APUEN/CFGUEN (volatile) as a side effect. CPU must be halted.
proc gemifi_isp_enable {} {
    numicro read_isp 0x00300000
}

proc gemifi_read_config {} {
    gemifi_isp_enable
    set c0 [gemifi_isp_read 0x00300000]
    set c1 [gemifi_isp_read 0x00300004]
    echo [format "GEMIFI CONFIG0 0x%08x" $c0]
    echo [format "GEMIFI CONFIG1 0x%08x" $c1]
}

# Write a raw binary to APROM (erases only the pages it covers), then verify it over the AHB.
proc gemifi_write_aprom {file} {
    flash write_image erase $file 0x00000000 bin
    verify_image $file 0x00000000 bin
    echo "GEMIFI WROTE aprom $file"
}

# Erase a whole region (all pages) and write a raw binary at its start. No AHB verify here:
# LDROM and data flash aren't readable as plain memory; the shell scripts verify them by ISP dump.
proc gemifi_write_isp_region {name base size file} {
    flash erase_address $base $size
    flash write_image $file $base bin
    echo "GEMIFI WROTE $name $file"
}

# One ISP command (ISPCMD 0x21 program / 0x22 page erase) with completion and ISPFF checks.
# Used only for CONFIG: OpenOCD 0.12's numicro driver gives the NUC120 CONFIG bank 8 bytes and
# therefore zero 512-byte sectors, so `flash erase_address 0x00300000 ...` can't erase it.
proc gemifi_isp_cmd {cmd addr data} {
    mww 0x5000C00C $cmd
    mww 0x5000C004 $addr
    mww 0x5000C008 $data
    mww 0x5000C010 1
    for {set i 0} {$i < 200} {incr i} {
        if {([lindex [read_memory 0x5000C010 32 1] 0] & 1) == 0} break
        sleep 1
    }
    set con [lindex [read_memory 0x5000C000 32 1] 0]
    if {$con & 0x40} {
        mww 0x5000C000 [expr {$con | 0x40}]
        error [format "GEMIFI ABORT: ISP command 0x%02x at 0x%08x failed (ISPFF set)" $cmd $addr]
    }
}

# CONFIG write: only ever called by restore-stock.sh --write-config after typed confirmation.
proc gemifi_write_config {c0 c1} {
    gemifi_isp_enable
    gemifi_isp_cmd 0x22 0x00300000 0xFFFFFFFF
    gemifi_isp_cmd 0x21 0x00300000 $c0
    gemifi_isp_cmd 0x21 0x00300004 $c1
    set r0 [gemifi_isp_read 0x00300000]
    set r1 [gemifi_isp_read 0x00300004]
    echo [format "GEMIFI CONFIG0 0x%08x" $r0]
    echo [format "GEMIFI CONFIG1 0x%08x" $r1]
    if {$r0 != $c0 || $r1 != $c1} { error "GEMIFI ABORT: CONFIG read-back mismatch" }
}
