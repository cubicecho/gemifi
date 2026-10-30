# READ-ONLY. Reads [START, END) through the flash controller's ISP READ command (ISPCMD 0x00 only)
# and appends "addr value" lines to OUT, flushing each line. Expects START, END and OUT set with -c,
# and lib.tcl loaded first. The CPU is halted for the session, then reset at the end.
# Used by isp-dump.sh (one short session per 256 words, because long sessions lock up the Bus Pirate).
init
gemifi_check_pdid
halt
gemifi_isp_enable
set f [open $OUT a]
for {set a $START} {$a < $END} {incr a 4} {
    puts $f [format "%08x %08x" $a [gemifi_isp_read $a]]
    flush $f
}
close $f
reset run
shutdown
