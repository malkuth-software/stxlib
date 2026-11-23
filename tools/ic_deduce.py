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
  The guard may be reached through a selector helper (a function that only
  returns one of several generated predicates, picked by its bool
  parameters): every guard of the helper counts, and the callers from the
  declared class and call must fix the switches to this declaration's
  predicate. The refusal may be named by a status (`FileStatus::too_large`,
  `Refusal::input_count`) that a `case` of a mapping switch turns into the
  declared error, passed to the mapper directly, through one relaying
  function, or returned and relayed by every caller.
- refusal-leaves: the guard comes before anything that could change the
  object: only const locals, other guards and their refusals, a reset of an
  output parameter and a void check that only reads, returns or ends the
  process precede it in its function (and the call that relays its status).
- typestate-refusal, transition and identity, for a declaration with a
  `represents` table (each state a condition over the object's members):
  every path through the call, read in a state outside its `in`, refuses
  as declared before it changes anything; every succeeding path, read in
  each admitting state, leaves the `moves` state; moving from the object
  and every stateful call leave and refuse the stale state
  (tools/ic_typestate.py, whose docstring says what it follows). Without a
  represents table, or where the code reaches the host before refusing,
  the obligation stays open with why, and tools/ic_declarations.py records
  the blocks or tests that cover it.
- an invariant (`given`, `from`, `covers`, `shows`): made by the base steps
  and kept by every other, each step a list of lemmas; a step that changes a
  clause's words or counters (`X_after`) must show the clause after it, and
  every write the `covers` patterns find must be cited by a step whose
  lemmas say how it changes the words the pattern names. Word identities in
  a lemma are decided bit by bit (&, |, ^ and ~ act on each bit alone; x &
  (x - 1) clears low(x), the lowest set bit, which lies within x).
- structural (`untouched`, `paths`, `metadata`): on every path to a refusal
  of the named functions no pointer is formed from the untouched names or a
  value computed from them, nor dereferenced; each call they reach is read
  the same way, and the metadata accessors are a premise.
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
import itertools
import json
import re
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ic_declarations as ic  # noqa: E402 (the declarations' own reader)
import ic_typestate as typestate  # noqa: E402 (states read against the calls' bodies)

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
        self.subset: set[tuple[str, str]] = set()   # (b, x): b's bits are x's
        self.aliases: dict[str, str] = {}           # a word named equal to an expression
        self.used: list[str] = []
        self.depth: int = 0

    def pop(self, inner: str) -> Linear:
        """pop(inner) as a linear form, by the laws; an atom when none applies."""
        text: str = strip_parentheses(word_text(inner))
        if text == '0':
            return constant(0)
        if text in self.aliases and self.depth < 16:
            self.note('a word equal to an expression has that expression\'s popcount')
            self.depth = self.depth + 1
            aliased: Linear = self.pop(self.aliases[text])
            self.depth = self.depth - 1
            return aliased
        low: re.Match[str] | None = re.fullmatch(r'low\((.+)\)', text)
        if low and low.group(1) in self.nonzero:
            self.note('pop of the lowest set bit of a non-zero word is 1')
            return constant(1)
        every: list[str] = split_all(text, '|')
        if len(every) > 2 and all(frozenset((every[i], every[j])) in self.disjoint
                                  for i in range(len(every)) for j in range(i + 1, len(every))):
            self.note('pop(a | b | ...) is the sum for pairwise disjoint words')
            total: Linear = {}
            for part in every:
                total = linear_add(total, self.pop(part))
            return total
        both: list[str] = split_all(text, '&')
        if len(both) == 2 and both[1].startswith('~') and (strip_parentheses(both[1][1:]), both[0]) in self.subset:
            self.note('pop(x & ~b) = pop(x) - pop(b) for b within x')
            return linear_add(self.pop(both[0]), self.pop(both[1][1:]), Fraction(-1))
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


def split_all(text: str, operator: str) -> list[str]:
    """`text` split at every `operator` outside parentheses."""
    parts: list[str] = []
    depth: int = 0
    start: int = 0
    index: int
    for index, character in enumerate(text):
        if character == '(':
            depth = depth + 1
        elif character == ')':
            depth = depth - 1
        elif character == operator and depth == 0:
            parts.append(strip_parentheses(text[start:index]))
            start = index + 1
    parts.append(strip_parentheses(text[start:]))
    return parts


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
    match: re.Match[str] | None = re.fullmatch(r'single_bit\((.+)\)', compact)
    if match:
        laws.single.add(match.group(1))
        return True
    if '==' in compact and compact.count('==') == 1 and 'pop(' not in compact:
        left: str
        right: str
        left, right = (strip_parentheses(part) for part in compact.split('=='))
        parts: list[str] = split_all(left, '&')
        if right == '0' and len(parts) == 2:
            laws.disjoint.add(frozenset(parts))
            return True
        if len(parts) == 2 and right in parts:
            other: str = parts[1] if parts[0] == right else parts[0]
            laws.subset.add((right, other))
            return True
        if re.fullmatch(r'[A-Za-z_]\w*', left) and re.search(r'[&|~^]', right):
            laws.aliases[left] = right
            return True
        if re.fullmatch(r'[A-Za-z_]\w*', left) and right == '0':
            laws.aliases[left] = right
            return False  # also a fact of the arithmetic (a counter at zero)
        if re.fullmatch(r'[A-Za-z_]\w*', left) and re.fullmatch(r'[A-Za-z_]\w*', right):
            laws.aliases[left] = right
            return False  # a renaming: also a fact of the arithmetic
    match = re.fullmatch(r'(.+)!=0', compact)
    if match:
        laws.nonzero.add(match.group(1))
        return True
    return False


# ---- bitwise identities, bit by bit ------------------------------------------------------------

BIT_ATOM: str = r'low\([A-Za-z_]\w*\)|[A-Za-z_]\w*'


def low_rewritten(text: str) -> str:
    """`x & (x - 1)` as `(x & ~low(x))`: clearing the lowest set bit (low(0) is 0)."""
    return re.sub(r'\b([A-Za-z_]\w*)&\(\1-1\)', r'(\1&~low(\1))', text)


def bitwise_relation(text: str) -> bool:
    """Whether `text` is an equation of words under &, |, ^ and ~ only (decided bit by bit)."""
    compact: str = low_rewritten(word_text(text))
    if compact.count('==') != 1 or 'pop(' in compact or '!=' in compact:
        return False
    rest: str = re.sub(BIT_ATOM, 'a', compact.replace('==', '='))
    return bool(re.fullmatch(r'[a0&|^~()=]+', rest))


def bit_operators(text: str) -> bool:
    """Whether `text` uses a bit operator outside pop(...)."""
    outside: str = text
    while 'pop(' in outside:
        start: int = outside.index('pop(')
        inner: str | None = inside_parentheses(outside, start + 3)
        if inner is None:
            break
        outside = outside[:start] + 'P' + outside[start + 3 + len(inner) + 2:]
    return bool(re.search(r'[&|^~]', outside))


class BitReader:
    """A word expression evaluated at one bit: atoms are the bit's values."""

    def __init__(self, text: str) -> None:
        self.items: list[str] = re.findall(BIT_ATOM + r'|0|[&|^~()]', text)
        if ''.join(self.items) != text:
            raise DeductionError('cannot read the word expression ' + repr(text))
        self.position: int = 0

    def atoms(self) -> list[str]:
        return [item for item in self.items if re.fullmatch(BIT_ATOM, item)]

    def value(self, bits: dict[str, bool]) -> bool:
        self.position = 0
        result: bool = self.disjunction(bits)
        if self.position != len(self.items):
            raise DeductionError('cannot read ' + repr(''.join(self.items)))
        return result

    def peek(self) -> str:
        return self.items[self.position] if self.position < len(self.items) else ''

    def disjunction(self, bits: dict[str, bool]) -> bool:
        result: bool = self.exclusive(bits)
        while self.peek() == '|':
            self.position = self.position + 1
            result = self.exclusive(bits) or result
        return result

    def exclusive(self, bits: dict[str, bool]) -> bool:
        result: bool = self.conjunction(bits)
        while self.peek() == '^':
            self.position = self.position + 1
            result = result != self.conjunction(bits)
        return result

    def conjunction(self, bits: dict[str, bool]) -> bool:
        result: bool = self.unary(bits)
        while self.peek() == '&':
            self.position = self.position + 1
            result = self.unary(bits) and result
        return result

    def unary(self, bits: dict[str, bool]) -> bool:
        item: str = self.peek()
        self.position = self.position + 1
        if item == '~':
            return not self.unary(bits)
        if item == '(':
            inner: bool = self.disjunction(bits)
            if self.peek() != ')':
                raise DeductionError('unbalanced parentheses')
            self.position = self.position + 1
            return inner
        if item == '0':
            return False
        if re.fullmatch(BIT_ATOM, item):
            return bits[item]
        raise DeductionError('unexpected ' + repr(item))


def bit_proves(givens: list[str], goal: str) -> bool:
    """Whether the word equations `givens` imply `goal` at every bit (each bit is independent under &, |, ^, ~;
    the lowest set bit of x is within x)."""
    def sides(text: str) -> tuple[BitReader, BitReader]:
        compact: str = low_rewritten(word_text(text))
        left, right = compact.split('==')
        return BitReader(left), BitReader(right)
    equations: list[tuple[BitReader, BitReader]] = [sides(text) for text in givens]
    target: tuple[BitReader, BitReader] = sides(goal)
    atoms: list[str] = []
    for left, right in equations + [target]:
        for atom in left.atoms() + right.atoms():
            if atom not in atoms:
                atoms.append(atom)
    lows: list[tuple[str, str]] = [(atom, atom[4:-1]) for atom in atoms if atom.startswith('low(')]
    for _, inner in lows:
        if inner not in atoms:
            atoms.append(inner)
    if len(atoms) > 16:
        raise DeductionError('a bitwise lemma over more than 16 words')
    values: tuple[bool, ...]
    for values in itertools.product((False, True), repeat=len(atoms)):
        bits: dict[str, bool] = dict(zip(atoms, values))
        if any(bits[low] and not bits[inner] for low, inner in lows):
            continue  # low(x) is a bit of x
        if all(left.value(bits) == right.value(bits) for left, right in equations):
            if target[0].value(bits) != target[1].value(bits):
                return False
    return True


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
    words: list[str] = [text for text in given if bitwise_relation(text)]
    text: str
    for text in given:
        if not register_law(text, laws) and not bit_operators(text) and not text.startswith('single_bit('):
            arithmetic.append(text)
    facts: list[Fact] = []
    for text in arithmetic:
        facts.extend(facts_of(text, laws, {}))
    within: str
    whole: str
    for within, whole in sorted(laws.subset):
        # The bits of a word within another: pop(within) <= pop(whole).
        facts.append(fact(linear_add(laws.pop(whole), laws.pop(within), Fraction(-1))))
        laws.note('pop(b) <= pop(x) for b within x')
    goals: list[str] = [part.strip() for part in shows.split(' and ')]
    goal_text: str
    for goal_text in goals:
        if bitwise_relation(goal_text) and bit_operators(goal_text):
            if not bit_proves(words, goal_text):
                return False, laws.used
            laws.note('bitwise identities decided bit by bit (with low(x) within x)')
            continue
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
    # Line endings normalised: a Windows checkout may hold the sources with CRLF.
    return hashlib.sha256(file.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


HEADER_START: re.Pattern[str] = re.compile(r'[A-Za-z_\[]')
NOT_HEADER: tuple[str, ...] = ('namespace', 'using', 'static_assert', 'template', 'struct', 'class', 'enum')


def function_start(lines: list[str], index: int) -> int:
    """The first line of the function that holds line `index`: a header at column 0 (it may span lines), or a
    member function's header indented by four."""
    position: int
    for position in range(index, -1, -1):
        line: str = lines[position]
        if line and HEADER_START.match(line) and '(' in line and not line.rstrip().endswith(';') and \
                not line.startswith(NOT_HEADER):
            return position
        if line.startswith('    ') and line.strip().endswith('{') and '(' in line and \
                re.match(r'\s{4}(\[\[nodiscard\]\] )?[A-Za-z_:<>&* ]+\(', line) and \
                not re.match(r'\s+(if|for|while|switch)\b', line):
            return position
    return 0


def body_open(lines: list[str], start: int) -> int:
    """The line that opens the body of the function whose header starts at `start`."""
    position: int
    for position in range(start, min(len(lines), start + 12)):
        if re.sub(r'//.*', '', lines[position]).rstrip().endswith('{'):
            return position
    return start


def function_end(lines: list[str], start: int) -> int:
    """The line that closes the function whose header starts at `start` (its own indent and `}`)."""
    indent: str = lines[start][:len(lines[start]) - len(lines[start].lstrip())]
    position: int
    for position in range(body_open(lines, start) + 1, len(lines)):
        if lines[position].rstrip() == indent + '}':
            return position
    return len(lines) - 1


def header_text(lines: list[str], start: int) -> str:
    return ' '.join(line.strip() for line in lines[start:body_open(lines, start) + 1])


def function_name(header: str) -> str:
    """The qualified name a header declares (`PlanAccess::make32`)."""
    plain: str = re.sub(r'\[\[[^\]]*\]\]', '', header)
    match: re.Match[str] | None = re.search(r'([A-Za-z_~][A-Za-z0-9_:]*)\s*\(', plain)
    return match.group(1) if match else ''


def split_top(text: str) -> list[str]:
    """`text` split at commas outside parentheses, brackets, braces and angle brackets of a template."""
    parts: list[str] = []
    depth: int = 0
    current: str = ''
    character: str
    for character in text:
        if character in '([{':
            depth = depth + 1
        elif character in ')]}':
            depth = depth - 1
        if character == ',' and depth == 0:
            parts.append(current.strip())
            current = ''
            continue
        current = current + character
    if current.strip():
        parts.append(current.strip())
    return parts


def inside_parentheses(text: str, open_index: int) -> str | None:
    """The text between the parenthesis at `open_index` and its match."""
    depth: int = 0
    index: int
    for index in range(open_index, len(text)):
        if text[index] == '(':
            depth = depth + 1
        elif text[index] == ')':
            depth = depth - 1
            if depth == 0:
                return text[open_index + 1:index]
    return None


def parameters_of(header: str) -> list[tuple[str, str]]:
    """(type, name) of each parameter a header declares."""
    name: str = function_name(header)
    position: int = header.find(name + '(') if name else -1
    if position < 0:
        match: re.Match[str] | None = re.search(re.escape(name) + r'\s*\(', header)
        if match is None:
            return []
        position = match.start()
    inner: str | None = inside_parentheses(header, header.index('(', position))
    if not inner:
        return []
    result: list[tuple[str, str]] = []
    part: str
    for part in split_top(inner):
        found: re.Match[str] | None = re.match(r'(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*$', part)
        if found:
            result.append((found.group(1).strip(), found.group(2)))
    return result


def call_arguments(lines: list[str], index: int, name: str) -> list[str] | None:
    """The arguments of the call of `name` that starts on line `index`."""
    window: str = ' '.join(line.strip() for line in lines[index:index + 8])
    match: re.Match[str] | None = re.search(r'\b' + re.escape(name) + r'\s*\(', window)
    if match is None:
        return None
    inner: str | None = inside_parentheses(window, match.end() - 1)
    return None if inner is None else split_top(inner)


def definition(lines: list[str], name: str) -> int | None:
    """The first line of the function `name` (its last name part) defined in `lines`."""
    index: int
    for index, line in enumerate(lines):
        if re.search(r'\b' + re.escape(name) + r'\s*\(', line) and function_start(lines, index) == index and \
                function_name(header_text(lines, index)).split('::')[-1] == name:
            return index
    return None


def body_statements(lines: list[str], start: int) -> list[str]:
    """The statements of a function's body, comments dropped, braced initializers as calls, flattened."""
    body: str = '\n'.join(re.sub(r'//.*', '', line) for line in lines[body_open(lines, start) + 1:
                                                                        function_end(lines, start)])
    previous: str = ''
    while previous != body:
        previous = body
        body = re.sub(r'([A-Za-z0-9_>])\{([^{}]*)\}', r'\1(\2)', body)
    return [' '.join(part.split()) for part in re.split(r'[;{}]', body) if part.strip()]


# ---- selector helpers: a function that returns one of several generated predicates ------------------

@dataclasses.dataclass
class Selector:
    """A helper whose result is one generated predicate, picked by its bool parameters."""
    name: str
    parameters: list[tuple[str, str]]
    switches: list[str]                       # its bool parameters, in order
    picks: dict[tuple[bool, ...], str]        # switch values -> the qualified predicate returned


class NotSelector(Exception):
    pass


def selector_of(lines: list[str], start: int) -> Selector | None:
    """The helper at `start` read as a selector, or None when its body is anything else."""
    header: str = header_text(lines, start)
    parameters: list[tuple[str, str]] = parameters_of(header)
    switches: list[str] = [name for kind, name in parameters if kind.replace('const', '').strip() == 'bool']
    if len(switches) > 6:
        return None
    text: str = ' '.join(' '.join(re.sub(r'//.*', '', line).split()) for line in
                         lines[body_open(lines, start) + 1:function_end(lines, start)])
    try:
        statements: list[tuple[object, ...]]
        statements, rest = parse_selector(text.strip())
        if rest.strip():
            return None
    except NotSelector:
        return None
    aliases: dict[str, str] = {str(item[1]): str(item[2]) for item in statements if item[0] == 'alias'}
    picks: dict[tuple[bool, ...], str] = {}
    values: tuple[bool, ...]
    for values in itertools.product((False, True), repeat=len(switches)):
        environment: dict[str, bool] = dict(zip(switches, values))
        try:
            picked: str | None = run_selector(statements, environment, {}, aliases)
        except NotSelector:
            return None
        if picked is None:
            return None
        picks[values] = picked
    if not picks:
        return None
    return Selector(function_name(header), parameters, switches, picks)


def parse_selector(text: str) -> tuple[list[tuple[object, ...]], str]:
    """Statements of a selector body: namespace aliases, `if (c) { ... }`, `if (c) return p;`, `return p;`,
    `const bool name = expression;`."""
    statements: list[tuple[object, ...]] = []
    while True:
        text = text.lstrip()
        if not text or text.startswith('}'):
            return statements, text
        match: re.Match[str] | None = re.match(r'namespace (\w+) = ([\w:]+);', text)
        if match:
            statements.append(('alias', match.group(1), match.group(2).lstrip(':')))
            text = text[match.end():]
            continue
        if text.startswith('if ('):
            inner: str | None = inside_parentheses(text, 3)
            if inner is None:
                raise NotSelector()
            text = text[3 + len(inner) + 2:].lstrip()
            if text.startswith('{'):
                block: list[tuple[object, ...]]
                block, text = parse_selector(text[1:])
                if not text.startswith('}'):
                    raise NotSelector()
                text = text[1:]
                statements.append(('if', inner, block))
                continue
            match = re.match(r'return ([^;]+);', text)
            if match is None:
                raise NotSelector()
            statements.append(('if', inner, [('return', match.group(1))]))
            text = text[match.end():]
            continue
        match = re.match(r'return ([^;]+);', text)
        if match:
            statements.append(('return', match.group(1)))
            text = text[match.end():]
            continue
        match = re.match(r'const bool (\w+) = ([^;]+);', text)
        if match:
            statements.append(('local', match.group(1), match.group(2)))
            text = text[match.end():]
            continue
        raise NotSelector()


def truth(condition: str, environment: dict[str, bool]) -> bool:
    """A condition over the switches: names, `!`, `&&`, `||`, true and false."""
    condition = strip_parentheses(condition.strip())
    parts: list[str] = [part for part in re.split(r'\s*\|\|\s*', condition)]
    if len(parts) > 1:
        return any(truth(part, environment) for part in parts)
    parts = [part for part in re.split(r'\s*&&\s*', condition)]
    if len(parts) > 1:
        return all(truth(part, environment) for part in parts)
    if condition.startswith('!'):
        return not truth(condition[1:], environment)
    if condition in ('true', 'false'):
        return condition == 'true'
    if condition in environment:
        return environment[condition]
    raise NotSelector()


def predicate_named(expression: str, aliases: dict[str, str]) -> str:
    """The qualified generated predicate an expression calls (it must be one call of an admits_ function)."""
    match: re.Match[str] | None = re.fullmatch(r'([\w:]*admits_\w+)\s*\((.*)\)', expression.strip())
    if match is None:
        raise NotSelector()
    name: str = match.group(1).lstrip(':')
    head: str = name.split('::')[0]
    if head in aliases:
        name = aliases[head] + name[len(head):]
    return name


def run_selector(statements: list[tuple[object, ...]], environment: dict[str, bool], held: dict[str, str],
                 aliases: dict[str, str]) -> str | None:
    """The predicate the body returns under `environment`; None when it falls through."""
    statement: tuple[object, ...]
    for statement in statements:
        kind: object = statement[0]
        if kind == 'alias':
            continue
        if kind == 'local':
            held[str(statement[1])] = chosen(str(statement[2]), environment, held, aliases)
            continue
        if kind == 'return':
            return chosen(str(statement[1]), environment, held, aliases)
        if kind == 'if' and truth(str(statement[1]), environment):
            picked: str | None = run_selector(statement[2], environment, held, aliases)  # type: ignore[arg-type]
            if picked is not None:
                return picked
    return None


def chosen(expression: str, environment: dict[str, bool], held: dict[str, str], aliases: dict[str, str]) -> str:
    expression = expression.strip()
    if expression in held:
        return held[expression]
    question: int = expression.find('?')
    if question >= 0:
        colon: int = expression.find(':', question)
        while colon >= 0 and expression[colon:colon + 2] == '::':
            colon = expression.find(':', colon + 2)
        if colon < 0:
            raise NotSelector()
        picked: str = expression[question + 1:colon] if truth(expression[:question], environment) else \
            expression[colon + 1:]
        return predicate_named(picked, aliases)
    return predicate_named(expression, aliases)


# ---- guard sites -------------------------------------------------------------------------------

@dataclasses.dataclass
class Site:
    """A guard of a generated predicate (or of a selector that returns it)."""
    path: str
    lines: list[str]
    index: int
    allowed: list[dict[str, bool]]   # names in the guard's function, values that make it this predicate's
    notes: list[str]


def guard_line(line: str, name: str) -> bool:
    """`if (!name(` or `if (... || !name(`: the refusal follows when the predicate is false."""
    pattern: str = r'(if \(|\|\| )!\s*[A-Za-z_:]*\b' + re.escape(name) + r'\('
    return bool(re.search(pattern, line)) and line.strip().startswith('if (')


def guard_sites(root: Path, declaration: ic.Declaration, qualified: str, short: str) \
        -> tuple[list[Site], list[str]]:
    """Every guard that evaluates the predicate, directly or through a selector helper; what cannot be read."""
    sites: list[Site] = []
    bad: list[str] = []
    helpers: set[tuple[str, int]] = set()
    assert declaration.checks is not None
    path: str
    lines: list[str]
    index: int
    for path, lines, index in site_lines(root, declaration.checks.used_in, short):
        line: str = lines[index]
        where: str = path + ':' + str(index + 1)
        if guard_line(line, short):
            sites.append(Site(path, lines, index, [{}], []))
            continue
        under: re.Match[str] | None = re.match(r'\s*if \(((?:!?[a-z_][a-z0-9_]* && )+)!\s*[\w:]*\b' +
                                               re.escape(short) + r'\(', line)
        if under:
            wanted: dict[str, bool] = {}
            name: str
            for name in [part.strip() for part in under.group(1).split('&&') if part.strip()]:
                wanted[name.lstrip('!')] = not name.startswith('!')
            sites.append(Site(path, lines, index, [wanted], ['the guard holds the predicate under ' +
                                                                under.group(1).strip(' &')]))
            continue
        start: int = function_start(lines, index)
        if (path, start) in helpers:
            continue  # the same selector, read from another of its lines
        helpers.add((path, start))
        selector: Selector | None = selector_of(lines, start)
        if selector is None or qualified not in selector.picks.values():
            bad.append(where + ': not a guard, nor a selector helper that returns the predicate')
            continue
        helper: str = selector.name.split('::')[-1]
        used: int = 0
        other: int
        for other in range(len(lines)):
            if other == start or not re.search(r'\b' + re.escape(helper) + r'\s*\(', lines[other]):
                continue
            if function_start(lines, other) == start:
                continue
            other_where: str = path + ':' + str(other + 1)
            if not guard_line(lines[other], helper):
                bad.append(other_where + ': ' + helper + ' (a selector of the predicate) is called outside a guard')
                continue
            arguments: list[str] | None = call_arguments(lines, other, helper)
            if arguments is None or len(arguments) != len(selector.parameters):
                bad.append(other_where + ': the arguments of ' + helper + ' cannot be read')
                continue
            allowed: list[dict[str, bool]] = []
            values: tuple[bool, ...]
            for values, picked in selector.picks.items():
                if picked != qualified:
                    continue
                wanted = {}
                consistent: bool = True
                switch: str
                value: bool
                for switch, value in zip(selector.switches, values):
                    position: int = [name for _, name in selector.parameters].index(switch)
                    argument: str = arguments[position]
                    if argument in ('true', 'false'):
                        consistent = consistent and (argument == 'true') == value
                    elif re.fullmatch(r'[a-z_][a-z0-9_]*', argument):
                        if wanted.get(argument, value) != value:
                            consistent = False
                        wanted[argument] = value
                    else:
                        consistent = False
                if consistent:
                    allowed.append(wanted)
            if not allowed:
                continue  # this guard evaluates another declaration's predicate
            used = used + 1
            sites.append(Site(path, lines, other, allowed, ['through the selector ' + helper + ' (' + where +
                                                            ')']))
        if used == 0:
            bad.append(where + ': no guard of ' + helper + ' evaluates the predicate')
    return sites, bad


def selector_value(expression: str, parameters: list[tuple[str, str]], arguments: list[str]) -> bool | None:
    """A switch's value at a caller: the parameter's argument, or `parameter ==/!= Enum::value` of it."""
    names: list[str] = [name for _, name in parameters]
    expression = strip_parentheses(expression.strip())
    if expression in names:
        argument: str = arguments[names.index(expression)].strip()
        return {'true': True, 'false': False}.get(argument)
    if expression.startswith('!'):
        inner: bool | None = selector_value(expression[1:], parameters, arguments)
        return None if inner is None else not inner
    match: re.Match[str] | None = re.fullmatch(r'(\w+)\s*(==|!=)\s*([\w:]+)', expression)
    if match and match.group(1) in names:
        argument = arguments[names.index(match.group(1))].strip()
        if not re.fullmatch(r'[\w:]+', argument) or '::' not in argument:
            return None
        same: bool = argument.split('::')[-2:] == match.group(3).split('::')[-2:]
        return same if match.group(2) == '==' else not same
    return None


def selector_holds(root: Path, declaration: ic.Declaration, call: ic.Call, site: Site) -> tuple[bool, list[str]]:
    """Whether every caller from the declared call picks this predicate at `site`; the steps or the reason."""
    if site.allowed == [{}]:
        return True, []
    lines: list[str] = site.lines
    start: int = function_start(lines, site.index)
    header: str = header_text(lines, start)
    function: str = function_name(header).split('::')[-1]
    parameters: list[tuple[str, str]] = parameters_of(header)
    needed: set[str] = {name for wanted in site.allowed for name in wanted}
    expressions: dict[str, str] = {}
    name: str
    for name in sorted(needed):
        if name in [parameter for _, parameter in parameters]:
            expressions[name] = name
            continue
        local: re.Match[str] | None = None
        statement: str
        for statement in body_statements(lines, start):
            local = re.fullmatch(r'const bool ' + re.escape(name) + r' = (.+)', statement)
            if local:
                break
        if local is None:
            return False, [site.path + ':' + str(site.index + 1) + ': the switch ' + name + ' is neither a '
                           'parameter nor a const bool local of ' + function]
        expressions[name] = local.group(1)
    owner: str = declaration.manifest_key().split('::')[-1]
    steps: list[str] = []
    matched: int = 0
    assert declaration.checks is not None
    path: str
    caller_lines: list[str]
    index: int
    for path, caller_lines, index in site_lines(root, declaration.checks.used_in, function):
        if function_start(caller_lines, index) == index:
            continue  # the definition
        caller_start: int = function_start(caller_lines, index)
        caller: str = header_text(caller_lines, caller_start)
        if owner not in caller or not re.search(r'::' + re.escape(call.name) + r'\d*\s*\(', caller):
            continue
        arguments: list[str] | None = call_arguments(caller_lines, index, function)
        where: str = path + ':' + str(index + 1)
        if arguments is None or len(arguments) != len(parameters):
            return False, [where + ': the arguments of ' + function + ' cannot be read']
        values: dict[str, bool] = {}
        for name, expression in expressions.items():
            value: bool | None = selector_value(expression, parameters, arguments)
            if value is None:
                return False, [where + ': the switch ' + name + ' (' + expression + ') is not a literal there']
            values[name] = value
        if not any(all(values[key] == wanted[key] for key in wanted) for wanted in site.allowed):
            return False, [where + ': ' + function_name(caller) + ' picks another predicate at ' + site.path + ':' +
                           str(site.index + 1)]
        matched = matched + 1
        steps.append(function_name(caller) + ' (' + where + ') calls ' + function + ' with ' +
                     ', '.join(key + ' = ' + str(values[key]).lower() for key in sorted(values)))
    if matched == 0:
        return False, [site.path + ':' + str(site.index + 1) + ': no caller of ' + function + ' from ' + owner +
                       '::' + call.name + ' found to fix its switches']
    return True, steps


# ---- refusals: named at the guard, or relayed through a status and its mapping ------------------

def guard_statement(lines: list[str], index: int) -> str:
    window: str = ' '.join(line.strip() for line in lines[index:index + 8])
    end: int = window.find(';')
    return window[:end + 1] if end >= 0 else window


def guard_returns(lines: list[str], index: int, refusal: ic.Refusal, codes: str) -> bool:
    """Whether the guard at `index` refuses as declared, read from its line and the few after it."""
    statement: str = guard_statement(lines, index)
    if refusal.kind == 'null':
        return 'return nullptr' in statement
    if refusal.kind == 'value':
        return 'return ' in statement and refusal.value.split('::')[-1] in statement
    if refusal.kind == 'failure':
        block: str = ' '.join(lines[index:index + 8])
        named: list[str] = []
        if refusal.failure != 'any':
            named.append('ErrorKind::' + refusal.failure)
        if refusal.code:
            named.append('::' + refusal.code)
        return 'return' in block and any(text in block for text in named)
    return False


@dataclasses.dataclass
class Relay:
    steps: list[str]
    premises: list[str]
    callers: list[tuple[str, list[str], int]]   # where a returned status is relayed (they precede the guard)


def case_mapping(lines: list[str], case: int, codes: str) -> tuple[list[str], list[str]]:
    """The error kinds and codes a `case` of a mapping switch names: its own lines, else the function's text
    outside the switch."""
    code_type: str = codes.split('::')[-1] if codes else 'HostCode'
    text: str = ''
    position: int = case
    while position < len(lines):
        line: str = lines[position]
        text = text + ' ' + line
        if 'break;' in line or re.search(r'\breturn\b', line):
            break
        position = position + 1
    kinds: list[str] = re.findall(r'ErrorKind::(\w+)', text)
    found_codes: list[str] = re.findall(code_type + r'::(\w+)', text)
    start: int = function_start(lines, case)
    switch: int = case
    while switch > start and not re.match(r'\s*switch\b', lines[switch]):
        switch = switch - 1
    indent: str = lines[switch][:len(lines[switch]) - len(lines[switch].lstrip())]
    switch_end: int = switch
    while switch_end < len(lines) and lines[switch_end].rstrip() != indent + '}':
        switch_end = switch_end + 1
    outside: str = ' '.join(lines[body_open(lines, start) + 1:switch] +
                            lines[switch_end + 1:function_end(lines, start)])
    if not kinds:
        kinds = sorted(set(re.findall(r'ErrorKind::(\w+)', outside)))
    if not found_codes:
        found_codes = sorted(set(item for item in re.findall(code_type + r'::(\w+)', outside) if item != 'none'))
    return kinds, found_codes


def relayed(root: Path, paths: tuple[str, ...], lines: list[str], index: int, refusal: ic.Refusal,
            codes: str) -> tuple[Relay | None, str]:
    """A refusal named by a status the guard passes or returns, mapped to the declared error by a switch."""
    if refusal.kind != 'failure':
        return None, 'the refusal is not a failure'
    statement: str = guard_statement(lines, index)
    statuses: list[tuple[str, str]] = [item for item in re.findall(r'\b([A-Z][A-Za-z0-9]*)::([a-z_][a-z0-9_]*)\b',
                                                                   statement)
                                       if item[0] not in ('ErrorKind', 'HostCode', 'OperationStatus',
                                                          'OperationResult')]
    if len(statuses) != 1:
        return None, 'the guard names no single status'
    enum: str
    value: str
    enum, value = statuses[0]
    files: dict[str, list[str]] = {}
    path: str
    for path in paths:
        file: Path = root / path
        if file.is_file():
            files[path] = file.read_text(encoding='utf-8').split('\n')
    mapping: tuple[str, list[str], int] | None = None
    lines_of: list[str]
    for path, lines_of in files.items():
        position: int
        for position, line in enumerate(lines_of):
            if re.match(r'\s*case\s+(?:[\w:]*::)?' + enum + '::' + value + r'\s*:', line):
                mapping = (path, lines_of, position)
                break
        if mapping:
            break
    if mapping is None:
        return None, 'no case maps ' + enum + '::' + value
    map_path: str
    map_lines: list[str]
    case: int
    map_path, map_lines, case = mapping
    kinds: list[str]
    found_codes: list[str]
    kinds, found_codes = case_mapping(map_lines, case, codes)
    if refusal.failure != 'any' and kinds != [refusal.failure]:
        return None, enum + '::' + value + ' maps to ' + (', '.join(kinds) or 'no kind') + ' (' + map_path + ':' + \
            str(case + 1) + ')'
    if refusal.code and found_codes != [refusal.code]:
        return None, enum + '::' + value + ' maps to code ' + (', '.join(found_codes) or 'none') + ' (' + \
            map_path + ':' + str(case + 1) + ')'
    mapper: str = function_name(header_text(map_lines, function_start(map_lines, case))).split('::')[-1]
    steps: list[str] = [enum + '::' + value + ' maps to ' + refusal.described() + ' in ' + mapper + ' (' +
                        map_path + ':' + str(case + 1) + ')']
    premises: list[str] = []
    callers: list[tuple[str, list[str], int]] = []
    if re.search(r'\b' + mapper + r'\s*\(', statement):
        return Relay(steps, premises, callers), ''
    called: str
    for called in re.findall(r'\b([a-z_][a-z0-9_]*)\s*\(', statement):
        for path, lines_of in files.items():
            start: int | None = definition(lines_of, called)
            if start is None:
                continue
            body: str = ' '.join(lines_of[start:function_end(lines_of, start) + 1])
            if re.search(r'\b' + mapper + r'\s*\(', body):
                steps.append(called + ' (' + path + ':' + str(start + 1) + ') passes it to ' + mapper)
                premises.append('the functions between the guard and ' + mapper + ' pass the status through '
                                'unchanged (read at ' + path + ':' + str(start + 1) + ')')
                return Relay(steps, premises, callers), ''
    if not re.search(r'\breturn\s+(?:[\w:]*::)?' + enum + '::' + value + r'\s*;', statement):
        return None, 'the status reaches ' + mapper + ' by no path read here'
    # The guard's function returns the status: every caller must relay it to the mapper.
    start = function_start(lines, index)
    returning: str = function_name(header_text(lines, start)).split('::')[-1]
    for path, lines_of in files.items():
        position = 0
        for position, line in enumerate(lines_of):
            if not re.search(r'\b' + returning + r'\s*\(', line) or function_start(lines_of, position) == position:
                continue
            assigned: re.Match[str] | None = re.search(r'const [\w:]+ (\w+) = [\w:]*\b' + returning + r'\s*\(', line)
            where: str = path + ':' + str(position + 1)
            if assigned is None:
                return None, where + ': the status ' + returning + ' returns is not kept in a const local'
            variable: str = assigned.group(1)
            following: str = ' '.join(item.strip() for item in lines_of[position:position + 5])
            if not re.search(r'if \(\s*' + variable + r'\s*!=\s*[\w:]*' + enum + r'::\w+\s*\)\s*return\b[^;]*\b' +
                             mapper + r'\(\s*' + variable + r'\s*\)', following):
                return None, where + ': the status is not relayed to ' + mapper
            callers.append((path, lines_of, position))
    if not callers:
        return None, 'no caller relays the status ' + returning + ' returns'
    steps.append(returning + ' returns it; each of its ' + str(len(callers)) + ' callers returns ' + mapper +
                 ' of it when it is not the none status')
    premises.append('a status other than the none value is the refusal (each caller tests it against none)')
    return Relay(steps, premises, callers), ''


# ---- what precedes a guard ------------------------------------------------------------------------

SAFE_STATEMENT: re.Pattern[str] = re.compile(r'(const |constexpr |static_assert|STXLIB_REQUIRE|if \()')


def output_parameters(header: str) -> list[str]:
    """The non-const reference parameters of a function: its outputs."""
    return [name for kind, name in parameters_of(header) if kind.endswith('&') and not kind.startswith('const ')]


def quiet_check(lines: list[str], name: str) -> bool:
    """Whether `name` is a void function of this file whose body only reads, returns or ends the process."""
    start: int | None = definition(lines, name)
    if start is None:
        return False
    header: str = header_text(lines, start)
    if not re.match(r'(static |inline )*void\b', re.sub(r'\[\[[^\]]*\]\]\s*', '', header)):
        return False
    statement: str
    for statement in body_statements(lines, start):
        if statement.startswith('if (') and statement.endswith('return'):
            continue
        if SAFE_STATEMENT.match(statement) and not statement.startswith('if ('):
            continue
        if re.match(r'(::stxlib::)?fail_fast\(', statement) or statement == 'return':
            continue
        return False
    return True


def precedes_cleanly(lines: list[str], guard: int) -> tuple[bool, str, list[str]]:
    """Whether only const locals, guards and their refusal paths, output resets and checks that only read or end
    the process precede line `guard` in its function; the offending statement; what was allowed and why."""
    start: int = function_start(lines, guard)
    opening: int = body_open(lines, start)
    outputs: list[str] = output_parameters(header_text(lines, start))
    body: str = '\n'.join(re.sub(r'//.*', '', line) for line in lines[opening + 1:guard])
    # A braced initializer (`ErrorDetail{...}`) is an expression; a braced
    # block that ends in a return is a refusal path: drop it.
    previous: str = ''
    while previous != body:
        previous = body
        body = re.sub(r'([A-Za-z0-9_>])\{([^{}]*)\}', r'\1(\2)', body)
        body = re.sub(r'\{[^{}]*\breturn\b[^{}]*\}', ' return;', body)
    statements: list[str] = [part.strip() for part in body.split(';') if part.strip()]
    notes: list[str] = []
    statement: str
    for statement in statements:
        flat: str = ' '.join(statement.split())
        reset: re.Match[str] | None = re.fullmatch(r'(\w+) = [\w:<>]+\(\)', flat)
        if reset and reset.group(1) in outputs:
            notes.append('the output parameter ' + reset.group(1) + ' is reset before the guard (an output, not '
                         'the object)')
            continue
        check: re.Match[str] | None = re.fullmatch(r'([a-z_][a-z0-9_]*)\(.*\)', flat)
        if check and quiet_check(lines, check.group(1)):
            notes.append(check.group(1) + ' precedes the guard: it only reads, returns or ends the process (read '
                         'from its body)')
            continue
        if not SAFE_STATEMENT.match(flat):
            return False, flat[:80], notes
        if flat.startswith('if (') and 'return' not in flat:
            return False, flat[:80], notes
    return True, '', notes


# ---- structural: no refused path forms a pointer from the untouched names ------------------------

CAST: re.Pattern[str] = re.compile(r'\b(reinterpret_cast|static_cast|const_cast)\s*<([^<>]*(?:<[^<>]*>[^<>]*)*)>\s*\(')
NOT_CALLS: frozenset[str] = frozenset({'if', 'while', 'for', 'switch', 'return', 'sizeof', 'alignof', 'decltype',
                                       'static_cast', 'reinterpret_cast', 'const_cast', 'noexcept'})


def mentions(text: str, names: set[str]) -> bool:
    return any(re.search(r'(?<![\w.])' + re.escape(name) + r'\b', text) for name in names)


def without_metadata(text: str, metadata: tuple[str, ...]) -> str:
    """`text` with each metadata accessor call or table (and the index after it) replaced by META."""
    name: str
    for name in metadata:
        while True:
            match: re.Match[str] | None = re.search(r'(?<![\w.])' + re.escape(name) + r'\s*[(\[]', text)
            if match is None:
                break
            end: int = match.end() - 1
            if text[end] == '(':
                inner: str | None = inside_parentheses(text, end)
                if inner is None:
                    break
                end = end + len(inner) + 2
            while end < len(text) and text[end] == '[':
                depth: int = 0
                position: int = end
                while position < len(text):
                    if text[position] == '[':
                        depth = depth + 1
                    elif text[position] == ']':
                        depth = depth - 1
                        if depth == 0:
                            break
                    position = position + 1
                end = position + 1
            text = text[:match.start()] + 'META' + text[end:]
    return text


def unary_star(text: str, name: str) -> bool:
    """Whether `*name` dereferences (a unary star) somewhere in `text`."""
    match: re.Match[str]
    for match in re.finditer(r'\*\s*' + re.escape(name) + r'\b', text):
        before: str = text[:match.start()].rstrip()
        if not before or before[-1] in '(=,!&|?:{;' or before.endswith('return'):
            return True
    return False


class Untouched:
    """Reads the functions a structural obligation names, following each call the untouched names reach."""

    def __init__(self, root: Path, metadata: tuple[str, ...], listed: dict[str, str], directories: list[str]):
        self.root: Path = root
        self.metadata: tuple[str, ...] = metadata
        self.listed: dict[str, str] = listed          # function -> its refusal text
        self.files: dict[str, list[str]] = {}
        directory: str
        for directory in directories:
            file: Path
            for file in sorted((root / directory).iterdir()):
                if file.suffix in ('.hpp', '.cpp', '.h'):
                    self.files[file.relative_to(root).as_posix()] = file.read_text(encoding='utf-8').split('\n')
        self.read: list[str] = []
        self.problems: list[str] = []
        self.steps: list[str] = []
        self.visited: set[tuple[str, tuple[str, ...]]] = set()

    def find(self, name: str) -> tuple[str, list[str], int] | None:
        path: str
        lines: list[str]
        for path, lines in self.files.items():
            start: int | None = definition(lines, name)
            if start is not None:
                return path, lines, start
        return None

    def function(self, name: str, tainted: set[str], depth: int) -> None:
        key: tuple[str, tuple[str, ...]] = (name, tuple(sorted(tainted)))
        if key in self.visited:
            return
        self.visited.add(key)
        if depth > 6:
            self.problems.append(name + ': calls nest past six levels')
            return
        found: tuple[str, list[str], int] | None = self.find(name)
        if found is None:
            self.problems.append(name + ' receives an untouched value and is not read (no definition in ' +
                                 ', '.join(sorted({path.rsplit('/', 1)[0] for path in self.files})) + ')')
            return
        path: str
        lines: list[str]
        start: int
        path, lines, start = found
        if path not in self.read:
            self.read.append(path)
        statements: list[str] = body_statements(lines, start)
        refusal: str | None = self.listed.get(name)
        last: int = len(statements) - 1
        if refusal is not None:
            # A refusal: the function's own, or a call of another listed function (which may refuse).
            others: list[str] = [other for other in self.listed if other != name]
            marks: list[int] = [index for index, statement in enumerate(statements) if refusal in statement or
                                any(re.search(r'(?<![\w.:>])' + re.escape(other) + r'\s*\(', statement)
                                    for other in others)]
            if not marks:
                self.problems.append(name + ' (' + path + ':' + str(start + 1) + ') has no refusal ' + repr(refusal))
                return
            last = marks[-1]
            self.steps.append(name + ' (' + path + ':' + str(start + 1) + '): ' + str(len(marks)) + ' refusal(s), '
                              'statements read up to the last')
        held: set[str] = set(tainted)
        index: int
        statement: str
        for index, statement in enumerate(statements[:last + 1]):
            where: str = name + ' (' + path + ')'
            text: str = without_metadata(statement, self.metadata)
            cast: re.Match[str]
            for cast in CAST.finditer(text):
                inner: str | None = inside_parentheses(text, cast.end() - 1)
                if '*' in cast.group(2) and inner is not None and mentions(inner, held):
                    self.problems.append(where + ' forms a pointer from an untouched value: ' + statement[:90])
            variable: str
            for variable in held:
                if unary_star(text, variable) or re.search(r'(?<![\w.])' + re.escape(variable) + r'\s*(\[|->)', text):
                    self.problems.append(where + ' dereferences ' + variable + ': ' + statement[:90])
            constructed: re.Match[str] | None = re.match(r'(?:const\s+)?[\w:]+(?:<[^<>]*>)?[&*]?\s+(\w+)\(', text)
            call: re.Match[str]
            for call in re.finditer(r'(?<![\w.>:])([A-Za-z_][\w:]*)\s*\(', text):
                callee: str = call.group(1)
                short: str = callee.split('::')[-1]
                if short in NOT_CALLS or short == 'META':
                    continue
                if constructed and call.start() == constructed.start(1):
                    if mentions(inside_parentheses(text, call.end() - 1) or '', held):
                        held.add(short)  # a local constructed from an untouched value
                    continue
                arguments_text: str | None = inside_parentheses(text, call.end() - 1)
                if arguments_text is None:
                    continue
                arguments: list[str] = split_top(arguments_text)
                reached: list[int] = [position for position, argument in enumerate(arguments)
                                      if mentions(argument, held)]
                if not reached:
                    continue
                if short in self.listed:
                    continue  # read on its own, with its refusals
                target: tuple[str, list[str], int] | None = self.find(short)
                if target is None:
                    self.problems.append(where + ' passes an untouched value to ' + callee + ', which is not read')
                    continue
                parameters: list[tuple[str, str]] = parameters_of(header_text(target[1], target[2]))
                passed: set[str] = {parameters[position][1] for position in reached if position < len(parameters)}
                self.function(short, passed, depth + 1)
            local: re.Match[str] | None = re.match(r'(?:const\s+)?[\w:<>]+[&*]?\s+(?:const\s+)?(\w+)\s*=\s*(.+)',
                                                   statement)
            if local and mentions(without_metadata(local.group(2), self.metadata), held):
                held.add(local.group(1))
            assigned: re.Match[str] | None = re.match(r'(\w+)\s*=\s*(.+)', statement)
            if assigned and mentions(without_metadata(assigned.group(2), self.metadata), held):
                held.add(assigned.group(1))


def structural(root: Path, obligation: ic.Obligation) -> tuple[bool, list[str], list[str], list[str]]:
    """Whether no refused path of the named functions forms a pointer from the untouched names; the steps, the
    files read and the problems."""
    listed: dict[str, str] = {}
    directories: list[str] = []
    order: list[tuple[str, str]] = []
    entry: str
    for entry in obligation.paths:
        parts: list[str] = [part.strip() for part in entry.split(':', 2)]
        if len(parts) != 3:
            return False, [], [], [repr(entry) + ' is not `path: function: refusal`']
        path, function, refusal = parts
        if not (root / path).is_file():
            return False, [], [], [path + ' is missing']
        listed[function] = refusal
        directory: str = path.rsplit('/', 1)[0]
        if directory not in directories:
            directories.append(directory)
        order.append((path, function))
    reader: Untouched = Untouched(root, obligation.metadata, listed, directories)
    for path, function in order:
        start: int | None = definition(reader.files.get(path, []), function)
        if start is None:
            reader.problems.append(function + ' is not defined in ' + path)
            continue
        header: str = header_text(reader.files[path], start)
        names: set[str] = {name for _, name in parameters_of(header) if name in obligation.untouched}
        if not names:
            reader.problems.append(function + ' takes none of ' + ', '.join(obligation.untouched))
            continue
        reader.function(function, names, 0)
    return not reader.problems, reader.steps, reader.read, reader.problems


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
            qualified: str = declaration.checks.namespace.lstrip(':') + '::' + name
            entry_id: str = base + '.requires.' + requirement.check
            codes: str = declaration.engine.codes if declaration.engine is not None else ''
            if requirement.refuses.kind == 'requirement':
                raw: list[tuple[str, list[str], int]] = site_lines(root, declaration.checks.used_in, name)
                if not raw:
                    open_items.append((entry_id, 'no call of ' + name + ' in ' +
                                       ', '.join(declaration.checks.used_in)))
                    continue
                good: list[str] = []
                bad: list[str] = []
                path: str
                lines: list[str]
                index: int
                for path, lines, index in raw:
                    statement: str = ' '.join(lines[index:index + 2])
                    where: str = path + ':' + str(index + 1)
                    if 'STXLIB_REQUIRE(' in lines[index] and requirement.check + '_message' in statement:
                        good.append(where)
                    else:
                        bad.append(where + ' is not STXLIB_REQUIRE with ' + requirement.check + '_message')
                if bad:
                    open_items.append((entry_id, '; '.join(bad)))
                    continue
                records.append(Record(entry_id, declaration, list(declaration.checks.used_in),
                                      'source structure: each call of the predicate guards the declared refusal',
                                      ['the generated predicate is the clause (tools/ic_declarations.py writes '
                                       'both from one text)', 'the guard\'s arguments are the declared parameters '
                                       '(as read at each site)', 'STXLIB_REQUIRE ends the process when its '
                                       'condition is false (include/stxlib/require.hpp)'],
                                      ['guard at ' + site for site in good]))
                continue
            sites: list[Site]
            unread: list[str]
            sites, unread = guard_sites(root, declaration, qualified, name)
            if not sites and not unread:
                open_items.append((entry_id, 'no call of ' + name + ' in ' + ', '.join(declaration.checks.used_in)))
                continue
            problems: list[str] = list(unread)
            steps: list[str] = []
            premises: list[str] = ['the generated predicate is the clause (tools/ic_declarations.py writes both '
                                   'from one text)', 'the guard\'s arguments are the declared parameters (as read '
                                   'at each site)']
            leave_sites: list[tuple[str, list[str], int]] = []
            site: Site
            for site in sites:
                where = site.path + ':' + str(site.index + 1)
                fixed: bool
                found: list[str]
                fixed, found = selector_holds(root, declaration, call, site)
                if not fixed:
                    problems.extend(found)
                    continue
                if guard_returns(site.lines, site.index, requirement.refuses, codes):
                    steps.append('guard at ' + where + (' (' + '; '.join(site.notes) + ')' if site.notes else ''))
                    steps.extend(found)
                    leave_sites.append((site.path, site.lines, site.index))
                    continue
                relay: Relay | None
                reason: str
                relay, reason = relayed(root, declaration.checks.used_in, site.lines, site.index,
                                        requirement.refuses, codes)
                if relay is None:
                    problems.append(where + ': the refusal does not name ' + requirement.refuses.described() +
                                    ' (' + reason + ')')
                    continue
                steps.append('guard at ' + where + (' (' + '; '.join(site.notes) + ')' if site.notes else ''))
                steps.extend(found)
                steps.extend(relay.steps)
                for premise in relay.premises:
                    if premise not in premises:
                        premises.append(premise)
                leave_sites.append((site.path, site.lines, site.index))
                leave_sites.extend(relay.callers)
            if any(site.allowed != [{}] for site in sites) and not problems:
                premises.append('the switches read at the callers decide which declaration\'s predicate a shared '
                                'guard evaluates (callers named by their class and call)')
            if problems:
                open_items.append((entry_id, '; '.join(problems)))
            else:
                records.append(Record(entry_id, declaration, list(declaration.checks.used_in),
                                      'source structure: each evaluation of the predicate guards the declared '
                                      'refusal (through selector helpers and status mappings where the source '
                                      'uses them)', premises, steps))
            if requirement.leaves == 'unchanged':
                leaves_id: str = base + '.refusal.' + requirement.check
                if problems:
                    open_items.append((leaves_id, 'its entry check is not discharged'))
                    continue
                unclean: list[str] = []
                allowed_notes: list[str] = []
                seen: set[tuple[str, int]] = set()
                leave_path: str
                leave_lines: list[str]
                leave_index: int
                for leave_path, leave_lines, leave_index in leave_sites:
                    if (leave_path, leave_index) in seen:
                        continue
                    seen.add((leave_path, leave_index))
                    clean: bool
                    offending: str
                    notes: list[str]
                    clean, offending, notes = precedes_cleanly(leave_lines, leave_index)
                    if not clean:
                        unclean.append(leave_path + ':' + str(leave_index + 1) + ' follows ' + repr(offending))
                    allowed_notes.extend(note for note in notes if note not in allowed_notes)
                if unclean:
                    open_items.append((leaves_id, '; '.join(unclean)))
                else:
                    leave_premises: list[str] = ['a const local\'s initializer changes nothing observable (it reads)',
                                                 'an earlier guard\'s refusal path ends in a return']
                    if allowed_notes:
                        leave_premises.append('a check that ends the process changes nothing a caller observes '
                                              'when it returns; an output parameter is not the object')
                    records.append(Record(leaves_id, declaration, list(declaration.checks.used_in),
                                          'source structure: only const locals, guards and their refusals, output '
                                          'resets and read-only checks precede the guard in its function (and the '
                                          'call that relays its status)', leave_premises,
                                          ['guard at ' + seen_path + ':' + str(seen_index + 1) for seen_path,
                                           seen_index in sorted(seen)] + allowed_notes))
    typestate_records, typestate_open = deduce_typestate(root, declaration, prefix)
    records.extend(typestate_records)
    open_items.extend(typestate_open)
    obligation: ic.Obligation
    composites: list[ic.Obligation] = []
    for obligation in declaration.obligations:
        if obligation.composes:
            composites.append(obligation)
            continue
        identifier = prefix + '.' + obligation.identifier
        lacking: str = ' (missing: ' + obligation.missing + ')' if obligation.missing else ''
        if obligation.untouched:
            held: bool
            path_steps: list[str]
            read: list[str]
            trouble: list[str]
            held, path_steps, read, trouble = structural(root, obligation)
            if not held:
                open_items.append((identifier, '; '.join(trouble) + lacking))
                continue
            records.append(Record(identifier, declaration, read, 'source structure: on every path to a refusal, '
                                  'no pointer is formed from ' + ', '.join(obligation.untouched) + ' or a value '
                                  'computed from them, nor dereferenced; each call they reach is read the same '
                                  'way', list(obligation.premises) + [
                                      'the metadata accessors (' + ', '.join(obligation.metadata) + ') return '
                                      'pointers outside the block and its neighbours',
                                      'a member function of an atomic or of metadata takes the value, not memory '
                                      'at it', 'a statement after a function\'s last refusal is on no refused path'
                                      ' of that function'], path_steps + ['files read: ' + ', '.join(read)]))
            continue
        if not obligation.shows:
            open_items.append((identifier, 'stated in words only (no given and shows)' + lacking))
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
    discharged: dict[str, Record] = {record.identifier: record for record in records}
    stated: dict[str, ic.Obligation] = {prefix + '.' + item.identifier: item for item in declaration.obligations}
    for obligation in composites:
        identifier = prefix + '.' + obligation.identifier
        held, steps, read, trouble = invariant_kept(root, prefix, obligation, discharged, stated)
        if not held:
            lacking = ' (missing: ' + obligation.missing + ')' if obligation.missing else ''
            open_items.append((identifier, '; '.join(trouble) + lacking))
            continue
        records.append(Record(identifier, declaration, read, (obligation.method or 'an invariant kept by every '
                              'step') + ' (' + CREDIT + ')', list(obligation.premises) + [
                                  'the steps listed are every change of the invariant\'s words and counters (each '
                                  'write the covers patterns find is cited by a step\'s lemma)',
                                  'each segment between step marks is atomic (the step model)'], steps))
    return records, open_items


def typestate_view(root: Path, declaration: ic.Declaration) -> typestate.View:
    represents: ic.Represents | None = declaration.represents
    assert represents is not None
    owner: re.Match[str] | None = re.match(r'\s*(?:class|struct)\s+([A-Za-z_]\w*)', declaration.anchor)
    texts: dict[str, str] = {}
    path: str
    for path in represents.sources:
        file: Path = root / path
        if not file.is_file():
            raise DeductionError(declaration.path + ': represents names a missing source ' + path)
        texts[path] = typestate.preprocessed(file.read_text(encoding='utf-8'), represents.build)
    view: typestate.View = typestate.View(owner.group(1) if owner else '', dict(represents.states),
                                          list(represents.assume), dict(represents.succeeds),
                                          list(represents.untouching), texts, represents.build)
    return view


def deduce_typestate(root: Path, declaration: ic.Declaration, prefix: str) \
        -> tuple[list[Record], list[tuple[str, str]]]:
    """The typestate refusals, transitions and identity of a declaration with a represents table."""
    records: list[Record] = []
    open_items: list[tuple[str, str]] = []
    if declaration.represents is None:
        for call in declaration.calls:
            for state, _ in call.outside:
                open_items.append((prefix + '.' + call.identifier + '.outside.' + state,
                                   'the declaration has no represents table: its states are not conditions the '
                                   'deducer can read against the code'))
            if call.moves:
                open_items.append((prefix + '.' + call.identifier + '.moves', 'the declaration has no represents '
                                   'table: its states are not conditions the deducer can read against the code'))
        if declaration.identity_kind and declaration.identity_stale is not None:
            open_items.append((prefix + '.identity', 'a stale identity here is an address, not a state of an object '
                               '(no represents table): that every address no live block starts at is refused needs '
                               'the release path\'s tests read as one condition equal to that, which the deducer '
                               'does not do'))
        return records, open_items
    view: typestate.View = typestate_view(root, declaration)
    codes: str = declaration.engine.codes if declaration.engine is not None and declaration.engine.codes else \
        'stxlib::HostCode'
    build: list[str] = (['read with ' + ', '.join(view.build) + ' defined (the branches the sources take '
                         'under it)'] if view.build else [])
    discharged: set[str] = set()
    call: ic.Call
    for call in declaration.calls:
        base: str = prefix + '.' + call.identifier
        for state, refusal in call.outside:
            identifier: str = base + '.outside.' + state
            outcome: typestate.Outcome = typestate.typestate_refusal(view, call.name, state, refusal, codes,
                                                                     case_mapping)
            if not outcome.held:
                open_items.append((identifier, outcome.why))
                continue
            discharged.add(identifier)
            records.append(Record(identifier, declaration, outcome.read, 'every path through the call read in the '
                                  'state, its conditions decided over their comparisons (tools/ic_typestate.py)',
                                  build + outcome.premises, outcome.steps))
        if call.moves:
            identifier = base + '.moves'
            outcome = typestate.transition(view, call.name, call.admitting, call.moves)
            if not outcome.held:
                open_items.append((identifier, outcome.why))
                continue
            discharged.add(identifier)
            records.append(Record(identifier, declaration, outcome.read, 'every succeeding path through the call '
                                  'read in each admitting state, the moved-to state decided after its writes '
                                  '(tools/ic_typestate.py)', build + outcome.premises, outcome.steps))
    if declaration.identity_kind and declaration.identity_stale is not None:
        identifier = prefix + '.identity'
        stale: str = declaration.identity_state
        if not stale:
            open_items.append((identifier, 'identity.state does not name the state a stale identity is in'))
            return records, open_items
        problems: list[str] = []
        steps: list[str] = []
        moved: typestate.Outcome = typestate.moved_from(view, stale)
        if not moved.held:
            problems.append(moved.why)
        steps.extend(moved.steps)
        for call in declaration.calls:
            if not call.admitting:
                continue
            refusals: dict[str, ic.Refusal] = dict(call.outside)
            if stale in call.admitting:
                problems.append(call.name + ' admits the stale state ' + stale)
                continue
            if stale not in refusals or refusals[stale].record() != declaration.identity_stale.record():
                problems.append(call.name + ' in ' + stale + ' is not declared to refuse as a stale identity does')
                continue
            name: str = prefix + '.' + call.identifier + '.outside.' + stale
            if name not in discharged:
                problems.append(call.name + ' in ' + stale + ' is not discharged')
                continue
            steps.append(call.name + ' in ' + stale + ': ' + call.identifier + '.outside.' + stale + ' (discharged)')
        for call in declaration.calls:
            if call.moves == stale and prefix + '.' + call.identifier + '.moves' in discharged:
                steps.append(call.name + ' leaves the object in ' + stale + ' (' + call.identifier + '.moves, '
                             'discharged)')
        if problems:
            open_items.append((identifier, '; '.join(problems)))
        else:
            records.append(Record(identifier, declaration, moved.read, 'a stale identity is the state ' + stale +
                                  ', which moving from the object leaves and every stateful call refuses as '
                                  'declared (tools/ic_typestate.py)',
                                  ['a stale identity is ' + declaration.identity_kind + ': the object moved from or '
                                   'closed, which is the state ' + stale + ' (' + view.states[stale] + ')',
                                   'a self-move returns before it changes anything (the move assignment\'s '
                                   'first statement)'], steps))
    return records, open_items


def normal(text: str) -> str:
    return re.sub(r'\s+', '', text)


def renamed_after(clause: str, atoms: set[str]) -> str:
    """The clause with each of `atoms` as its value after the step (`X_after`)."""
    return re.sub(r'\b(' + '|'.join(sorted(atoms, key=len, reverse=True)) + r')\b(?!_after)',
                  lambda match: match.group(1) + '_after', clause) if atoms else clause


def clause_atoms(clause: str) -> set[str]:
    return {name for name in re.findall(r'\b[A-Za-z_]\w*\b', clause) if name not in ('pop', 'low', 'single_bit')}


def invariant_kept(root: Path, prefix: str, obligation: ic.Obligation, discharged: dict[str, Record],
                   stated: dict[str, ic.Obligation]) -> tuple[bool, list[str], list[str], list[str]]:
    """An invariant (`given`) made by the base step and kept by every other: each step's steps_of are discharged and
    show every clause whose words or counters the step changes, after it; every write the `covers` patterns find
    is cited by a step lemma's sources; and the invariant gives `shows`."""
    problems: list[str] = []
    steps: list[str] = []
    read: list[str] = []
    invariant: list[str] = list(obligation.given)
    atoms: set[str] = set()
    for clause in invariant:
        atoms |= clause_atoms(clause)
    cited: list[tuple[str, str]] = []
    citing: list[tuple[str, set[str], list[tuple[str, str]]]] = []   # step, what it changes, what it cites
    based: bool = False
    entry: str
    for entry in obligation.composes:
        if ':' not in entry:
            problems.append(repr(entry) + ' is not `step: lemma, lemma`')
            continue
        step, names = (part.strip() for part in entry.split(':', 1))
        steps_of: list[ic.Obligation] = []
        name: str
        for name in [part.strip() for part in names.split(',') if part.strip()]:
            full: str = prefix + '.' + name
            if full not in stated:
                problems.append(step + ': no obligation ' + name)
                continue
            if full not in discharged:
                problems.append(step + ': ' + name + ' is not discharged')
                continue
            steps_of.append(stated[full])
        shown: set[str] = {normal(part) for part in steps_of for part in part.shows.split(' and ')}
        mentioned: str = ' '.join(' '.join(part.given) + ' ' + part.shows for part in steps_of)
        changed: set[str] = {atom for atom in atoms if re.search(r'\b' + atom + r'_after\b', mentioned)}
        if step.startswith('base'):
            based = True
            required: list[str] = invariant
        else:
            required = [renamed_after(clause, changed) for clause in invariant if clause_atoms(clause) & changed]
        lacking: list[str] = [clause for clause in required if normal(clause) not in shown]
        if lacking:
            problems.append(step + ' does not show ' + '; '.join(lacking))
        own: list[tuple[str, str]] = []
        for part in steps_of:
            for source in part.sources:
                if ': ' in source:
                    path, text = source.split(': ', 1)
                    cited.append((path, text.strip()))
                    own.append((path, text.strip()))
        if not step.startswith('base'):
            citing.append((step, changed, own))
        steps.append(step + ': ' + ', '.join(part.identifier for part in steps_of) + (' keep ' + ', '.join(
            sorted(changed)) if not step.startswith('base') else ' make the invariant'))
    if not based:
        problems.append('no base step makes the invariant')
    for entry in obligation.covers:
        if ': ' not in entry:
            problems.append(repr(entry) + ' is not `path: pattern`')
            continue
        place, pattern = entry.split(': ', 1)
        written: set[str] = set()
        if ' => ' in pattern:
            pattern, names_text = pattern.rsplit(' => ', 1)
            written = {name.strip() for name in names_text.split(',') if name.strip()}
        target: Path = root / place
        files: list[Path] = sorted(item for item in target.iterdir() if item.suffix in ('.hpp', '.cpp')) \
            if target.is_dir() else [target]
        if not files or not all(item.is_file() for item in files):
            problems.append(place + ' is missing')
            continue
        found: int = 0
        file: Path
        for file in files:
            relative: str = file.relative_to(root).as_posix()
            number: int
            for number, line in enumerate(file.read_text(encoding='utf-8').split('\n')):
                if not re.search(pattern, line) or line.strip().startswith('//'):
                    continue
                found = found + 1
                if relative not in read:
                    read.append(relative)
                if not any(path == relative and text and text in line for path, text in cited):
                    problems.append(relative + ':' + str(number + 1) + ' writes ' + repr(line.strip()[:70]) +
                                    ', which no step cites')
                for step, changed, own in citing:
                    if any(path == relative and text and text in line for path, text in own) and \
                            not written <= changed:
                        problems.append(step + ' cites ' + relative + ':' + str(number + 1) + ', which changes ' +
                                        ', '.join(sorted(written - changed)) + ', but its lemmas do not say how')
        steps.append('covers ' + place + ' ' + repr(pattern) + ': ' + str(found) + ' write(s), each cited')
    for path, _ in cited:
        if path not in read and (root / path).is_file():
            read.append(path)
    try:
        proved, laws = lemma(tuple(invariant), obligation.shows)
    except DeductionError as error:
        problems.append(str(error))
        proved, laws = False, []
    if not proved:
        problems.append('the invariant does not yield ' + obligation.shows)
    steps.append('the invariant gives ' + obligation.shows + ' (' + (', '.join(laws) or 'linear arithmetic') + ')')
    return not problems, steps, read, problems


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
