---
name: es2-symbols
description: Look up Everspace 2 engine/game symbols, class memory layouts, enums, vtable slots, and disassembly from the game's full PDB. Use when you need a function's RVA, a struct field offset, an enum's values, a virtual's vtable index, or to read what a function does. Triggers: "find the RVA of", "what's the offset of", "class layout", "enum values", "vtable slot", "disassemble".
---

# Query the ES2 PDB / symbols

The game ships a **full 1.8 GB PDB**; everything is recoverable. Pre-built indexes live in `sdk/`.

## Symbols (functions + globals, with RVAs)
`sdk/symbols.tsv` = `rva<TAB>kind<TAB>mangled<TAB>demangled` (1.1M rows).
- RVA of a function: `grep -F '<demangled signature>' sdk/symbols.tsv | cut -f1,4`
- e.g. `grep 'UWorld::Listen(' sdk/symbols.tsv | cut -f1,4`
- **Check uniqueness before hooking**: an RVA shared by >1 symbol is ICF-folded — never hook it (`grep -c '^<RVA>\t' sdk/symbols.tsv`). `tools/gen_sdk.py` flags these automatically.
- Game-only function list: `sdk/es2_functions.txt`; classes ranked by size: `sdk/es2_classes_by_funcs.txt`; all UClasses: `sdk/uclasses.txt`.

## Types (layouts / enums / vtables) — via the TPI dump (needs `sdk/raw/types.txt`)
- Class layout with offsets: `python3 tools/pdb_types.py layout sdk/raw/types.txt <ClassName>`
- Enum values: `python3 tools/pdb_types.py enum sdk/raw/types.txt <EnumName>`
- Virtual functions + vtable index: `python3 tools/pdb_types.py vtable sdk/raw/types.txt <ClassName>`
- Find a type by regex: `python3 tools/pdb_types.py find sdk/raw/types.txt <regex>`

## Disassembly (annotated with symbol names)
`python3 tools/disasm.py <name-substring|0xRVA> [maxbytes]` — call/jump targets are annotated with their symbols.

## To make a symbol usable by the mod
Add it to `tools/gen_sdk.py` (RVAS for functions/globals, OFFSETS/BITFIELDS for fields) then run `es2-sdkgen`.

## Rebuild the raw dumps (if `sdk/raw/*` is missing)
`PDB="$HOME/es2game/ES2/Binaries/Win64/ES2-Win64-Shipping.pdb"`
- publics: `llvm-pdbutil dump --publics "$PDB"` → `tools/pdb_publics.py` → `sdk/symbols.tsv`
- types: `llvm-pdbutil dump --types "$PDB" > sdk/raw/types.txt` then `python3 tools/pdb_types.py index sdk/raw/types.txt`
