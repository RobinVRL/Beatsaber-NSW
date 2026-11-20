# Unity JNI class-name register snapshots (read-only)

The 50-byte UnityPlayerForActivityOrService class uses Unity's guest heap
string, while the first 30-byte UnityPlayer name uses inline storage.
Eden has observed a NULL argument in FindClass, but which instruction first
loses the pointer is still unknown.

The standalone source/unity_guest_register_diagnostic.[ch] decodes evidence
supplied by an external guest CPU debugger. It never installs a breakpoint,
rewrites an instruction, directly dereferences guest memory, or logs data.

| Original libunity ELF-relative PC | Sample | Evidence |
| --- | --- | --- |
| 0x5C6D54 | After original allocator call | x0 = allocated buffer; x19 = growing string object |
| 0x9115E8 | After string append | SP = completed 40-byte object; heap pointer at [SP], size [SP+16], tag [SP+31] |
| 0x5D6E58 | Before CSEL x1,x19,x10,cc | x19 = object; x10 = heap pointer; w8 = tag; x1 still stale |
| 0x5D6E5C | Before FindClass call | x1 = actual class-name argument; compare with [x19] |

Provide the original AArch64 x0..x30, SP, and ELF-relative PC. The checked
read_guest callback must use the debugger's safe guest-memory accessor, and
return false when an address is inaccessible. The helper accepts only these
four PCs. Each capture performs at most TWO reads: 40 bytes for the object
and 51 bytes for the class. The class read occurs only for a non-NULL pointer
with valid long-string metadata of exactly 50 characters.

At 0x5C6D54, a returned x0=0 means allocation failed; original Unity code
would dereference that allocation at 0x5C6D64, before FindClass. A valid
object after 0x9115E8 but a NULL [x19] at 0x5D6E58 suggests an overwrite.
A correct x10 before CSEL but NULL x1 immediately after CSEL indicates an
instruction/register/flags issue rather than allocator failure.

The existing hook_arm64() changes 16 bytes of guest instructions and cannot
be used with the no-code-change constraint. The JNI FindClass shim already
logs its NULL name argument and return address, but cannot recover earlier
allocator registers or the original string object. so_dump_maps supplies
module mappings but no live guest CPU registers. No arbitrary-PC,
non-mutating register capture hook exists in the current loader.

Prime should supply snapshots through Eden's debugger/CPU trace if such an
interface is available. Prefer instruction trace or hardware breakpoints
where supported; software breakpoints may modify guest instruction memory.
No assumption is made that the installed Eden build exposes these features.
