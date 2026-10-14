#!/usr/bin/env python3
"""Enforce OpenASP's parameter-count based C signature layout."""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys
from collections.abc import Iterable


SOURCE_SUFFIXES = {".c", ".h"}
SKIPPED_DIRECTORIES = {".git", ".dbg", ".run", "build", "release"}
CONTROL_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "_Alignof"}
NON_TYPE_KEYWORDS = {"break", "case", "continue", "else", "goto", "return"}
DECLARATION_PREFIX_KEYWORDS = {
    "auto",
    "const",
    "enum",
    "extern",
    "inline",
    "register",
    "restrict",
    "signed",
    "static",
    "struct",
    "typedef",
    "union",
    "unsigned",
    "void",
    "volatile",
    "_Atomic",
    "_Noreturn",
}
TOKEN_PATTERN = re.compile(
    r"""
    (?P<space>\s+)
  | (?P<line_comment>//[^\n]*)
  | (?P<block_comment>/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\])*")
  | (?P<char>'(?:\\.|[^'\\])*')
  | (?P<identifier>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<number>(?:0[xX][0-9A-Fa-f]+|(?:\d+(?:\.\d*)?|\.\d+)(?:[Ee][+-]?\d+)?)[A-Za-z0-9_]*)
  | (?P<punct>\.\.\.|->|<<=|>>=|==|!=|<=|>=|&&|\|\||\+\+|--|<<|>>|[{}()\[\];,=*:.?+\-/%&|^!~<>])
  | (?P<other>.)
    """,
    re.VERBOSE | re.DOTALL,
)


@dataclasses.dataclass(frozen=True)
class Token:
    text: str
    start: int
    end: int
    line: int
    kind: str


@dataclasses.dataclass(frozen=True)
class Signature:
    opening: int
    closing: int
    commas: tuple[int, ...]
    parameter_count: int
    line: int


def source_files(paths: list[str]) -> Iterable[pathlib.Path]:
    seen: set[pathlib.Path] = set()
    for raw_path in paths:
        path = pathlib.Path(raw_path)
        candidates = path.rglob("*") if path.is_dir() else (path,)
        for candidate in candidates:
            if candidate.suffix not in SOURCE_SUFFIXES or not candidate.is_file():
                continue
            if any(part in SKIPPED_DIRECTORIES for part in candidate.parts):
                continue
            resolved = candidate.resolve()
            if resolved not in seen:
                seen.add(resolved)
                yield candidate


def preprocessor_ranges(text: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    offset = 0
    lines = text.splitlines(keepends=True)
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.lstrip().startswith("#"):
            start = offset
            while line.rstrip("\r\n").endswith("\\") and index + 1 < len(lines):
                offset += len(line)
                index += 1
                line = lines[index]
            ranges.append((start, offset + len(line)))
        offset += len(line)
        index += 1
    return ranges


def tokenize(text: str) -> list[Token]:
    ranges = iter(preprocessor_ranges(text))
    current_range = next(ranges, None)
    tokens: list[Token] = []
    line = 1
    for match in TOKEN_PATTERN.finditer(text):
        start, end = match.span()
        while current_range is not None and start >= current_range[1]:
            current_range = next(ranges, None)
        kind = match.lastgroup or "other"
        token_text = match.group()
        in_preprocessor = current_range is not None and start >= current_range[0]
        if kind != "space" and not kind.endswith("comment") and not in_preprocessor:
            tokens.append(Token(token_text, start, end, line, kind))
        line += token_text.count("\n")
    return tokens


def matching_parentheses(tokens: list[Token]) -> tuple[dict[int, int], dict[int, int]]:
    openings: list[int] = []
    forward: dict[int, int] = {}
    backward: dict[int, int] = {}
    for index, token in enumerate(tokens):
        if token.text == "(":
            openings.append(index)
        elif token.text == ")" and openings:
            opening = openings.pop()
            forward[opening] = index
            backward[index] = opening
    return forward, backward


def statement_prefix(tokens: list[Token], name_index: int) -> list[Token]:
    index = name_index - 1
    square_depth = 0
    parenthesis_depth = 0
    while index >= 0:
        text = tokens[index].text
        if text == "]":
            square_depth += 1
        elif text == "[":
            square_depth -= 1
        elif text == ")":
            parenthesis_depth += 1
        elif text == "(":
            parenthesis_depth -= 1
        elif square_depth == 0 and parenthesis_depth == 0 and text in {";", "{", "}"}:
            break
        index -= 1
    return tokens[index + 1 : name_index]


def is_function_pointer_name(tokens: list[Token], close_index: int, backward: dict[int, int]) -> bool:
    opening = backward.get(close_index)
    if opening is None:
        return False
    inner = tokens[opening + 1 : close_index]
    if not inner or not any(token.text == "*" for token in inner):
        return False
    return any(token.kind == "identifier" and token.text not in DECLARATION_PREFIX_KEYWORDS for token in inner)


def is_declaration_prefix(prefix: list[Token]) -> bool:
    if not prefix:
        return False
    if any(token.kind != "identifier" and token.text != "*" for token in prefix):
        return False
    identifiers = [token.text for token in prefix if token.kind == "identifier"]
    if not identifiers or any(identifier in NON_TYPE_KEYWORDS for identifier in identifiers):
        return False
    return True


def direct_commas(tokens: list[Token], opening: int, closing: int) -> tuple[int, ...]:
    commas: list[int] = []
    parenthesis_depth = 0
    square_depth = 0
    brace_depth = 0
    for index in range(opening + 1, closing):
        text = tokens[index].text
        if text == "(":
            parenthesis_depth += 1
        elif text == ")":
            parenthesis_depth -= 1
        elif text == "[":
            square_depth += 1
        elif text == "]":
            square_depth -= 1
        elif text == "{":
            brace_depth += 1
        elif text == "}":
            brace_depth -= 1
        elif text == "," and parenthesis_depth == 0 and square_depth == 0 and brace_depth == 0:
            commas.append(index)
    return tuple(commas)


def find_signatures(text: str) -> tuple[list[Token], list[Signature]]:
    tokens = tokenize(text)
    forward, backward = matching_parentheses(tokens)
    signatures: list[Signature] = []
    for opening, closing in forward.items():
        wrapped_prototype = (
            opening >= 2
            and closing + 2 < len(tokens)
            and tokens[opening - 1].text == "("
            and tokens[opening - 2].text == "__P"
            and tokens[closing + 1].text == ")"
            and tokens[closing + 2].text == ";"
        )
        if not wrapped_prototype and (closing + 1 >= len(tokens) or tokens[closing + 1].text not in {";", "{"}):
            continue
        if opening == 0:
            continue
        previous = tokens[opening - 1]
        if wrapped_prototype:
            pass
        elif previous.kind == "identifier":
            if previous.text in CONTROL_KEYWORDS:
                continue
            if previous.text == "__P":
                continue
            if not is_declaration_prefix(statement_prefix(tokens, opening - 1)):
                continue
        elif previous.text == ")":
            if not is_function_pointer_name(tokens, opening - 1, backward):
                continue
        else:
            continue
        commas = direct_commas(tokens, opening, closing)
        interior = text[tokens[opening].end : tokens[closing].start].strip()
        if "//" in interior or "/*" in interior:
            continue
        parameter_count = 0 if not interior or interior == "void" else len(commas) + 1
        signatures.append(Signature(opening, closing, commas, parameter_count, tokens[opening].line))
    return tokens, signatures


def replacement_for_signature(text: str, tokens: list[Token], signature: Signature) -> str:
    opening = tokens[signature.opening]
    closing = tokens[signature.closing]
    interior_start = opening.end
    interior_end = closing.start
    interior = text[interior_start:interior_end]
    if "\n" not in interior and signature.parameter_count <= 5:
        return interior

    relative_tokens = tokens[signature.opening + 1 : signature.closing]
    pieces: list[str] = []
    cursor = interior_start
    break_offset = tokens[signature.commas[4]].end if signature.parameter_count > 5 else -1
    for token in relative_tokens:
        whitespace = text[cursor:token.start]
        if "\n" in whitespace:
            whitespace = " "
        if cursor == break_offset:
            whitespace = "\n" + " " * (opening.start - text.rfind("\n", 0, opening.start))
        pieces.append(whitespace)
        pieces.append(text[token.start:token.end])
        cursor = token.end
    trailing = text[cursor:interior_end]
    if "\n" in trailing:
        trailing = " "
    pieces.append(trailing)
    return "".join(pieces)


def format_text(text: str) -> tuple[str, list[int]]:
    tokens, signatures = find_signatures(text)
    replacements: list[tuple[int, int, str, int]] = []
    for signature in signatures:
        opening = tokens[signature.opening]
        closing = tokens[signature.closing]
        old = text[opening.end : closing.start]
        new = replacement_for_signature(text, tokens, signature)
        if old != new:
            replacements.append((opening.end, closing.start, new, signature.line))
    formatted = text
    for start, end, replacement, _line in reversed(replacements):
        formatted = formatted[:start] + replacement + formatted[end:]
    return formatted, [line for _start, _end, _replacement, line in replacements]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="rewrite non-conforming files")
    parser.add_argument("paths", nargs="*", default=["."], help="files or directories to inspect")
    args = parser.parse_args()

    changed = 0
    for path in sorted(source_files(args.paths)):
        text = path.read_text(encoding="utf-8")
        formatted, lines = format_text(text)
        if not lines:
            continue
        changed += 1
        if args.write:
            path.write_text(formatted, encoding="utf-8")
        else:
            locations = ", ".join(str(line) for line in lines)
            print(f"{path}:{locations}: function parameter layout violation")
    if changed and not args.write:
        print(f"{changed} file(s) require formatting; run scripts/format_c_parameters.py --write", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
