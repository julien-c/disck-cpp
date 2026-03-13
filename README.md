# hf-cache-dirs

Inspect your local Hugging Face cache (`~/.cache/huggingface/hub`).

Lists all cached repos (models, datasets, spaces) with their refs, snapshots, and files. **Folder names are clickable hyperlinks to the corresponding Hugging Face page.**

## Sample output

<p align="center"><img src="sample.svg" alt="sample output"/></p>

## Build

```bash
make
```

Requires a C++17 compiler.

## Usage

```bash
# List cached repos
./hf-cache-dirs

# Create a "hub" symlink in the current directory
./hf-cache-dirs --link

# Remove it
./hf-cache-dirs --unlink
```

## Format

```bash
make format
```

Uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) with a config based on llama.cpp's style.
