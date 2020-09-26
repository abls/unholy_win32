#include <stdint.h>

#include "win32bridges.hpp"
#include "win32memory.hpp"

// If a function has the suffix "_ptbl" you can assume that it is position-independent AND
// accesses no global information. It is completely self-contained and safe to copy around
// locally or even to remote processes. "ptbl" = "portable"... :)

// Useful macros used internally for easy parsing of arg_info
#define ARGINFO_NSLOTS(x) (x & 0xff)
#define ARGINFO_IDX1(x)  ((x >> 8) & 0xff)
#define ARGINFO_IDX2(x)  ((x >> 16) & 0xff)

// Typedefs for passing addresses of winapi functions to portable functions.
typedef LPVOID(WINAPI* VirtualAlloc_t)(LPVOID, SIZE_T, DWORD, DWORD);
typedef LPVOID(WINAPI* VirtualAllocEx_t)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
typedef BOOL(WINAPI* VirtualFree_t)(LPVOID, SIZE_T, DWORD);
typedef BOOL(WINAPI* VirtualFreeEx_t)(HANDLE, LPVOID, SIZE_T, DWORD);
typedef BOOL(WINAPI* ReadProcessMemory_t)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T);
typedef BOOL(WINAPI* WriteProcessMemory_t)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T);
typedef HANDLE(WINAPI* CreateRemoteThread_t)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, void*, LPVOID, DWORD, LPDWORD);
typedef DWORD(WINAPI* WaitForSingleObject_t)(HANDLE, DWORD);

// Struct used by probe functions to call target functions.
typedef struct ProbeParameters {
    void* target_func;
    int arg_info;
    void** argdata; // TODO: rename to arg_data // TODO: I mean... make this an int pointer? 
    void* rtnval_addr;
} ProbeParameters;

// Struct that gets used by bridge functions so that they have context of how to read args and how to call the target function.
typedef struct BridgeData {
    HANDLE local_handle;
    HANDLE rmt_handle;

    void* probe_func;
    int local_probe_func_size;

    void* target_func;
    int arg_info;
    bool pass_handle;

    VirtualAlloc_t fnVirtualAlloc;
    VirtualAllocEx_t fnVirtualAllocEx;
    VirtualFree_t fnVirtualFree;
    VirtualFreeEx_t fnVirtualFreeEx;
    ReadProcessMemory_t fnReadProcessMemory;
    WriteProcessMemory_t fnWriteProcessMemory;
    CreateRemoteThread_t fnCreateRemoteThread;
    WaitForSingleObject_t fnWaitForSingleObject;
} BridgeData;

// If you are curious about how arg_info is encoded...
// It is a 32-bit value.
// Low byte is the number of stack slots the arguments would occupy (stack slot is 4 bytes)
// Second lowest byte is number of the stack slot of the first arg that is 4 bytes or less.
//   This is useful for fastcall.
//   It is 0xff if there is no arg found.
// Third lowest byte is number of the stack slot of the second arg that is 4 bytes or less.
//   This is useful for fastcall.
//   It is 0xff if there is no arg found.
// Highest byte is unused.

//  ---------------------------
//  |     PROBE FUNCTIONS     |
//  ---------------------------
//
// Probe functions are helper functions used by bridge functions to
// call the target function. Probe functions are copied into the
// remote process and run in remote address space to do the actual
// calling of the target function. Probes are meant to be called with
// CreateRemoteThread with a pointer to a ProbeParameters struct
// passed as the thread argument.

// BRICKS
#pragma region Probe Bricks

// THE BRICKS THAT MAKE UP A PROBE
//   - Set up stack frame
//   - Load argdata and arginfo.nslots from ProbeParameters
//   - Prepare args for the function call
//   - Call the target function
//   - Remove args from stack if necessary
//   - Retrieve return value
//   - Clean up stack frame, return

// Set up stack frame
#define BRK_PROBE_STACKFRAME_SETUP __asm {    \
    __asm push ebp                            \
    __asm mov ebp, esp                        \
    __asm push esi                            \
    __asm push edi                            \
    __asm push ebx                            \
}

// Load argdata and arginfo.nslots from ProbeParameters
#define BRK_PROBE_LOADPARAMS __asm {                      \
    __asm mov ebx, [ebp + 8] /* probe_params */           \
    __asm mov esi, [ebx]ProbeParameters.arg_info          \
    __asm and esi, 0xff /* get nslots from arg_info */    \
    __asm mov eax, [ebx]ProbeParameters.argdata           \
}

// Prepare args for function call
#define BRK_PROBE_PREPARGS __asm {      \
    __asm argloop_start:                \
    __asm dec esi                       \
    __asm cmp esi, 0                    \
    __asm jl short argloop_end          \
    __asm push[eax + esi * TYPE int]    \
    __asm jmp short argloop_start       \
    __asm argloop_end:                  \
}

// Prepare args for function call
#define BRK_PROBE_PREPARGS_FASTCALL __asm {                                                                 \
    __asm mov edi, [ebx]ProbeParameters.arg_info                                                            \
    __asm shr edi, 16                                                                                       \
    __asm and edi, 0xff /* edi will be second slot index */                                                 \
                                                                                                            \
    __asm mov ebx, [ebx]ProbeParameters.arg_info                                                            \
    __asm shr ebx, 8                                                                                        \
    __asm and ebx, 0xff /* ebx will be first slot index */                                                  \
                                                                                                            \
    __asm argloop_start:                                                                                    \
    __asm dec esi                                                                                           \
    __asm cmp esi, 0                                                                                        \
    __asm jl short argloop_end                                                                              \
                                                                                                            \
    __asm cmp esi, ebx /* test if current arg is marked as first arg that is 4 bytes or less */             \
    __asm jne short test_second_reg_arg                                                                     \
    __asm mov ecx, [eax + esi * TYPE int] /* load first register arg */                                     \
    __asm jmp short argloop_start                                                                           \
                                                                                                            \
    __asm test_second_reg_arg: /* test if current arg is marked as second arg that is 4 bytes or less */    \
    __asm cmp esi, edi                                                                                      \
    __asm jne short load_stack_arg                                                                          \
    __asm mov edx, [eax + esi * TYPE int] /* load second register arg */                                    \
    __asm jmp short argloop_start                                                                           \
                                                                                                            \
    __asm load_stack_arg:                                                                                   \
    __asm push[eax + esi * TYPE int] /* push arg onto stack */                                              \
    __asm jmp short argloop_start                                                                           \
    __asm argloop_end:                                                                                      \
}

// Call target function
#define BRK_PROBE_CALLTARGET __asm {               \
    __asm call [ebx]ProbeParameters.target_func    \
}

// Call target function
#define BRK_PROBE_CALLTARGET_FASTCALL __asm {      \
    __asm mov ebx, [ebp + 8] /* probe_params */    \
    __asm call [ebx]ProbeParameters.target_func    \
}

// Remove args from stack
#define BRK_PROBE_REMOVEARGS __asm {                      \
    __asm mov ecx, [ebx]ProbeParameters.arg_info          \
    __asm and ecx, 0xff /* get nslots from arg_info */    \
    __asm lea esp, [esp + ecx * TYPE int]                 \
}

// Retrieve return value (int)
#define BRK_PROBE_GETRTNVAL_INT __asm {                \
    __asm mov ecx, [ebx]ProbeParameters.rtnval_addr    \
    __asm cmp ecx, 0                                   \
    __asm je short noret                               \
    __asm mov[ecx], eax                                \
    __asm noret:                                       \
}

// Retrieve return value (int64)
#define BRK_PROBE_GETRTNVAL_INT64 __asm {              \
    __asm mov ecx, [ebx]ProbeParameters.rtnval_addr    \
    __asm cmp ecx, 0                                   \
    __asm je short noret                               \
    __asm mov[ecx], eax                                \
    __asm mov[ecx + 4], edx                            \
    __asm noret:                                       \
}

// Retrieve return value (float)
#define BRK_PROBE_GETRTNVAL_FLOAT __asm {              \
    __asm mov ecx, [ebx]ProbeParameters.rtnval_addr    \
    __asm cmp ecx, 0                                   \
    __asm je short noret                               \
    __asm fstp dword ptr[ecx]                          \
    __asm noret:                                       \
}

// Retrieve return value (double)
#define BRK_PROBE_GETRTNVAL_DOUBLE __asm {             \
    __asm mov ecx, [ebx]ProbeParameters.rtnval_addr    \
    __asm cmp ecx, 0                                   \
    __asm je short noret                               \
    __asm fstp qword ptr[ecx]                          \
    __asm noret:                                       \
}

// Clean up stack frame and return
#define BRK_PROBE_STACKFRAME_CLEANUP __asm {    \
    __asm pop ebx                               \
    __asm pop edi                               \
    __asm pop esi                               \
    __asm pop ebp                               \
    __asm ret 4                                 \
}

#pragma endregion

// FUNCTIONS
#pragma region Probe Functions

// For cdecl that returns 32 bit values (int, pointers, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeCdecl_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_REMOVEARGS
    BRK_PROBE_GETRTNVAL_INT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For cdecl that returns 64 bit structs (__int64, actual structs, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeCdeclRtn64_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_REMOVEARGS
    BRK_PROBE_GETRTNVAL_INT64
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For cdecl that returns single-precision floating-point values
__declspec(naked) DWORD WINAPI probeCdeclRtnFlt_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_REMOVEARGS
    BRK_PROBE_GETRTNVAL_FLOAT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For cdecl that returns double-precision floating-point values
__declspec(naked) DWORD WINAPI probeCdeclRtnDbl_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_REMOVEARGS
    BRK_PROBE_GETRTNVAL_DOUBLE
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For stdcall that returns 32 bit values (int, pointers, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeStdcall_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_GETRTNVAL_INT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For stdcall that returns 64 bit structs (__int64, actual structs, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeStdcallRtn64_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_GETRTNVAL_INT64
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For stdcall that returns single-precision floating-point values
__declspec(naked) DWORD WINAPI probeStdcallRtnFlt_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_GETRTNVAL_FLOAT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For stdcall that returns double-precision floating-point values
__declspec(naked) DWORD WINAPI probeStdcallRtnDbl_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS
    BRK_PROBE_CALLTARGET
    BRK_PROBE_GETRTNVAL_DOUBLE
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For fastcall that returns 32 bit values (int, pointers, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeFastcall_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS_FASTCALL
    BRK_PROBE_CALLTARGET_FASTCALL
    BRK_PROBE_GETRTNVAL_INT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For fastcall that returns 64 bit structs (__int64, actual structs, etc) that ARE NOT floating-point values
__declspec(naked) DWORD WINAPI probeFastcallRtn64_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS_FASTCALL
    BRK_PROBE_CALLTARGET_FASTCALL
    BRK_PROBE_GETRTNVAL_INT64
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For fastcall that returns single-precision floating-point values
__declspec(naked) DWORD WINAPI probeFastcallRtnFlt_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS_FASTCALL
    BRK_PROBE_CALLTARGET_FASTCALL
    BRK_PROBE_GETRTNVAL_FLOAT
    BRK_PROBE_STACKFRAME_CLEANUP
}

// For fastcall that returns double-precision floating-point values
__declspec(naked) DWORD WINAPI probeFastcallRtnDbl_ptbl(LPVOID) {
    BRK_PROBE_STACKFRAME_SETUP
    BRK_PROBE_LOADPARAMS
    BRK_PROBE_PREPARGS_FASTCALL
    BRK_PROBE_CALLTARGET_FASTCALL
    BRK_PROBE_GETRTNVAL_DOUBLE
    BRK_PROBE_STACKFRAME_CLEANUP
}

#pragma endregion

//  ---------------------------
//  |    BRIDGE FUNCTIONS     |
//  ---------------------------
//
// Bridge functions are functions that when called, will read the
// args passed to it, do everything needed to call the remote target
// function with the given args, then return the value returned from
// the target function. Before a bridge can be used, values have to
// be patched into it, so it is best to use bridges by creating new
// ones through bridge creation functions.

// BRICKS
#pragma region Bridge Bricks

// Set up stack and preserve registers
#define BRK_BRIDGE_A __asm {       \
    __asm push ebp                 \
    __asm mov ebp, esp             \
    __asm sub esp, __LOCAL_SIZE    \
    __asm push ebx                 \
    __asm push esi                 \
    __asm push edi                 \
}

#define BRK_BRIDGE_B_FASTCALL __asm {                 \
    __asm push edx /* store to be handled later */    \
    __asm push ecx /* store to be handled later */    \
}

// Declare variables all at once because this is for naked function
#define BRK_BRIDGE_C                        \
    BridgeData* bridge_data;                \
    ProbeParameters* local_probe_params;    \
    int argdata_size;                       \
    void** local_probe_argdata;             \
    void* rmt_allocated_chonk;              \
    void* rmt_probe_params;                 \
    void* rmt_probe_func;                   \
    HANDLE rmt_thread_handle;

#define BRK_BRIDGE_D_FASTCALL      \
    int arg_first_idx;             \
    int arg_second_idx;            \
    int arg_reg_count;

#define BRK_BRIDGE_E_INTFLT   int rtn_value;
#define BRK_BRIDGE_E_INT64DBL __int64 rtn_value;

#define BRK_BRIDGE_F                                                                                                                                               \
    /* Address of BridgeData struct to be patched in when function is copied, done in asm to avoid problematic compiler optimization */                            \
    __asm { mov bridge_data, 0xBAADB00F }                                                                                                                          \
                                                                                                                                                                   \
    /* Allocate local ProbeParameters struct and write the target function address into the struct */                                                              \
    local_probe_params = reinterpret_cast<ProbeParameters*>(bridge_data->fnVirtualAlloc(0, sizeof(ProbeParameters), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));    \
    local_probe_params->target_func = bridge_data->target_func;

// Copy the arg info to the probe parameters, and adjust the nslots if a handle is to be passed
#define BRK_BRIDGE_G_CDECLSTDCALL                                    \
    local_probe_params->arg_info = bridge_data->arg_info;            \
    if (ARGINFO_NSLOTS(local_probe_params->arg_info) < 0xff)         \
        local_probe_params->arg_info += bridge_data->pass_handle;
//
#define BRK_BRIDGE_G_FASTCALL                                                               \
    local_probe_params->arg_info = bridge_data->arg_info;                                   \
    if (bridge_data->pass_handle) {                                                         \
        if (ARGINFO_NSLOTS(local_probe_params->arg_info) < 0xff)                            \
            local_probe_params->arg_info += 1; /* nslots + 1 */                             \
                                                                                            \
        if (ARGINFO_IDX1(local_probe_params->arg_info) != 0x00) {                           \
            /* set idx2 to current idx1, then set idx1 to 0x00 */                           \
            local_probe_params->arg_info = (local_probe_params->arg_info & 0xff0000ff) |    \
                ((local_probe_params->arg_info << 8) & 0x00ff0000);                         \
        }                                                                                   \
    }

#define BRK_BRIDGE_H                                                                                                                                               \
    /* Calculate size of memory chunk needed to store args */                                                                                                      \
    argdata_size = ARGINFO_NSLOTS(local_probe_params->arg_info) * sizeof(void*);                                                                                   \
                                                                                                                                                                   \
    /* Allocate space locally for argument data */                                                                                                                 \
    local_probe_argdata = reinterpret_cast<void**>(bridge_data->fnVirtualAlloc(0, argdata_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));                        \
    /* If local handle is to be passed to target function, then add it to args... also adjust the start addr of the arg data chunk to simplify the asm ahead */    \
    if (bridge_data->pass_handle) {                                                                                                                                \
        local_probe_argdata[0] = bridge_data->local_handle;                                                                                                        \
        local_probe_argdata += 1;  /* hacky but efficient k don't @ me */                                                                                          \
    }


// Read args from stack into allocated argument data chunk
#define BRK_BRIDGE_I_CDECLSTDCALL                                        \
    for (int i = 0; i < ARGINFO_NSLOTS(bridge_data->arg_info); i++) {    \
        __asm {                                                          \
            __asm mov ecx, [i]                                           \
            __asm shl ecx, 2                                             \
            __asm mov eax, [ebp + 8 + ecx]                               \
            __asm mov edx, [local_probe_argdata]                         \
            __asm mov[edx + ecx], eax                                    \
        }                                                                \
    }
//
#define BRK_BRIDGE_I_FASTCALL                                                                                             \
    arg_first_idx = ARGINFO_IDX1(bridge_data->arg_info); /* slot index of arg passed through ecx */                       \
    arg_second_idx = ARGINFO_IDX2(bridge_data->arg_info); /* slot index of arg passed through edx */                      \
    arg_reg_count = 0; /* to be set to number of arguments passed in registers */                                         \
    for (int i = 0; i < ARGINFO_NSLOTS(bridge_data->arg_info); i++) {                                                     \
        __asm {                                                                                                           \
            __asm mov ecx, [i]                                                                                            \
                                                                                                                          \
            /* test if this is the arg that was passed through ecx, if so then retrieve, and write to argdata chunk */    \
            __asm mov eax, [arg_first_idx]                                                                                \
            __asm cmp eax, ecx                                                                                            \
            __asm jne short arg_test_second                                                                               \
            __asm mov edx, [local_probe_argdata]                                                                          \
            __asm mov eax, [esp] /* stored value of ecx */                                                                \
            __asm mov [edx + ecx * TYPE int], eax                                                                         \
            __asm inc [arg_reg_count]                                                                                     \
            __asm jmp short cont                                                                                          \
                                                                                                                          \
            /* test if this is the arg that was passed through edx, if so then retrieve, and write to argdata chunk */    \
            __asm arg_test_second:                                                                                        \
            __asm mov eax, [arg_second_idx]                                                                               \
            __asm cmp eax, ecx                                                                                            \
            __asm jne short arg_stk_copy                                                                                  \
            __asm mov edx, [local_probe_argdata]                                                                          \
            __asm mov eax, [esp + 4] /* stored value of edx */                                                            \
            __asm mov [edx + ecx * TYPE int], eax                                                                         \
            __asm inc [arg_reg_count]                                                                                     \
            __asm jmp short cont                                                                                          \
                                                                                                                          \
            /* retrieve arg that was passed to this function via the stack and write to argdata chunk */                  \
            __asm arg_stk_copy:                                                                                           \
            __asm mov edx, [local_probe_argdata]                                                                          \
            __asm sub ecx, [arg_reg_count]                                                                                \
            __asm shl ecx, 2                                                                                              \
            __asm mov eax, [ebp + 8 + ecx] /* value of the arg passed on the stack */                                     \
            __asm shr ecx, 2                                                                                              \
            __asm add ecx, [arg_reg_count]                                                                                \
            __asm shl ecx, 2                                                                                              \
            __asm mov [edx + ecx], eax                                                                                    \
                                                                                                                          \
            __asm cont: /* continue with loop */                                                                          \
        }                                                                                                                 \
    }

// If local handle is to be passed, re-adjust the start addr of the arg data chunk to how it should be
#define BRK_BRIDGE_J                 \
    if (bridge_data->pass_handle)    \
        local_probe_argdata -= 1;

// Allocate space for argdata, return value, probe params, and probe func inside remote process all in one call and easy to clean up data chonk
#define BRK_BRIDGE_K_INTFLT                                                                                    \
    rmt_allocated_chonk = reinterpret_cast<void*>(bridge_data->fnVirtualAllocEx(bridge_data->rmt_handle, 0,    \
        argdata_size + 4 + sizeof(ProbeParameters) + bridge_data->local_probe_func_size,                       \
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
//
#define BRK_BRIDGE_K_INT64DBL                                                                                  \
    rmt_allocated_chonk = reinterpret_cast<void*>(bridge_data->fnVirtualAllocEx(bridge_data->rmt_handle, 0,    \
        argdata_size + 8 + sizeof(ProbeParameters) + bridge_data->local_probe_func_size,                       \
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

#define BRK_BRIDGE_L                                                                                                                  \
    /* Set up the probe params struct to know the address of the allocated space for argdata, */                                      \
    /* and copy the local argument data to that remote space */                                                                       \
    local_probe_params->argdata = reinterpret_cast<void**>(rmt_allocated_chonk);                                                      \
    bridge_data->fnWriteProcessMemory(bridge_data->rmt_handle, local_probe_params->argdata, local_probe_argdata, argdata_size, 0);    \
                                                                                                                                      \
    /* Set up the probe params struct to know the address of the allocated space for the return value, */                             \
    /* and store so that the retval can be read once the rmt function returns */                                                      \
    local_probe_params->rtnval_addr = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(rmt_allocated_chonk) + argdata_size);

// Store the address of the allocated space for the rmt probe params
#define BRK_BRIDGE_M_INTFLT \
    rmt_probe_params = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(rmt_allocated_chonk) + argdata_size + 4);
//
#define BRK_BRIDGE_M_INT64DBL \
    rmt_probe_params = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(rmt_allocated_chonk) + argdata_size + 8);

// Copy the local probe params struct to rmt
#define BRK_BRIDGE_N \
    bridge_data->fnWriteProcessMemory(bridge_data->rmt_handle, rmt_probe_params, local_probe_params, sizeof(ProbeParameters), 0);

// If the probe func size is set to zero, that means that the probe func already exists in the rmt process and doesn't need to be copied. Otherwise, copy it over.
#define BRK_BRIDGE_O_INTFLT                                                                                                                            \
    if (bridge_data->local_probe_func_size) {                                                                                                          \
        rmt_probe_func = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(rmt_allocated_chonk) + argdata_size + 4 + sizeof(ProbeParameters));        \
        bridge_data->fnWriteProcessMemory(bridge_data->rmt_handle, rmt_probe_func, bridge_data->probe_func, bridge_data->local_probe_func_size, 0);    \
    } else {                                                                                                                                           \
        rmt_probe_func = bridge_data->probe_func;                                                                                                      \
    }
#define BRK_BRIDGE_O_INT64DBL                                                                                                                          \
    if (bridge_data->local_probe_func_size) {                                                                                                          \
        rmt_probe_func = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(rmt_allocated_chonk) + argdata_size + 8 + sizeof(ProbeParameters));        \
        bridge_data->fnWriteProcessMemory(bridge_data->rmt_handle, rmt_probe_func, bridge_data->probe_func, bridge_data->local_probe_func_size, 0);    \
    } else {                                                                                                                                           \
        rmt_probe_func = bridge_data->probe_func;                                                                                                      \
    }

// Create new remote thread starting on the remote probe function, pass the address of the remote ProbeParameters struct to the remote probe function
#define BRK_BRIDGE_P                                                                                                                 \
    rmt_thread_handle = bridge_data->fnCreateRemoteThread(bridge_data->rmt_handle, 0, 0, rmt_probe_func, rmt_probe_params, 0, 0);    \
    bridge_data->fnWaitForSingleObject(rmt_thread_handle, INFINITE);                                                                 \
    rtn_value = 0;

// Retrieve return value from remote process
#define BRK_BRIDGE_Q_INTFLT \
    bridge_data->fnReadProcessMemory(bridge_data->rmt_handle, local_probe_params->rtnval_addr, &rtn_value, 4, 0);
//
#define BRK_BRIDGE_Q_INT64DBL \
    bridge_data->fnReadProcessMemory(bridge_data->rmt_handle, local_probe_params->rtnval_addr, &rtn_value, 8, 0);

// Free all left over allocated space
#define BRK_BRIDGE_R                                                                               \
    bridge_data->fnVirtualFreeEx(bridge_data->rmt_handle, rmt_allocated_chonk, 0, MEM_RELEASE);    \
    bridge_data->fnVirtualFree(local_probe_params, 0, MEM_RELEASE);                                \
    bridge_data->fnVirtualFree(local_probe_argdata, 0, MEM_RELEASE);

// Properly pass the return value
#define BRK_BRIDGE_S_INT __asm mov eax, [rtn_value]
#define BRK_BRIDGE_S_INT64 __asm {    \
    __asm lea ecx, [rtn_value]        \
    __asm mov eax, [ecx]              \
    __asm mov edx, [ecx + 4]          \
}
#define BRK_BRIDGE_S_FLT __asm fld dword ptr[rtn_value]
#define BRK_BRIDGE_S_DBL __asm fld qword ptr[rtn_value]

// Clean up stack
#define BRK_BRIDGE_T_STDCALLFASTCALL __asm {                                     \
    __asm mov ecx, [bridge_data]                                                 \
    __asm mov ecx, [ecx]BridgeData.arg_info                                      \
    __asm and ecx, 0xff /* get nslots from arg info for later arg clearing */    \
}
//
#define BRK_BRIDGE_U_FASTCALL __asm {                                                                   \
    __asm sub ecx, [arg_reg_count] /* adjust to be number of slots that are actually on the stack */    \
                                                                                                        \
    __asm add esp, 8 /* because I pushed ecx and edx to save their values */                            \
}

// Restore registers
#define BRK_BRIDGE_V __asm {    \
    __asm pop edi               \
    __asm pop esi               \
    __asm pop ebx               \
    __asm mov esp, ebp          \
    __asm pop ebp               \
}

// very hacky way to clear args off of stack while keeping the return address on top
// has to be hacky like this since I want to actually use ret and not fake it with a jmp
#define BRK_BRIDGE_W_STDCALLFASTCALL __asm {    \
    __asm argloop_start:                        \
    __asm dec ecx                               \
    __asm cmp ecx, 0                            \
    __asm jl short argloop_end                  \
    __asm pop dword ptr [esp]                   \
    __asm jmp short argloop_start               \
    __asm argloop_end:                          \
}

#define BRK_BRIDGE_X __asm ret

#pragma endregion

// FUNCTIONS
#pragma region Bridge Functions

__declspec(naked) void* __cdecl bridgeCdecl_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT
    BRK_BRIDGE_V
    BRK_BRIDGE_X
}

__declspec(naked) void* __cdecl bridgeCdeclRtn64_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT64
    BRK_BRIDGE_V
    BRK_BRIDGE_X
}

__declspec(naked) void* __cdecl bridgeCdeclRtnFlt_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_FLT
    BRK_BRIDGE_V
    BRK_BRIDGE_X
}

__declspec(naked) void* __cdecl bridgeCdeclRtnDbl_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_DBL
    BRK_BRIDGE_V
    BRK_BRIDGE_X
}

__declspec(naked) void* __stdcall bridgeStdcall_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __stdcall bridgeStdcallRtn64_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT64
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __stdcall bridgeStdcallRtnFlt_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_FLT
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __stdcall bridgeStdcallRtnDbl_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_C
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_CDECLSTDCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_CDECLSTDCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_DBL
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __fastcall bridgeFastcall_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_B_FASTCALL
    BRK_BRIDGE_C
    BRK_BRIDGE_D_FASTCALL
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_FASTCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_FASTCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_U_FASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __fastcall bridgeFastcallRtn64_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_B_FASTCALL
    BRK_BRIDGE_C
    BRK_BRIDGE_D_FASTCALL
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_FASTCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_FASTCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_INT64
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_U_FASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __fastcall bridgeFastcallRtnFlt_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_B_FASTCALL
    BRK_BRIDGE_C
    BRK_BRIDGE_D_FASTCALL
    BRK_BRIDGE_E_INTFLT
    BRK_BRIDGE_F
    BRK_BRIDGE_G_FASTCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_FASTCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INTFLT
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INTFLT
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INTFLT
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INTFLT
    BRK_BRIDGE_R
    BRK_BRIDGE_S_FLT
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_U_FASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

__declspec(naked) void* __fastcall bridgeFastcallRtnDbl_ptbl() {
    BRK_BRIDGE_A
    BRK_BRIDGE_B_FASTCALL
    BRK_BRIDGE_C
    BRK_BRIDGE_D_FASTCALL
    BRK_BRIDGE_E_INT64DBL
    BRK_BRIDGE_F
    BRK_BRIDGE_G_FASTCALL
    BRK_BRIDGE_H
    BRK_BRIDGE_I_FASTCALL
    BRK_BRIDGE_J
    BRK_BRIDGE_K_INT64DBL
    BRK_BRIDGE_L
    BRK_BRIDGE_M_INT64DBL
    BRK_BRIDGE_N
    BRK_BRIDGE_O_INT64DBL
    BRK_BRIDGE_P
    BRK_BRIDGE_Q_INT64DBL
    BRK_BRIDGE_R
    BRK_BRIDGE_S_DBL
    BRK_BRIDGE_T_STDCALLFASTCALL
    BRK_BRIDGE_U_FASTCALL
    BRK_BRIDGE_V
    BRK_BRIDGE_W_STDCALLFASTCALL
    BRK_BRIDGE_X
}

#pragma endregion

//  ---------------------------
//  |     BRIDGE CREATION     |
//  ---------------------------
//
// Bridge creation functions make a new copy of the correct bridge
// type for the given target function. They pack all of the data
// necessary for the bridge to operate into a struct and patch the
// address of that struct into the newly copied bridge. When the
// bridge is called, it does everything necessary to call the
// remote function and return its return value.
//
// Parameters:
//   rmt_handle     = a handle to the remote process
//   target_func    = the function to be called in remote process (or
//                    local process if reverse_bridge is true)
//   func_type      = one of severals constants that specifies the
//                    calling convention of the function to be called
//   arg_info       = information, encoded into an int, necessary to
//                    read the arguments passed to the bridge...
//                    use BRIDGE_ARGS(...) macro to generate arg_info
//                    ex:
//                        BRIDGE_ARGS(void*, int, int, double)
//   reverse_bridge = bool to specify whether or not the bridge is
//                    from a remote target_func into the local
//                    process (false), or if the bridge is from a
//                    local target function to remote process (true)
//                    it also passes a handle of the remote process
//                    as the first arg to the local target_func if
//                    set to true
//
// Some bridge quirks and limitations to be aware of:
//   - Just to be super clear: reverse bridges create a bridge
//     function in a remote process that will call a local target
//     function when the remote bridge function is called.
//   - When creating a reverse bridge, the receiving end of the
//     bridge (the local func) will receive a HANDLE as its first
//     arg. Do not include this handle in the BRIDGE_ARGS list of arg
//     types.
// 
//   The following info is probably irrelevant but should be noted...
//   - Maximum number of bytes that the args can take up is 1020.
//     That means if all args are 4 bytes wide, then the maximum
//     number of args a bridge can handle is 255. If you are in a
//     situation where you are hitting this limit, you have my
//     thoughts and prayers.
//     - In a reverse bridge the max number of arg "slots" is 254,
//       because one is reserved for the handle.

void* Bridges::_createBridge(HANDLE rmt_handle, void* target_func, int func_type, int arg_info, bool reverse_bridge) {
    // Select correct bridge and probe functions based on the given func_type
    void* local_bridge_func = bridgeCdecl_ptbl;
    void* local_probe_func = probeCdecl_ptbl;
    switch (func_type) {
    case TFUNC_CDECL:
        local_bridge_func = bridgeCdecl_ptbl;
        local_probe_func = probeCdecl_ptbl;
        break;
    case TFUNC_CDECL_RTN64:
        local_bridge_func = bridgeCdeclRtn64_ptbl;
        local_probe_func = probeCdeclRtn64_ptbl;
        break;
    case TFUNC_CDECL_RTNFLT:
        local_bridge_func = bridgeCdeclRtnFlt_ptbl;
        local_probe_func = probeCdeclRtnFlt_ptbl;
        break;
    case TFUNC_CDECL_RTNDBL:
        local_bridge_func = bridgeCdeclRtnDbl_ptbl;
        local_probe_func = probeCdeclRtnDbl_ptbl;
        break;
    case TFUNC_STDCALL:
        local_bridge_func = bridgeStdcall_ptbl;
        local_probe_func = probeStdcall_ptbl;
        break;
    case TFUNC_STDCALL_RTN64:
        local_bridge_func = bridgeStdcallRtn64_ptbl;
        local_probe_func = probeStdcallRtn64_ptbl;
        break;
    case TFUNC_STDCALL_RTNFLT:
        local_bridge_func = bridgeStdcallRtnFlt_ptbl;
        local_probe_func = probeStdcallRtnFlt_ptbl;
        break;
    case TFUNC_STDCALL_RTNDBL:
        local_bridge_func = bridgeStdcallRtnDbl_ptbl;
        local_probe_func = probeStdcallRtnDbl_ptbl;
        break;
    case TFUNC_FASTCALL:
        local_bridge_func = bridgeFastcall_ptbl;
        local_probe_func = probeFastcall_ptbl;
        break;
    case TFUNC_FASTCALL_RTN64:
        local_bridge_func = bridgeFastcallRtn64_ptbl;
        local_probe_func = probeFastcallRtn64_ptbl;
        break;
    case TFUNC_FASTCALL_RTNFLT:
        local_bridge_func = bridgeFastcallRtnFlt_ptbl;
        local_probe_func = probeFastcallRtnFlt_ptbl;
        break;
    case TFUNC_FASTCALL_RTNDBL:
        local_bridge_func = bridgeFastcallRtnDbl_ptbl;
        local_probe_func = probeFastcallRtnDbl_ptbl;
        break;
    default:
        return 0; // Unsupported func_type disculpa por favor
        break;
    }

    // Handles that are rmt/local relative to whichever process the portable bridge function is in
    HANDLE new_rmt_handle = 0;
    HANDLE new_local_handle = 0;

    if (reverse_bridge) {
        DuplicateHandle(GetCurrentProcess(), rmt_handle, GetCurrentProcess(), &new_local_handle, PROCESS_ALL_ACCESS, FALSE, 0); // Duplicate the rmt handle to be used by local
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), rmt_handle, &new_rmt_handle, PROCESS_ALL_ACCESS, FALSE, 0); // Duplicate the local handle to be used by rmt
    } else {
        DuplicateHandle(GetCurrentProcess(), rmt_handle, GetCurrentProcess(), &new_rmt_handle, PROCESS_ALL_ACCESS, FALSE, 0); // Duplicate the rmt handle to be used by local
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), rmt_handle, &new_local_handle, PROCESS_ALL_ACCESS, FALSE, 0); // Duplicate the local handle to be used by rmt
    }

    // Address of probe function to use. Can be in either local or remote process depending on whether or not it is a reversed bridge
    // If func_size is zero, it is a signal to the bridge that there is no local probe func to copy, it is already in rmt (relative to the bridge func)
    void* probe_func = local_probe_func;
    int local_probe_func_size = 0;

    // Bridge is not reversed, so the probe needs to be copied, so supply the size of the probe
    if (!reverse_bridge)
        local_probe_func_size = Memory::Local::calcFuncSize(local_probe_func);

    // Get handle to kernel32 TODO more error checking like this in other places
    HMODULE kernel32_handle = GetModuleHandle("kernel32.dll");
    if (kernel32_handle == NULL) {
        return 0;
    }

    // BridgeData struct to be patched into the copied bridge function
    BridgeData* local_bridge_data = new BridgeData{
        new_local_handle,
        new_rmt_handle,

        probe_func,
        local_probe_func_size,

        target_func,
        arg_info,
        reverse_bridge,

        reinterpret_cast<VirtualAlloc_t>(GetProcAddress(kernel32_handle, "VirtualAlloc")),
        reinterpret_cast<VirtualAllocEx_t>(GetProcAddress(kernel32_handle, "VirtualAllocEx")),
        reinterpret_cast<VirtualFree_t>(GetProcAddress(kernel32_handle, "VirtualFree")),
        reinterpret_cast<VirtualFreeEx_t>(GetProcAddress(kernel32_handle, "VirtualFreeEx")),
        reinterpret_cast<ReadProcessMemory_t>(GetProcAddress(kernel32_handle, "ReadProcessMemory")),
        reinterpret_cast<WriteProcessMemory_t>(GetProcAddress(kernel32_handle, "WriteProcessMemory")),
        reinterpret_cast<CreateRemoteThread_t>(GetProcAddress(kernel32_handle, "CreateRemoteThread")),
        reinterpret_cast<WaitForSingleObject_t>(GetProcAddress(kernel32_handle, "WaitForSingleObject"))
    };

    // Length of the bridge function and copy of the bridge function in either local or rmt process depending on if the bridge is reversed
    size_t bridge_func_len = Memory::Local::calcFuncSize(local_bridge_func);
    void* bridge_func_copy = 0;

    // If the bridge is reversed, then copy the bridge into the remote process. Otherwise, duplicate the bridge locally.
    // Also patch the address of the BridgeData struct into the duplicated bridge function.
    if (reverse_bridge) {
        // Copy bridge and BridgeData struct to remote process
        bridge_func_copy = Memory::Remote::allocWriteCode(rmt_handle, local_bridge_func, bridge_func_len);
        void* rmt_bridge_data = Memory::Remote::allocWriteData(rmt_handle, local_bridge_data, sizeof(BridgeData));

        // Patch address of remote BridgeData struct into remote bridge
        for (size_t i = 0; i < bridge_func_len; i++) {
            if (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(local_bridge_func) + i) == 0xBAADB00F) {
                WriteProcessMemory(rmt_handle, reinterpret_cast<char*>(bridge_func_copy) + i, &rmt_bridge_data, sizeof(void*), 0);
            }
        }

        // Deallocate local bridge data struct as it is no longer needed
        delete local_bridge_data;
    } else {
        // Duplicate the local bridge func
        bridge_func_copy = Memory::Local::duplicateFunc(local_bridge_func);

        // Patch address of local BridgeData struct into duplicate of local bridge func
        for (size_t i = 0; i < bridge_func_len; i++) {
            if (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(bridge_func_copy) + i) == 0xBAADB00F) {
                *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(bridge_func_copy) + i) = reinterpret_cast<uint32_t>(local_bridge_data);
            }
        }
    }

    // Return new copy of bridge
    return bridge_func_copy;
}

// TODO: improve how users specify return type
// TODO: run some benchmarking comparing bridges to locally calling functions... I'm curious if there is a significant speed difference
// TODO: make a function that lets you call remote functions without building a bridge, just like a small little callRmt() or something
// TODO: make a way to destroy bridges that cleans up everything in both processes (dont forget to close the handles!)