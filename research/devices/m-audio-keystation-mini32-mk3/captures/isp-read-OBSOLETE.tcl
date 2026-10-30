# READ-ONLY dump of NUC120 CONFIG0/1, LDROM and data flash through the flash controller's ISP READ command.
# Only ISPCMD 0x00 (READ) is ever issued. numicro read_isp enables the ISP block (volatile registers, cleared by reset).
proc isp_read {addr} {
    mww 0x5000C00C 0x00          ;# ISPCMD = READ
    mww 0x5000C004 $addr         ;# ISPADR
    mww 0x5000C010 1             ;# ISPTRG = GO
    for {set i 0} {$i < 50} {incr i} {
        if {([lindex [read_memory 0x5000C010 32 1] 0] & 1) == 0} { break }
    }
    return [lindex [read_memory 0x5000C008 32 1] 0]
}
proc dump_region {base size path} {
    set f [open $path wb]
    for {set a $base} {$a < $base + $size} {incr a 4} {
        puts -nonewline $f [binary format i [isp_read $a]]
    }
    close $f
    echo "dumped $size bytes from [format 0x%08x $base] to $path"
}
init
halt
numicro read_isp 0x00300000
numicro read_isp 0x00300004
set c0 [isp_read 0x00300000]
set c1 [isp_read 0x00300004]
set f [open backup/config.txt w]
puts $f [format "CONFIG0 0x%08x\nCONFIG1 0x%08x" $c0 $c1]
close $f
echo [format "CONFIG0 0x%08x CONFIG1 0x%08x" $c0 $c1]
dump_region 0x00100000 0x1000 backup/ldrom-read1.bin
dump_region 0x00100000 0x1000 backup/ldrom-read2.bin
dump_region 0x0001F000 0x1000 backup/dataflash-read1.bin
dump_region 0x0001F000 0x1000 backup/dataflash-read2.bin
reset run
shutdown
