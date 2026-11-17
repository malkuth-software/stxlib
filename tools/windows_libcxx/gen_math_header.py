#!/usr/bin/env python3
"""Regenerate the declaration block of src/hosted/windows/crt/include/math.h
(between "BEGIN GENERATED" and "END GENERATED") from the ucrtbase.dll export
snapshot.

For each C17 7.12 function: the double form is an import; the float form is
an import when ucrtbase.dll exports it and otherwise an inline form over the
double one; the long double form is always an inline form over the double
one (long double is double on x86_64-pc-windows-msvc).

  gen_math_header.py           check that math.h matches; exit 1 if not
  gen_math_header.py --write   rewrite the block
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CRT = ROOT / "src" / "hosted" / "windows" / "crt"
HEADER = CRT / "include" / "math.h"
EXPORTS = CRT / "ucrtbase_exports.txt"
BEGIN = "/* BEGIN GENERATED */\n"
END = "/* END GENERATED */\n"

# (name, return type, parameters); "T" stands for the floating type.
FUNCTIONS = [
    ("acos", "T", "T x"), ("asin", "T", "T x"), ("atan", "T", "T x"), ("atan2", "T", "T y, T x"),
    ("cos", "T", "T x"), ("sin", "T", "T x"), ("tan", "T", "T x"),
    ("acosh", "T", "T x"), ("asinh", "T", "T x"), ("atanh", "T", "T x"),
    ("cosh", "T", "T x"), ("sinh", "T", "T x"), ("tanh", "T", "T x"),
    ("exp", "T", "T x"), ("exp2", "T", "T x"), ("expm1", "T", "T x"),
    ("frexp", "T", "T x, int* exponent"), ("ilogb", "int", "T x"), ("ldexp", "T", "T x, int exponent"),
    ("log", "T", "T x"), ("log10", "T", "T x"), ("log1p", "T", "T x"), ("log2", "T", "T x"), ("logb", "T", "T x"),
    ("modf", "T", "T x, T* integral"), ("scalbn", "T", "T x, int exponent"), ("scalbln", "T", "T x, long exponent"),
    ("cbrt", "T", "T x"), ("fabs", "T", "T x"), ("hypot", "T", "T x, T y"), ("pow", "T", "T x, T y"),
    ("sqrt", "T", "T x"),
    ("erf", "T", "T x"), ("erfc", "T", "T x"), ("lgamma", "T", "T x"), ("tgamma", "T", "T x"),
    ("ceil", "T", "T x"), ("floor", "T", "T x"), ("nearbyint", "T", "T x"), ("rint", "T", "T x"),
    ("lrint", "long", "T x"), ("llrint", "long long", "T x"), ("round", "T", "T x"), ("lround", "long", "T x"),
    ("llround", "long long", "T x"), ("trunc", "T", "T x"),
    ("fmod", "T", "T x, T y"), ("remainder", "T", "T x, T y"), ("remquo", "T", "T x, T y, int* quotient"),
    ("copysign", "T", "T x, T y"), ("nan", "T", "const char* tag"), ("nextafter", "T", "T x, T y"),
    ("nexttoward", "T", "T x, long double y"),
    ("fdim", "T", "T x, T y"), ("fmax", "T", "T x, T y"), ("fmin", "T", "T x, T y"), ("fma", "T", "T x, T y, T z"),
]

# Float forms the UCRT does not export under the standard name but under
# another (or as a compiler builtin).
FLOAT_CALLS = {"hypot": "_hypotf(x, y)", "fabs": "__builtin_fabsf(x)"}


def split_parameters(parameters: str) -> list:
    return [part.strip() for part in parameters.split(",")]


def parameter_name(parameter: str) -> str:
    return parameter.split()[-1].lstrip("*")


def typed_parameters(parameters: str, floating: str) -> str:
    typed = []
    for parameter in split_parameters(parameters):
        if parameter.startswith("T*"):
            typed.append(floating + "* " + parameter_name(parameter))
        elif parameter.startswith("T "):
            typed.append(floating + " " + parameter_name(parameter))
        else:
            typed.append(parameter)
    return ", ".join(typed)


def forwarded_arguments(parameters: str) -> str:
    arguments = []
    for parameter in split_parameters(parameters):
        name = parameter_name(parameter)
        if parameter.startswith("T*"):
            arguments.append("&value_" + name)
        elif parameter.startswith("T "):
            arguments.append("(double)" + name)
        elif parameter.startswith("long double"):
            arguments.append("(long double)" + name)
        else:
            arguments.append(name)
    return ", ".join(arguments)


def inline_form(name: str, suffix: str, floating: str, result_type: str, parameters: str) -> list:
    pointers = [p for p in split_parameters(parameters) if p.startswith("T*")]
    lines = [f"_STXLIB_CRT_INLINE {result_type} __cdecl {name}{suffix}({typed_parameters(parameters, floating)}) {{"]
    for pointer in pointers:
        lines.append(f"    double value_{parameter_name(pointer)} = 0.0;")
    call = f"{name}({forwarded_arguments(parameters)})"
    if suffix == "f" and name in FLOAT_CALLS:
        call = FLOAT_CALLS[name]
    if result_type == floating:
        lines.append(f"    const {floating} result = ({floating}){call};")
    else:
        lines.append(f"    const {result_type} result = {call};")
    for pointer in pointers:
        pointer_name = parameter_name(pointer)
        lines.append(f"    *{pointer_name} = ({floating})value_{pointer_name};")
    lines.append("    return result;")
    lines.append("}")
    return lines


def generate(exports: set) -> str:
    lines = []
    for name, result, parameters in FUNCTIONS:
        if name not in exports:
            raise SystemExit(f"gen_math_header: ucrtbase.dll does not export {name}")
        result_type = "double" if result == "T" else result
        lines.append(f"_STXLIB_UCRT {result_type} __cdecl {name}({typed_parameters(parameters, 'double')});")
    for suffix, floating in (("f", "float"), ("l", "long double")):
        lines.append("")
        for name, result, parameters in FUNCTIONS:
            result_type = floating if result == "T" else result
            if suffix == "f" and name + suffix in exports:
                lines.append(
                    f"_STXLIB_UCRT {result_type} __cdecl {name}{suffix}({typed_parameters(parameters, floating)});")
            else:
                lines.extend(inline_form(name, suffix, floating, result_type, parameters))
    return "\n".join(lines) + "\n"


def main() -> int:
    exports = {line.strip() for line in EXPORTS.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.startswith("#")}
    text = HEADER.read_text(encoding="utf-8").replace("\r\n", "\n")
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    expected = head + BEGIN + generate(exports) + END + tail
    if "--write" in sys.argv[1:]:
        HEADER.write_text(expected, encoding="utf-8", newline="\n")
        print("gen_math_header: written")
        return 0
    if expected != text:
        print("gen_math_header: math.h differs from the generator (run with --write)")
        return 1
    print("gen_math_header: math.h matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
