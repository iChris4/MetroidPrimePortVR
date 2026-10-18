# Decompilation build and notes

Inherited from [PrimeDecomp](https://github.com/PrimeDecomp/prime): the
ninja build that compiles the matching decompilation under `src/` and
`include/` back into the original GameCube DOL and RELs, and notes on how that
code is worked on. None of it is needed to build, run, test or package the
port (CMake, CI and the packaging scripts never touch this directory); see
`docs/NATIVE_PORT.md` for that.

## Layout

- `configure.py`: generates `build.ninja` for one disc version.
- `tools/`: the dtk-template build helpers `configure.py` uses (`project.py`,
  `ninja_syntax.py`, `download_tool.py`, `decompctx.py`, `transform_dep.py`,
  `changes_fmt.py`) and `metaforce_renames.py`.
- `config/<version>/`: decomp-toolkit's `config.yml`, symbol maps
  (`symbols.txt`), splits and the expected output hashes (`build.sha1`).
- `orig/<version>/`: where the original disc files go (gitignored).
- `scripts/`: `rename_symbol.py` renames a symbol in a version's
  `symbols.txt`; `symbols.py` is the parser it uses.
- `sha1/`, `buildstrings/`: hashes and build strings from the older,
  pre-dtk build, kept for reference.
- `docs/`: `metaforce.md` (converting Metaforce code into matching C++98),
  `map.md` (using the demo build's debug link map), `nes-emulator.md` (the NES
  emulator REL modules).

## Building the DOL

Everything runs from the repository root, because the sources stay in `src/`
and `include/` there and the outputs land in `build/<version>/`:

```sh
# extract the disc's sys/main.dol and files/*.rel into decomp/orig/GM8E01_00/
python3 decomp/configure.py --version GM8E01_00
ninja
```

The first `ninja` run downloads decomp-toolkit, the Metrowerks compilers and
the other tools into `build/tools/` and `build/compilers/`. Pass `--help` to
`configure.py` for the other options.
