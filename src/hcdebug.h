#ifndef HC_DEBUG__
#define HC_DEBUG__

#include <stdarg.h>
#include <stdint.h>

typedef struct hc_DebuggerIf hc_DebuggerIf;

typedef struct {
    struct {
        char const* name;
        unsigned width_bytes;
        uint64_t (*get)(hc_DebuggerIf const* debugger_if, unsigned index);
        char const* const* bits;

        /* set can be null if the register can't be changed or if it doesn't make sense to do so */ 
        void (*set)(hc_DebuggerIf const* debugger_if, unsigned index, uint64_t value);
    }
    v1;
}
hc_Register;

typedef struct {
    struct {
        char const* description;
        uint64_t base_address;
        uint64_t size;
        int is_main;
        uint8_t (*peek)(hc_DebuggerIf const* debugger_if, unsigned index, uint64_t address);

        /* poke can be null for read-only memory but all memory should be writeable to allow patching */
        /* poke can be non-null and still don't change the value, i.e. for the main memory region when the address is in rom */
        void (*poke)(hc_DebuggerIf const* debugger_if, unsigned index, uint64_t address, uint8_t value);

        /* set_watch_point can be null when not supported */
        unsigned (*set_watch_point)(hc_DebuggerIf const* debugger_if, unsigned index, uint64_t address, uint64_t length, int read, int write);
    }
    v1;
}
hc_Memory;

typedef enum {
    HC_Z80
}
hc_CpuType;

typedef struct {
    struct {
        hc_CpuType type;
        char const* description;
        hc_Register const* const* registers;
        unsigned num_registers;
        hc_Memory const* const* memory_regions;
        unsigned num_memory_regions;

        /* these can be null if the cpu doesn't support debugging; the main cpu must support them */
        void (*pause)(hc_DebuggerIf const* debugger_if, unsigned index);
        void (*resume)(hc_DebuggerIf const* debugger_if, unsigned index);
        void (*step)(hc_DebuggerIf const* debugger_if, unsigned index);

        /* set_break_point can be null when not supported */
        unsigned (*set_break_point)(hc_DebuggerIf const* debugger_if, unsigned index, uint64_t address);
    }
    v1;
}
hc_Cpu;

typedef struct {
    struct {
        char const* description;
        hc_Cpu const* const* cpus;
        unsigned num_cpus;
        hc_Register const* const* registers;
        unsigned num_registers;
        hc_Memory const* const* memory_regions;
        unsigned num_memory_regions;
    }
    v1;
}
hc_System;

struct hc_DebuggerIf {
    unsigned const version;

    struct {
        hc_System const* system;
    }
    v1;
};

typedef void (*hc_Set)(hc_DebuggerIf* const debugger_if);

#endif /* HC_DEBUG__ */
