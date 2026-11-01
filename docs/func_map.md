# Function map: what still touches emulation, function by function

`tools/func_map/func_map.py` draws every EE function of a port's game ELF as one square, in address order, coloured by
how far it is from running without emulation:

| State | Meaning |
| --- | --- |
| Native C++ | An override the port registers by default runs instead of the game's code, or a VU program entry the port runs as C++ |
| Recompiled, clean | Generated C++ from the MIPS; nothing found that touches emulated hardware, in the function or one or two calls down |
| Touches emulation (direct) | The function reads or writes emulated hardware itself: the port's to-do list |
| Calls emulation (indirect) | Calls, one or two levels down, a function that touches emulation or an emulation-backed stub |
| Runtime stub / HLE | A library function the runtime answers by name, or a kernel syscall wrapper, split into native and emulation-backed |

The recompiled IRX modules get a smaller map of their own, a row of badges gives the runtime-level emulation the
port's config describes (the GS front end, the SPU2, the IOP kernel), and each run adds a point to a trend line. With a
coverage file from a game session (`PS2X_COVERAGE`, below) the page also says which functions ran, and an "Only what
ran" switch dims the rest.

It is static analysis of the MIPS plus the port's override sources and PS2Recomp's stub sources, and it is wrong in
known ways: the page lists its rules and blind spots, and the main ones are at the end of this file. Read the counts as
a trend and a list of places to look, not as a measurement of how native a port is.

Python 3.8 or newer with the standard library only, on Linux and Windows (a bundled embeddable Python runs it).

## Running it

```sh
python3 PS2Recomp/tools/func_map/func_map.py --config recomp/func_map.json            # writes the config's out
python3 PS2Recomp/tools/func_map/func_map.py --config recomp/func_map.json --check    # what the config resolves to
python3 PS2Recomp/tools/func_map/func_map.py --config recomp/func_map.json --coverage build/coverage.txt --out <dir>
```

`--elf`, `--generated` and `--ps2recomp` override the config's paths for one run; `--history` keeps the trend file
somewhere else than `<out>/history.json`. The run prints the counts:

```text
12910 EE functions: native 19, clean 11563, direct 122 (12 uncertain), indirect 934, stubs 272 (44 emulation-backed); ...
```

It writes into `--out`:

| File | Content | Publish? |
| --- | --- | --- |
| `map.html` | One self-contained interactive page: the maps, search by address or name, the details of each function, badges, trend, rules | No: names game addresses |
| `map.json` | Every function with its state, reason, calls and region | No: names game addresses |
| `history.json` | One point per run (port and PS2Recomp commits, counts) | No: kept with the outputs |
| `map.svg` | The squares and the counts only: no address, no name | Yes, for a README |

Everything but the SVG is derived from the game: keep it out of the port's repository (put `--out` in the build
tree). The SVG holds coloured squares and totals, so a port can commit it to show its progress.

## The port's config

One JSON file in the port's repository (JSON rather than TOML so that Python before 3.11 reads it). Relative paths are
taken from the config file's folder; `~` and `${VAR}` expand; a path given as a list is the first candidate that
exists. A path that starts with `ps2recomp:` (in a badge or VU condition) is inside the PS2Recomp checkout.

| Key | What | Default |
| --- | --- | --- |
| `title`, `game_id`, `short_name` | Shown on the page and in the trend | `game_id`, then "port" |
| `elf` | The game ELF (disc files: never in the repository) | required |
| `recomp_config` | The ps2_recomp TOML: `stubs` and `entry_points` give names, `[mmio]` is a second source of hardware accesses | none |
| `generated` | The generated tree: the function list and boundaries, stub bindings, `iop/` for the IRX map | none |
| `function_list_csv` | `name,address,...` CSV (Ghidra export) used without a generated tree; ends become approximations | none |
| `ps2recomp` | The PS2Recomp checkout whose `ps2xRuntime` answers the stubs and syscalls | the checkout holding the tool |
| `ps2recomp_submodule` | Submodule path in the port repository, to record its pinned commit | `PS2Recomp` |
| `port_root` | The port repository, for its commit | git toplevel of the config's folder |
| `out` | Output folder | `./funcmap` |
| `runtime_sources` | Globs of the port's runtime sources to scan for overrides and their constants | none |
| `override_patterns` | Regexes with named groups `target` and `handler`, one match per registration line | `registerFunction` and `replaceFunction` |
| `wrapper_patterns` | Regexes that mark a handler body as calling the original back | `g_name(rdram, ctx, runtime)`, `s.name(rdram, ctx, runtime)`; a call of the generated `..._0x<addr>` always counts |
| `vu_programs` | `lift_spec` (a `{"programs": {key: {unit, entries, pairs, file}}}` file), `lift_how`, and `programs`: `[{program, unit, entries, how, pairs, require}]` | none |
| `irx.lists_dir` | Folder of `iop_<MODULE>_list.txt` (`tools/iop_recomp.py --list`) | none |
| `libraries` | `[[label, regex]]` tried before the built-in SDK library name patterns, to group named functions into regions | none |
| `game_code_end` | Address from which a library run ends the "game code" region | the first library run |
| `unnamed_library_label` | Region name of the unnamed code after the game's | `unnamed library code` |
| `runtime_badges` | `[{name, rules: [{if: [conditions], status, evidence}]}]`, first rule that holds wins; status `present`, `partial`, `diagnostic` or `absent` | none |
| `blind_spots` | Port-specific lines added to the page's blind spots | none |

A condition is `{"file": path, "match": regex}` or `{"file": path, "exists": true}`, with `"not": true` to negate it.
Badges are the port's statement of what its defaults still run as a model of the PS2; the tool only checks the files
the rules name, so keep the rules pointing at the code that switches each model on or off.

Override registrations: the tool reads each `runtime_sources` file, keeps the `constexpr uint32_t kName = 0x...`
constants (also several on one line) and `constexpr uint32_t kName[] = {...}` arrays, and for every line a pattern
matches resolves the target: a hex literal, a constant, or the variable of a `for (const uint32_t address : kArray)`
loop. A registration inside an `if (std::getenv("..."))` block is a diagnostic and is skipped; one inside
`if (const char *v = std::getenv("X"); !v || ...)` (on unless switched off) is kept. Targets computed at run time
(`hook.address`, `table[k]`, lambdas looked up by name) are counted as unresolved: `--check` lists them, so a port can
see what the map misses.

Two worked examples: `tools/func_map/examples/sc3.json` (Soulcalibur III: a VU lift spec plus one hand-written VU
program, six badges) and `tools/func_map/examples/tmb.json` (Twisted Metal: Black: overrides spread over several
files, badges for the GS, SPU2, IOP and frame pacing). Both are written for the port repository's `recomp/` folder.

## Coverage: what ran

`PS2X_COVERAGE=<file>` makes the runtime record every guest PC its dispatcher looks up
(`PS2Runtime::lookupFunction`: calls, jumps, returns into a caller, interrupt, alarm and callback entries), one bit
per word of EE RAM (1 MiB), and write the list of recorded PCs to `<file>`:

- every `PS2X_COVERAGE_INTERVAL` seconds (default 10; 0 writes only at the end), from a writer thread, by writing
  `<file>.partial` and renaming it over `<file>`, so a run killed at any moment leaves the last complete list;
- when `PS2Runtime::run` returns (the window closed) and at process exit.

```text
# ps2x coverage v1
# 8123 guest PCs dispatched
0x00100008
0x0010012c
...
```

Off, it costs one relaxed atomic load and a branch per dispatch. On, a load and a test of the PC's bit, and an atomic
OR the first time a PC is seen. `ps2_coverage::start(path, interval)`, `write()`, `stop()` and `ran(pc)`
(`ps2xRuntime/include/ps2_coverage.h`) do the same from code, for a test or a port's own switch.

`func_map.py --coverage <file>` marks a function as ran when the file holds a PC inside its range (a return into a
caller records a PC in the middle of it), prints the counts of what ran by state, and the page gets the "Only what
ran" switch and the counts under the headline. Any text file with one hex address per line works as well.

## Blind spots

- Indirect calls are not followed: JALR, jump tables, function pointers, callbacks and interrupt handlers. A function
  only reached that way can look clean while it is called by emulation-backed code, and a function calling through a
  pointer into emulated hardware is not marked indirect.
- The constant tracking is a linear pass without control flow or memory: a hardware address loaded from a table or a
  global is missed, and a register reused on two paths can be wrong.
- Packet builders that only fill buffers look clean: the emulated DMA, VIF or GS reads those buffers later, so the
  data still feeds the emulation. GIF and DMA tags (64- and 128-bit values) are not recognised; only some VIF codes
  are, and only as 32-bit constants (shown as uncertain).
- Stub and override classification is lexical: a handler reaching emulation through a deeper helper, an override
  behind a condition other than an environment variable, or an overlay module (`replaceModuleFunction`) is misjudged
  or absent.
- Native C++ means the game's code no longer runs there, not that the replacement is emulation-free.
- Runtime-level emulation is not tied to functions: it is in the badges, which are only as true as the port's rules.
- Coverage only sees the dispatcher: a recompiled function a native override calls directly as C++ is not recorded,
  and overlay modules sharing an address are attributed to whatever the main ELF has there.
