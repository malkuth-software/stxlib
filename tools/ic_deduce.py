#!/usr/bin/env python3
"""Deductions over the behavioural constraint declarations (IC4): discharge proof obligations where they pay.

The declarations (docs/ic_declarations.md in template_harness) state each
call's clauses; tools/ic_declarations.py turns them into checks, blocks and
open proof obligations. This tool reads the same declarations and the
sources they name, and discharges what deduction can:

- predicate-exact: a generated predicate computes its clause exactly: no
  subtraction below zero, no sum or product past its type, in C++'s order of
  evaluation (a conjunct is evaluated only when those before it hold).
  Linear arithmetic over the rationals (Fourier-Motzkin elimination), which
  refutes an integer case soundly.
- entry-check: every call of the generated predicate in the files the
  declaration names guards the declared refusal: STXLIB_REQUIRE with the
  generated message, or `if (!admits_...)` followed by a return that names
  the declared kind or code, a null or the declared value.
- refusal-leaves: the guard comes before anything that could change the
  object: only const locals, other guards and their refusals precede it in
  its function.
- stated: an obligation with `given` and `shows` (an algebraic lemma), proved
  from its givens with the counter algebra of the allocator study's sxv:
  counters as finite sums (a sum updated at one word is the sum less the old
  word's part plus the new), popcount by its algebra (pop(a | b) =
  pop(a) + pop(b) for disjoint a and b; pop(x & (x - 1)) = pop(x) - 1 for
  x != 0; pop of one bit is 1; pop(2^r - 1) = r; 0 <= pop <= 64). Its
  `sources` name the code lines each step reads; the deduction records their
  hashes. The method and the laws are sxv's (stxlib_notes, planning/
  stx_minmallocpp/SXV_EVALUATION.md, 2026-11-18); sxv itself stays
  study-only (owner, IC decision 6), so this is a re-implementation, not a
  use of it.

Each deduction names its premises (what it assumes rather than proves) and
the hashes of the declaration and sources it read; tools/ic_declarations.py
merges a deduction into the obligation only while those hashes match, so a
changed source reopens it. What is not discharged stays open, with why.

Usage:
  python3 -B tools/ic_deduce.py generate --config tools/ic/project.toml [--root DIR]
  python3 -B tools/ic_deduce.py check    --config tools/ic/project.toml [--root DIR]

Exit status: 0 current; 1 STALE (the deductions file differs); 2 an error.
Standard library only; Python 3.11 or later.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import re
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ic_declarations as ic  # noqa: E402 (the declarations' own reader)

TOOL: str = 'tools/ic_deduce.py'
CREDIT: str = ('the counter and popcount algebra is the allocator study\'s sxv (stxlib_notes, planning/'
               'stx_minmallocpp/SXV_EVALUATION.md, 2026-11-18), re-implemented here; sxv stays study-only '
               '(owner, IC decision 6)')
WORD_BITS: int = 64
TYPE_LARGEST: dict[str, int] = {'u8': 2 ** 8 - 1, 'u16': 2 ** 16 - 1, 'u32': 2 ** 32 - 1, 'u64': 2 ** 64 - 1,
                                'usize': 2 ** 32 - 1, 'Byte': 2 ** 8 - 1, 'bool': 1}
# usize is taken at its narrowest (wasm32), so a sum proved within it is within it everywhere.


class DeductionError(Exception):
    """A deduction input that cannot be read."""


# ---- linear arithmetic -----------------------------------------------------------------------

Linear = dict[str, Fraction]  # atom -> coefficient; '' is the constant


def linear_add(left: Linear, right: Linear, scale: Fraction = Fraction(1)) -> Linear:
    result: Linear = dict(left)
    atom: str
    for atom in right:
        result[atom] = result.get(atom, Fraction(0)) + right[atom] * scale
        if result[atom] == 0:
            del result[atom]
    return result


def constant(value: int | Fraction) -> Linear:
    return {'': Fraction(value)} if value != 0 else {}


def atom_of(name: str) -> Linear:
    return {name: Fraction(1)}


def is_constant(form: Linear) -> bool:
    return all(atom == '' for atom in form)


def constant_part(form: Linear) -> Fraction:
    return form.get('', Fraction(0))


@dataclasses.dataclass(frozen=True)
class Fact:
    """form >= 0 (strict: form > 0), or form == 0 when equal."""
    form: tuple[tuple[str, Fraction], ...]
    equal: bool
    strict: bool

    def linear(self) -> Linear:
        return dict(self.form)


def fact(form: Linear, equal: bool = False, strict: bool = False) -> Fact:
    return Fact(tuple(sorted(form.items())), equal, strict)


def refuted(facts: list[Fact], limit: int = 4000) -> bool:
    """Whether the facts have no rational solution (Fourier-Motzkin elimination)."""
    rows: list[tuple[Linear, bool]] = []  # form >= 0, or > 0 when strict
    item: Fact
    for item in facts:
        form: Linear = item.linear()
        if item.equal:
            rows.append((form, False))
            rows.append(({atom: -value for atom, value in form.items()}, False))
        else:
            rows.append((form, item.strict))
    while True:
        for form, strict in rows:
            if is_constant(form):
                value: Fraction = constant_part(form)
                if value < 0 or (strict and value == 0):
                    return True
        atoms: set[str] = {atom for form, _ in rows for atom in form if atom != ''}
        if not atoms:
            return False
        # The atom whose elimination makes the fewest rows.
        best: str = ''
        best_size: int = 0
        name: str
        for name in sorted(atoms):
            size: int = elimination_size(rows, name)
            if not best or size < best_size:
                best = name
                best_size = size
        positive: list[tuple[Linear, bool]] = [row for row in rows if row[0].get(best, 0) > 0]
        negative: list[tuple[Linear, bool]] = [row for row in rows if row[0].get(best, 0) < 0]
        kept: list[tuple[Linear, bool]] = [row for row in rows if row[0].get(best, 0) == 0]
        for upper, upper_strict in positive:
            for lower, lower_strict in negative:
                combined: Linear = linear_add(linear_add({}, upper, Fraction(1) / upper[best]), lower,
                                              Fraction(1) / -lower[best])
                combined.pop(best, None)
                kept.append((combined, upper_strict or lower_strict))
        if len(kept) > limit:
            return False
        rows = kept


def elimination_size(rows: list[tuple[Linear, bool]], name: str) -> int:
    positive: int = sum(1 for form, _ in rows if form.get(name, 0) > 0)
    negative: int = sum(1 for form, _ in rows if form.get(name, 0) < 0)
    return positive * negative - positive - negative


def proves(facts: list[Fact], goal: Linear, relation: str) -> bool:
    """Whether the facts imply `goal relation 0` (relation: '>=', '>', '==')."""
    if relation == '==':
        return proves(facts, goal, '>=') and proves(facts, {atom: -value for atom, value in goal.items()}, '>=')
    negated: Linear = {atom: -value for atom, value in goal.items()}
    # not (goal >= 0) is -goal > 0; not (goal > 0) is -goal >= 0.
    return refuted(facts + [fact(negated, strict=(relation == '>='))])


# ---- expressions over counters and words ------------------------------------------------------

TOKEN: re.Pattern[str] = re.compile(r'\s*(0[xX][0-9A-Fa-f]+|[0-9]+|[A-Za-z_][A-Za-z0-9_.]*|<=|>=|==|!=|[-+*()<>,^|&])')


def tokens(text: str) -> list[str]:
    found: list[str] = []
    position: int = 0
    while position < len(text):
        if text[position].isspace():
            position = position + 1
            continue
        match: re.Match[str] | None = TOKEN.match(text, position)
        if match is None:
            raise DeductionError('cannot read ' + repr(text[position:]) + ' in ' + repr(text))
        found.append(match.group(1))
        position = match.end()
    return found


def word_text(text: str) -> str:
    """A word expression (inside pop) in one spelling: no spaces, u64_max as its value."""
    compact: str = re.sub(r'\s+', '', text)
    compact = compact.replace('u64_max', '18446744073709551615')
    return compact


class Laws:
    """The givens a lemma's popcount laws may use: disjoint words, non-zero words, single bits, bounded exponents."""

    def __init__(self) -> None:
        self.disjoint: set[frozenset[str]] = set()
        self.nonzero: set[str] = set()
        self.single: set[str] = set()
        self.used: list[str] = []

    def pop(self, inner: str) -> Linear:
        """pop(inner) as a linear form, by the laws; an atom when none applies."""
        text: str = word_text(inner)
        if text == '0':
            return constant(0)
        if text == '18446744073709551615':
            self.note('pop of all 64 bits is 64')
            return constant(WORD_BITS)
        if text in self.single:
            self.note('pop of one bit is 1')
            return constant(1)
        parts: list[str] = top_split(text, '|')
        if len(parts) == 2 and frozenset(parts) in self.disjoint:
            self.note('pop(a | b) = pop(a) + pop(b) for disjoint a, b')
            return linear_add(self.pop(parts[0]), self.pop(parts[1]))
        match: re.Match[str] | None = re.fullmatch(r'(.+)&\(\1-1\)', text)
        if match and match.group(1) in self.nonzero:
            self.note('pop(x & (x - 1)) = pop(x) - 1 for x != 0')
            return linear_add(self.pop(match.group(1)), constant(-1))
        match = re.fullmatch(r'\(?2\^([A-Za-z_][A-Za-z0-9_]*)\)?-1', text)
        if match:
            self.note('pop(2^r - 1) = r for 0 <= r <= 64')
            return atom_of(match.group(1))
        return atom_of('pop(' + text + ')')

    def note(self, law: str) -> None:
        if law not in self.used:
            self.used.append(law)


def top_split(text: str, operator: str) -> list[str]:
    """`text` split at `operator` outside parentheses (once at most, at the first)."""
    depth: int = 0
    index: int
    for index, character in enumerate(text):
        if character == '(':
            depth = depth + 1
        elif character == ')':
            depth = depth - 1
        elif character == operator and depth == 0:
            return [strip_parentheses(text[:index]), strip_parentheses(text[index + 1:])]
    return [text]


def strip_parentheses(text: str) -> str:
    while text.startswith('(') and text.endswith(')') and balanced(text[1:-1]):
        text = text[1:-1]
    return text


def balanced(text: str) -> bool:
    depth: int = 0
    for character in text:
        if character == '(':
            depth = depth + 1
        elif character == ')':
            depth = depth - 1
            if depth < 0:
                return False
    return depth == 0


class Reader:
    """A linear expression: sums and differences of numbers, names and pop(...), a product with a number."""

    def __init__(self, text: str, laws: Laws, values: dict[str, int]) -> None:
        self.text: str = text
        self.items: list[str] = tokens(text)
        self.position: int = 0
        self.laws: Laws = laws
        self.values: dict[str, int] = values

    def peek(self) -> str:
        return self.items[self.position] if self.position < len(self.items) else ''

    def take(self) -> str:
        item: str = self.peek()
        self.position = self.position + 1
        return item

    def sum(self) -> Linear:
        form: Linear = self.product()
        while self.peek() in ('+', '-'):
            operator: str = self.take()
            right: Linear = self.product()
            form = linear_add(form, right, Fraction(1 if operator == '+' else -1))
        return form

    def product(self) -> Linear:
        form: Linear = self.factor()
        while self.peek() == '*':
            self.take()
            right: Linear = self.factor()
            if is_constant(form):
                form = {atom: value * constant_part(form) for atom, value in right.items()}
            elif is_constant(right):
                form = {atom: value * constant_part(right) for atom, value in form.items()}
            else:
                raise DeductionError('a product of two unknowns in ' + repr(self.text))
        return form

    def factor(self) -> Linear:
        item: str = self.take()
        if item == '(':
            inner: Linear = self.sum()
            if self.take() != ')':
                raise DeductionError('unbalanced parentheses in ' + repr(self.text))
            return inner
        if item == '-':
            return {atom: -value for atom, value in self.factor().items()}
        if item == 'pop' and self.peek() == '(':
            self.take()
            start: int = self.position
            depth: int = 1
            while depth > 0:
                piece: str = self.take()
                if piece == '':
                    raise DeductionError('unbalanced pop( in ' + repr(self.text))
                if piece == '(':
                    depth = depth + 1
                elif piece == ')':
                    depth = depth - 1
            inner_text: str = ''.join(self.items[start:self.position - 1])
            return self.laws.pop(inner_text)
        if re.fullmatch(r'0[xX][0-9A-Fa-f]+|[0-9]+', item):
            value: int = int(item, 0)
            if self.peek() == '^':
                self.take()
                exponent: str = self.take()
                value = value ** int(exponent, 0)
            return constant(value)
        if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.]*', item):
            if item in self.values:
                return constant(self.values[item])
            return atom_of(item)
        raise DeductionError('unexpected ' + repr(item) + ' in ' + repr(self.text))


RELATIONS: tuple[str, ...] = ('==', '!=', '<=', '>=', '<', '>')


def relation_of(text: str) -> tuple[str, str, str]:
    depth: int = 0
    index: int = 0
    while index < len(text):
        character: str = text[index]
        if character == '(':
            depth = depth + 1
        elif character == ')':
            depth = depth - 1
        elif depth == 0:
            relation: str
            for relation in RELATIONS:
                if text.startswith(relation, index):
                    return text[:index].strip(), relation, text[index + len(relation):].strip()
        index = index + 1
    raise DeductionError('no relation in ' + repr(text))


def facts_of(text: str, laws: Laws, values: dict[str, int]) -> list[Fact]:
    """A given as facts: a relation between linear expressions."""
    left: str
    relation: str
    right: str
    left, relation, right = relation_of(text)
    difference: Linear = linear_add(Reader(left, laws, values).sum(), Reader(right, laws, values).sum(), Fraction(-1))
    if relation == '==':
        return [fact(difference, equal=True)]
    if relation == '>=':
        return [fact(difference)]
    if relation == '>':
        return [fact(difference, strict=True)]
    negated: Linear = {atom: -value for atom, value in difference.items()}
    if relation == '<=':
        return [fact(negated)]
    if relation == '<':
        return [fact(negated, strict=True)]
    return []  # != carries no linear fact


def register_law(text: str, laws: Laws) -> bool:
    """A given that feeds a popcount law rather than the arithmetic; True when it was one."""
    compact: str = word_text(text)
    match: re.Match[str] | None = re.fullmatch(r'\(?(.+?)&(.+?)\)?==0', compact)
    if match and '(' not in match.group(1) and '(' not in match.group(2):
        laws.disjoint.add(frozenset((match.group(1), match.group(2))))
        return True
    match = re.fullmatch(r'(.+)!=0', compact)
    if match:
        laws.nonzero.add(match.group(1))
        return True
    match = re.fullmatch(r'single_bit\((.+)\)', compact)
    if match:
        laws.single.add(match.group(1))
        return True
    return False


def bounds_of_pops(facts: list[Fact]) -> list[Fact]:
    """0 <= pop(w) <= 64 for every pop atom the facts name."""
    atoms: set[str] = {atom for item in facts for atom, _ in item.form if atom.startswith('pop(')}
    extra: list[Fact] = []
    atom: str
    for atom in sorted(atoms):
        extra.append(fact(atom_of(atom)))
        extra.append(fact(linear_add(constant(WORD_BITS), atom_of(atom), Fraction(-1))))
    return extra


def lemma(given: tuple[str, ...], shows: str) -> tuple[bool, list[str]]:
    """Whether `shows` (one or more relations joined by ' and ') follows from `given`; the laws it used."""
    laws: Laws = Laws()
    arithmetic: list[str] = []
    text: str
    for text in given:
        if not register_law(text, laws):
            arithmetic.append(text)
    facts: list[Fact] = []
    for text in arithmetic:
        facts.extend(facts_of(text, laws, {}))
    goals: list[str] = [part.strip() for part in shows.split(' and ')]
    goal_text: str
    for goal_text in goals:
        left: str
        relation: str
        right: str
        left, relation, right = relation_of(goal_text)
        difference: Linear = linear_add(Reader(left, laws, {}).sum(), Reader(right, laws, {}).sum(), Fraction(-1))
        everything: list[Fact] = facts + bounds_of_pops(facts + [fact(difference)])
        mapped: dict[str, tuple[Linear, str]] = {
            '==': (difference, '=='), '>=': (difference, '>='), '>': (difference, '>'),
            '<=': ({atom: -value for atom, value in difference.items()}, '>='),
            '<': ({atom: -value for atom, value in difference.items()}, '>')}
        if relation not in mapped:
            return False, laws.used
        goal: Linear
        goal_relation: str
        goal, goal_relation = mapped[relation]
        if not proves(everything, goal, goal_relation):
            return False, laws.used
    return True, laws.used


# ---- predicate-exact --------------------------------------------------------------------------

def exact_failures(clause: ic.Node, types: dict[str, str], values: dict[str, int]) -> list[str]:
    """The arithmetic in `clause` not shown free of wrapping in C++'s order of evaluation; empty when all is."""
    failures: list[str] = []
    walk(clause, [], types, values, failures)
    return failures


def type_facts(names: list[str], types: dict[str, str]) -> list[Fact]:
    facts: list[Fact] = []
    name: str
    for name in names:
        facts.append(fact(atom_of(name)))
        largest: int = TYPE_LARGEST.get(types.get(name, 'u64'), 2 ** 64 - 1)
        facts.append(fact(linear_add(constant(largest), atom_of(name), Fraction(-1))))
    return facts


def form_of(node: ic.Node, values: dict[str, int]) -> Linear | None:
    """A clause's arithmetic node as a linear form (None for a product of two unknowns)."""
    if node.kind == 'name':
        return constant(values[node.text]) if node.text in values else atom_of(node.text)
    if node.kind == 'number':
        return constant(node.value)
    if node.kind == 'arithmetic':
        left: Linear | None = form_of(node.items[0], values)
        right: Linear | None = form_of(node.items[1], values)
        if left is None or right is None:
            return None
        if node.operator == '+':
            return linear_add(left, right)
        if node.operator == '-':
            return linear_add(left, right, Fraction(-1))
        if node.operator == '*':
            if is_constant(left):
                return {atom: value * constant_part(left) for atom, value in right.items()}
            if is_constant(right):
                return {atom: value * constant_part(right) for atom, value in left.items()}
            return None
        if node.operator == '^' and is_constant(left) and is_constant(right):
            return constant(int(constant_part(left)) ** int(constant_part(right)))
    return None


def condition_facts(node: ic.Node, values: dict[str, int]) -> list[Fact]:
    """What a boolean node, taken as true, gives the arithmetic (comparisons and intervals only)."""
    if node.kind == 'compare':
        left: Linear | None = form_of(node.items[0], values)
        right: Linear | None = form_of(node.items[1], values)
        if left is None or right is None:
            return []
        difference: Linear = linear_add(left, right, Fraction(-1))
        negated: Linear = {atom: -value for atom, value in difference.items()}
        mapping: dict[str, list[Fact]] = {'<=': [fact(negated)], '<': [fact(negated, strict=True)],
                                          '>=': [fact(difference)], '>': [fact(difference, strict=True)],
                                          '==': [fact(difference, equal=True)], '!=': []}
        return mapping[node.operator]
    if node.kind == 'interval':
        subject: ic.Node = node.items[0]
        low: ic.Node = ic.Node('compare', operator='<' if node.low_open else '<=', items=(node.items[1], subject))
        high: ic.Node = ic.Node('compare', operator='<' if node.high_open else '<=', items=(subject, node.items[2]))
        return condition_facts(low, values) + condition_facts(high, values)
    if node.kind == 'logic' and node.operator == 'and':
        result: list[Fact] = []
        item: ic.Node
        for item in node.items:
            result.extend(condition_facts(item, values))
        return result
    return []


def walk(node: ic.Node, assumed: list[Fact], types: dict[str, str], values: dict[str, int],
         failures: list[str]) -> None:
    if node.kind == 'logic':
        held: list[Fact] = list(assumed)
        item: ic.Node
        for item in node.items:
            walk(item, held, types, values, failures)
            if node.operator == 'and':
                held = held + condition_facts(item, values)
        return
    if node.kind == 'arithmetic':
        child: ic.Node
        for child in node.items:
            walk(child, assumed, types, values, failures)
        names: list[str] = []
        ic.names_of(node, names)
        unknown: list[str] = [name for name in names if name not in values]
        facts: list[Fact] = assumed + type_facts(unknown, types)
        form: Linear | None = form_of(node, values)
        text: str = ic.outer_stripped(ic.cpp_of(node))
        if form is None:
            failures.append(text + ': a product of two unknowns')
            return
        if node.operator == '-' and not proves(facts, form, '>='):
            failures.append(text + ': not shown at least zero')
        largest: int = min((TYPE_LARGEST.get(types.get(name, 'u64'), 2 ** 64 - 1) for name in unknown),
                           default=2 ** 64 - 1)
        if node.operator in ('+', '*') and not proves(facts, linear_add(constant(largest), form, Fraction(-1)),
                                                      '>='):
            failures.append(text + ': not shown within its type')
        return
    for child in node.items:
        walk(child, assumed, types, values, failures)


# ---- reading the sources ----------------------------------------------------------------------

def file_hash(root: Path, path: str) -> str:
    file: Path = root / path
    if not file.is_file():
        raise DeductionError('a named source is missing: ' + path)
    return hashlib.sha256(file.read_bytes()).hexdigest()


def function_start(lines: list[str], index: int) -> int:
    """The line opening the function that holds line `index` (a line at column 0 ending with `{`)."""
    position: int
    for position in range(index, -1, -1):
        line: str = lines[position]
        if line and not line[0].isspace() and line.rstrip().endswith('{') and '(' in line:
            return position
        if line.startswith('    ') and line.strip().endswith('{') and '(' in line and \
                re.match(r'\s{4}(\[\[nodiscard\]\] )?[A-Za-z_:<>&* ]+\(', line) and \
                not re.match(r'\s+(if|for|while|switch)\b', line):
            return position
    return 0


SAFE_STATEMENT: re.Pattern[str] = re.compile(r'(const |constexpr |static_assert|STXLIB_REQUIRE|if \()')


def precedes_cleanly(lines: list[str], guard: int) -> tuple[bool, str]:
    """Whether only const locals, guards and their refusal paths precede line `guard` in its function."""
    start: int = function_start(lines, guard)
    body: str = '\n'.join(re.sub(r'//.*', '', line) for line in lines[start + 1:guard])
    # A braced initializer (`ErrorDetail{...}`) is an expression; a braced
    # block that ends in a return is a refusal path: drop it.
    previous: str = ''
    while previous != body:
        previous = body
        body = re.sub(r'([A-Za-z0-9_>])\{([^{}]*)\}', r'\1(\2)', body)
        body = re.sub(r'\{[^{}]*\breturn\b[^{}]*\}', ' return;', body)
    statements: list[str] = [part.strip() for part in body.split(';') if part.strip()]
    statement: str
    for statement in statements:
        flat: str = ' '.join(statement.split())
        if not SAFE_STATEMENT.match(flat):
            return False, flat[:80]
        if flat.startswith('if (') and 'return' not in flat:
            return False, flat[:80]
    return True, ''


def guard_returns(lines: list[str], index: int, refusal: ic.Refusal, codes: str) -> bool:
    """Whether the guard at `index` refuses as declared, read from its line and the few after it."""
    window: str = ' '.join(lines[index:index + 8])
    end: int = window.find(';')
    statement: str = window[:end + 1] if end >= 0 else window
    if refusal.kind == 'null':
        return 'return nullptr' in statement
    if refusal.kind == 'value':
        return 'return ' in statement and refusal.value.split('::')[-1] in statement
    if refusal.kind == 'failure':
        named: list[str] = []
        if refusal.failure != 'any':
            named.append('ErrorKind::' + refusal.failure)
        if refusal.code:
            named.append('::' + refusal.code)
        block: str = ' '.join(lines[index:index + 8])
        return 'return' in block and any(text in block for text in named)
    return False


@dataclasses.dataclass
class Record:
    identifier: str
    declaration: ic.Declaration
    sources: list[str]
    method: str
    premises: list[str]
    steps: list[str]


def site_lines(root: Path, paths: tuple[str, ...], name: str) -> list[tuple[str, list[str], int]]:
    found: list[tuple[str, list[str], int]] = []
    path: str
    for path in paths:
        file: Path = root / path
        if not file.is_file():
            continue
        lines: list[str] = file.read_text(encoding='utf-8').split('\n')
        index: int
        for index, line in enumerate(lines):
            if re.search(r'\b' + re.escape(name) + r'\s*\(', line):
                found.append((path, lines, index))
    return found


def deduce_declaration(root: Path, declaration: ic.Declaration) -> tuple[list[Record], list[tuple[str, str]]]:
    records: list[Record] = []
    open_items: list[tuple[str, str]] = []
    prefix: str = declaration.manifest_key().replace('::', '.').lower()
    constants: dict[str, int] = {item.name: item.value for item in declaration.constants if not item.cpp}
    types: dict[str, str] = dict(declaration.terms)
    for item in declaration.constants:
        types[item.name] = item.type
    call: ic.Call
    for call in declaration.calls:
        base: str = prefix + '.' + call.identifier
        call_types: dict[str, str] = dict(types)
        call_types.update(dict(call.parameters))
        requirement: ic.Requirement
        for requirement in call.requires:
            # predicate-exact
            arithmetic: bool = '-' in requirement.holds or '+' in requirement.holds or '*' in requirement.holds
            if arithmetic:
                identifier: str = base + '.exact.' + requirement.check
                failures: list[str] = exact_failures(requirement.clause, call_types, constants)
                if failures:
                    open_items.append((identifier, '; '.join(failures)))
                else:
                    records.append(Record(identifier, declaration, [], 'interval and linear arithmetic over the '
                                          'clause in C++\'s order of evaluation (Fourier-Motzkin elimination)',
                                          ['each name lies within its declared type (usize at 32 bits)',
                                           'the generated predicate evaluates the conjuncts in the clause\'s order'
                                           ' (tools/ic_declarations.py writes && in order)'],
                                          ['every subtraction shown at least zero and every sum within its type '
                                           'under the conjuncts before it']))
            if declaration.checks is None:
                continue
            name: str = 'admits_' + requirement.check
            sites: list[tuple[str, list[str], int]] = site_lines(root, declaration.checks.used_in, name)
            entry_id: str = base + '.requires.' + requirement.check
            if not sites:
                open_items.append((entry_id, 'no call of ' + name + ' in ' + ', '.join(declaration.checks.used_in)))
                continue
            codes: str = declaration.engine.codes if declaration.engine is not None else ''
            good: list[str] = []
            bad: list[str] = []
            path: str
            lines: list[str]
            index: int
            for path, lines, index in sites:
                line: str = lines[index]
                where: str = path + ':' + str(index + 1)
                if requirement.refuses.kind == 'requirement':
                    statement: str = ' '.join(lines[index:index + 2])
                    if 'STXLIB_REQUIRE(' in line and requirement.check + '_message' in statement:
                        good.append(where)
                    else:
                        bad.append(where + ' is not STXLIB_REQUIRE with ' + requirement.check + '_message')
                    continue
                # `if (!<predicate>(` or `if (... || !<predicate>(`: the refusal follows when it is false.
                guard_pattern: str = r'(if \(|\|\| )!\s*[A-Za-z_:]*' + re.escape(name) + r'\('
                if re.search(guard_pattern, line) and line.strip().startswith('if ('):
                    if guard_returns(lines, index, requirement.refuses, codes):
                        good.append(where)
                    else:
                        bad.append(where + ': the refusal does not name ' + requirement.refuses.described())
                    continue
                if '?' in line or 'const bool' in line or 'return float64' in line:
                    bad.append(where + ': the predicate is combined with others, read by hand')
                    continue
                bad.append(where + ': not a guard')
            if bad:
                open_items.append((entry_id, '; '.join(bad)))
            else:
                premises: list[str] = ['the generated predicate is the clause (tools/ic_declarations.py writes '
                                       'both from one text)', 'the guard\'s arguments are the declared parameters '
                                       '(as read at each site)']
                if requirement.refuses.kind == 'requirement':
                    premises.append('STXLIB_REQUIRE ends the process when its condition is false '
                                    '(include/stxlib/require.hpp)')
                records.append(Record(entry_id, declaration, list(declaration.checks.used_in),
                                      'source structure: each call of the predicate guards the declared refusal',
                                      premises, ['guard at ' + site for site in good]))
            if requirement.refuses.kind != 'requirement' and requirement.leaves == 'unchanged':
                leaves_id: str = base + '.refusal.' + requirement.check
                if bad:
                    open_items.append((leaves_id, 'its entry check is not discharged'))
                    continue
                unclean: list[str] = []
                for path, lines, index in sites:
                    clean: bool
                    offending: str
                    clean, offending = precedes_cleanly(lines, index)
                    if not clean:
                        unclean.append(path + ':' + str(index + 1) + ' follows ' + repr(offending))
                if unclean:
                    open_items.append((leaves_id, '; '.join(unclean)))
                else:
                    records.append(Record(leaves_id, declaration, list(declaration.checks.used_in),
                                          'source structure: only const locals, guards and their refusals precede '
                                          'the guard in its function',
                                          ['a const local\'s initializer changes nothing observable (it reads)',
                                           'an earlier guard\'s refusal path ends in a return'],
                                          ['guard at ' + path + ':' + str(index + 1) for path, _, index in sites]))
    obligation: ic.Obligation
    for obligation in declaration.obligations:
        identifier = prefix + '.' + obligation.identifier
        if not obligation.shows:
            open_items.append((identifier, 'stated in words only (no given and shows)'))
            continue
        proved: bool
        laws: list[str]
        try:
            proved, laws = lemma(obligation.given, obligation.shows)
        except DeductionError as error:
            open_items.append((identifier, str(error)))
            continue
        missing: list[str] = []
        source_paths: list[str] = []
        source: str
        for source in obligation.sources:
            if ': ' not in source:
                missing.append(repr(source) + ' is not `path: text`')
                continue
            path_text: str
            wanted: str
            path_text, wanted = source.split(': ', 1)
            file: Path = root / path_text
            if not file.is_file() or wanted not in file.read_text(encoding='utf-8'):
                missing.append(path_text + ' does not hold ' + repr(wanted))
                continue
            if path_text not in source_paths:
                source_paths.append(path_text)
        if not proved:
            open_items.append((identifier, 'the givens do not yield ' + obligation.shows))
            continue
        if missing:
            open_items.append((identifier, '; '.join(missing)))
            continue
        records.append(Record(identifier, declaration, source_paths, (obligation.method or 'sxv\'s algebra')
                              + ' (' + CREDIT + ')', list(obligation.premises),
                              ['from ' + '; '.join(obligation.given), 'shown ' + obligation.shows,
                               'laws: ' + (', '.join(laws) if laws else 'linear arithmetic only'),
                               'read in ' + ', '.join(obligation.sources)]))
    return records, open_items


def deductions_text(project: ic.Project, declarations: list[ic.Declaration]) -> str:
    deductions: list[dict[str, object]] = []
    still_open: list[dict[str, str]] = []
    declaration: ic.Declaration
    for declaration in declarations:
        records: list[Record]
        open_items: list[tuple[str, str]]
        records, open_items = deduce_declaration(project.root, declaration)
        record: Record
        for record in records:
            sources: dict[str, str] = {path: file_hash(project.root, path) for path in record.sources}
            deductions.append({
                'id': record.identifier,
                'declaration': declaration.path,
                'declaration_sha256': hashlib.sha256(declaration.text.encode('utf-8')).hexdigest(),
                'sources': sources,
                'level': 'deduced',
                'tool': TOOL,
                'method': record.method,
                'premises': record.premises,
                'steps': record.steps,
            })
        identifier: str
        why: str
        for identifier, why in open_items:
            still_open.append({'id': identifier, 'declaration': declaration.path, 'why': why})
    document: dict[str, object] = {
        'schema': ic.SCHEMA,
        'generated_by': TOOL,
        'record': 'each deduction holds while its declaration and sources hash as recorded; '
                  'tools/ic_declarations.py merges it into the obligation then, and only then',
        'credit': CREDIT,
        'deductions': deductions,
        'open': still_open,
    }
    return json.dumps(document, indent=2) + '\n'


def run(command: str, config: Path, root: Path) -> int:
    try:
        project: ic.Project = ic.load_project(config, root)
    except (ic.DeclarationError, OSError) as error:
        print('ERROR ' + str(config) + ': ' + str(error))
        return 2
    if not project.deductions:
        print('ERROR ' + str(config) + ': the project file names no deductions file')
        return 2
    declarations: list[ic.Declaration]
    errors: list[str]
    declarations, errors = ic.loaded(project)
    if errors:
        message: str
        for message in errors:
            print('ERROR ' + message)
        return 2
    try:
        text: str = deductions_text(project, declarations)
    except (DeductionError, OSError) as error:
        print('ERROR ' + str(error))
        return 2
    target: Path = root / project.deductions
    current: str | None = target.read_text(encoding='utf-8') if target.is_file() else None
    document: dict[str, object] = json.loads(text)
    discharged: int = len(document['deductions'])  # type: ignore[arg-type]
    remaining: int = len(document['open'])  # type: ignore[arg-type]
    if current == text:
        print('SUMMARY ' + str(discharged) + ' discharged, ' + str(remaining) + ' open; current')
        return 0
    if command == 'generate':
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
        print('WROTE ' + project.deductions)
        print('SUMMARY ' + str(discharged) + ' discharged, ' + str(remaining) + ' open')
        return 0
    print('STALE ' + project.deductions)
    return 1


def main(arguments: list[str]) -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('command', choices=('generate', 'check'))
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--root', type=Path, default=Path('.'))
    options: argparse.Namespace = parser.parse_args(arguments)
    status: int = run(options.command, options.config, options.root.resolve())
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
