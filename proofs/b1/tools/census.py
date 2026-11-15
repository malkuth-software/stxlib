#!/usr/bin/env python3
"""census.py: the construct census of B1 section 5.3, per lifted function.

For each row's treatment harness, the code under test is lifted as the
lifter does (tools/lift.py) but without any edit (no substitution, contract
or mutant: the census describes the source, not the environment), the
#line directives are blanked so clang's locations are the unit's own lines,
and the pinned clang (22.1.8) dumps the AST as JSON:

  clang++ -std=c++20 -fsyntax-only -Xclang -ast-dump=json <defines> <includes> unit.cpp

Only nodes whose location falls on a lifted line are counted, and each is
attributed to the lifted function that contains it (or to "<namespace
scope>"). Template patterns are counted once; implicit instantiations are
skipped (they repeat the pattern's lines). The counts are mechanical rules
over the AST, stated with each construct below, so the census is
reproducible; where a rule is syntactic only (a loop's bound class, a
fall-through) the record says so.

Constructs (B1.md section 5.3):
  C1  loops: for, while, do, range-for; nesting depth; bound class by
      syntax (constant: the condition compares with an integer literal;
      other: anything else)
  C2  recursion: direct, or a cycle among the row's lifted functions
  C3  pointer arithmetic (+, - on a pointer operand), raw-pointer indexing,
      reinterpret_cast, memcpy (type punning)
  C4  possible aliasing: pairs of pointer or reference parameters with the
      same pointee type, at least one not const; out-parameters (non-const
      pointer or reference parameters)
  C5  side effects in expressions: (a) assignment, compound assignment,
      ++ and -- outside statement position; (b) calls nested in a larger
      expression (statement position, a declaration's whole initialiser,
      a return's whole operand and the whole right side of a statement
      assignment are not nested)
  C6  indirect calls: through a function pointer or a virtual method
  C7  dynamic allocation: new, delete, malloc family; declarations of
      allocating standard types (strings, containers, owning pointers)
  C8  exceptions: throw, try, catch
  C9  templates: patterns defined in the span; calls of template
      specialisations; template types in declarations
  C10 standard-library calls: callees declared outside stx_auxlib and the
      harness, by name
  C11 floating point: floating literals, operations and conversions;
      changes of the floating-point environment (fesetround, fesetenv,
      feupdateenv, feraiseexcept, feclearexcept, fesetexceptflag,
      feholdexcept)
  C12 concurrency: atomic operations, memory orders named, lock, thread
      and condition-variable types
  C13 compiler extensions: __builtin calls and vector builtins, vector
      types, __int128, non-standard attributes, inline assembly
  C14 control transfers: goto, switch, case fall-through (syntactic),
      return count, break, continue, variadic functions
  C15 address of a local or parameter taken with &; escapes (a return of
      &local, of a local object where the function returns a reference,
      of a local array as a pointer, of an owning local as a view; &local
      stored into a non-local lvalue; and clang's -Wdangling,
      -Wreturn-stack-address and -Wdangling-gsl warnings on lifted lines);
      &local passed to a call (escapes only if the callee keeps it, which
      the census cannot see); static locals; mutable namespace-scope
      variables
  C16 value-changing integer conversions (explicit and implicit, by the
      LP64 widths of arm64 macOS); functions marked no_sanitize
      (STX_AUXLIB_MODULAR: intended wraparound)
  C17 volatile
  C18 member functions, constructors and destructors defined; overloaded
      operator calls; lambdas; local variables of class type
  C19 mutation of inputs (house style: read-only by type; views are
      read-only by default and writing needs an explicitly mutable type):
      an input that crosses as a read-only type and is written (const
      removed by const_cast or a C-style cast on a parameter: distance);
      explicitly mutable parameters (pointer or reference to non-const,
      span of non-const) written through (assignment, ++/--, compound or
      operator assignment through *, [], -> or the reference; non-const
      member calls) or passed on: permitted; parameters of such types
      and non-const locals never written: advisory, as clang-tidy's
      misc-const-correctness reports them (never fails). Syntactic: a
      local is taken as written when assigned, its address is taken, it
      is bound to a non-const reference or passed as a bare lvalue, or a
      non-const member is called on it; captures by lambdas are not
      followed, so an advisory entry can be a false report.

Dialect columns (B1-5 = 1) and house style (owner direction of
2026-11-13, decisions/2026-11-13-locals-float-mutability.md). Each entry
carries a status: "distance" (counts), "permitted by house style"
(recorded, does not count), "advisory" (misc-const-correctness: reported,
never counted), "recorded (not decidable from the AST)" or "tool gap".
  clight:      C5a, C5b, C8 (distance)
  strictc:     C14 goto, fall-through, variadic; C6 function pointers
               (distance); C15 address-of and static locals, C11 floating
               point (permitted by house style)
  cpp-verify:  C18 members, lambdas and struct return types, C9 templates
               defined in the code (not library template uses), C8, C6
               virtual, C14 switch, C1 range-for, C7 containers, C11
               (status "tool gap": B1-5 puts the tool subsets on the tool
               side; measured from cpp-verify's behaviour, so the
               release notes' "templates" is read as the code's own)
  house-style: C15 escaping local address, C11 environment change and
               C19 write to a read-only input (distance); C19 writes
               through explicitly mutable types (permitted); C19 never
               written but not const (advisory); C15 &local passed to a
               call (recorded, not decidable from the AST)
Results outside a function's stated domain (unannounced NaN or infinity)
are a proof property, not a census item. The summary counts "distance"
entries only.

Usage:
  census.py --sources sources.json --out OUT [--rows 03,07] [--clang clang++]
            [--rows-directory D] [--source KEY]
  census.py --sources sources.json --out OUT --source stxlib --tree [--exclude P,Q]
  The second form is the distance of STXlib's own code, run at every fold
  (coordinator's amendment of 2026-11-13): every file under include/ and
  src/ is one unit (#line-labelled as the sources key), compiled with
  -I include -I src, and every function it defines is counted. Paths under
  --exclude (for example src/hosted/windows on macOS) are left out and
  listed by the caller with the reason.
Writes OUT/census/<row>.json and OUT/census/summary.txt; keeps each AST
dump's stderr in OUT/census/<row>.clang.txt. The dump itself is not kept.
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
B1 = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lift  # noqa: E402

# The sources key whose lifted lines are counted: stx_auxlib for B1's rows,
# stxlib for phase D's rows and the tree census (--source, --tree).
SOURCE_KEY = ['stx_auxlib']


def strip_source(label):
    return label[len(SOURCE_KEY[0]) + 1:]


FUNCTION_KINDS = {'FunctionDecl', 'CXXMethodDecl', 'CXXConstructorDecl', 'CXXDestructorDecl',
                  'CXXConversionDecl'}
LOOP_KINDS = {'ForStmt', 'WhileStmt', 'DoStmt', 'CXXForRangeStmt'}
CALL_KINDS = {'CallExpr', 'CXXMemberCallExpr'}
DEPENDENT_CALLEES = {'UnresolvedLookupExpr', 'UnresolvedMemberExpr', 'CXXDependentScopeMemberExpr',
                     'DependentScopeDeclRefExpr', 'CXXUnresolvedConstructExpr'}
WRAPPERS = {'ImplicitCastExpr', 'ParenExpr', 'ExprWithCleanups', 'MaterializeTemporaryExpr',
            'CXXBindTemporaryExpr', 'ConstantExpr'}
STANDARD_ATTRIBUTES = {'WarnUnusedResultAttr', 'CXX11NoReturnAttr', 'C11NoReturnAttr', 'FallThroughAttr',
                       'LikelyAttr', 'UnlikelyAttr', 'NoUniqueAddressAttr', 'DeprecatedAttr', 'UnusedAttr',
                       'AlignedAttr', 'FinalAttr', 'OverrideAttr'}
ALLOCATING_TYPE = re.compile(r'basic_string|\bvector<|\bdeque<|\blist<|\bmap<|\bset<|unordered_|unique_ptr<|'
                             r'shared_ptr<|function<')
CONTAINER_TYPE = re.compile(r'basic_string|\bvector<|\bdeque<|\blist<|\bmap<|\bset<|unordered_')
CONCURRENCY_TYPE = re.compile(r'\bmutex\b|condition_variable|\bthread\b|unique_lock|lock_guard|atomic<|'
                              r'pthread_')
ATOMIC_OPERATIONS = {'load', 'store', 'exchange', 'fetch_add', 'fetch_sub', 'fetch_and', 'fetch_or',
                     'fetch_xor', 'compare_exchange_weak', 'compare_exchange_strong', 'wait', 'notify_one',
                     'notify_all', 'test_and_set', 'clear'}
WIDTHS = {'bool': (1, False), 'char': (8, True), 'signed char': (8, True), 'unsigned char': (8, False),
          'char8_t': (8, False), 'short': (16, True), 'unsigned short': (16, False), 'char16_t': (16, False),
          'int': (32, True), 'unsigned int': (32, False), 'char32_t': (32, False), 'wchar_t': (32, True),
          'long': (64, True), 'unsigned long': (64, False), 'long long': (64, True),
          'unsigned long long': (64, False), '__int128': (128, True), 'unsigned __int128': (128, False)}


def type_text(node):
    kind = node.get('type') or {}
    return kind.get('desugaredQualType') or kind.get('qualType') or ''


def bare(text):
    return re.sub(r'\b(const|volatile)\b', '', text).strip()


def integer_width(text):
    return WIDTHS.get(re.sub(r'\s+', ' ', bare(text)))


def fits(value, width):
    try:
        number = int(str(value), 0)
    except ValueError:
        return False
    bits, signed = width
    return (-(1 << (bits - 1)) <= number < (1 << (bits - 1))) if signed else (0 <= number < (1 << bits))


def is_pointer(text):
    return bool(re.search(r'\*\s*(const\s*)?(volatile\s*)?$', text))


def strip_wrappers(node):
    while node and node.get('kind') in ('ImplicitCastExpr', 'ParenExpr', 'ExprWithCleanups',
                                        'MaterializeTemporaryExpr', 'CXXBindTemporaryExpr'):
        inner = node.get('inner') or []
        node = inner[0] if inner else None
    return node


class Unit:
    """A lifted unit with blanked #line directives and its line map."""

    def __init__(self, text):
        self.map = {}
        lines = text.split('\n')
        current = None
        out = []
        for number, line in enumerate(lines, start=1):
            match = re.match(r'#line (\d+) "([^"]+)"', line)
            if match:
                first = int(match.group(1))
                label = match.group(2)
                current = (label, first) if label.startswith(SOURCE_KEY[0] + '/') else None
                out.append('')
                continue
            if current is not None:
                label, first = current
                self.map[number] = (label, first)
                current = (label, first + 1)
            out.append(line)
        self.text = '\n'.join(out)

    def original(self, line):
        return self.map.get(line)


def empty_record():
    return {
        'C1': {'loops': 0, 'range_for': 0, 'max_depth': 0, 'constant_bound': 0, 'other_bound': 0, 'lines': []},
        'C2': {'recursive': False, 'cycle': []},
        'C3': {'pointer_arithmetic': 0, 'raw_indexing': 0, 'reinterpret_cast': 0, 'memcpy': 0, 'lines': []},
        'C4': {'alias_pairs': 0, 'out_parameters': 0, 'parameters': []},
        'C5': {'effects_in_expressions': 0, 'nested_calls': 0, 'lines_a': [], 'lines_b': []},
        'C6': {'function_pointer_calls': 0, 'virtual_calls': 0, 'lines': []},
        'C7': {'new': 0, 'delete': 0, 'malloc_family': 0, 'allocating_declarations': 0,
               'container_declarations': 0, 'lines': []},
        'C8': {'throw': 0, 'try': 0, 'catch': 0, 'lines': []},
        'C9': {'patterns': 0, 'template_calls': [], 'own_template_calls': [], 'template_types': [], 'lines': []},
        'C10': {'calls': {}},
        'C11': {'literals': 0, 'operations': 0, 'conversions': 0, 'environment_changes': 0, 'lines': [],
                'environment_lines': []},
        'C12': {'atomic_operations': 0, 'memory_orders': [], 'concurrency_declarations': 0, 'lines': []},
        'C13': {'builtin_calls': [], 'vector_types': 0, 'int128': 0, 'attributes': [], 'inline_asm': 0,
                'lines': []},
        'C14': {'goto': 0, 'switch': 0, 'fall_through': 0, 'returns': 0, 'break': 0, 'continue': 0,
                'variadic': False, 'lines': []},
        'C15': {'address_of_local': 0, 'escaping': 0, 'passed_to_call': 0, 'static_locals': 0,
                'mutable_globals': 0, 'lines': [], 'escaping_lines': []},
        'C16': {'explicit_value_changing': 0, 'implicit_value_changing': 0, 'modular': False, 'lines': []},
        'C17': {'volatile': 0, 'lines': []},
        'C18': {'members_defined': 0, 'operator_calls': 0, 'lambdas': 0, 'class_locals': 0, 'class_returns': 0,
                'lines': []},
        'C19': {'written': [], 'passed_on': [], 'read_only_written': 0, 'advisory_parameters': [],
                'advisory_locals': [], 'lines': [], 'read_only_lines': []},
    }


class Census:
    def __init__(self, unit, unit_path, auxlib_root):
        self.unit = unit
        self.unit_path = unit_path
        self.auxlib_root = os.path.realpath(auxlib_root)
        self.last_file = None
        self.last_line = None
        self.decl_file = {}       # function decl id -> file
        self.decl_name = {}       # function decl id -> key (mangled or qualified name)
        self.decl_virtual = set()
        self.decl_template = set()
        self.records = {}
        self.calls = {}           # function key -> set of callee keys
        self.function_keys = {}   # function key -> display name
        self.local_ids = set()
        self.local_types = {}     # local decl id -> type text
        self.parameters = {}      # parameter decl id -> (function display, name, type)
        self.const_methods = set()
        self.written_ids = set()  # locals and parameters possibly written (directly)
        self.local_decls = {}     # local VarDecl id -> (function display, name, type)
        self.line_function = {}   # lifted line -> function display
        self.record_scope = {}    # record decl id -> qualified name (out-of-line members, defect 3)
        self.record_names = set() # bare names of class and class-template declarations
        self.enum_names = set()   # bare names of enumerations

    # Location tracking, in the dump's key order (fields are elided when
    # they repeat the previous location's).
    def update(self, location):
        if not location:
            return None
        if 'spellingLoc' in location or 'expansionLoc' in location:
            self.update(location.get('spellingLoc'))
            return self.update(location.get('expansionLoc'))
        if 'file' in location:
            self.last_file = location['file']
        if 'line' in location:
            self.last_line = location['line']
        if 'offset' not in location:
            return None
        return self.last_file, self.last_line

    def place(self, node):
        found = None
        for key in node:
            if key == 'loc':
                found = self.update(node['loc']) or found
            elif key == 'range':
                begin = self.update(node['range'].get('begin'))
                self.update(node['range'].get('end'))
                if found is None:
                    found = begin
        return found

    def lifted_line(self, where):
        if where is None:
            return None
        path, line = where
        if path is None or os.path.realpath(path) != os.path.realpath(self.unit_path):
            return None
        original = self.unit.original(line)
        if original is None:
            return None
        return f'{strip_source(original[0])}:{original[1]}'

    def record(self, function):
        if function not in self.records:
            self.records[function] = empty_record()
        return self.records[function]

    def walk(self, node, context):
        if not isinstance(node, dict):
            return
        where = self.place(node)
        kind = node.get('kind', '')
        line = self.lifted_line(where)
        skip = context['skip']
        if kind == 'ClassTemplateSpecializationDecl' or (node.get('isImplicit') and kind in FUNCTION_KINDS):
            skip = True
        child = dict(context)
        child['skip'] = skip

        if kind in FUNCTION_KINDS:
            key = node.get('mangledName') or '::'.join(context['scope'] + [node.get('name', '?')])
            self.decl_name[node.get('id')] = key
            self.decl_file[node.get('id')] = where[0] if where else None
            if node.get('virtual'):
                self.decl_virtual.add(node.get('id'))
            if re.search(r'\)\s*const\b', (node.get('type') or {}).get('qualType', '')):
                self.const_methods.add(node.get('id'))
            if context['in_template_instance'] or context['template_rank'] > 0:
                self.decl_template.add(node.get('id'))
            is_definition = any(isinstance(inner, dict) and inner.get('kind') in ('CompoundStmt', 'CXXTryStmt')
                                for inner in node.get('inner') or [])
            if is_definition and line is not None and not skip:
                # An out-of-line member is lexically in the namespace; its
                # semantic parent (the class) names it (census-2 defect 3).
                owner = self.record_scope.get(node.get('parentDeclContextId'))
                qualified = (owner.split('::') if owner else context['scope']) + [node.get('name', '?')]
                display = '::'.join(qualified) + '@' + line
                self.function_keys[key] = display
                child['function'] = display
                child['function_key'] = key
                child['depth'] = 0
                returned = (node.get('type') or {}).get('qualType', '').split('(', 1)[0].strip()
                child['returns_address'] = 'reference' if returned.endswith('&') else \
                    'pointer' if returned.endswith('*') else \
                    'view' if re.search(r'string_view|span<', returned) else None
                record = self.record(display)
                if context['template_rank'] > 0 and not context['in_template_instance']:
                    # A function template, or a member of a class template,
                    # defined in the span: a pattern of its own function
                    # (census-2 counted it at namespace scope, defect 2).
                    record['C9']['patterns'] += 1
                    record['C9']['lines'].append(line)
                if self.returns_class(returned):
                    record['C18']['class_returns'] += 1
                    record['C18']['lines'].append(line)
                if kind != 'FunctionDecl':
                    record['C18']['members_defined'] += 1
                    record['C18']['lines'].append(line)
                if node.get('variadic'):
                    record['C14']['variadic'] = True
        if kind == 'NamespaceDecl':
            child['scope'] = context['scope'] + [node.get('name') or '(anonymous)']
        if kind in ('CXXRecordDecl', 'ClassTemplateDecl', 'ClassTemplateSpecializationDecl') and node.get('name'):
            self.record_names.add(node['name'])
            if kind != 'ClassTemplateDecl':
                self.record_scope.setdefault(node.get('id'), '::'.join(context['scope'] + [node['name']]))
        if kind == 'EnumDecl' and node.get('name'):
            self.enum_names.add(node['name'])
        if kind in ('CXXRecordDecl', 'ClassTemplateSpecializationDecl') and node.get('name'):
            child['scope'] = context['scope'] + [node['name']]
        if kind == 'ClassTemplateSpecializationDecl':
            child['in_template_instance'] = True
        if kind in ('FunctionTemplateDecl', 'ClassTemplateDecl'):
            child['template_rank'] = context['template_rank'] + 1

        function = context.get('function')
        counting = line is not None and not skip
        if counting:
            if function:
                self.line_function.setdefault(line, function)
            target = self.record(function or '<namespace scope>')
            self.count(node, kind, line, target, context)

        inner = node.get('inner') or []
        statement_children = kind in ('CompoundStmt', 'CaseStmt', 'DefaultStmt', 'LabelStmt')
        seen_pattern = False
        previous = None
        void_cast = kind in ('CStyleCastExpr', 'CXXStaticCastExpr') and type_text(node) == 'void'
        for index, item in enumerate(inner):
            grand = dict(child)
            if kind in WRAPPERS or void_cast:
                grand['statement'] = context.get('statement', False)
                grand['call_ok'] = context.get('call_ok', False) or context.get('statement', False)
            else:
                grand['statement'] = statement_children or self.statement_slot(node, kind, index, inner)
                grand['call_ok'] = grand['statement'] or kind in ('VarDecl', 'ReturnStmt') or (
                    kind == 'BinaryOperator' and node.get('opcode') == '=' and context.get('statement', False)
                    and index == 1)
            grand['parent'] = kind
            if kind == 'CXXForRangeStmt' and index != len(inner) - 1:
                grand['skip'] = True
            if kind in LOOP_KINDS:
                grand['depth'] = child.get('depth', 0) + 1
            if kind == 'FunctionTemplateDecl' and isinstance(item, dict) and item.get('kind') in FUNCTION_KINDS:
                if seen_pattern:
                    grand['skip'] = True
                seen_pattern = True
            if function and not skip and kind == 'CompoundStmt' \
                    and context.get('parent') == 'SwitchStmt' and isinstance(item, dict) \
                    and item.get('kind') in ('CaseStmt', 'DefaultStmt') and previous is not None \
                    and previous.get('kind') not in ('BreakStmt', 'ReturnStmt', 'ContinueStmt', 'GotoStmt',
                                                     'CXXThrowExpr', 'CaseStmt', 'DefaultStmt'):
                where_case = self.lifted_line(self.peek(item))
                if where_case is not None and function:
                    target = self.record(function)
                    target['C14']['fall_through'] += 1
                    target['C14']['lines'].append(where_case)
            self.walk(item, grand)
            previous = item if isinstance(item, dict) else previous

    def peek(self, node):
        location = node.get('loc') or {}
        if 'expansionLoc' in location:
            location = location['expansionLoc']
        line = location.get('line', self.last_line)
        file = location.get('file', self.last_file)
        return file, line

    @staticmethod
    def statement_slot(node, kind, index, inner):
        # Clang's JSON keeps a null child as an empty object, so a for
        # statement has five children: init, condition variable, condition,
        # increment, body (assumption checked by the first census run).
        if kind == 'ForStmt':
            return index in (0, len(inner) - 2, len(inner) - 1)
        if kind in ('IfStmt', 'WhileStmt', 'SwitchStmt'):
            condition = (1 if node.get('hasInit') else 0) + (1 if node.get('hasVar') else 0)
            return index != condition
        if kind == 'DoStmt':
            return index == 0
        return False

    def count(self, node, kind, line, record, context):
        statement = context.get('statement', False)
        call_ok = context.get('call_ok', False) or statement
        if kind in LOOP_KINDS:
            record['C1']['loops'] += 1
            record['C1']['lines'].append(line)
            record['C1']['max_depth'] = max(record['C1']['max_depth'], context.get('depth', 0) + 1)
            if kind == 'CXXForRangeStmt':
                record['C1']['range_for'] += 1
            elif self.constant_bound(node):
                record['C1']['constant_bound'] += 1
            else:
                record['C1']['other_bound'] += 1
        elif kind in ('BinaryOperator', 'CompoundAssignOperator', 'UnaryOperator'):
            opcode = node.get('opcode', '')
            inner = node.get('inner') or []
            if opcode in ('+', '-') and kind == 'BinaryOperator' and any(
                    is_pointer(type_text(item)) for item in inner if isinstance(item, dict)):
                record['C3']['pointer_arithmetic'] += 1
                record['C3']['lines'].append(line)
            effect = kind == 'CompoundAssignOperator' or opcode == '=' or opcode in ('++', '--')
            if effect and inner:
                self.note_write(inner[0], line, record)
            if opcode == '=' and len(inner) == 2 and self.address_of_local(inner[1]) and \
                    not self.is_local(inner[0]):
                record['C15']['escaping'] += 1
                record['C15']['escaping_lines'].append(line)
            if effect and not statement:
                record['C5']['effects_in_expressions'] += 1
                record['C5']['lines_a'].append(line)
            if kind == 'UnaryOperator' and opcode == '&' and inner:
                self.mark_direct(inner[0])
                target = strip_wrappers(inner[0])
                if target and target.get('kind') == 'DeclRefExpr' and \
                        (target.get('referencedDecl') or {}).get('id') in self.local_ids:
                    record['C15']['address_of_local'] += 1
                    record['C15']['lines'].append(line)
            if type_text(node) in ('float', 'double', 'long double'):
                record['C11']['operations'] += 1
                record['C11']['lines'].append(line)
        elif kind == 'ArraySubscriptExpr':
            base = (node.get('inner') or [None])[0]
            if isinstance(base, dict) and not (base.get('kind') == 'ImplicitCastExpr'
                                               and base.get('castKind') == 'ArrayToPointerDecay'):
                if is_pointer(type_text(base)):
                    record['C3']['raw_indexing'] += 1
                    record['C3']['lines'].append(line)
        elif kind == 'CXXReinterpretCastExpr':
            record['C3']['reinterpret_cast'] += 1
            record['C3']['lines'].append(line)
        elif kind in CALL_KINDS:
            self.count_call(node, kind, line, record, context, call_ok)
        elif kind == 'CXXOperatorCallExpr':
            record['C18']['operator_calls'] += 1
            inner = node.get('inner') or []
            callee = strip_wrappers(inner[0]) if inner else None
            operator = ((callee or {}).get('referencedDecl') or {}).get('name', '')
            if re.fullmatch(r'operator(=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)', operator) and len(inner) > 1:
                self.note_write(inner[1], line, record)
        elif kind == 'ReturnStmt' and context.get('returns_address') and (node.get('inner') or []):
            record['C14']['returns'] += 1
            operand = strip_wrappers(node['inner'][0])
            if self.address_of_local(operand) or self.returns_local(operand, context['returns_address']):
                record['C15']['escaping'] += 1
                record['C15']['escaping_lines'].append(line)
        elif kind == 'GCCAsmStmt' or kind == 'MSAsmStmt':
            record['C13']['inline_asm'] += 1
            record['C13']['lines'].append(line)
        elif kind == 'LambdaExpr':
            record['C18']['lambdas'] += 1
            record['C18']['lines'].append(line)
        elif kind == 'CXXNewExpr':
            record['C7']['new'] += 1
            record['C7']['lines'].append(line)
        elif kind == 'CXXDeleteExpr':
            record['C7']['delete'] += 1
            record['C7']['lines'].append(line)
        elif kind == 'CXXThrowExpr':
            record['C8']['throw'] += 1
            record['C8']['lines'].append(line)
        elif kind == 'CXXTryStmt':
            record['C8']['try'] += 1
            record['C8']['lines'].append(line)
        elif kind == 'CXXCatchStmt':
            record['C8']['catch'] += 1
            record['C8']['lines'].append(line)
        elif kind == 'ClassTemplateDecl':
            record['C9']['patterns'] += 1
            record['C9']['lines'].append(line)
        elif kind == 'FloatingLiteral':
            record['C11']['literals'] += 1
            record['C11']['lines'].append(line)
        elif kind in ('ConvertVectorExpr', 'ShuffleVectorExpr'):
            record['C13']['builtin_calls'].append(kind)
            record['C13']['lines'].append(line)
        elif kind == 'GotoStmt':
            record['C14']['goto'] += 1
            record['C14']['lines'].append(line)
        elif kind == 'SwitchStmt':
            record['C14']['switch'] += 1
            record['C14']['lines'].append(line)
        elif kind == 'ReturnStmt':
            record['C14']['returns'] += 1
        elif kind == 'BreakStmt':
            record['C14']['break'] += 1
        elif kind == 'ContinueStmt':
            record['C14']['continue'] += 1
        elif kind == 'DeclRefExpr':
            name = (node.get('referencedDecl') or {}).get('name', '')
            if name.startswith('memory_order'):
                if name not in record['C12']['memory_orders']:
                    record['C12']['memory_orders'].append(name)
                record['C12']['lines'].append(line)
        elif kind.endswith('Attr') and not node.get('implicit'):
            if kind not in STANDARD_ATTRIBUTES:
                record['C13']['attributes'].append(kind)
                record['C13']['lines'].append(line)
            if kind == 'NoSanitizeAttr':
                record['C16']['modular'] = True
        if kind in ('CXXConstCastExpr', 'CStyleCastExpr') and node.get('inner'):
            operand = node['inner'][0]
            removes_const = kind == 'CXXConstCastExpr' or (
                'const' in type_text(operand) and 'const' not in type_text(node) and
                (is_pointer(type_text(node)) or type_text(node).endswith('&')))
            if removes_const and self.resolve_parameter(operand):
                record['C19']['read_only_written'] += 1
                record['C19']['read_only_lines'].append(line)
        if kind in ('ImplicitCastExpr', 'CStyleCastExpr', 'CXXStaticCastExpr', 'CXXFunctionalCastExpr'):
            self.count_conversion(node, kind, line, record)
        if kind in ('VarDecl', 'ParmVarDecl', 'FieldDecl'):
            self.count_declaration(node, kind, line, record, context)
        text = type_text(node)
        if 'volatile' in text and kind in ('VarDecl', 'ParmVarDecl', 'FieldDecl', 'DeclRefExpr'):
            record['C17']['volatile'] += 1
            record['C17']['lines'].append(line)
        if '__int128' in text and kind in ('VarDecl', 'ParmVarDecl', 'FieldDecl', 'CStyleCastExpr',
                                           'CXXStaticCastExpr'):
            record['C13']['int128'] += 1
            record['C13']['lines'].append(line)
        if ('vector_size' in text or 'ext_vector' in text) and kind in ('VarDecl', 'ParmVarDecl', 'FieldDecl',
                                                                        'TypedefDecl'):
            record['C13']['vector_types'] += 1
            record['C13']['lines'].append(line)

    @staticmethod
    def constant_bound(node):
        inner = node.get('inner') or []
        for item in inner:
            if isinstance(item, dict) and item.get('kind') == 'BinaryOperator' and \
                    item.get('opcode') in ('<', '<=', '!=', '>', '>='):
                operands = [strip_wrappers(operand) for operand in item.get('inner') or []]
                if any(operand and operand.get('kind') == 'IntegerLiteral' for operand in operands):
                    return True
        return False

    def count_call(self, node, kind, line, record, context, allowed):
        inner = node.get('inner') or []
        callee = strip_wrappers(inner[0]) if inner else None
        name = ''
        decl_id = None
        if callee and callee.get('kind') == 'DeclRefExpr':
            referenced = callee.get('referencedDecl') or {}
            name = referenced.get('name', '')
            decl_id = referenced.get('id')
            if referenced.get('kind') not in ('FunctionDecl', 'CXXMethodDecl', None):
                record['C6']['function_pointer_calls'] += 1
                record['C6']['lines'].append(line)
        elif callee and callee.get('kind') == 'MemberExpr':
            name = callee.get('name', '')
            decl_id = callee.get('referencedMemberDecl')
            if decl_id in self.decl_virtual:
                record['C6']['virtual_calls'] += 1
                record['C6']['lines'].append(line)
            base = (callee.get('inner') or [None])[0]
            if name in ATOMIC_OPERATIONS and isinstance(base, dict) and 'atomic' in type_text(base):
                record['C12']['atomic_operations'] += 1
                record['C12']['lines'].append(line)
        elif callee is not None and callee.get('kind') not in DEPENDENT_CALLEES:
            # A callee still unresolved in a template pattern is a direct
            # call once instantiated (census-2 defect 1); what is left here
            # is a call through a pointer or reference to a function.
            record['C6']['function_pointer_calls'] += 1
            record['C6']['lines'].append(line)
        if callee and callee.get('kind') == 'MemberExpr' and decl_id not in self.const_methods:
            base = (callee.get('inner') or [None])[0]
            if isinstance(base, dict):
                if callee.get('isArrow'):
                    self.note_write({'kind': 'UnaryOperator', 'opcode': '*', 'inner': [base]}, line, record)
                else:
                    self.note_write(base, line, record)
        for argument in inner[1:]:
            if not isinstance(argument, dict):
                continue
            if argument.get('kind') == 'DeclRefExpr':
                # A bare lvalue argument binds to a non-const reference
                # (a by-value or const-reference argument carries a cast or
                # a construction): the callee may write it.
                self.mark_direct(argument)
            if self.address_of_local(argument):
                record['C15']['passed_to_call'] += 1
                record['C15']['lines'].append(line)
            target = strip_wrappers(argument)
            if target and target.get('kind') == 'DeclRefExpr':
                parameter = self.parameters.get((target.get('referencedDecl') or {}).get('id'))
                if parameter and self.writable(parameter[2]) and parameter[1] not in record['C19']['passed_on']:
                    record['C19']['passed_on'].append(parameter[1])
        if name in ('fesetround', 'fesetenv', 'feupdateenv', 'feraiseexcept', 'feclearexcept',
                    'fesetexceptflag', 'feholdexcept', '_controlfp', '_controlfp_s'):
            record['C11']['environment_changes'] += 1
            record['C11']['environment_lines'].append(line)
        if not allowed and kind in CALL_KINDS:
            record['C5']['nested_calls'] += 1
            record['C5']['lines_b'].append(line)
        if name in ('memcpy', 'memmove'):
            record['C3']['memcpy'] += 1
            record['C3']['lines'].append(line)
        if name in ('malloc', 'calloc', 'realloc', 'free', 'aligned_alloc', 'posix_memalign'):
            record['C7']['malloc_family'] += 1
            record['C7']['lines'].append(line)
        if name.startswith('__builtin'):
            record['C13']['builtin_calls'].append(name)
            record['C13']['lines'].append(line)
        declared_in = self.decl_file.get(decl_id)
        if decl_id is not None and decl_id in self.decl_template and name not in record['C9']['template_calls']:
            record['C9']['template_calls'].append(name)
            if declared_in is not None and not self.is_library(declared_in):
                record['C9']['own_template_calls'].append(name)
        if declared_in is not None and self.is_library(declared_in):
            record['C10']['calls'][name] = record['C10']['calls'].get(name, 0) + 1
        key = context.get('function_key')
        if key is not None and decl_id is not None and decl_id in self.decl_name:
            self.calls.setdefault(key, set()).add(self.decl_name[decl_id])

    def returns_class(self, returned):
        """Whether a function's return type (as written) is a class type
        returned by value: cpp-verify 0.1.1 names these "unsupported C++
        type in verification" (rows 13 and 15)."""
        text = bare(returned or '')
        text = re.sub(r'^(static|inline|constexpr|consteval|virtual|explicit|\[\[[^\]]*\]\])\s+', '', text)
        if not text or text.endswith(('&', '*')) or integer_width(text) is not None:
            return False
        name = re.sub(r'<.*$', '', text).split('::')[-1].strip()
        return name in self.record_names and name not in self.enum_names

    def is_library(self, path):
        real = os.path.realpath(path)
        if real == os.path.realpath(self.unit_path):
            return False
        if real.startswith(self.auxlib_root + os.sep) or real.startswith(os.path.realpath(B1) + os.sep):
            return False
        return True

    def count_conversion(self, node, kind, line, record):
        if node.get('castKind') in ('IntegralToFloating', 'FloatingToIntegral', 'FloatingCast'):
            record['C11']['conversions'] += 1
            record['C11']['lines'].append(line)
            return
        if node.get('castKind') != 'IntegralCast':
            return
        inner = node.get('inner') or []
        if not inner or not isinstance(inner[0], dict):
            return
        source = integer_width(type_text(inner[0]))
        target = integer_width(type_text(node))
        if source is None or target is None:
            return
        literal = strip_wrappers(inner[0])
        if literal and literal.get('kind') == 'IntegerLiteral' and fits(literal.get('value'), target):
            return  # the literal's value is kept (census-2 defect 4)
        changes = target[0] < source[0] or (target[0] == source[0] and target[1] != source[1]) or \
            (source[1] and not target[1])
        if not changes:
            return
        if kind == 'ImplicitCastExpr':
            record['C16']['implicit_value_changing'] += 1
        else:
            record['C16']['explicit_value_changing'] += 1
        record['C16']['lines'].append(line)

    def count_declaration(self, node, kind, line, record, context):
        text = type_text(node)
        in_function = context.get('function') is not None
        if kind in ('VarDecl', 'ParmVarDecl') and in_function and node.get('storageClass') != 'static':
            self.local_ids.add(node.get('id'))
            self.local_types[node.get('id')] = text
        if kind == 'VarDecl' and in_function and node.get('storageClass') != 'static' and \
                not node.get('isImplicit') and node.get('name'):
            self.local_decls[node.get('id')] = (context.get('function'), node['name'], text,
                                                bool(node.get('constexpr')))
        if kind == 'VarDecl' and in_function and text.endswith('&') and not text.startswith('const ') \
                and isinstance((node.get('inner') or [None])[0], dict) \
                and node['inner'][0].get('kind') == 'DeclRefExpr':
            self.mark_direct(node['inner'][0])
        if kind == 'VarDecl' and in_function and node.get('storageClass') == 'static':
            record['C15']['static_locals'] += 1
            record['C15']['lines'].append(line)
        if kind == 'VarDecl' and not in_function and not node.get('constexpr') and \
                not text.startswith('const ') and not re.search(r'\bconst$', text):
            record['C15']['mutable_globals'] += 1
            record['C15']['lines'].append(line)
        if kind == 'ParmVarDecl' and in_function:
            record['C4']['parameters'].append(text)
            self.parameters[node.get('id')] = (context.get('function'), node.get('name', '?'), text)
        if ALLOCATING_TYPE.search(text):
            record['C7']['allocating_declarations'] += 1
            record['C7']['lines'].append(line)
            if CONTAINER_TYPE.search(text):
                record['C7']['container_declarations'] += 1
        if CONCURRENCY_TYPE.search(text):
            record['C12']['concurrency_declarations'] += 1
            record['C12']['lines'].append(line)
        if '<' in text:
            template_name = re.sub(r'^(const\s+)?', '', text).split('<', 1)[0].strip()
            if template_name and template_name not in record['C9']['template_types']:
                record['C9']['template_types'].append(template_name)
        if kind == 'VarDecl' and in_function and (text.startswith('std::') or text.startswith('struct ') or
                                                  text.startswith('class ') or '::' in bare(text)):
            if integer_width(text) is None and not is_pointer(text) and not text.endswith('&'):
                record['C18']['class_locals'] += 1

    @staticmethod
    def writable(text):
        """An explicitly mutable input type: a pointer or reference to
        non-const, or a span of non-const elements (house style: views are
        read-only by default)."""
        if re.search(r'\bspan<', text):
            return not re.search(r'\bspan<\s*const\b', text)
        if not (is_pointer(text) or text.endswith('&')):
            return False
        pointee = re.sub(r'(\*\s*(const\s*)?|&+)$', '', text.strip()).strip()
        return not (pointee.startswith('const ') or pointee.endswith(' const'))

    def address_of_local(self, node):
        """&x where x is a local object (not a reference: its referent
        outlives it)."""
        target = strip_wrappers(node)
        if not target or target.get('kind') != 'UnaryOperator' or target.get('opcode') != '&':
            return False
        operand = strip_wrappers((target.get('inner') or [None])[0])
        if not operand or operand.get('kind') != 'DeclRefExpr':
            return False
        referenced = (operand.get('referencedDecl') or {}).get('id')
        return referenced in self.local_types and not self.local_types[referenced].endswith('&')

    def is_local(self, node):
        """True when the lvalue is a local variable itself (storing into it
        does not make anything outlive the function)."""
        target = strip_wrappers(node)
        return bool(target and target.get('kind') == 'DeclRefExpr' and
                    (target.get('referencedDecl') or {}).get('id') in self.local_ids and
                    not self.local_types.get((target.get('referencedDecl') or {}).get('id'), '').endswith('&'))

    def returns_local(self, node, returns):
        """A local named in a return whose value refers into it: a local
        object returned by reference, a local array returned as a pointer
        (the decay is a wrapper, so the operand is the array), or an owning
        local returned as a view."""
        target = strip_wrappers(node)
        if not target or target.get('kind') != 'DeclRefExpr':
            return False
        text = self.local_types.get((target.get('referencedDecl') or {}).get('id'))
        if text is None or text.endswith('&'):
            return False
        if returns == 'reference':
            return True
        if returns == 'pointer':
            return '[' in text
        if returns == 'view':
            return not (is_pointer(text) or re.search(r'string_view|span<', text))
        return False

    def resolve_parameter(self, node):
        target = strip_wrappers(node)
        while target:
            kind = target.get('kind')
            if kind == 'DeclRefExpr':
                return self.parameters.get((target.get('referencedDecl') or {}).get('id'))
            if kind in ('MemberExpr', 'ArraySubscriptExpr') or (kind == 'UnaryOperator' and target.get('opcode') == '*'):
                target = strip_wrappers((target.get('inner') or [None])[0])
                continue
            return None
        return None

    def direct_root(self, node):
        """The variable an lvalue writes itself (not through a pointer):
        x, x.member, x[i] on a local array; None through *, -> or a
        pointer subscript."""
        target = strip_wrappers(node)
        while target:
            kind = target.get('kind')
            if kind == 'DeclRefExpr':
                return (target.get('referencedDecl') or {}).get('id')
            if kind == 'MemberExpr' and not target.get('isArrow'):
                target = strip_wrappers((target.get('inner') or [None])[0])
                continue
            if kind == 'ArraySubscriptExpr':
                base = strip_wrappers((target.get('inner') or [None])[0])
                if base and '[' in type_text(base):
                    target = base
                    continue
            return None
        return None

    def mark_direct(self, node):
        root = self.direct_root(node)
        if root is not None:
            self.written_ids.add(root)

    def note_write(self, node, line, record):
        self.mark_direct(node)
        parameter = self.resolve_parameter(node)
        if parameter and self.writable(parameter[2]) and parameter[1] not in record['C19']['written']:
            record['C19']['written'].append(parameter[1])
            record['C19']['lines'].append(line)

    def finish(self):
        for display, record in self.records.items():
            parameters = record['C4']['parameters']
            pointees = []
            for text in parameters:
                if is_pointer(text) or text.endswith('&'):
                    pointee = text.rstrip('&').rstrip()
                    pointee = re.sub(r'\*\s*(const\s*)?$', '', pointee).strip()
                    constant = pointee.startswith('const ') or pointee.endswith(' const')
                    pointees.append((bare(pointee), constant))
                    if not constant:
                        record['C4']['out_parameters'] += 1
            for first in range(len(pointees)):
                for second in range(first + 1, len(pointees)):
                    same = pointees[first][0] == pointees[second][0]
                    if same and not (pointees[first][1] and pointees[second][1]):
                        record['C4']['alias_pairs'] += 1
        for decl_id, (display, name, text) in self.parameters.items():
            if display in self.records and self.writable(text):
                record = self.records[display]
                if name not in record['C19']['written'] and name not in record['C19']['passed_on'] and \
                        name not in record['C19']['advisory_parameters']:
                    record['C19']['advisory_parameters'].append(name)
        for decl_id, (display, name, text, constexpr) in self.local_decls.items():
            if display not in self.records or decl_id in self.written_ids or constexpr:
                continue
            if text.startswith('const ') or re.search(r'\bconst$', text) or text.endswith('&'):
                continue
            record = self.records[display]
            if name not in record['C19']['advisory_locals']:
                record['C19']['advisory_locals'].append(name)
        keys = set(self.function_keys)
        graph = {key: {callee for callee in self.calls.get(key, set()) if callee in keys} for key in keys}
        for key in keys:
            reached = set()
            frontier = list(graph[key])
            while frontier:
                current = frontier.pop()
                if current in reached:
                    continue
                reached.add(current)
                frontier.extend(graph.get(current, set()))
            if key in reached:
                record = self.records[self.function_keys[key]]
                record['C2']['recursive'] = True
                record['C2']['cycle'] = sorted(self.function_keys[other] for other in reached
                                               if key in self.reach(graph, other))
        for record in self.records.values():
            for entry in record.values():
                for field in list(entry):
                    if field.startswith('lines'):
                        entry[field] = sorted(set(entry[field]), key=self.line_key)

    @staticmethod
    def reach(graph, start):
        reached = set()
        frontier = list(graph.get(start, set()))
        while frontier:
            current = frontier.pop()
            if current in reached:
                continue
            reached.add(current)
            frontier.extend(graph.get(current, set()))
        return reached

    @staticmethod
    def line_key(text):
        path, _, number = text.rpartition(':')
        return path, int(number) if number.isdigit() else 0


DISTANCE = 'distance'
PERMITTED = 'permitted by house style'
RECORDED = 'recorded (not decidable from the AST)'
ADVISORY = 'advisory (misc-const-correctness; reports, never fails)'
TOOL = 'tool gap'  # B1-5 = 1: the tool subsets are measured on the tool side


def dialect_gaps(record):
    """Each construct outside a column, with its status under the owner's
    direction of 2026-11-13 (decisions/2026-11-13-locals-float-mutability.md):
    floating point, local and static local variables and taking a local's
    address are permitted by house style; an escaping local address and
    floating point that leaks (here: a change of the floating-point
    environment; results outside the stated domain are proof properties,
    not census items) are distance; inputs are read-only by type, so a
    write to an input that crosses as a read-only type is distance, a
    write through an explicitly mutable type is permitted, and a value
    never written but not const is advisory (misc-const-correctness)."""
    gaps = {'clight': [], 'strictc': [], 'cpp-verify': [], 'house-style': []}

    def add(column, construct, status):
        gaps[column].append({'construct': construct, 'status': status})

    if record['C5']['effects_in_expressions']:
        add('clight', 'C5a side effects in expressions', DISTANCE)
    if record['C5']['nested_calls']:
        add('clight', 'C5b calls nested in expressions', DISTANCE)
    if record['C8']['throw'] or record['C8']['try']:
        add('clight', 'C8 exceptions', DISTANCE)
        add('cpp-verify', 'C8 exceptions', TOOL)
    if record['C15']['address_of_local']:
        add('strictc', 'C15 address of a local', PERMITTED)
    if record['C15']['static_locals']:
        add('strictc', 'C15 static local', PERMITTED)
    if record['C11']['literals'] or record['C11']['operations'] or record['C11']['conversions']:
        add('strictc', 'C11 floating point', PERMITTED)
        add('cpp-verify', 'C11 floating point', TOOL)
    if record['C14']['goto']:
        add('strictc', 'C14 goto', DISTANCE)
    if record['C14']['fall_through']:
        add('strictc', 'C14 switch fall-through', DISTANCE)
    if record['C14']['variadic']:
        add('strictc', 'C14 variadic', DISTANCE)
    if record['C6']['function_pointer_calls']:
        add('strictc', 'C6 function pointer call', DISTANCE)
    if record['C18']['members_defined']:
        add('cpp-verify', 'C18 member function', TOOL)
    if record['C18']['lambdas']:
        add('cpp-verify', 'C18 lambda', TOOL)
    # Measured from cpp-verify's behaviour, not its release notes (rule of
    # 2026-11-13): templates defined in the code under test count; uses of
    # library templates are recorded under C9 and C10 and do not (rows 01
    # and 02 call std::numeric_limits<>::max and are proved).
    if record['C9']['patterns'] or record['C9']['own_template_calls']:
        add('cpp-verify', 'C9 templates defined in the code', TOOL)
    if record['C18']['class_returns']:
        add('cpp-verify', 'C18 struct return type', TOOL)
    if record['C6']['virtual_calls']:
        add('cpp-verify', 'C6 virtual dispatch', TOOL)
    if record['C14']['switch']:
        add('cpp-verify', 'C14 switch', TOOL)
    if record['C1']['range_for']:
        add('cpp-verify', 'C1 range-for', TOOL)
    if record['C7']['container_declarations']:
        add('cpp-verify', 'C7 standard containers', TOOL)
    if record['C15']['escaping']:
        add('house-style', 'C15 a local address escapes its lifetime', DISTANCE)
    if record['C15']['passed_to_call']:
        add('house-style', 'C15 a local address passed to a call (escapes unless the callee keeps it no longer)',
            RECORDED)
    if record['C11']['environment_changes']:
        add('house-style', 'C11 floating-point environment changed (leak)', DISTANCE)
    if record['C19']['read_only_written']:
        add('house-style', 'C19 an input that crosses as a read-only type is written (const removed)', DISTANCE)
    if record['C19']['written']:
        add('house-style', 'C19 inputs written through explicitly mutable parameter types', PERMITTED)
    if record['C19']['advisory_parameters'] or record['C19']['advisory_locals']:
        add('house-style', 'C19 never written but not declared const', ADVISORY)
    return gaps


def census_row(row_directory, row, sources, arguments):
    harness_spec = row['harnesses'][0]
    harness = os.path.join(row_directory, harness_spec['file'])
    try:
        text, report = lift.lift(sources, harness, None, apply_edits=False)
    except lift.LiftError as error:
        return {'row': row['row'], 'error': f'lift: {error}'}
    defines = {'STX_TOOL_GENMC' if any(tool.startswith('genmc') for tool in harness_spec['tools'])
               else 'STX_TOOL_ESBMC': '1',
               'STX_DECLARE_INTRINSICS': '1', 'STX_N': '16'}
    defines.update(harness_spec.get('defines', {}))
    configs = harness_spec.get('configs')
    if configs:
        defines.update(configs[0].get('defines', {}))
    includes = []
    for entry in harness_spec.get('includes', []) + ['harness:common', 'harness:row']:
        kind, relative = entry.split(':', 1)
        if kind == 'harness':
            if relative == 'common':
                path = os.path.join(B1, 'common')
            elif relative == 'd-common':
                path = os.path.join(os.path.dirname(B1), 'd', 'common')
            else:
                path = row_directory
        else:
            path = sources.file(kind, relative)
        includes.append(path)
    result = census_unit(row['row'], text, includes, defines, sources.root(SOURCE_KEY[0]), arguments)
    if 'error' in result:
        return result
    result.update({'title': row['title'], 'spans': report['spans']})
    return result


def census_unit(identifier, text, includes, defines, root, arguments):
    """The census of one unit: a lifted harness (rows) or one whole source
    file of the tree (--tree), whose lines carry #line labels under the
    sources key."""
    unit = Unit(text)
    directory = os.path.join(arguments.out, 'census')
    unit_path = os.path.join(directory, f'{identifier}.unit.cpp')
    os.makedirs(os.path.dirname(unit_path), exist_ok=True)
    with open(unit_path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(unit.text)
    command = [arguments.clang, '-std=c++20', '-fsyntax-only', '-Wdangling', '-Wreturn-stack-address',
               '-Wdangling-gsl', '-Xclang', '-ast-dump=json']
    for key, value in defines.items():
        command.append(f'-D{key}={value}')
    for path in includes:
        command.extend(['-I', path])
    command.append(unit_path)
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    with open(os.path.join(directory, f'{identifier}.clang.txt'), 'w', encoding='utf-8') as handle:
        handle.write(' '.join(command) + '\n' + result.stderr)
    # Without its edits a harness may not type-check (row 24 calls the
    # lifted acquire with its model lock); clang still dumps the AST, and
    # the lifted code's own diagnostics are in <row>.clang.txt.
    if not result.stdout.strip():
        return {'row': identifier, 'error': f'clang exited {result.returncode}; see {identifier}.clang.txt'}
    tree = json.loads(result.stdout)
    census = Census(unit, unit_path, root)
    census.walk(tree, {'skip': False, 'scope': [], 'function': None, 'depth': 0, 'template_rank': 0,
                       'in_template_instance': False, 'statement': False, 'parent': None})
    for match in re.finditer(r'^(?:.*/)?' + re.escape(os.path.basename(unit_path)) +
                             r':(\d+):\d+: warning: ([^\n]*(?:stack memory|dangling|address of local|'
                             r'temporary whose address)[^\n]*)', result.stderr, re.MULTILINE):
        original = unit.original(int(match.group(1)))
        if original is None:
            continue
        place = f'{strip_source(original[0])}:{original[1]}'
        function = census.line_function.get(place, '<namespace scope>')
        record = census.record(function)
        record['C15']['escaping'] += 1
        record['C15']['escaping_lines'].append(place + ' (clang: ' + match.group(2).strip() + ')')
    census.finish()
    functions = {}
    for name, record in census.records.items():
        functions[name] = {'census': record, 'outside': dialect_gaps(record)}
    # Where clang's errors fall: a harness line (the census lifts without
    # the harness's substitutions, so harness code written against a model
    # may not type-check; the lifted code's AST is complete) or a lifted one.
    error_lines = [int(line) for line in re.findall(
        r'^(?:.*/)?' + re.escape(os.path.basename(unit_path)) + r':(\d+):\d+: error', result.stderr, re.MULTILINE)]
    in_lifted = sorted({f'{strip_source(place[0])}:{place[1]}' for place in
                        (unit.original(line) for line in error_lines) if place is not None})
    return {'row': identifier, 'clang': arguments.clang,
            'clang_exit': result.returncode,
            'clang_errors': {'harness_lines': sum(1 for line in error_lines if unit.original(line) is None),
                             'lifted_lines': in_lifted},
            'functions': functions}


# The tree census (--tree): every STXlib source and header, each as its own
# unit, so the distance is measured on the code itself at every fold
# (coordinator's amendment of 2026-11-13), not only on lifted spans.
TREE_DIRECTORIES = ('include', 'src')
TREE_SUFFIXES = ('.cpp', '.hpp', '.h')


def tree_files(root, exclude):
    found = []
    for top in TREE_DIRECTORIES:
        for directory, subdirectories, files in os.walk(os.path.join(root, top)):
            subdirectories.sort()
            for name in sorted(files):
                if not name.endswith(TREE_SUFFIXES):
                    continue
                relative = os.path.relpath(os.path.join(directory, name), root)
                if any(relative == item or relative.startswith(item.rstrip('/') + '/') for item in exclude):
                    continue
                found.append(relative)
    return found


def census_tree(sources, arguments):
    root = sources.root(SOURCE_KEY[0])
    exclude = [item for item in (arguments.exclude or '').split(',') if item]
    results = []
    for relative in tree_files(root, exclude):
        with open(os.path.join(root, relative), 'r', encoding='utf-8', errors='surrogateescape') as handle:
            body = handle.read()
        # A header compiled as a main file warns about #pragma once; the
        # line stays so every line keeps its number.
        text = f'#line 1 "{SOURCE_KEY[0]}/{relative}"\n' + body
        identifier = relative.replace('/', '__')
        defines = {'STXLIB_CENSUS': '1'}
        for item in arguments.define or []:
            key, _, value = item.partition('=')
            defines[key] = value or '1'
        includes = [os.path.join(root, 'include'), os.path.join(root, 'src')]
        result = census_unit(identifier, text, includes, defines, root, arguments)
        result['file'] = relative
        results.append(result)
    return results


def summary_line(result):
    if 'error' in result:
        return f'{result["row"]}  error: {result["error"]}'
    parts = []
    for name, entry in result['functions'].items():
        if name == '<namespace scope>':
            continue
        outside = entry['outside']
        counts = ', '.join(f'{column} {sum(1 for gap in gaps if gap["status"] == DISTANCE)}'
                           for column, gaps in outside.items() if column != 'cpp-verify')
        parts.append(f'{name} [distance: {counts}; cpp-verify tool gaps {len(outside["cpp-verify"])}]')
    return f'{result["row"]}  ' + '; '.join(parts)


def censused(result):
    """True when the result is a complete census: clang produced an AST and
    reported no error on a counted (lifted, or in --tree every) line.
    Errors on a harness's own lines do not count (census_unit)."""
    if 'error' in result:
        return False
    return not result['clang_errors']['lifted_lines']


def finish(failed, arguments):
    if not failed:
        return 0
    print(f'not censused ({len(failed)}): ' + ', '.join(failed), flush=True)
    return 1 if arguments.fail_on_error else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--sources', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--rows')
    parser.add_argument('--clang', default='clang++')
    parser.add_argument('--rows-directory', help='rows to census (default rows/; phase D: proofs/d/rows)')
    parser.add_argument('--source', default='stx_auxlib',
                        help='the sources key whose lifted lines are counted (stxlib for phase D)')
    parser.add_argument('--tree', action='store_true',
                        help='census every file of the --source tree (include/ and src/) instead of rows')
    parser.add_argument('--exclude', help='with --tree: comma-separated paths under the tree to leave out')
    parser.add_argument('--define', action='append',
                        help='with --tree: KEY=VALUE defined for every file (repeatable), as the build would, '
                             'for example STXLIB_MATH_VARIANT=native')
    parser.add_argument('--fail-on-error', action='store_true',
                        help='exit 1 when a file or row could not be censused (no AST), or when clang reports '
                             'an error on a counted line, so a CI step fails (house style addendum: a check that '
                             'reports a failure makes its caller fail)')
    arguments = parser.parse_args()
    SOURCE_KEY[0] = arguments.source
    arguments.out = os.path.abspath(arguments.out)
    repository = os.path.realpath(os.path.dirname(os.path.dirname(B1)))
    if os.path.commonpath([os.path.realpath(arguments.out), repository]) == repository:
        # The M4 copy of the repository is synced with rsync --delete, so
        # anything written inside it is lost at the next sync.
        sys.exit(f'--out must be outside the repository ({repository})')
    os.makedirs(os.path.join(arguments.out, 'census'), exist_ok=True)
    sources = lift.Sources(arguments.sources)
    selected = set(arguments.rows.split(',')) if arguments.rows else None
    lines = []
    failed = []
    if arguments.tree:
        for result in census_tree(sources, arguments):
            with open(os.path.join(arguments.out, 'census', f'{result["row"]}.json'), 'w', encoding='utf-8') as handle:
                json.dump(result, handle, indent=1)
            lines.append(summary_line(result))
            print(lines[-1], flush=True)
            if not censused(result):
                failed.append(result['row'])
        with open(os.path.join(arguments.out, 'census', 'summary.txt'), 'w', encoding='utf-8') as handle:
            handle.write('\n'.join(lines) + '\n')
        return finish(failed, arguments)
    rows_directory = os.path.abspath(arguments.rows_directory) if arguments.rows_directory else os.path.join(B1, 'rows')
    for row_name in sorted(os.listdir(rows_directory)):
        row_file = os.path.join(rows_directory, row_name, 'row.json')
        if not os.path.isfile(row_file):
            continue
        with open(row_file, 'r', encoding='utf-8') as handle:
            row = json.load(handle)
        if selected and row['row'] not in selected:
            continue
        result = census_row(os.path.join(rows_directory, row_name), row, sources, arguments)
        with open(os.path.join(arguments.out, 'census', f'{row["row"]}.json'), 'w', encoding='utf-8') as handle:
            json.dump(result, handle, indent=1)
        lines.append(summary_line(result))
        print(lines[-1], flush=True)
        if not censused(result):
            failed.append(row['row'])
    with open(os.path.join(arguments.out, 'census', 'summary.txt'), 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(lines) + '\n')
    return finish(failed, arguments)


if __name__ == '__main__':
    sys.exit(main())
