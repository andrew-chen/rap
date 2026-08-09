# tests/

Example `.rap` files for the `rap_doctest` test runner.  Each file is both a
runnable `.rap` program and a self-describing test: it embeds the expected
output using `;;; EXPECT` markers (triple-semicolon, distinct from the
single/double-semicolon comment convention used elsewhere).

## Marker syntax

```
;;; ARGS: token1 token2 ...      (optional; whitespace-separated symbol tokens)
;;; EXPECT
;;; (expected output term 1)
;;; (expected output term 2)
;;; END EXPECT
```

- **`;;; ARGS:`** — space-separated symbol tokens to pass as the `args` list to
  `main`.  Omit the line entirely for programs that ignore `args`.
- **`;;; EXPECT` / `;;; END EXPECT`** — each line between these markers (with the
  leading `;;; ` stripped) is one expected output term, in order.
- Only one `EXPECT` block per file is supported.
- The markers use triple-semicolons (`;;;`) to avoid collision with the
  single-semicolon line comments and double-semicolon section comments used in
  `.rap` files by convention.

## Running the doctest suite

```bash
make raptests
```

Or manually, passing the files you want to check:

```bash
./rap_doctest tests/test_hello.rap tests/test_multi.rap tests/test_with_args.rap
```

Individual files can also still be run through `raprunner` to inspect their
live output (useful when developing a new test):

```bash
echo "" | ./raprunner tests/test_hello.rap
echo "" | ./raprunner tests/test_multi.rap
echo "" | ./raprunner tests/test_with_args.rap hello
```

## Stdlib loading (three-step fallback)

`rap_doctest` loads `stdlib/core.rap` before loading each test file so that
tests can use standard relations (`groundo`, `addo`, `mulo`, etc.).  The
runner uses a three-step fallback chain:

1. **Explicit override** — pass `--stdlib PATH` to specify a stdlib file.
   The runner loads that file (and only that file) as the stdlib:
   ```bash
   ./rap_doctest --stdlib /path/to/core.rap tests/test_hello.rap
   ```

2. **Default path** — if `--stdlib` is not given, the runner tries
   `stdlib/core.rap` relative to the current working directory.  This is the
   path used when you run from the project root (which is the normal case for
   `make raptests`).

3. **Visible warning and continue** — if neither step 1 nor step 2 succeeds,
   the runner prints a warning to stderr and continues without any stdlib
   loaded:
   ```
   rap_doctest: warning: could not load stdlib/core.rap (tried 'stdlib/core.rap');
   proceeding without it — tests using stdlib relations may fail
   ```
   This is never a fatal error — the run continues, but any test that uses
   stdlib relations will fail with an undefined-relation error.  If you see
   this warning, either run from the project root or pass `--stdlib`.

The three fixture files in this directory (`test_hello.rap`,
`test_multi.rap`, `test_with_args.rap`) use only engine built-ins (`==`,
`fresh`, `conj`, `cons-ops`, `no-ops`, `output`) and pass regardless of
whether stdlib is loaded.

## Rel comparison limitation

The runner compares output terms structurally (Sym values are compared by
their string content, not pointer identity, so cross-intern-table comparisons
are safe).  However, anonymous `#<rel/N>` values (`TermTag::Rel`) cannot be
meaningfully compared — if an expected or actual output term is a Rel, the
runner reports it as an automatic mismatch with a clear message.  Do not put
Rel values in `;;; EXPECT` blocks.  See `docs/ROADMAP.md` "Future Work" for
the `terms_equal` note on this limitation.

## Files

| File | Description |
|------|-------------|
| `test_hello.rap` | Single output term, no ARGS |
| `test_multi.rap` | Multiple output lines |
| `test_with_args.rap` | Uses `;;; ARGS: hello` to pass a CLI argument |
