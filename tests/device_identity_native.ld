ENTRY(_start)
PHDRS { start PT_LOAD FLAGS(5); logger PT_LOAD FLAGS(5); native PT_LOAD FLAGS(5); compare PT_LOAD FLAGS(5); strings PT_LOAD FLAGS(4); oslog PT_LOAD FLAGS(5); strrchr PT_LOAD FLAGS(5); guard PT_LOAD FLAGS(6); globals PT_LOAD FLAGS(6); }
SECTIONS {
 .start 0x40080000 : { *(.text.start) } :start
 .logger 0x19ceee6d8 : { *(.logger) } :logger
 .native 0x19cef0394 : { *(.native) } :native
 .strncmp 0x19cf4d0f0 : { *(.strncmp) } :compare
 .native_strings 0x19cf54cae : { *(.native_strings) } :strings
 .oslog 0x1a1c70a84 : { *(.oslog) } :oslog
 .strrchr 0x1a1c70b2c : { *(.strrchr) } :strrchr
 .guard 0x1e5e8c7d0 (NOLOAD) : { *(.guard) } :guard
 .globals 0x1f2937260 (NOLOAD) : { *(.globals) } :globals
 /DISCARD/ : { *(.comment) *(.note*) }
}
