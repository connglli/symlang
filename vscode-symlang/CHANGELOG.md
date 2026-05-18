# Changelog

All notable changes to the "symlang-syntax" extension will be documented in this file.

## [0.2.0] - 2026-05-18

- Add v0.2.0 pointer keywords: `addr`, `load`, `store` highlighted as memory operators.
- Add `ptr` type keyword (same color family as `i32`, `f64`).
- Add `null` constant (same scope as `undef` — `constant.language`).
- Move `undef` from `keyword.other` to `constant.language` for correct theming.
- Fix `language-configuration.json`: bracket pairs were malformed (single strings
  instead of two-element arrays) and had missing commas — this prevented the
  extension from activating entirely.

## [0.1.0] - 2026-01-31

- Initial release of SymLang syntax highlighting.
- Basic support for keywords, types, identifiers, and literals.
