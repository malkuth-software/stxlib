"""Reader for windows-rs's Win32 metadata in RDL text form.

The source is microsoft/windows-rs `metadata/win32/*.rdl` (MIT OR
Apache-2.0), one file per Windows SDK header, pinned in `PIN.json`. The
attribute vocabulary is `metadata/metadata.rdl`. This module only reads the
text; it holds no Microsoft header content.

The reader covers the subset the Win32 files use:

  mod NAME { item* }
  item := attribute* ( struct | union | enum | interface | type | const | extern fn )
  struct NAME { field* }          union NAME { field* }
  field := attribute* NAME ':' field_type ','
         | attribute* (struct | union) NAME { field* }        (explicit nested record)
  field_type := type | (struct | union) { field* } | type { bit* }   (bit-field block)
  bit := NAME ':' type ':' INTEGER ','     ('_' names padding)
  enum NAME { NAME '=' value ',' * }
  interface NAME (':' type)? { attribute* fn NAME '(' param* ')' ('->' type)? ';' * }
  type NAME '=' type ';'
  const NAME ':' type '=' value ';'
  extern ('"C"')? fn NAME '(' param* ')' ('->' type)? ';'
  type := '*mut' type | '*const' type | '&' type | '&mut' type
        | '[' type ';' INTEGER ']' | 'ptr64' '<' type '>' | path

A declaration is a plain record (`Decl`) with named fields; types are
`TypeRef` records. Nothing here decides C spelling: that is `generate.py`.
"""

import pathlib


class RdlError(Exception):
    """A metadata file that does not follow the subset above."""


class Token:
    """One lexical token: kind is 'name', 'number', 'string' or 'punct'."""

    def __init__(self, kind: str, text: str, line: int) -> None:
        self.kind = kind
        self.text = text
        self.line = line


PUNCTUATION = ['#[', '...', '->', '::', '{', '}', '(', ')', '[', ']', '<', '>', ',', ';', ':',
               '=', '*', '&', '|', '-', '.']


def is_name_start(character: str) -> bool:
    return character.isalpha() or character == '_'


def is_name_part(character: str) -> bool:
    return character.isalnum() or character == '_'


def tokenize(text: str, path: str) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    line = 1
    length = len(text)
    while index < length:
        character = text[index]
        if character == '\n':
            line += 1
            index += 1
            continue
        if character in ' \t\r':
            index += 1
            continue
        if text.startswith('//', index):
            end = text.find('\n', index)
            index = length if end < 0 else end
            continue
        if text.startswith('r#', index) and index + 2 < length and is_name_start(text[index + 2]):
            # A raw identifier, `r#type`, names `type`.
            index += 2
            continue
        if is_name_start(character):
            end = index + 1
            while end < length and is_name_part(text[end]):
                end += 1
            tokens.append(Token('name', text[index:end], line))
            index = end
            continue
        if character.isdigit():
            end = index + 1
            while end < length and (is_name_part(text[end]) or text[end] == '.'):
                end += 1
            # An exponent sign belongs to the number: 1e-05.
            while end < length and text[end] in '+-' and text[end - 1] in 'eE' \
                    and not text[index:end].lower().startswith('0x'):
                end += 1
                while end < length and is_name_part(text[end]):
                    end += 1
            tokens.append(Token('number', text[index:end], line))
            index = end
            continue
        if character == '"':
            end = index + 1
            pieces: list[str] = []
            while end < length and text[end] != '"':
                if text[end] == '\\':
                    pieces.append(text[end:end + 2])
                    end += 2
                    continue
                pieces.append(text[end])
                end += 1
            if end >= length:
                raise RdlError(f'{path}:{line}: unterminated string')
            tokens.append(Token('string', ''.join(pieces), line))
            index = end + 1
            continue
        matched = ''
        for punct in PUNCTUATION:
            if text.startswith(punct, index):
                matched = punct
                break
        if not matched:
            raise RdlError(f'{path}:{line}: unexpected character {character!r}')
        tokens.append(Token('punct', matched, line))
        index += len(matched)
    return tokens


class TypeRef:
    """A metadata type.

    kind: 'named' (name holds the path's last segment, namespace the
    'super::' path before it or ''), 'pointer' (mutable says *mut or
    *const), 'reference' (&T), 'array' (count), 'ptr64' (a 64-bit pointer
    wrapper), 'record' (an inline struct or union in a field: record holds
    the Decl), 'function' (an inline function-pointer type: parameter_types,
    target is the result or None, abi).
    """

    def __init__(self, kind: str) -> None:
        self.kind = kind
        self.name = ''
        self.namespace = ''
        self.mutable = False
        self.count = 0
        self.target: 'TypeRef | None' = None
        self.record: 'Decl | None' = None
        self.abi = 'system'
        self.parameter_types: list['TypeRef | None'] = []

    def describe(self) -> str:
        if self.kind == 'named':
            return self.namespace + self.name
        if self.kind == 'pointer':
            return ('*mut ' if self.mutable else '*const ') + self.target.describe()
        if self.kind == 'reference':
            return ('&mut ' if self.mutable else '&') + self.target.describe()
        if self.kind == 'array':
            return f'[{self.target.describe()}; {self.count}]'
        if self.kind == 'ptr64':
            return f'ptr64<{self.target.describe()}>'
        if self.kind == 'function':
            return 'extern fn(...)'
        return f'{self.record.kind} {{...}}'


class Attribute:
    """`#[name(arguments)]`: arguments are kept as raw token texts."""

    def __init__(self, name: str, arguments: list[str]) -> None:
        self.name = name
        self.arguments = arguments


class Bit:
    """One logical member of a bit-field block; name '_' is padding."""

    def __init__(self, name: str, member_type: TypeRef, width: int) -> None:
        self.name = name
        self.member_type = member_type
        self.width = width


class Field:
    def __init__(self, name: str, field_type: TypeRef, attributes: list[Attribute]) -> None:
        self.name = name
        self.field_type = field_type
        self.attributes = attributes
        self.bits: list[Bit] = []


class Param:
    """A parameter; name '...' marks C varargs."""

    def __init__(self, name: str, param_type: 'TypeRef | None', attributes: list[Attribute]) -> None:
        self.name = name
        self.param_type = param_type
        self.attributes = attributes


class Method:
    def __init__(self, name: str, params: list[Param], result: 'TypeRef | None',
                 attributes: list[Attribute]) -> None:
        self.name = name
        self.params = params
        self.result = result
        self.attributes = attributes


class Decl:
    """A top-level (or nested record) declaration.

    kind: 'struct', 'union', 'enum', 'interface', 'type', 'const', 'function'
    (an `extern fn` with a library), 'callback' (an `extern fn` without one:
    a function-pointer type). abi is 'system' or 'C'.
    """

    def __init__(self, kind: str, name: str, attributes: list[Attribute], source: str, line: int) -> None:
        self.kind = kind
        self.name = name
        self.attributes = attributes
        self.source = source
        self.line = line
        self.fields: list[Field] = []
        self.nested: list['Decl'] = []
        self.members: list[tuple[str, str]] = []
        self.base: 'TypeRef | None' = None
        self.methods: list[Method] = []
        self.aliased: 'TypeRef | None' = None
        self.value_type: 'TypeRef | None' = None
        self.value = ''
        self.value_kind = ''
        self.params: list[Param] = []
        self.result: 'TypeRef | None' = None
        self.abi = 'system'

    def attribute(self, name: str) -> 'Attribute | None':
        for item in self.attributes:
            if item.name == name:
                return item
        return None


def find_attribute(attributes: list[Attribute], name: str) -> 'Attribute | None':
    for item in attributes:
        if item.name == name:
            return item
    return None


class Parser:
    def __init__(self, tokens: list[Token], source: str) -> None:
        self.tokens = tokens
        self.position = 0
        self.source = source

    def peek(self, offset: int = 0) -> 'Token | None':
        index = self.position + offset
        if index < len(self.tokens):
            return self.tokens[index]
        return None

    def fail(self, message: str) -> RdlError:
        token = self.peek()
        line = token.line if token is not None else -1
        found = token.text if token is not None else 'end of file'
        return RdlError(f'{self.source}:{line}: {message} (found {found!r})')

    def at(self, text: str) -> bool:
        token = self.peek()
        return token is not None and token.text == text and token.kind in ('punct', 'name')

    def take(self, text: str) -> Token:
        token = self.peek()
        if token is None or token.text != text:
            raise self.fail(f'expected {text!r}')
        self.position += 1
        return token

    def take_name(self) -> str:
        token = self.peek()
        if token is None or token.kind != 'name':
            raise self.fail('expected a name')
        self.position += 1
        return token.text

    def take_integer(self) -> int:
        token = self.peek()
        if token is None or token.kind != 'number':
            raise self.fail('expected an integer')
        self.position += 1
        return int(token.text.replace('_', ''), 0)

    def attributes(self) -> list[Attribute]:
        collected: list[Attribute] = []
        while self.at('#['):
            self.take('#[')
            name = self.take_name()
            arguments: list[str] = []
            if self.at('('):
                self.take('(')
                depth = 1
                while depth > 0:
                    token = self.peek()
                    if token is None:
                        raise self.fail('unterminated attribute')
                    self.position += 1
                    if token.text == '(' and token.kind == 'punct':
                        depth += 1
                    elif token.text == ')' and token.kind == 'punct':
                        depth -= 1
                        if depth == 0:
                            break
                    if token.kind == 'string':
                        arguments.append('"' + token.text + '"')
                    else:
                        arguments.append(token.text)
            self.take(']')
            collected.append(Attribute(name, arguments))
        return collected

    def type_ref(self) -> TypeRef:
        if self.at('extern'):
            # An inline function-pointer type: `extern fn(*mut void) -> HRESULT`.
            self.take('extern')
            result = TypeRef('function')
            after = self.peek()
            if after is not None and after.kind == 'string':
                result.abi = after.text
                self.position += 1
            self.take('fn')
            self.take('(')
            while not self.at(')'):
                if self.at('...'):
                    self.take('...')
                    result.parameter_types.append(None)
                else:
                    if self.peek(1) is not None and self.peek(1).text == ':' and self.peek().kind == 'name':
                        self.take_name()
                        self.take(':')
                    result.parameter_types.append(self.type_ref())
                if self.at(','):
                    self.take(',')
            self.take(')')
            result.target = self.result_type()
            return result
        if self.at('*'):
            self.take('*')
            qualifier = self.take_name()
            if qualifier not in ('mut', 'const'):
                raise self.fail('expected mut or const after *')
            result = TypeRef('pointer')
            result.mutable = qualifier == 'mut'
            result.target = self.type_ref()
            return result
        if self.at('&'):
            self.take('&')
            result = TypeRef('reference')
            if self.at('mut'):
                self.take('mut')
                result.mutable = True
            result.target = self.type_ref()
            return result
        if self.at('['):
            self.take('[')
            result = TypeRef('array')
            result.target = self.type_ref()
            self.take(';')
            result.count = self.take_integer()
            self.take(']')
            return result
        if self.at('ptr64') and self.peek(1) is not None and self.peek(1).text == '<':
            self.take('ptr64')
            self.take('<')
            result = TypeRef('ptr64')
            result.target = self.type_ref()
            self.take('>')
            return result
        namespace = ''
        name = self.take_name()
        while self.at('::'):
            self.take('::')
            namespace += name + '::'
            name = self.take_name()
        result = TypeRef('named')
        result.name = name
        result.namespace = namespace
        return result

    def record_body(self, record: Decl) -> None:
        self.take('{')
        while not self.at('}'):
            attributes = self.attributes()
            if self.at('const'):
                # A record-scoped constant (a C++ static member).
                token = self.take('const')
                constant = Decl('const', self.take_name(), attributes, self.source, token.line)
                self.take(':')
                constant.value_type = self.type_ref()
                self.take('=')
                constant.value_kind, constant.value = self.value()
                self.take(';')
                record.nested.append(constant)
                continue
            if self.at('struct') or self.at('union'):
                kind = self.take_name()
                token = self.peek()
                nested = Decl(kind, self.take_name(), attributes, self.source, token.line)
                self.record_body(nested)
                record.nested.append(nested)
                if self.at(','):
                    self.take(',')
                continue
            name = self.take_name()
            self.take(':')
            field_type: TypeRef
            bits: list[Bit] = []
            # An inline record may carry its own attributes: `#[packed(4)] union {`.
            inline_attributes = self.attributes()
            if self.at('struct') or self.at('union'):
                kind = self.take_name()
                token = self.peek()
                inline = Decl(kind, '', inline_attributes, self.source, token.line)
                self.record_body(inline)
                field_type = TypeRef('record')
                field_type.record = inline
            else:
                field_type = self.type_ref()
                if self.at('{'):
                    self.take('{')
                    while not self.at('}'):
                        bit_name = self.take_name()
                        self.take(':')
                        if self.peek() is not None and self.peek().kind == 'number':
                            # Padding: `_: 3`.
                            bits.append(Bit(bit_name, field_type, self.take_integer()))
                        else:
                            bit_type = self.type_ref()
                            self.take(':')
                            bits.append(Bit(bit_name, bit_type, self.take_integer()))
                        if self.at(','):
                            self.take(',')
                    self.take('}')
            field = Field(name, field_type, attributes)
            field.bits = bits
            record.fields.append(field)
            if self.at(','):
                self.take(',')
        self.take('}')

    def value(self) -> tuple[str, str]:
        """Returns (kind, text): kind is 'number', 'string', 'bool' or 'name'."""
        negative = False
        if self.at('-'):
            self.take('-')
            negative = True
        token = self.peek()
        if token is None:
            raise self.fail('expected a value')
        self.position += 1
        if token.kind == 'number':
            return ('number', ('-' if negative else '') + token.text)
        if token.kind == 'string':
            return ('string', token.text)
        if token.text in ('true', 'false'):
            return ('bool', token.text)
        return ('name', token.text)

    def params(self) -> list[Param]:
        self.take('(')
        collected: list[Param] = []
        while not self.at(')'):
            attributes = self.attributes()
            if self.at('...'):
                self.take('...')
                collected.append(Param('...', None, attributes))
            elif self.at('&') and self.peek(1) is not None and self.peek(1).text in ('self', 'mut'):
                self.take('&')
                if self.at('mut'):
                    self.take('mut')
                self.take('self')
            else:
                name = self.take_name()
                self.take(':')
                collected.append(Param(name, self.type_ref(), attributes))
            if self.at(','):
                self.take(',')
        self.take(')')
        return collected

    def result_type(self) -> 'TypeRef | None':
        if self.at('->'):
            self.take('->')
            return self.type_ref()
        return None

    def items(self, declarations: list[Decl]) -> None:
        while not self.at('}'):
            attributes = self.attributes()
            token = self.peek()
            if token is None:
                raise self.fail('unexpected end of file')
            keyword = token.text
            if keyword == 'mod':
                self.take('mod')
                self.take_name()
                self.take('{')
                self.items(declarations)
                self.take('}')
                continue
            if keyword in ('struct', 'union'):
                self.take(keyword)
                declaration = Decl(keyword, self.take_name(), attributes, self.source, token.line)
                self.record_body(declaration)
                declarations.append(declaration)
                continue
            if keyword == 'enum':
                self.take('enum')
                declaration = Decl('enum', self.take_name(), attributes, self.source, token.line)
                self.take('{')
                while not self.at('}'):
                    member = self.take_name()
                    self.take('=')
                    kind, text = self.value()
                    if kind != 'number':
                        raise self.fail('expected a numeric enumerator')
                    declaration.members.append((member, text))
                    if self.at(','):
                        self.take(',')
                self.take('}')
                declarations.append(declaration)
                continue
            if keyword == 'interface':
                self.take('interface')
                declaration = Decl('interface', self.take_name(), attributes, self.source, token.line)
                if self.at(':'):
                    self.take(':')
                    declaration.base = self.type_ref()
                self.take('{')
                while not self.at('}'):
                    method_attributes = self.attributes()
                    self.take('fn')
                    method_name = self.take_name()
                    method_params = self.params()
                    method_result = self.result_type()
                    self.take(';')
                    declaration.methods.append(Method(method_name, method_params, method_result,
                                                      method_attributes))
                self.take('}')
                declarations.append(declaration)
                continue
            if keyword == 'type':
                self.take('type')
                declaration = Decl('type', self.take_name(), attributes, self.source, token.line)
                self.take('=')
                declaration.aliased = self.type_ref()
                self.take(';')
                declarations.append(declaration)
                continue
            if keyword == 'const':
                self.take('const')
                declaration = Decl('const', self.take_name(), attributes, self.source, token.line)
                self.take(':')
                declaration.value_type = self.type_ref()
                self.take('=')
                declaration.value_kind, declaration.value = self.value()
                self.take(';')
                declarations.append(declaration)
                continue
            if keyword == 'extern':
                self.take('extern')
                abi = 'system'
                after = self.peek()
                if after is not None and after.kind == 'string':
                    abi = after.text
                    self.position += 1
                self.take('fn')
                name = self.take_name()
                kind = 'function' if find_attribute(attributes, 'library') is not None else 'callback'
                declaration = Decl(kind, name, attributes, self.source, token.line)
                declaration.abi = abi
                declaration.params = self.params()
                declaration.result = self.result_type()
                self.take(';')
                declarations.append(declaration)
                continue
            raise self.fail('expected a declaration')


def parse_file(path: pathlib.Path) -> list[Decl]:
    text = path.read_text(encoding='utf-8')
    tokens = tokenize(text, path.name)
    # A sentinel closing brace lets items() run to the end of the file.
    tokens.append(Token('punct', '}', -1))
    parser = Parser(tokens, path.stem)
    declarations: list[Decl] = []
    parser.items(declarations)
    if parser.position != len(tokens) - 1:
        raise parser.fail('trailing tokens')
    return declarations


def arch_allows(declaration_attributes: list[Attribute], arch: str) -> bool:
    """True when the declaration applies to arch ('X86', 'X64' or 'Arm64')."""
    restriction = find_attribute(declaration_attributes, 'arch')
    if restriction is None:
        return True
    return arch in restriction.arguments


def load_directory(directory: pathlib.Path) -> list[Decl]:
    """Every declaration in directory/*.rdl, files in name order."""
    declarations: list[Decl] = []
    paths = sorted(directory.glob('*.rdl'))
    for path in paths:
        declarations.extend(parse_file(path))
    return declarations
