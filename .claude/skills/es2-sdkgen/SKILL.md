---
name: es2-sdkgen
description: Regenerate the Everspace 2 mod's SDK headers (function RVAs and struct offsets) from the game PDB — after adding a symbol to the spec, or after a game update changes addresses. Triggers: "regenerate the sdk", "add a symbol/offset to the mod", "game updated fix addresses", "rvas.h", "offsets.h".
---

# Regenerate SDK headers (sdk/gen/*.h)

`tools/gen_sdk.py` turns named symbols/offsets into `sdk/gen/rvas.h` (function & global RVAs) and `sdk/gen/offsets.h` (class member offsets + bitfield masks) that the mod compiles against.

## Add a symbol/offset
Edit `tools/gen_sdk.py`:
- `RVAS[name] = '<exact demangled signature>'` (or `'regex:<pattern>'` matching exactly one). Copy the signature from `sdk/symbols.tsv` (see `es2-symbols`).
- `OFFSETS[ClassName] = [member, ...]` for field offsets; `BITFIELDS[ClassName] = [boolMember, ...]` for `bool:1` bitfields (emits `<m>_off` + `<m>_mask`).

## Generate
`python3 tools/gen_sdk.py sdk/symbols.tsv sdk/raw/types.txt sdk/gen`
- Exits non-zero and prints `ERROR:` for any RVA that matched 0 or >1 symbols — fix the signature until each is unique.
- Warns (does not fail) for missing offset members and for ICF-folded RVAs (`!!! ICF-FOLDED` — do **not** hook those).
Then rebuild the mod (`es2-build`).

## After a game update
Addresses change, so regenerate everything:
1. `PDB="$HOME/es2game/ES2/Binaries/Win64/ES2-Win64-Shipping.pdb"`
2. `llvm-pdbutil dump --publics "$PDB" > sdk/raw/publics.txt; llvm-pdbutil dump --section-headers "$PDB" > sdk/raw/sections.txt; python3 tools/pdb_publics.py sdk/raw/sections.txt sdk/raw/publics.txt sdk/symbols.tsv`
3. `llvm-pdbutil dump --types "$PDB" > sdk/raw/types.txt; python3 tools/pdb_types.py index sdk/raw/types.txt`
4. `python3 tools/gen_sdk.py sdk/symbols.tsv sdk/raw/types.txt sdk/gen`
5. Update the PE-timestamp guard: it's read from the exe automatically by the mod; the value in `rvas.h` (`PE_TIMESTAMP`) is regenerated — if the game changed, bump the constant in `gen_sdk.py` (`PE_TIMESTAMP = ...`) to the new `TimeDateStamp`.
6. Rebuild (`es2-build`), retest (`es2-launch`).
