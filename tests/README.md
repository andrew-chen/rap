# tests/

Example `.rap` files for the doctest framework.  Each file is both a runnable
`.rap` program and a self-describing test: it embeds the expected output using
`;;; EXPECT` markers (triple-semicolon, distinct from the single/double-semicolon
comment convention used elsewhere).

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

## Running manually

```
echo "" | ./raprunner tests/test_hello.rap
echo "" | ./raprunner tests/test_multi.rap
echo "" | ./raprunner tests/test_with_args.rap hello
```

## Rel comparison limitation

The doctest runner (when implemented) compares output terms as S-expression
strings, not as structural terms.  Terms that print identically are considered
equal; terms involving anonymous `#<rel/N>` values cannot be meaningfully
compared and should not appear in `;;; EXPECT` blocks.  See
`docs/ROADMAP.md` "Future Work" for a note on structural `terms_equal`.

## Files

| File | Description |
|------|-------------|
| `test_hello.rap` | Single output term, no ARGS |
| `test_multi.rap` | Multiple output lines |
| `test_with_args.rap` | Uses `;;; ARGS: hello` to pass a CLI argument |
