"""Typestate deductions for tools/ic_deduce.py (the IC study's phase IC4).

A declaration's `represents` table says what each of its states is, as a C++
condition over the object's members (`closed = "handle_ == closed"`). With it,
three kinds of obligation become deducible by reading the calls' bodies:

- typestate-refusal: a call in a state outside its `in` refuses as declared
  and changes nothing observable before it does;
- transition: a call that succeeds in one of its admitting states leaves the
  object in its `moves` state;
- identity: a stale identity is refused, when moving from an object and
  closing it leave the stale state and every stateful call refuses there.

The reader follows a small part of C++, the part these calls are written in:
declarations, assignments (atomic stores and loads read as plain ones),
`if`/`else`, `return`, calls of functions defined in the sources it reads
(executed with their arguments), `STXLIB_REQUIRE` and the `STXLIB_*_CHECK`
macros (a check that ends the process when its condition is false), and
fail_fast or a [[noreturn]] function (the process ends). A loop is read only
for what it writes. A condition it cannot decide forks the path; every path is
followed. Conditions are decided by enumeration over their comparisons, with
equality's laws (a union of equal terms; distinct constants are unequal).

What it cannot follow it says, and the obligation stays open with that reason.
Each deduction records its premises: the state conditions, the conditions the
declaration assumes (each with its reason), the calls taken as changing no
state member, and that a member is named with a trailing underscore (STXlib's
AGENTS.md), which is how an opaque call is seen to touch the object.

Standard library only; Python 3.11 or later.
"""

from __future__ import annotations

import dataclasses
import itertools
import re
from pathlib import Path
from typing import Callable

MAXIMUM_PATHS: int = 256
MAXIMUM_DEPTH: int = 6
MAXIMUM_ATOMS: int = 14
NOT_CALLED: frozenset[str] = frozenset({'if', 'while', 'for', 'switch', 'return', 'sizeof', 'alignof', 'decltype',
                                        'static_cast', 'reinterpret_cast', 'const_cast', 'noexcept', 'defined'})
STATUS_TYPES: frozenset[str] = frozenset({'ErrorKind', 'HostCode', 'OperationStatus', 'OperationResult',
                                          'Operation', 'ErrorRecord', 'ErrorDetail'})


class Unread(Exception):
    """The code is outside what this reader follows."""


# ---- the declaration's view ------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class View:
    """What the reader needs from a declaration: the object's class, its states and what it may assume."""
    owner: str                                # the class (`File`), or '' for free functions
    states: dict[str, str]                    # state -> condition over the members
    assume: list[tuple[str, str]]             # (condition, why)
    succeeds: dict[str, str]                  # call name -> condition over `result` its success meets
    untouching: list[tuple[str, str]]         # (call text, why): opaque calls taken as changing no state member
    texts: dict[str, str]                     # source path -> preprocessed text
    build: tuple[str, ...]


def preprocessed(text: str, defined: tuple[str, ...]) -> str:
    """`text` without comments, with #if branches chosen for the macros `defined` (others undefined); a branch on
    a condition this reader does not decide becomes a statement it refuses to read."""
    out: list[str] = []
    index: int = 0
    while index < len(text):
        character: str = text[index]
        if text.startswith('//', index):
            end: int = text.find('\n', index)
            index = len(text) if end < 0 else end
            continue
        if text.startswith('/*', index):
            end = text.find('*/', index + 2)
            stop: int = len(text) if end < 0 else end + 2
            out.append('\n' * text.count('\n', index, stop))
            index = stop
            continue
        if character in '"\'':
            end = index + 1
            while end < len(text) and text[end] != character:
                end = end + (2 if text[end] == '\\' else 1)
            out.append(text[index:end + 1])
            index = end + 1
            continue
        out.append(character)
        index = index + 1
    lines: list[str] = ''.join(out).split('\n')
    kept: list[str] = []
    stack: list[tuple[str, bool]] = []   # (decided: 'yes'/'no'/'unread', taken so far)
    line: str
    for line in lines:
        directive: re.Match[str] | None = re.match(r'\s*#\s*(\w+)\s*(.*)', line)
        if directive is None:
            active: bool = all(state == 'yes' for state, _ in stack)
            unread: bool = any(state == 'unread' for state, _ in stack) and \
                all(state != 'no' for state, _ in stack)
            kept.append('__unread_preprocessor__();' if unread and line.strip() else (line if active else ''))
            continue
        word: str = directive.group(1)
        rest: str = directive.group(2).strip()
        if word in ('if', 'ifdef', 'ifndef'):
            decided: str = branch(word, rest, defined)
            stack.append((decided, decided == 'yes'))
        elif word == 'elif' and stack:
            previous, taken = stack[-1]
            decided = 'no' if taken else branch('if', rest, defined)
            stack[-1] = (decided, taken or decided == 'yes')
        elif word == 'else' and stack:
            previous, taken = stack[-1]
            stack[-1] = ('unread' if previous == 'unread' else ('no' if taken else 'yes'), True)
        elif word == 'endif' and stack:
            stack.pop()
        kept.append('')
    return '\n'.join(kept)


def branch(word: str, rest: str, defined: tuple[str, ...]) -> str:
    if word == 'ifdef':
        return 'yes' if rest in defined else 'no'
    if word == 'ifndef':
        return 'no' if rest in defined else 'yes'
    match: re.Match[str] | None = re.fullmatch(r'(!?)\s*defined\s*\(?\s*(\w+)\s*\)?', rest)
    if match:
        value: bool = match.group(2) in defined
        return 'yes' if value != bool(match.group(1)) else 'no'
    return 'unread'


# ---- functions ---------------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Function:
    path: str
    qualifier: str
    name: str
    parameters: list[tuple[str, str]]   # (type, name)
    body: str
    noreturn: bool
    line: int


def matching(text: str, start: int, opening: str, closing: str) -> int:
    """The index of the bracket that closes the one at `start`."""
    depth: int = 0
    index: int
    for index in range(start, len(text)):
        if text[index] == opening:
            depth = depth + 1
        elif text[index] == closing:
            depth = depth - 1
            if depth == 0:
                return index
    raise Unread('an unbalanced ' + opening)


def split_top(text: str, separator: str = ',') -> list[str]:
    parts: list[str] = []
    depth: int = 0
    current: str = ''
    index: int = 0
    while index < len(text):
        character: str = text[index]
        if character in '([{':
            depth = depth + 1
        elif character in ')]}':
            depth = depth - 1
        if depth == 0 and text.startswith(separator, index):
            parts.append(current.strip())
            current = ''
            index = index + len(separator)
            continue
        current = current + character
        index = index + 1
    if current.strip():
        parts.append(current.strip())
    return parts


def parameters_of(text: str) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    part: str
    for part in split_top(text):
        part = part.split('=')[0].strip()
        if not part or part == 'void':
            continue
        found: re.Match[str] | None = re.match(r'(.*?)([A-Za-z_]\w*)\s*$', part)
        if found and found.group(1).strip():
            result.append((found.group(1).strip(), found.group(2)))
    return result


def functions_named(view: View, name: str) -> list[Function]:
    """Every definition of `name` in the sources: the class's members first, then free functions."""
    found: list[Function] = []
    path: str
    text: str
    for path, text in view.texts.items():
        match: re.Match[str]
        for match in re.finditer(r'(?<![\w.>~])((?:[A-Za-z_]\w*::)*)(' + re.escape(name) + r')\s*\(', text):
            line_start: int = text.rfind('\n', 0, match.start()) + 1
            before: str = text[line_start:match.start()]
            # A pointer return type (`Mutex* release(`) is a declarator, not a product.
            declared: str = re.sub(r'(?<=[\w>])\s*\*+\s*$', ' ', before)
            if re.search(r'[=(,!?|+\-*/%]|\breturn\b|\.\s*$|->\s*$|\bnew\b', declared) or \
                    re.search(r'\b(if|while|for|switch)\s*$', before):
                continue
            open_index: int = match.end() - 1
            try:
                close: int = matching(text, open_index, '(', ')')
            except Unread:
                continue
            after: str = text[close + 1:close + 400]
            tail: re.Match[str] | None = re.match(r'\s*(?:const\s*)?(?:noexcept(?:\s*\([^)]*\))?\s*)?(?:override\s*)?'
                                                  r'(?:->\s*[\w:<>&*\s]+?)?\s*([{:])', after)
            if tail is None:
                continue
            body_start: int = close + 1 + tail.end() - 1
            if tail.group(1) == ':':
                if re.match(r'\s*:', after) and not re.match(r'\s*:\s*[A-Za-z_]', after[:tail.end() + 40]):
                    continue
                brace: int = text.find('{', body_start)
                while brace >= 0:
                    head: str = text[body_start:brace]
                    if head.count('(') == head.count(')'):
                        break
                    brace = text.find('{', brace + 1)
                if brace < 0:
                    continue
                body_start = brace
            if not before.strip() and not match.group(1) and not looks_declared(text, line_start):
                continue
            try:
                body_end: int = matching(text, body_start, '{', '}')
            except Unread:
                continue
            qualifier: str = match.group(1).rstrip(':').split('::')[-1] if match.group(1) else \
                enclosing_class(text, match.start())
            found.append(Function(path, qualifier, name, parameters_of(text[open_index + 1:close]),
                                  text[body_start + 1:body_end], '[[noreturn]]' in before or
                                  '[[noreturn]]' in text[max(0, line_start - 40):line_start],
                                  text.count('\n', 0, match.start()) + 1))
    owned: list[Function] = [item for item in found if view.owner and item.qualifier == view.owner]
    free: list[Function] = [item for item in found if item.qualifier != view.owner]
    return owned + free


def looks_declared(text: str, line_start: int) -> bool:
    """A name at the start of its line is a definition only after a type on the line before (a split header)."""
    previous: str = text[:line_start].rstrip().split('\n')[-1].strip() if line_start else ''
    return bool(re.search(r'[\w>&*]\s*$', previous)) and not previous.endswith(';') and \
        not re.search(r'\breturn\b|=', previous)


def enclosing_class(text: str, position: int) -> str:
    """The class whose body holds `position` (a member defined in its class), or ''."""
    for match in reversed(list(re.finditer(r'\b(?:class|struct)\s+([A-Za-z_]\w*)[^;{]*\{', text[:position]))):
        try:
            end: int = matching(text, match.end() - 1, '{', '}')
        except Unread:
            continue
        if end > position:
            return match.group(1)
    return ''


# ---- statements ----------------------------------------------------------------------------------------

@dataclasses.dataclass
class Statement:
    kind: str                 # 'simple', 'return', 'if', 'block', 'loop'
    text: str = ''
    condition: str = ''
    then: list['Statement'] = dataclasses.field(default_factory=list)
    otherwise: list['Statement'] = dataclasses.field(default_factory=list)


def statements_of(text: str) -> list[Statement]:
    result: list[Statement] = []
    index: int = 0
    while True:
        statement: Statement | None
        statement, index = one_statement(text, index)
        if statement is None:
            return result
        result.append(statement)


def skip_space(text: str, index: int) -> int:
    while index < len(text) and text[index].isspace():
        index = index + 1
    return index


def end_of_simple(text: str, index: int) -> int:
    depth: int = 0
    position: int
    for position in range(index, len(text)):
        character: str = text[position]
        if character in '([{':
            depth = depth + 1
        elif character in ')]}':
            depth = depth - 1
        elif character == ';' and depth == 0:
            return position
    raise Unread('a statement without its semicolon')


def one_statement(text: str, index: int) -> tuple[Statement | None, int]:
    index = skip_space(text, index)
    if index >= len(text):
        return None, index
    if text[index] == ';':
        return Statement('block'), index + 1
    if text[index] == '{':
        close: int = matching(text, index, '{', '}')
        return Statement('block', then=statements_of(text[index + 1:close])), close + 1
    keyword: re.Match[str] | None = re.match(r'(if|for|while|switch|do|else)\b', text[index:])
    if keyword and keyword.group(1) == 'if':
        open_index: int = text.index('(', index)
        close = matching(text, open_index, '(', ')')
        condition: str = text[open_index + 1:close]
        then: Statement | None
        then, after = one_statement(text, close + 1)
        if then is None:
            raise Unread('an if without its statement')
        statement: Statement = Statement('if', condition=' '.join(condition.split()), then=[then])
        following: int = skip_space(text, after)
        if re.match(r'else\b', text[following:]):
            otherwise: Statement | None
            otherwise, after = one_statement(text, following + 4)
            if otherwise is None:
                raise Unread('an else without its statement')
            statement.otherwise = [otherwise]
        return statement, after
    if keyword and keyword.group(1) in ('for', 'while', 'switch'):
        open_index = text.index('(', index)
        close = matching(text, open_index, '(', ')')
        body: Statement | None
        body, after = one_statement(text, close + 1)
        return Statement('loop', text=text[index:after]), after
    if keyword and keyword.group(1) == 'do':
        body, after = one_statement(text, index + 2)
        end: int = end_of_simple(text, after)
        return Statement('loop', text=text[index:end + 1]), end + 1
    end = end_of_simple(text, index)
    flat: str = ' '.join(text[index:end].split())
    if re.match(r'return\b', flat):
        return Statement('return', text=flat[6:].strip()), end + 1
    return Statement('simple', text=flat), end + 1


# ---- terms ------------------------------------------------------------------------------------------------

def strip_parentheses(text: str) -> str:
    text = text.strip()
    while text.startswith('(') and text.endswith(')'):
        try:
            if matching(text, 0, '(', ')') != len(text) - 1:
                break
        except Unread:
            break
        text = text[1:-1].strip()
    return text


def canon(text: str) -> str:
    """A term's one spelling: spaces, arrows, atomic accesses, redundant parentheses, enumerator prefixes and
    number suffixes normalised."""
    text = ' '.join(text.split())
    text = re.sub(r'\s*(\.|->|::)\s*', r'\1', text)
    previous: str = ''
    while previous != text:
        previous = text
        text = re.sub(r'\b([A-Za-z_]\w*)->', r'(*\1).', text)
        text = re.sub(r'\(\*\(\*(\w+)\)\)', r'(*\1)', text)
        text = re.sub(r'(?:std::)?atomic_ref<[^<>()]*>\(([^()]*(?:\([^()]*\))*[^()]*)\)', r'(\1)', text)
        text = re.sub(r'\.load\((?:std::memory_order_\w+)?\)', '', text)
        text = re.sub(r'(?<![\w>\]])\(\s*([A-Za-z_][\w:]*(?:\.\w+)*)\s*\)(?!\s*\()', r'\1', text)
        text = re.sub(r'\(\s*\*\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\)', r'(*\1)', text)
        text = re.sub(r'(?<![\w>\]])\(\((\*\w+)\)\)', r'(\1)', text)
        text = re.sub(r'\b(?:[a-z_]\w*::)+([A-Z]\w*::\w+)\b', r'\1', text)
        text = re.sub(r'\b(0x[0-9A-Fa-f]+|\d+)(?:[uU][lL]{0,2}|[lL]{1,2}[uU]?)\b', r'\1', text)
        text = re.sub(r'static_cast<[\w:]+>\((-?\d+)\)', r'\1', text)
    text = re.sub(r'\b0x([0-9A-Fa-f]+)\b', lambda match: str(int(match.group(1), 16)), text)
    return strip_parentheses(text) if is_balanced_whole(text) else text


def whole_call(text: str) -> re.Match[str] | None:
    """`receiver.qualifier::name(arguments)` when the call's parentheses close at the end of `text`."""
    match: re.Match[str] | None = re.match(r'((?:[\w\[\]()*>]+?\.)*)((?:[A-Za-z_]\w*::)*)([A-Za-z_]\w*)\s*\(', text)
    if match is None:
        return None
    try:
        if matching(text, match.end() - 1, '(', ')') != len(text) - 1:
            return None
    except Unread:
        return None
    return match


def is_balanced_whole(text: str) -> bool:
    return text.startswith('(') and text.endswith(')')


def constant(term: str) -> bool:
    return bool(re.fullmatch(r'-?\d+|nullptr|true|false|[A-Z]\w*::\w+', term))


# ---- conditions ----------------------------------------------------------------------------------------

Formula = tuple  # ('or', [..]) ('and', [..]) ('not', f) ('atom', text) ('const', bool)


def parse(text: str) -> Formula:
    text = strip_parentheses(' '.join(text.split()))
    parts: list[str] = split_top(text, '||')
    if len(parts) > 1:
        return ('or', [parse(part) for part in parts])
    parts = split_top(text, '&&')
    if len(parts) > 1:
        return ('and', [parse(part) for part in parts])
    if text.startswith('!') and not text.startswith('!='):
        return ('not', parse(text[1:]))
    if text in ('true', 'false'):
        return ('const', text == 'true')
    for operator in ('!=', '=='):
        sides: list[str] = split_top(text, operator)
        if len(sides) == 2 and not re.search(r'[<>]=?$', sides[0]) and not sides[1].startswith('='):
            left: str = canon(sides[0])
            right: str = canon(sides[1])
            if left == right:
                return ('const', operator == '==')
            atom: Formula = ('atom', ' == '.join(sorted((left, right))))
            return ('not', atom) if operator == '!=' else atom
    return ('atom', canon(text))


def atoms_of(formula: Formula, into: set[str]) -> None:
    if formula[0] == 'atom':
        into.add(formula[1])
    elif formula[0] in ('or', 'and'):
        for part in formula[1]:
            atoms_of(part, into)
    elif formula[0] == 'not':
        atoms_of(formula[1], into)


def value_of(formula: Formula, truth: dict[str, bool]) -> bool:
    kind: str = formula[0]
    if kind == 'const':
        return bool(formula[1])
    if kind == 'atom':
        return truth[formula[1]]
    if kind == 'not':
        return not value_of(formula[1], truth)
    if kind == 'and':
        return all(value_of(part, truth) for part in formula[1])
    return any(value_of(part, truth) for part in formula[1])


def consistent(truth: dict[str, bool]) -> bool:
    """Equality's laws over the comparisons: equal terms form classes, which hold at most one constant and
    split no comparison taken false."""
    parent: dict[str, str] = {}

    def root(term: str) -> str:
        while parent.get(term, term) != term:
            term = parent[term]
        return term

    pairs: list[tuple[str, str, bool]] = []
    atom: str
    for atom, value in truth.items():
        sides: list[str] = atom.split(' == ')
        if len(sides) != 2:
            continue
        pairs.append((sides[0], sides[1], value))
        if value:
            parent[root(sides[0])] = root(sides[1])
    constants: dict[str, str] = {}
    term: str
    for term in {side for left, right, _ in pairs for side in (left, right)}:
        if constant(term):
            top: str = root(term)
            if top in constants and constants[top] != term:
                return False
            constants[top] = term
    return all(value or root(left) != root(right) for left, right, value in pairs)


def decide(goal: Formula, givens: list[Formula]) -> str:
    """'true' when the givens imply the goal, 'false' when they imply its negation, else 'unknown'; 'none' when
    the givens contradict each other."""
    atoms: set[str] = set()
    for formula in [goal] + givens:
        atoms_of(formula, atoms)
    names: list[str] = sorted(atoms)
    if len(names) > MAXIMUM_ATOMS:
        raise Unread('a condition over more than ' + str(MAXIMUM_ATOMS) + ' comparisons')
    seen: set[bool] = set()
    values: tuple[bool, ...]
    for values in itertools.product((False, True), repeat=len(names)):
        truth: dict[str, bool] = dict(zip(names, values))
        if not consistent(truth) or not all(value_of(given, truth) for given in givens):
            continue
        seen.add(value_of(goal, truth))
        if len(seen) == 2:
            return 'unknown'
    if not seen:
        return 'none'
    return 'true' if seen == {True} else 'false'


# ---- paths --------------------------------------------------------------------------------------------------

@dataclasses.dataclass
class Frame:
    names: dict[str, str]             # local or parameter -> its value in the call's opening terms
    aliases: dict[str, str]           # reference -> the location it names, in the call's opening terms
    born: dict[str, int]              # name -> how many writes had been made when it was given its value
    outputs: frozenset[str]           # the declared call's output parameters (writes to them are not the object)


@dataclasses.dataclass
class Path:
    frames: list[Frame]
    writes: dict[str, str]            # location (opening terms) -> value (opening terms)
    order: list[str]                  # the locations in the order written
    facts: list[Formula]              # what the path has taken to hold
    effects: list[str]                # what it did that a caller may observe, before it ended
    notes: list[str]                  # premises it took
    allowed: list[str]                # what it did that is not the object (outputs, checks)
    end: str = ''                     # 'return', 'stop', 'fall'
    value: str = ''                   # what it returned

    def fork(self) -> 'Path':
        return Path([Frame(dict(frame.names), dict(frame.aliases), dict(frame.born), frame.outputs)
                     for frame in self.frames], dict(self.writes), list(self.order), list(self.facts),
                    list(self.effects), list(self.notes), list(self.allowed), self.end, self.value)


class Reader:
    """Executes a call's body over every path, under a state's condition."""

    def __init__(self, view: View, members: set[str]) -> None:
        self.view: View = view
        self.members: set[str] = members    # the state conditions' locations
        self.read: list[str] = []
        self.depth: int = 0

    # -- terms in the call's opening terms ------------------------------------------------------------------

    def term(self, text: str, path: Path) -> str:
        """`text` (current terms) in the call's opening terms: names replaced by their values, then written
        locations by what was written."""
        frame: Frame = path.frames[-1]
        text = canon(text)

        def named(match: re.Match[str]) -> str:
            word: str = match.group(0)
            if word in frame.aliases:
                return '⟦' + frame.aliases[word] + '⟧'
            if word in frame.names:
                return '⟦' + frame.names[word] + '⟧'
            return word

        text = re.sub(r'(?<![\w.:>])(?<!\.)\b[A-Za-z_]\w*\b(?!\s*::)', named, text)
        pieces: list[str] = re.split(r'(⟦[^⟦⟧]*⟧)', text)
        out: list[str] = []
        piece: str
        for piece in pieces:
            if piece.startswith('⟦'):
                out.append('(' + piece[1:-1] + ')')
            else:
                out.append(self.written(piece, path))
        joined: str = canon(''.join(out))
        whole: str | None = path.writes.get(joined)
        return canon(whole) if whole is not None else self.written(joined, path, whole_only=True)

    def written(self, text: str, path: Path, whole_only: bool = False) -> str:
        if not path.writes:
            return text
        location: str
        for location in sorted(path.writes, key=len):
            if whole_only:
                continue
            pattern: str = r'(?<![\w.])' + re.escape(location) + r'(?![\w(])'
            text = re.sub(pattern, lambda match, value=path.writes[location]: '(' + value + ')', text)
        return text

    def stale(self, text: str, path: Path) -> bool:
        """Whether `text` uses a name given its value before a member it mentions was written."""
        frame: Frame = path.frames[-1]
        word: str
        for word in re.findall(r'\b[A-Za-z_]\w*\b', text):
            if word in frame.born:
                value: str = frame.aliases.get(word, frame.names.get(word, ''))
                later: list[str] = path.order[frame.born[word]:]
                if any(re.fullmatch(r'[A-Za-z_]\w*', location) and
                       re.search(r'(?<![\w.])' + re.escape(location) + r'\b', value) for location in later):
                    return True
        return False

    def location(self, text: str, path: Path) -> str:
        """The location an lvalue names, in opening terms (its parts substituted, the whole not)."""
        if self.stale(text, path):
            raise Unread('a write through a name given its value before the object changed: ' + text)
        frame: Frame = path.frames[-1]
        text = canon(text)
        match: re.Match[str] | None = re.fullmatch(r'([A-Za-z_]\w*)((?:\.\w+|\[[^\]]*\])*)', text)
        if match and match.group(1) in frame.aliases:
            return canon('(' + frame.aliases[match.group(1)] + ')' + match.group(2))
        if match and match.group(1) in frame.names and not match.group(2):
            return '§' + match.group(1)   # a local
        deref: re.Match[str] | None = re.fullmatch(r'\(\*([A-Za-z_]\w*)\)((?:\.\w+)*)', text)
        if deref and deref.group(1) in frame.names:
            return canon('(*' + frame.names[deref.group(1)] + ')' + deref.group(2))
        if deref and deref.group(1) in frame.aliases:
            return canon('(*' + frame.aliases[deref.group(1)] + ')' + deref.group(2))
        return self.written(text, path) if '(' in text else text

    def formula(self, text: str, path: Path) -> Formula:
        if self.stale(text, path):
            raise Unread('a condition on a name given its value before the object changed: ' + text)
        return self.simplified(parse(self.pure(self.term(text, path), path)), path)

    def simplified(self, formula: Formula, path: Path) -> Formula:
        """A comparison of a failure's `failed()` (or `ok()`) decided from the failure's text."""
        if formula[0] == 'atom':
            found: re.Match[str] | None = re.fullmatch(r'(.*)\.(failed|ok)\(\)', formula[1])
            if found and self.failing(found.group(1)) is not None:
                return ('const', (found.group(2) == 'failed') == self.failing(found.group(1)))
            return formula
        if formula[0] in ('or', 'and'):
            return (formula[0], [self.simplified(part, path) for part in formula[1]])
        if formula[0] == 'not':
            return ('not', self.simplified(formula[1], path))
        return formula

    def failing(self, text: str) -> bool | None:
        """True for a failure's text, False for a success's, None when unknown."""
        outer: re.Match[str] | None = re.match(r'[\w:<>]*::(failure|success)\(', text)
        if outer:
            return outer.group(1) == 'failure'
        expanded: str = self.expanded(text, 3)
        failure: bool = bool(re.search(r'::failure\(|\bfailed\(|\brefuse\w*\(', expanded))
        success: bool = bool(re.search(r'::success\(', expanded))
        if failure and not success:
            return True
        if success and not failure:
            return False
        return None

    def expanded(self, text: str, depth: int) -> str:
        """`text` followed by the bodies of the functions it calls, `depth` calls deep."""
        result: str = text
        frontier: str = text
        seen: set[str] = set()
        for _ in range(depth):
            added: str = ''
            name: str
            for name in re.findall(r'(?<![\w.>])([a-z_]\w*)\s*\(', frontier):
                if name in seen or name in NOT_CALLED:
                    continue
                seen.add(name)
                for function in functions_named(self.view, name)[:1]:
                    added = added + ' ' + function.body
            if not added:
                break
            result = result + ' ' + added
            frontier = added
        return result

    def pure(self, text: str, path: Path) -> str:
        """`text` with each call of a function the sources define, whose body yields one value without effects
        or forks, replaced by that value."""
        if self.depth > MAXIMUM_DEPTH:
            return text
        for _ in range(8):
            changed: bool = False
            match: re.Match[str]
            for match in re.finditer(r'(?<![\w.>:])((?:[A-Za-z_]\w*::)*)([a-z_]\w*)\s*\(', text):
                name: str = match.group(2)
                if name in NOT_CALLED:
                    continue
                close: int = matching(text, match.end() - 1, '(', ')')
                arguments: list[str] = split_top(text[match.end():close])
                value: str | None = self.value_of_call(name, arguments, path)
                if value is None:
                    continue
                text = text[:match.start()] + '(' + value + ')' + text[close + 1:]
                changed = True
                break
            if not changed:
                break
        return canon(text)

    def value_of_call(self, name: str, arguments: list[str], path: Path) -> str | None:
        functions: list[Function] = [item for item in functions_named(self.view, name)
                                     if len(item.parameters) == len(arguments)]
        if not functions:
            return None
        function: Function = functions[0]
        trial: Path = path.fork()
        trial.effects = []
        trial.end = ''
        try:
            self.depth = self.depth + 1
            ends: list[Path] = self.call(function, arguments, trial, True)
        except Unread:
            return None
        finally:
            self.depth = self.depth - 1
        if len(ends) != 1 or ends[0].effects or ends[0].end != 'return' or len(ends[0].facts) != len(path.facts):
            return None
        return ends[0].value

    # -- execution ----------------------------------------------------------------------------------------------

    def call(self, function: Function, arguments: list[str], path: Path, quiet: bool = False) -> list[Path]:
        """The paths through `function` called with `arguments` (current terms of the caller); each ends in
        'return' (value set), 'stop' or 'fall'."""
        if self.depth > MAXIMUM_DEPTH:
            raise Unread('calls nested deeper than ' + str(MAXIMUM_DEPTH))
        if function.path not in self.read:
            self.read.append(function.path)
        frame: Frame = Frame({}, {}, {}, path.frames[-1].outputs if len(path.frames) == 1 else frozenset())
        caller: Frame = path.frames[-1]
        outputs: set[str] = set()
        kind: str
        name: str
        argument: str
        for (kind, name), argument in zip(function.parameters, arguments):
            if kind.endswith('&') and not kind.startswith('const ') and canon(argument) in caller.outputs:
                outputs.add(name)
            if kind.endswith('&'):
                frame.aliases[name] = self.location(argument, path) if not argument.startswith('*') else \
                    canon('(' + self.term(argument, path) + ')')
                if frame.aliases[name].startswith('§'):
                    frame.names[name] = caller.names[frame.aliases[name][1:]]
                    del frame.aliases[name]
            else:
                frame.names[name] = self.pure(self.term(argument, path), path)
            frame.born[name] = len(path.order)
        if outputs:
            frame.outputs = frozenset(outputs)
        path.frames.append(frame)
        if function.noreturn:
            path.frames.pop()
            path.end = 'stop'
            return [path]
        ends: list[Path] = self.block(statements_of(function.body), [path])
        result: list[Path] = []
        for end in ends:
            end.frames.pop()
            if end.end in ('return', 'fall'):
                end.end = 'return' if end.end == 'return' else 'fall'
            result.append(end)
        return result

    def block(self, statements: list[Statement], paths: list[Path]) -> list[Path]:
        live: list[Path] = paths
        done: list[Path] = []
        statement: Statement
        for statement in statements:
            following: list[Path] = []
            for path in live:
                for after in self.statement(statement, path):
                    (done if after.end else following).append(after)
            live = following
            if len(live) + len(done) > MAXIMUM_PATHS:
                raise Unread('more than ' + str(MAXIMUM_PATHS) + ' paths')
            if not live:
                break
        for path in live:
            path.end = 'fall'
        return done + live

    def statement(self, statement: Statement, path: Path) -> list[Path]:
        if statement.kind == 'block':
            ends: list[Path] = self.block(statement.then, [path])
            for end in ends:
                if end.end == 'fall':
                    end.end = ''
            return ends
        if statement.kind == 'return':
            if statement.text:
                path.value = self.pure(self.term(statement.text, path), path) if not self.stale(statement.text,
                                                                                             path) else '?'
            path.end = 'return'
            return [path]
        if statement.kind == 'if':
            condition: Formula = self.formula(statement.condition, path)
            answer: str = decide(condition, path.facts)
            self.note_calls(statement.condition, path)
            if answer == 'true':
                return self.branch(statement.then, path)
            if answer == 'false':
                return self.branch(statement.otherwise, path)
            if answer == 'none':
                return []
            other: Path = path.fork()
            path.facts.append(condition)
            other.facts.append(('not', condition))
            return self.branch(statement.then, path) + self.branch(statement.otherwise, other)
        if statement.kind == 'loop':
            return [self.loop(statement.text, path)]
        return self.simple(statement.text, path)

    def branch(self, statements: list[Statement], path: Path) -> list[Path]:
        ends: list[Path] = self.block(statements, [path])
        for end in ends:
            if end.end == 'fall':
                end.end = ''
        return ends

    def loop(self, text: str, path: Path) -> Path:
        if re.search(r'\breturn\b|\bbreak\s*;\s*\}?\s*$', text) and re.search(r'\breturn\b', text):
            raise Unread('a loop that may return: ' + text[:60])
        member: str
        for member in self.member_names():
            if re.search(r'(?<![\w.])' + re.escape(member) + r'\b[^;=]*?(?<![=!<>])=(?!=)', text) or \
                    re.search(r'(?<![\w.])' + re.escape(member) + r'\b[\w.\[\]]*\.store\(', text):
                raise Unread('a loop writes ' + member)
        path.effects.append('a loop: ' + text[:60])
        path.notes.append('a loop that writes no state member: ' + ' '.join(text.split())[:70])
        return path

    def member_names(self) -> set[str]:
        names: set[str] = set()
        location: str
        for location in self.members:
            names.update(re.findall(r'\b[a-z_]\w*_\b', location))
            field: re.Match[str] | None = re.search(r'\.(\w+)$', location)
            if field:
                names.add(field.group(1))
        return names

    def note_calls(self, text: str, path: Path) -> None:
        name: str
        for name in re.findall(r'(?<![\w.>])([a-z_]\w*)\s*\(', text):
            if name not in NOT_CALLED and not functions_named(self.view, name):
                note: str = name + '() in a condition only reads'
                if note not in path.notes:
                    path.notes.append(note)

    def simple(self, text: str, path: Path) -> list[Path]:
        if text.startswith('__unread_preprocessor__'):
            raise Unread('a branch of #if on a condition this reader does not decide')
        if text.startswith('static_assert') or text.startswith('static_cast<void>(0)'):
            return [path]
        check: re.Match[str] | None = re.match(r'(STXLIB_REQUIRE|STXLIB_\w*CHECK\w*)\s*\(', text)
        if check:
            close: int = matching(text, check.end() - 1, '(', ')')
            condition: str = split_top(text[check.end():close])[0]
            formula: Formula = self.formula(condition, path)
            answer: str = decide(formula, path.facts)
            path.allowed.append(check.group(1) + ' (a check that ends the process or does nothing)')
            if answer == 'true':
                return [path]
            if answer == 'false':
                path.end = 'stop'
                return [path]
            if answer == 'none':
                return []
            stopped: Path = path.fork()
            path.facts.append(formula)
            stopped.facts.append(('not', formula))
            stopped.end = 'stop'
            return [path, stopped]
        if re.match(r'(::stxlib::|stxlib::)?fail_fast\s*\(', text):
            path.end = 'stop'
            return [path]
        store: re.Match[str] | None = re.fullmatch(r'(.+?)\.store\((.+)\)', text)
        if store:
            parts: list[str] = split_top(store.group(2))
            return self.assign(store.group(1), parts[0], path)
        assignment: re.Match[str] | None = re.fullmatch(r'([\w.\[\]()*>-]+?)\s*=\s*(.+)', text)
        if assignment and not re.match(r'(const|constexpr|auto)\b', text) and \
                not re.search(r'[\w>&*]\s+[A-Za-z_]\w*\s*$', assignment.group(1)):
            return self.assign(assignment.group(1), assignment.group(2), path)
        declaration: re.Match[str] | None = re.fullmatch(
            r'((?:const\s+|constexpr\s+|static\s+)*[A-Za-z_][\w:<>,\s]*?(?:\s*[&*]\s*(?:const\s+)?|\s+))'
            r'([A-Za-z_]\w*)\s*(?:=\s*(.+)|(\((.*)\))|(\{(.*)\}))?', text)
        if declaration and not re.fullmatch(r'\s*(return|delete|throw|else)\s*', declaration.group(1)) and \
                declaration.group(1).strip() not in ('', 'const'):
            return self.declare(declaration.group(1).strip(), declaration.group(2),
                                declaration.group(3) or '', declaration.group(5) or declaration.group(7) or '',
                                path)
        call: re.Match[str] | None = whole_call(text)
        if call:
            return self.statement_call(call.group(1), call.group(3), split_top(text[call.end():-1]), text, path)
        raise Unread('a statement this reader does not follow: ' + text[:70])

    def assign(self, target: str, value: str, path: Path) -> list[Path]:
        where: str = self.location(target, path)
        if where.startswith('§'):
            frame: Frame = path.frames[-1]
            frame.names[where[1:]] = self.pure(self.term(value, path), path)
            frame.born[where[1:]] = len(path.order)
            return [path]
        new: str = self.pure(self.term(value, path), path)
        output: bool = any(re.fullmatch(r'\(?' + re.escape(name) + r'\)?(\.\w+)*', canon(target)) for name in
                           path.frames[-1].outputs) or where in path.frames[0].outputs
        path.writes[where] = new
        path.order.append(where)
        if output:
            path.allowed.append('the output parameter ' + canon(target) + ' is written (an output, not the object)')
        else:
            path.effects.append('writes ' + where)
        return [path]

    def declare(self, kind: str, name: str, value: str, arguments: str, path: Path) -> list[Path]:
        frame: Frame = path.frames[-1]
        if kind.rstrip().endswith('&') and value:
            target: str = value.strip()
            frame.aliases[name] = canon('(' + self.term(target, path) + ')') if target.startswith('*') else \
                self.location(target, path)
            frame.born[name] = len(path.order)
            return [path]
        initial: str = value or arguments
        call: re.Match[str] | None = whole_call(initial.strip()) if value else None
        if call and not call.group(1) and functions_named(self.view, call.group(3)) and \
                call.group(3) not in NOT_CALLED:
            inner: str = initial.strip()[call.end():-1]
            ends: list[Path] = self.statement_call('', call.group(3), split_top(inner), initial, path)
            for end in ends:
                if end.end == '':
                    end.frames[-1].names[name] = end.value or self.pure(self.term(initial, end), end)
                    end.frames[-1].born[name] = len(end.order)
                    end.value = ''
            return ends
        built: bool = (self.touches(initial, path) if whole_call(initial.strip()) else
                       bool(re.search(r'\b\w+_\b|\bthis\b', initial))) if initial and not value else False
        if built:
            path.effects.append('a local built from the object: ' + name)
        frame.names[name] = self.pure(self.term(initial, path), path) if initial else name + '§'
        frame.born[name] = len(path.order)
        return [path]

    def statement_call(self, receiver: str, name: str, arguments: list[str], text: str, path: Path) -> list[Path]:
        functions: list[Function] = [item for item in functions_named(self.view, name)
                                     if len(item.parameters) == len(arguments)] if not receiver else []
        if functions:
            function: Function = functions[0]
            snapshot: Path = path.fork()
            try:
                self.depth = self.depth + 1
                ends: list[Path] = self.call(function, arguments, path)
            except Unread:
                if self.touches(text, snapshot):
                    raise
                ends = [self.opaque(text, snapshot)]
            finally:
                self.depth = self.depth - 1
            result: list[Path] = []
            for end in ends:
                if end.end == 'fall':
                    end.end = 'return'
                    end.value = ''
                result.append(end)
            stops: list[Path] = [end for end in result if end.end == 'stop']
            for end in result:
                if end.end == 'return':
                    end.end = ''
            return [end for end in result if end.end != 'stop'] + stops
        return [self.opaque(text, path)]

    def opaque(self, text: str, path: Path) -> Path:
        """A call this reader does not enter: it must not touch the object, unless the declaration says so."""
        flat: str = canon(text)
        why: str
        for call_text, why in self.view.untouching:
            if call_text in flat:
                note: str = call_text + ' changes no state member (' + why + ')'
                if note not in path.notes:
                    path.notes.append(note)
                path.effects.append('calls ' + flat[:60])
                return path
        if self.touches(text, path):
            raise Unread('a call this reader does not enter touches the object: ' + flat.split('(')[0] + '(...)')
        path.effects.append('calls ' + flat[:60])
        return path

    def touches(self, text: str, path: Path) -> bool:
        """Whether a call's receiver or arguments name a state member, the object (`this`), or the whole of
        something a state member is a field of."""
        frame: Frame = path.frames[-1]
        flat: str = canon(text)
        match: re.Match[str] | None = whole_call(flat)
        if match is None:
            return True
        pieces: list[str] = split_top(flat[match.end():-1]) + ([match.group(1)[:-1]] if match.group(1) else [])
        piece: str
        for piece in pieces:
            piece = canon(piece.lstrip('&'))
            if piece in ('this', '*this'):
                return True
            if self.stale(piece, path):
                continue
            resolved: str = canon(self.term(piece, path)) if re.fullmatch(r'[\w.()*\[\]]+', piece) else piece
            for member in self.members:
                if resolved == member or member.startswith(resolved + '.') or \
                        member.startswith('(' + resolved + ').') or ('(*' + resolved + ')') == member.split('.')[0]:
                    return True
            if re.fullmatch(r'[\w.()*\[\]]+', piece) and piece in frame.aliases and \
                    any(member.startswith(canon('(' + frame.aliases[piece] + ')') + '.') or
                        member.startswith(frame.aliases[piece] + '.') for member in self.members):
                return True
        return False


# ---- the three kinds ----------------------------------------------------------------------------------------

@dataclasses.dataclass
class Outcome:
    held: bool
    steps: list[str]
    premises: list[str]
    read: list[str]
    why: str


def locations_of(view: View) -> set[str]:
    """The locations the state conditions read."""
    found: set[str] = set()
    condition: str
    for condition in view.states.values():
        atoms: set[str] = set()
        atoms_of(parse(condition), atoms)
        for atom in atoms:
            for side in atom.split(' == '):
                if not constant(side) and re.fullmatch(r'(\(\*\w+\)|\w+_)(\.\w+)*', side):
                    found.add(side)
    return found


def opening(view: View, state: str | None, outputs: frozenset[str], reader: 'Reader') -> tuple[Path, list[str]]:
    path: Path = Path([Frame({}, {}, {}, outputs)], {}, [], [], [], [], [])
    facts: list[Formula] = [normal_formula(reader, condition, path) for condition, _ in view.assume]
    premises: list[str] = ['assumed: ' + condition + ' (' + why + ')' for condition, why in view.assume]
    if state is not None:
        facts.append(normal_formula(reader, view.states[state], path))
        premises.insert(0, 'the state ' + state + ' is ' + view.states[state] + ' (the declaration\'s represents)')
    path.facts = facts
    return path, premises


def normal_formula(reader: 'Reader', condition: str, path: Path) -> Formula:
    """A declared condition spelled as the reader spells the code's: calls the sources define as one value
    replaced by it."""
    return parse(reader.pure(canon(condition), path))


def entry(view: View, name: str) -> Function:
    functions: list[Function] = [item for item in functions_named(view, name) if item.qualifier == view.owner]
    if not functions:
        raise Unread('no definition of ' + view.owner + '::' + name + ' in ' + ', '.join(view.texts))
    if len({(item.path, item.line) for item in functions}) > 1 and \
            len({len(item.parameters) for item in functions}) > 1:
        raise Unread(view.owner + '::' + name + ' is overloaded')
    return functions[0]


def outputs_of(function: Function) -> frozenset[str]:
    return frozenset(name for kind, name in function.parameters if kind.endswith('&') and
                     not kind.startswith('const '))


def run(view: View, name: str, state: str | None) -> tuple[list[Path], Function, list[str], Reader]:
    function: Function = entry(view, name)
    reader: Reader = Reader(view, locations_of(view))
    path: Path
    premises: list[str]
    path, premises = opening(view, state, outputs_of(function), reader)
    if decide(('const', True), path.facts) == 'none':
        raise Unread('the state ' + str(state) + ' contradicts the declaration\'s assumptions')
    frame: Frame = path.frames[0]
    kind: str
    parameter: str
    for kind, parameter in function.parameters:
        frame.names[parameter] = parameter
        frame.born[parameter] = 0
    path.frames[0].outputs = outputs_of(function)
    ends: list[Path] = reader.block(statements_of(function.body), [path])
    if function.path not in reader.read:
        reader.read.insert(0, function.path)
    return ends, function, premises, reader


def refusal_kind(reader: Reader, value: str, refusal: object, codes: str,
                 mapping: Callable[[list[str], int, str], tuple[list[str], list[str]]]) -> tuple[bool, str]:
    """Whether a returned value is the declared refusal; how it was seen."""
    kind: str = getattr(refusal, 'kind')
    if kind == 'value':
        wanted: str = canon(getattr(refusal, 'value'))
        return canon(value) == wanted, 'returns ' + canon(value)
    if kind != 'failure':
        return False, 'returns where the declaration says ' + kind
    if reader.failing(value) is not True:
        return False, 'returns ' + value[:60] + ', not seen to be a failure'
    failure: str = getattr(refusal, 'failure')
    code: str = getattr(refusal, 'code')
    code_type: str = codes.split('::')[-1] if codes else 'HostCode'
    level: str = value
    text: str = value
    seen: set[str] = set()
    for _ in range(4):
        kinds: set[str] = set(re.findall(r'ErrorKind::(\w+)', text))
        if len(kinds) == 1:
            found_codes: set[str] = set(re.findall(r'\b' + code_type + r'::(\w+)', text)) - {'none'}
            if failure not in ('any',) and kinds != {failure}:
                return False, 'names ErrorKind::' + next(iter(kinds))
            if code and code not in found_codes:
                return False, 'names no ' + code_type + '::' + code
            return True, 'names ' + ', '.join(['ErrorKind::' + item for item in kinds] +
                                              ([code_type + '::' + code] if code else []))
        if len(kinds) > 1:
            break
        added: str = ''
        name: str
        for name in re.findall(r'(?<![\w.>])([a-z_]\w*)\s*\(', level):
            if name in seen or name in NOT_CALLED:
                continue
            seen.add(name)
            for function in functions_named(reader.view, name)[:1]:
                added = added + ' ' + function.body
        if not added:
            break
        level = added
        text = added
    statuses: list[tuple[str, str]] = [item for item in re.findall(r'\b([A-Z]\w*)::([a-z_]\w*)\b',
                                                                   reader.expanded(value, 3))
                                       if item[0] not in STATUS_TYPES and item[0] != code_type]
    for enum, item in dict.fromkeys(statuses):
        path: str
        text_of: str
        for path, text_of in reader.view.texts.items():
            lines: list[str] = text_of.split('\n')
            index: int
            for index, line in enumerate(lines):
                if re.match(r'\s*case\s+(?:[\w:]*::)?' + enum + '::' + item + r'\s*:', line):
                    kinds_found, codes_found = mapping(lines, index, codes)
                    if failure != 'any' and kinds_found != [failure]:
                        continue
                    if code and codes_found != [code]:
                        continue
                    return True, enum + '::' + item + ' maps to ' + ', '.join(
                        ['ErrorKind::' + kind_found for kind_found in kinds_found] +
                        [code_type + '::' + code_found for code_found in codes_found]) + ' (' + path + ':' + \
                        str(index + 1) + ')'
    if failure == 'any':
        return True, 'returns a failure'
    return False, 'the failure returned is not seen to be ' + failure + (' (' + code + ')' if code else '')


def typestate_refusal(view: View, name: str, state: str, refusal: object, codes: str,
                      mapping: Callable[[list[str], int, str], tuple[list[str], list[str]]]) -> Outcome:
    try:
        ends: list[Path]
        function: Function
        premises: list[str]
        reader: Reader
        ends, function, premises, reader = run(view, name, state)
    except Unread as error:
        return Outcome(False, [], [], [], host_refusal(str(error)))
    steps: list[str] = []
    notes: list[str] = []
    problems: list[str] = []
    end: Path
    for number, end in enumerate(ends, 1):
        label: str = 'path ' + str(number) + (' (taking ' + '; '.join(render(fact) for fact in end.facts[
            len(view.assume) + 1:]) + ')' if len(end.facts) > len(view.assume) + 1 else '')
        if end.effects:
            problems.append(label + ' changes the object first: ' + '; '.join(end.effects[:3]))
            continue
        if getattr(refusal, 'kind') == 'requirement':
            if end.end != 'stop':
                problems.append(label + ' does not end the process (it ' + (end.end or 'falls through') + ')')
                continue
            steps.append(label + ': the process ends (a requirement)')
        else:
            if end.end != 'return':
                problems.append(label + ' ' + ('ends the process' if end.end == 'stop' else 'falls through') +
                                ' instead of refusing')
                continue
            held, how = refusal_kind(reader, end.value, refusal, codes, mapping)
            if not held:
                problems.append(label + ': ' + how)
                continue
            steps.append(label + ': ' + how)
        for note in end.notes + end.allowed:
            if note not in notes:
                notes.append(note)
    if problems:
        return Outcome(False, steps, [], reader.read, '; '.join(problems))
    steps.insert(0, view.owner + '::' + name + ' read at ' + function.path + ':' + str(function.line) +
                 ' in the state ' + state + ' (' + view.states[state] + ')' +
                 (' with ' + ', '.join(view.build) + ' defined' if view.build else ''))
    return Outcome(True, steps, premises + notes + common_premises(), reader.read, '')


def host_refusal(why: str) -> str:
    """The reason, said as where the refusal would have to come from when the code reaches the host first."""
    opaque: re.Match[str] | None = re.match(r'a call this reader does not enter touches the object: (.*)', why)
    if opaque:
        return ('no refusal in the code before ' + opaque.group(1) + ': the declared refusal would be the host\'s, '
                'from inside that call, which the deducer does not read')
    if why.startswith('a loop that may return'):
        return ('no refusal in the code before its loop: the declared refusal would be the host\'s, from the calls '
                'inside the loop, which the deducer does not read (' + why + ')')
    return why


def render(formula: Formula) -> str:
    kind: str = formula[0]
    if kind == 'atom':
        return formula[1]
    if kind == 'const':
        return 'true' if formula[1] else 'false'
    if kind == 'not':
        inner: Formula = formula[1]
        if inner[0] == 'atom' and ' == ' in inner[1]:
            return inner[1].replace(' == ', ' != ')
        return '!(' + render(inner) + ')'
    return (' && ' if kind == 'and' else ' || ').join('(' + render(part) + ')' for part in formula[1])


def common_premises() -> list[str]:
    return ['a member is named with a trailing underscore (AGENTS.md), so a call whose receiver and arguments '
            'name none, nor this, nor something a state member is a field of, does not change the state',
            'a function called inside a condition only reads',
            'atomic loads and stores read as plain ones: one thread acts on the object at a time, as the '
            'declaration\'s thread rule says']


def transition(view: View, name: str, admitting: tuple[str, ...], moves: str) -> Outcome:
    states: list[str | None] = list(admitting) if admitting else [None]
    steps: list[str] = []
    notes: list[str] = []
    premises: list[str] = []
    read: list[str] = []
    problems: list[str] = []
    succeeds: str = view.succeeds.get(name, '')
    goal_text: str = view.states[moves]
    state: str | None
    for state in states:
        try:
            ends: list[Path]
            function: Function
            taken: list[str]
            reader: Reader
            ends, function, taken, reader = run(view, name, state)
        except Unread as error:
            return Outcome(False, [], [], read, str(error))
        for item in taken:
            if item not in premises:
                premises.append(item)
        read.extend(item for item in reader.read if item not in read)
        where: str = 'in ' + state if state else 'in any state'
        counted: int = 0
        for number, end in enumerate(ends, 1):
            label: str = where + ', path ' + str(number)
            if end.end == 'stop':
                continue
            success: str = success_of(reader, end, succeeds)
            if success == 'false':
                continue
            counted = counted + 1
            goal: Formula = parse(reader.pure(reader.term(reader.pure(canon(goal_text), end), end), end))
            answer: str = decide(goal, end.facts)
            if answer != 'true':
                problems.append(label + (' (a success that may be a failure)' if success == 'unknown' else '') +
                                ': ' + moves + ' (' + goal_text + ') is ' + ('not shown' if answer == 'unknown'
                                                                             else answer) + ' after it')
                continue
            steps.append(label + ': ' + ('returns ' + end.value[:50] if end.value else 'returns') + '; after it '
                         + goal_text + ' holds' + (' (writes: ' + ', '.join(
                             location + ' = ' + end.writes[location] for location in end.order
                             if not location.startswith('§'))[:160] + ')' if end.order else ''))
            for note in end.notes:
                if note not in notes:
                    notes.append(note)
        if counted == 0:
            problems.append(where + ': no path succeeds')
    if problems:
        return Outcome(False, steps, [], read, '; '.join(problems))
    steps.insert(0, view.owner + '::' + name + ' read in ' + ', '.join(read[:2]) +
                 (' with ' + ', '.join(view.build) + ' defined' if view.build else ''))
    return Outcome(True, steps, premises + notes + common_premises() +
                   (['success is ' + succeeds + ' (the declaration\'s represents.succeeds)'] if succeeds else
                    ['success is every return not seen to be a failure']), read, '')


def success_of(reader: Reader, end: Path, succeeds: str) -> str:
    if succeeds:
        if not end.value:
            return 'unknown'
        condition: str = re.sub(r'\bresult\b', '(' + end.value + ')', succeeds)
        return decide(reader.simplified(parse(condition), end), end.facts)
    if not end.value:
        return 'true'
    failing: bool | None = reader.failing(end.value)
    if failing is True:
        return 'false'
    for method, means in (('failed', True), ('ok', False)):
        answer: str = decide(('atom', canon('(' + end.value + ').' + method + '()')), end.facts)
        if answer == ('true' if means else 'false'):
            return 'false'
    return 'true' if failing is False else 'unknown'


def moved_from(view: View, stale: str) -> Outcome:
    """Moving from the object (its move constructor and move assignment) leaves it in `stale`."""
    goal_text: str = view.states[stale]
    steps: list[str] = []
    read: list[str] = []
    problems: list[str] = []
    found: int = 0
    for name in (view.owner, 'operator='):
        functions: list[Function] = [item for item in functions_named(view, name)
                                     if item.qualifier == view.owner and len(item.parameters) == 1 and
                                     item.parameters[0][0].replace(' ', '') == view.owner + '&&']
        for function in functions:
            found = found + 1
            other: str = function.parameters[0][1]
            writes: dict[str, str] = {}
            statement: Statement
            for statement in statements_of(function.body):
                if statement.kind != 'simple':
                    continue
                assigned: re.Match[str] | None = re.fullmatch(re.escape(other) + r'\.(\w+)\s*=\s*(.+)', statement.text)
                if assigned:
                    writes[assigned.group(1)] = canon(assigned.group(2))
            goal: str = goal_text
            for member, value in writes.items():
                goal = re.sub(r'(?<![\w.])' + re.escape(member) + r'\b', '(' + value + ')', goal)
            answer: str = decide(parse(goal), [parse(condition) for condition, _ in view.assume])
            label: str = view.owner + '::' + name + '(' + view.owner + '&&) at ' + function.path + ':' + \
                str(function.line)
            if function.path not in read:
                read.append(function.path)
            if answer != 'true':
                problems.append(label + ' does not leave ' + other + ' in ' + stale)
                continue
            steps.append(label + ' writes ' + ', '.join(other + '.' + member + ' = ' + value for member, value in
                                                        writes.items()) + ': ' + other + ' is then ' + stale)
    if found == 0:
        problems.append('no move constructor or move assignment of ' + view.owner + ' read')
    return Outcome(not problems, steps, [], read, '; '.join(problems))
