"""Record layout from the metadata, by the x86_64 msvc rules.

Every scalar is aligned to its size (pointers 8), a record to its most
aligned member, `#[packed(N)]` caps member alignment at N (MSVC
`#pragma pack`), `#[align(N)]` raises it (`__declspec(align(N))`). A
union's members all start at 0. Bit-fields arrive coalesced by the
metadata into one backing integer field, so they lay out as that field.

The numbers are what `check/layout_check.cpp` asserts against the
compiler's view of the emitted headers.
"""

import rdl

SCALAR_SIZES = {
    'void': 0, 'bool': 1, 'i8': 1, 'u8': 1, 'i16': 2, 'u16': 2, 'i32': 4, 'u32': 4, 'i64': 8, 'u64': 8,
    'isize': 8, 'usize': 8, 'f32': 4, 'f64': 8,
}
POINTER_SIZE = 8


class LayoutError(Exception):
    pass


class RecordLayout:
    def __init__(self, size: int, alignment: int, offsets: list[tuple[str, int]]) -> None:
        self.size = size
        self.alignment = alignment
        self.offsets = offsets


def round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def is_anonymous_member(field: rdl.Field) -> bool:
    name = field.name
    if not name.startswith('Anonymous') or field.field_type.kind != 'record':
        return False
    return name[len('Anonymous'):] == '' or name[len('Anonymous'):].isdigit()


class Layout:
    def __init__(self, declarations: dict[str, rdl.Decl], primitives: dict[str, str]) -> None:
        self.declarations = declarations
        self.primitives = primitives
        self.cache: dict[str, RecordLayout] = {}

    def resolve(self, type_ref: rdl.TypeRef) -> rdl.TypeRef:
        """Follows typedefs to the first non-typedef type."""
        current = type_ref
        steps = 0
        while current.kind == 'named' and not current.namespace and current.name not in SCALAR_SIZES:
            declaration = self.declarations.get(current.name)
            if declaration is None or declaration.kind != 'type':
                return current
            current = declaration.aliased
            steps += 1
            if steps > 64:
                raise LayoutError(f'typedef chain too long at {type_ref.describe()}')
        return current

    def size_alignment(self, type_ref: rdl.TypeRef, pack: int) -> tuple[int, int]:
        resolved = self.resolve(type_ref)
        kind = resolved.kind
        if kind in ('pointer', 'reference', 'ptr64', 'function'):
            return (POINTER_SIZE, POINTER_SIZE)
        if kind == 'array':
            size, alignment = self.size_alignment(resolved.target, pack)
            return (size * resolved.count, alignment)
        if kind == 'record':
            record_layout = self.record_body(resolved.record, pack)
            return (record_layout.size, record_layout.alignment)
        if resolved.namespace:
            return (POINTER_SIZE, POINTER_SIZE)
        name = resolved.name
        if name in SCALAR_SIZES:
            size = SCALAR_SIZES[name]
            return (size, size)
        declaration = self.declarations.get(name)
        if declaration is None:
            raise LayoutError(f'{name}: by value but not in the metadata')
        if declaration.kind in ('interface', 'callback'):
            return (POINTER_SIZE, POINTER_SIZE)
        if declaration.kind == 'enum':
            representation = declaration.attribute('repr')
            base = representation.arguments[0] if representation is not None else 'i32'
            size = SCALAR_SIZES[base]
            return (size, size)
        if declaration.kind in ('struct', 'union'):
            record_layout = self.record(declaration)
            return (record_layout.size, record_layout.alignment)
        raise LayoutError(f'{name}: no layout for a {declaration.kind}')

    def record(self, declaration: rdl.Decl) -> RecordLayout:
        if declaration.name in self.cache:
            return self.cache[declaration.name]
        packed = declaration.attribute('packed')
        pack = int(packed.arguments[0]) if packed is not None else 0
        result = self.record_body(declaration, pack)
        self.cache[declaration.name] = result
        return result

    def record_body(self, record: rdl.Decl, pack: int) -> RecordLayout:
        own_pack = record.attribute('packed')
        if own_pack is not None:
            pack = int(own_pack.arguments[0])
        is_union = record.kind == 'union'
        offset = 0
        size = 0
        alignment = 1
        offsets: list[tuple[str, int]] = []
        for field in record.fields:
            field_size, field_alignment = self.size_alignment(field.field_type, pack)
            if pack:
                field_alignment = min(field_alignment, pack)
            raised = rdl.find_attribute(field.attributes, 'align')
            if raised is not None:
                field_alignment = max(field_alignment, int(raised.arguments[0]))
            field_alignment = max(field_alignment, 1)
            start = 0 if is_union else round_up(offset, field_alignment)
            if not field.bits:
                if is_anonymous_member(field):
                    inner = self.record_body(field.field_type.record, pack)
                    for member, member_offset in inner.offsets:
                        offsets.append((member, start + member_offset))
                else:
                    offsets.append((field.name if field.name not in KEYWORDS else field.name + '_', start))
            alignment = max(alignment, field_alignment)
            if is_union:
                size = max(size, field_size)
            else:
                offset = start + field_size
                size = offset
        raised = record.attribute('align')
        if raised is not None:
            alignment = max(alignment, int(raised.arguments[0]))
        size = round_up(size, alignment)
        return RecordLayout(size, alignment, offsets)

    def complete_dependencies(self, record: rdl.Decl) -> list[str]:
        """Records (and typedefs naming them) a record holds by value."""
        result: list[str] = []
        pending: list[rdl.TypeRef] = []
        for field in record.fields:
            pending.append(field.field_type)
        for nested in record.nested:
            if nested.kind in ('struct', 'union'):
                for field in nested.fields:
                    pending.append(field.field_type)
        while pending:
            current = pending.pop()
            if current.kind == 'array':
                pending.append(current.target)
                continue
            if current.kind == 'record':
                for field in current.record.fields:
                    pending.append(field.field_type)
                continue
            if current.kind != 'named' or current.namespace or current.name in SCALAR_SIZES:
                continue
            declaration = self.declarations.get(current.name)
            if declaration is None:
                continue
            if declaration.kind in ('struct', 'union'):
                result.append(declaration.name)
            elif declaration.kind == 'type':
                result.append(declaration.name)
                pending.append(declaration.aliased)
        return result


KEYWORDS = {'class', 'new', 'delete', 'default', 'operator', 'private', 'public', 'protected', 'template',
            'this', 'virtual', 'namespace', 'typename', 'using', 'friend', 'explicit', 'mutable', 'interface',
            'bool', 'true', 'false', 'register', 'auto', 'char8_t', 'concept', 'requires', 'export'}
